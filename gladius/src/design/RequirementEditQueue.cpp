#include "RequirementEditQueue.h"
#include "DesignGraph.h"

#include <algorithm>
#include <map>
#include <utility>

namespace gladius::design
{
    void RequirementEditQueue::stageAdd(std::string description,
                                        std::string category,
                                        bool mandatory)
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        RequirementEdit edit;
        edit.kind = RequirementEdit::Kind::Add;
        // The graph only names a requirement when it is applied, so the UI row needs a local id to
        // key itself by. A later edit of that row is re-pointed at the real id on apply.
        edit.id = "staged_req_" + std::to_string(m_nextStagedId++);
        edit.description = std::move(description);
        edit.category = std::move(category);
        edit.mandatory = mandatory;
        m_edits.push_back(std::move(edit));
    }

    void RequirementEditQueue::stageUpdate(std::string id,
                                           std::string description,
                                           std::string category,
                                           bool mandatory)
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        RequirementEdit edit;
        edit.kind = RequirementEdit::Kind::Update;
        edit.id = std::move(id);
        edit.description = std::move(description);
        edit.category = std::move(category);
        edit.mandatory = mandatory;
        replaceOrAppendLocked(std::move(edit));
    }

    void RequirementEditQueue::stageRemove(std::string id)
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        RequirementEdit edit;
        edit.kind = RequirementEdit::Kind::Remove;
        edit.id = std::move(id);
        replaceOrAppendLocked(std::move(edit));
    }

    std::vector<Requirement> RequirementEditQueue::mergedView(DesignGraph const & graph) const
    {
        std::vector<Requirement> view = graph.requirements();
        std::lock_guard<std::mutex> const lock(m_mutex);
        for (auto const & edit : m_edits)
        {
            if (edit.kind == RequirementEdit::Kind::Add)
            {
                view.push_back(
                  Requirement{edit.id, edit.description, edit.category, edit.mandatory});
                continue;
            }
            auto const it = std::find_if(view.begin(), view.end(), [&](Requirement const & r) {
                return r.id == edit.id;
            });
            if (it == view.end())
            {
                continue;
            }
            if (edit.kind == RequirementEdit::Kind::Remove)
            {
                view.erase(it);
                continue;
            }
            it->description = edit.description;
            it->category = edit.category;
            it->mandatory = edit.mandatory;
        }
        return view;
    }

    void RequirementEditQueue::applyTo(DesignGraph & graph)
    {
        std::vector<RequirementEdit> edits;
        {
            std::lock_guard<std::mutex> const lock(m_mutex);
            edits.swap(m_edits);
        }
        // A staged addition is applied first and renamed by the graph; edits the user made against
        // its provisional id have to follow it under the real id.
        std::map<std::string, std::string> provisionalToReal;
        for (auto & edit : edits)
        {
            auto const mapped = provisionalToReal.find(edit.id);
            if (mapped != provisionalToReal.end())
            {
                edit.id = mapped->second;
            }

            switch (edit.kind)
            {
                case RequirementEdit::Kind::Add: {
                    std::string const realId = graph.addRequirement(
                      edit.description, edit.category, edit.mandatory);
                    graph.recordUserAddedRequirement(realId);
                    provisionalToReal[edit.id] = realId;
                    break;
                }
                case RequirementEdit::Kind::Update:
                    graph.updateRequirement(edit.id,
                                            edit.description,
                                            edit.category,
                                            edit.mandatory,
                                            true);
                    break;
                case RequirementEdit::Kind::Remove:
                    graph.removeRequirement(edit.id, true);
                    break;
            }
        }
    }

    std::size_t RequirementEditQueue::size() const
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        return m_edits.size();
    }

    bool RequirementEditQueue::empty() const
    {
        return size() == 0;
    }

    void RequirementEditQueue::clear()
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        m_edits.clear();
    }

    void RequirementEditQueue::replaceOrAppendLocked(RequirementEdit edit)
    {
        auto const it = std::find_if(m_edits.begin(), m_edits.end(), [&](RequirementEdit const & e) {
            return e.id == edit.id;
        });
        if (it == m_edits.end())
        {
            m_edits.push_back(std::move(edit));
            return;
        }
        // Repeated keystrokes on one row must not each become a separate change line for the model.
        bool const keepAdd = it->kind == RequirementEdit::Kind::Add;
        if (keepAdd)
        {
            edit.kind = RequirementEdit::Kind::Add;
        }
        *it = std::move(edit);
    }
} // namespace gladius::design
