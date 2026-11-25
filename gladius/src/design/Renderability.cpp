#include "Renderability.h"

#include "Document.h"
#include "nodes/Assembly.h"
#include "nodes/Model.h"
#include "nodes/nodesfwd.h"

#include <fmt/format.h>

#include <algorithm>
#include <sstream>
#include <vector>

namespace gladius::design
{
    namespace
    {
        void addBlocker(RenderabilityFacts & facts,
                        std::string code,
                        std::string message,
                        std::string fix)
        {
            facts.blockers.push_back({std::move(code), std::move(message), std::move(fix)});
        }

        /// @brief True when the implicit function exposes the given scalar output port.
        bool hasOutputPort(Lib3MF::PFunction const & function, std::string const & channel)
        {
            if (!function)
            {
                return false;
            }
            try
            {
                return function->FindOutput(channel) != nullptr;
            }
            catch (Lib3MF::ELib3MFException const &)
            {
                return false;
            }
        }
    } // namespace

    std::string RenderabilityFacts::blockersAsText() const
    {
        std::ostringstream out;
        for (size_t i = 0; i < blockers.size(); ++i)
        {
            if (i != 0)
            {
                out << "; ";
            }
            out << blockers[i].code << ": " << blockers[i].message;
            if (!blockers[i].fix.empty())
            {
                out << " -> " << blockers[i].fix;
            }
        }
        return out.str();
    }

    ResourceId findRootFunctionId(Lib3MF::PModel const & model)
    {
        if (!model)
        {
            return 0;
        }

        try
        {
            auto levelSets = model->GetLevelSets();
            ResourceId fallback{};
            while (levelSets->MoveNext())
            {
                auto const levelSet
                  = std::dynamic_pointer_cast<Lib3MF::CLevelSet>(levelSets->GetCurrent());
                if (!levelSet)
                {
                    continue;
                }
                auto function = levelSet->GetFunction();
                if (!function)
                {
                    continue;
                }
                auto const id = static_cast<ResourceId>(function->GetModelResourceID());
                if (hasOutputPort(function, nodes::FieldNames::Shape))
                {
                    return id;
                }
                if (fallback == 0)
                {
                    fallback = id;
                }
            }
            return fallback;
        }
        catch (Lib3MF::ELib3MFException const &)
        {
            return 0;
        }
    }

    bool isRootShapeConsumed(Document * document)
    {
        if (!document || !document->getAssembly())
        {
            return false;
        }

        auto const & assemblyModel = document->getAssembly()->assemblyModel();
        if (!assemblyModel || !assemblyModel->getEndNode())
        {
            return false;
        }

        auto * shapeSink = assemblyModel->getEndNode()->getParameter(nodes::FieldNames::Shape);
        return shapeSink && shapeSink->getConstSource().has_value();
    }

    RenderabilityFacts collectRenderabilityFacts(Document * document, bool includeBounds)
    {
        RenderabilityFacts facts;
        if (!document)
        {
            addBlocker(facts,
                       "no_document",
                       "No document is attached to the design session",
                       "Open a document before designing");
            return facts;
        }

        auto model = document->get3mfModel();
        if (!model)
        {
            addBlocker(facts,
                       "no_3mf_model",
                       "The document has no 3MF model",
                       "Create a document before designing");
            return facts;
        }

        try
        {
            auto buildItems = model->GetBuildItems();
            while (buildItems->MoveNext())
            {
                ++facts.buildItemCount;
            }

            // A document can hold several level sets; the one whose function actually exposes a
            // scalar shape output is the one that shows up on screen, so prefer it as the root.
            struct LevelSetInfo
            {
                ResourceId functionId{};
                std::string name;
                std::string channel;
                bool hasShapeOutput{};
            };
            std::vector<LevelSetInfo> infos;

            auto levelSets = model->GetLevelSets();
            while (levelSets->MoveNext())
            {
                auto const levelSet
                  = std::dynamic_pointer_cast<Lib3MF::CLevelSet>(levelSets->GetCurrent());
                if (!levelSet)
                {
                    continue;
                }
                ++facts.levelSetCount;
                auto function = levelSet->GetFunction();
                if (!function)
                {
                    continue;
                }
                infos.push_back({static_cast<ResourceId>(function->GetModelResourceID()),
                                 function->GetDisplayName(),
                                 levelSet->GetChannelName(),
                                 hasOutputPort(function, nodes::FieldNames::Shape)});
            }

            auto preferred = std::find_if(infos.begin(), infos.end(),
                                          [](LevelSetInfo const & info)
                                          { return info.hasShapeOutput; });
            if (preferred == infos.end())
            {
                preferred = std::find_if(infos.begin(), infos.end(),
                                         [](LevelSetInfo const & info)
                                         { return info.functionId != 0; });
            }
            if (preferred != infos.end())
            {
                facts.rootFunctionId = preferred->functionId;
                facts.rootFunctionName = preferred->name;
                facts.rootFunctionHasShapeOutput = preferred->hasShapeOutput;
                facts.renderChannel = preferred->channel;
            }
        }
        catch (Lib3MF::ELib3MFException const & e)
        {
            addBlocker(facts,
                       "model_unreadable",
                       std::string("The 3MF model could not be inspected: ") + e.what(),
                       "Save and reload the document");
            return facts;
        }

        // A level set consumes its function's shape output by definition, so a bound root counts as
        // consumed even when the assembly's End node still holds the template default.
        facts.rootShapeConsumed = isRootShapeConsumed(document) || facts.rootFunctionId != 0;

        if (includeBounds)
        {
            if (auto const bounds = document->computeModelBoundsBlocking();
                bounds.has_value() && bounds->modelBounds.has_value())
            {
                auto const & b = *bounds->modelBounds;
                facts.boundsUsable = bounds->isUsable();
                facts.modelBounds = fmt::format("[{:.1f},{:.1f},{:.1f}]..[{:.1f},{:.1f},{:.1f}]",
                                                b.min[0],
                                                b.min[1],
                                                b.min[2],
                                                b.max[0],
                                                b.max[1],
                                                b.max[2]);
            }
        }

        if (facts.buildItemCount == 0)
        {
            addBlocker(facts,
                       "no_build_items",
                       "The scene contains no build items, so nothing is placed in the model",
                       "Call ensure_output_chain to add the level set and build item");
        }
        if (facts.levelSetCount == 0)
        {
            addBlocker(facts,
                       "no_levelset",
                       "No level set evaluates a function, so the program is never sampled",
                       "Call ensure_output_chain with the function you want to render");
        }
        if (facts.levelSetCount > 0 && facts.rootFunctionId == 0)
        {
            addBlocker(facts,
                       "levelset_without_function",
                       "A level set references no function",
                       "Bind the level set to a function with a scalar shape output");
        }
        if (facts.rootFunctionId != 0 && !facts.rootFunctionHasShapeOutput)
        {
            addBlocker(facts,
                       "root_without_shape_output",
                       fmt::format("The rendered function '{}' (ID {}) has no scalar 'shape' output",
                                   facts.rootFunctionName,
                                   facts.rootFunctionId),
                       "Return the distance from an output named shape in that function");
        }
        if (!facts.rootShapeConsumed)
        {
            addBlocker(facts,
                       "root_output_unsourced",
                       "The root function's output node has no source, so the scene is empty",
                       "Wire the level set's shape into the root function's shape output");
        }

        return facts;
    }
}
