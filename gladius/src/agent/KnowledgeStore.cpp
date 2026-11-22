#include "KnowledgeStore.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

#ifndef __EMSCRIPTEN__
#include <sago/platform_folders.h>
#endif

namespace gladius::agent
{
    namespace
    {
        std::string toLowerCopy(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return s;
        }

        std::vector<std::string> tokenizeLower(std::string const & text)
        {
            std::vector<std::string> tokens;
            std::string cur;
            for (unsigned char c : text)
            {
                if (std::isalnum(c))
                {
                    cur.push_back(static_cast<char>(std::tolower(c)));
                }
                else if (!cur.empty())
                {
                    if (cur.size() >= 2)
                    {
                        tokens.push_back(cur);
                    }
                    cur.clear();
                }
            }
            if (cur.size() >= 2)
            {
                tokens.push_back(std::move(cur));
            }
            return tokens;
        }
    } // namespace

    KnowledgeStore::KnowledgeStore(std::filesystem::path storagePath)
        : m_storagePath(std::move(storagePath))
    {
    }

    void KnowledgeStore::setStoragePath(std::filesystem::path storagePath)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_storagePath = std::move(storagePath);
    }

    std::filesystem::path KnowledgeStore::storagePath() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_storagePath;
    }

    std::filesystem::path KnowledgeStore::defaultStoragePath()
    {
#ifdef __EMSCRIPTEN__
        return std::filesystem::path{"/gladius"} / "agent_knowledge.json";
#else
        return std::filesystem::path{sago::getConfigHome()} / "gladius" / "agent_knowledge.json";
#endif
    }

    std::string KnowledgeStore::currentIsoTimestamp()
    {
        auto const now = std::chrono::system_clock::now();
        auto const tt = std::chrono::system_clock::to_time_t(now);
        std::tm tmBuf{};
#if defined(_WIN32)
        gmtime_s(&tmBuf, &tt);
#else
        gmtime_r(&tt, &tmBuf);
#endif
        std::ostringstream out;
        out << std::put_time(&tmBuf, "%Y-%m-%dT%H:%M:%SZ");
        return out.str();
    }

    bool KnowledgeStore::load()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_storagePath.empty() || !std::filesystem::exists(m_storagePath))
        {
            return false;
        }
        try
        {
            std::ifstream in(m_storagePath);
            if (!in)
            {
                return false;
            }
            nlohmann::json root;
            in >> root;
            if (!root.is_object())
            {
                return false;
            }
            m_entries.clear();
            if (root.contains("entries") && root["entries"].is_array())
            {
                for (auto const & j : root["entries"])
                {
                    KnowledgeEntry e;
                    e.id = j.value("id", "");
                    e.title = j.value("title", "");
                    e.category = j.value("category", "general");
                    e.tags = j.value("tags", std::vector<std::string>{});
                    e.summary = j.value("summary", "");
                    e.createdAt = j.value("createdAt", "");
                    e.updatedAt = j.value("updatedAt", "");
                    e.useCount = j.value("useCount", 0);
                    if (!e.id.empty() && !e.summary.empty())
                    {
                        m_entries.push_back(std::move(e));
                    }
                }
            }
            m_nextId = root.value("nextId", m_entries.size() + 1);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool KnowledgeStore::save() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_storagePath.empty())
        {
            return false;
        }
        try
        {
            std::error_code ec;
            auto const parent = m_storagePath.parent_path();
            if (!parent.empty() && !std::filesystem::exists(parent, ec))
            {
                std::filesystem::create_directories(parent, ec);
            }
            nlohmann::json arr = nlohmann::json::array();
            for (auto const & e : m_entries)
            {
                arr.push_back({
                  {"id", e.id},
                  {"title", e.title},
                  {"category", e.category},
                  {"tags", e.tags},
                  {"summary", e.summary},
                  {"createdAt", e.createdAt},
                  {"updatedAt", e.updatedAt},
                  {"useCount", e.useCount}});
            }
            nlohmann::json root = {{"version", 1}, {"nextId", m_nextId}, {"entries", std::move(arr)}};
            std::ofstream out(m_storagePath, std::ios::trunc);
            if (!out)
            {
                return false;
            }
            out << root.dump(2);
            return static_cast<bool>(out);
        }
        catch (...)
        {
            return false;
        }
    }

    std::string KnowledgeStore::store(std::string title,
                                      std::string category,
                                      std::vector<std::string> tags,
                                      std::string summary)
    {
        std::string id;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            id = "know_" + std::to_string(m_nextId++);
            std::string const ts = currentIsoTimestamp();
            m_entries.push_back(KnowledgeEntry{
              id,
              std::move(title),
              category.empty() ? "general" : std::move(category),
              std::move(tags),
              std::move(summary),
              ts,
              ts,
              0});
        }
        (void) save();
        return id;
    }

    bool KnowledgeStore::update(std::string const & id,
                                std::string const & title,
                                std::string const & category,
                                std::vector<std::string> const & tags,
                                std::string const & summary)
    {
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](KnowledgeEntry const & e) {
                return e.id == id;
            });
            if (it != m_entries.end())
            {
                if (!title.empty())
                {
                    it->title = title;
                }
                if (!category.empty())
                {
                    it->category = category;
                }
                if (!tags.empty())
                {
                    it->tags = tags;
                }
                if (!summary.empty())
                {
                    it->summary = summary;
                }
                it->updatedAt = currentIsoTimestamp();
                found = true;
            }
        }
        if (found)
        {
            (void) save();
        }
        return found;
    }

    bool KnowledgeStore::remove(std::string const & id)
    {
        bool removed = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto const oldSize = m_entries.size();
            m_entries.erase(
              std::remove_if(m_entries.begin(),
                             m_entries.end(),
                             [&](KnowledgeEntry const & e) { return e.id == id; }),
              m_entries.end());
            removed = m_entries.size() != oldSize;
        }
        if (removed)
        {
            (void) save();
        }
        return removed;
    }

    std::vector<KnowledgeEntry> KnowledgeStore::search(std::string const & query,
                                                       std::string const & category,
                                                       std::size_t limit)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string const catLower = toLowerCopy(category);
        auto const queryTokens = tokenizeLower(query);

        struct Scored
        {
            std::size_t index;
            int score;
        };
        std::vector<Scored> scored;

        for (std::size_t i = 0; i < m_entries.size(); ++i)
        {
            auto const & e = m_entries[i];
            if (!catLower.empty() && toLowerCopy(e.category) != catLower)
            {
                continue;
            }
            if (queryTokens.empty())
            {
                scored.push_back(Scored{i, 1 + e.useCount});
                continue;
            }

            std::string const titleLow = toLowerCopy(e.title);
            std::string const summaryLow = toLowerCopy(e.summary);
            std::string tagsJoined;
            for (auto const & t : e.tags)
            {
                tagsJoined += " " + toLowerCopy(t);
            }

            int score = 0;
            for (auto const & tok : queryTokens)
            {
                if (titleLow.find(tok) != std::string::npos)
                {
                    score += 4;
                }
                if (tagsJoined.find(tok) != std::string::npos)
                {
                    score += 3;
                }
                if (summaryLow.find(tok) != std::string::npos)
                {
                    score += 2;
                }
            }
            if (score > 0)
            {
                scored.push_back(Scored{i, score + std::min(e.useCount, 5)});
            }
        }

        std::stable_sort(scored.begin(), scored.end(), [](Scored const & a, Scored const & b) {
            return a.score > b.score;
        });

        std::vector<KnowledgeEntry> result;
        std::size_t const count = std::min(limit, scored.size());
        result.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            m_entries[scored[i].index].useCount += 1;
            result.push_back(m_entries[scored[i].index]);
        }
        return result;
    }

    std::vector<KnowledgeEntry> KnowledgeStore::listAll() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_entries;
    }

    std::string KnowledgeStore::summarizeForPrompt(std::string const & query, std::size_t limit)
    {
        auto const hits = search(query, "", limit);
        if (hits.empty())
        {
            return "";
        }
        std::ostringstream out;
        for (auto const & e : hits)
        {
            out << "- [" << e.category << "] " << e.title << ": " << e.summary << "\n";
        }
        return out.str();
    }

    void KnowledgeStore::clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
        m_nextId = 1;
    }
} // namespace gladius::agent
