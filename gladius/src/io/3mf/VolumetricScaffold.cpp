#include "VolumetricScaffold.h"

#include "ResourceIdUtil.h"

#include <array>

namespace gladius::io
{
    namespace
    {
        /// @brief Corner order of the domain box, unit cubes wound outwards.
        constexpr std::array<std::array<float, 3>, 8> kUnitCubeCorners{
          {{-1.0f, -1.0f, -1.0f},
           {1.0f, -1.0f, -1.0f},
           {1.0f, 1.0f, -1.0f},
           {-1.0f, 1.0f, -1.0f},
           {-1.0f, -1.0f, 1.0f},
           {1.0f, -1.0f, 1.0f},
           {1.0f, 1.0f, 1.0f},
           {-1.0f, 1.0f, 1.0f}}};

        /// @brief Index triples of the twelve triangles of the unit cube.
        constexpr std::array<std::array<Lib3MF_uint32, 3>, 12> kUnitCubeTriangles{
          {{0, 1, 2}, {0, 2, 3}, {4, 5, 6}, {4, 6, 7}, {0, 4, 7}, {0, 7, 3},
           {1, 5, 6}, {1, 6, 2}, {0, 1, 5}, {0, 5, 4}, {3, 7, 6}, {3, 6, 2}}};
    } // namespace

    Lib3MF::PFunction findFunctionResource(Lib3MF::PModel const & model, ResourceId functionId)
    {
        if (!model)
        {
            return nullptr;
        }

        // GetResourceByID expects the UniqueResourceID while Gladius IDs are ModelResourceIDs.
        auto const uniqueId = resourceIdToUniqueResourceId(model, functionId);
        if (uniqueId == 0)
        {
            return nullptr;
        }
        try
        {
            return std::dynamic_pointer_cast<Lib3MF::CFunction>(model->GetResourceByID(uniqueId));
        }
        catch (Lib3MF::ELib3MFException const &)
        {
            return nullptr;
        }
    }

    Lib3MF::PFunction findFunctionResourceByName(Lib3MF::PModel const & model,
                                                 std::string const & displayName)
    {
        if (!model || displayName.empty())
        {
            return nullptr;
        }

        try
        {
            auto const functions = model->GetFunctions();
            while (functions->MoveNext())
            {
                auto const function = functions->GetCurrentFunction();
                if (function && function->GetDisplayName() == displayName)
                {
                    return function;
                }
            }
        }
        catch (Lib3MF::ELib3MFException const &)
        {
            return nullptr;
        }
        return nullptr;
    }

    namespace
    {
        /// @brief True when a build item places the given resource into the scene.
        bool isPlacedAsBuildItem(Lib3MF::PModel const & model, Lib3MF_uint32 modelResourceId)
        {
            auto buildItems = model->GetBuildItems();
            while (buildItems->MoveNext())
            {
                auto const item = buildItems->GetCurrent();
                auto const placed = item ? item->GetObjectResource() : nullptr;
                if (placed && placed->GetModelResourceID() == modelResourceId)
                {
                    return true;
                }
            }
            return false;
        }

        /// @brief Finds the level set that already evaluates the function, if any.
        Lib3MF::PLevelSet findLevelSetOf(Lib3MF::PModel const & model,
                                         Lib3MF::PFunction const & function)
        {
            auto levelSets = model->GetLevelSets();
            while (levelSets->MoveNext())
            {
                auto const levelSet = std::dynamic_pointer_cast<Lib3MF::CLevelSet>(levelSets->GetCurrent());
                if (!levelSet)
                {
                    continue;
                }
                auto const bound = levelSet->GetFunction();
                if (bound && bound->GetModelResourceID() == function->GetModelResourceID())
                {
                    return levelSet;
                }
            }
            return nullptr;
        }

        /// @brief Finds the first level set, the one a rebound chain reuses.
        Lib3MF::PLevelSet findAnyLevelSet(Lib3MF::PModel const & model)
        {
            auto levelSets = model->GetLevelSets();
            while (levelSets->MoveNext())
            {
                if (auto const levelSet
                      = std::dynamic_pointer_cast<Lib3MF::CLevelSet>(levelSets->GetCurrent()))
                {
                    return levelSet;
                }
            }
            return nullptr;
        }
    } // namespace

    VolumetricOutputChainReconciliation reconcileVolumetricOutputChain(
      Lib3MF::PModel const & model,
      Lib3MF::PFunction const & function,
      std::array<float, 3> const & minPoint,
      std::array<float, 3> const & maxPoint,
      events::SharedLogger const & logger)
    {
        VolumetricOutputChainReconciliation reconciliation;
        if (!model || !function)
        {
            reconciliation.error = "A 3MF model and a function resource are required";
            return reconciliation;
        }

        try
        {
            // Reconciling, not appending: a second level set on the same function would place the
            // same geometry twice in the scene.
            if (auto const existing = findLevelSetOf(model, function); existing)
            {
                reconciliation.action = VolumetricOutputChainAction::AlreadyWired;
                reconciliation.levelSetId
                  = modelResourceIdToResourceId(existing->GetModelResourceID());
                reconciliation.channel = existing->GetChannelName();
                reconciliation.success = isPlacedAsBuildItem(model, existing->GetModelResourceID());
                if (!reconciliation.success)
                {
                    Lib3MF::sTransform identity;
                    setTransformToIdentity(identity);
                    model->AddBuildItem(existing.get(), identity);
                }
                return reconciliation;
            }

            if (auto const levelSet = findAnyLevelSet(model); levelSet)
            {
                levelSet->SetFunction(function);
                try
                {
                    levelSet->SetChannelName("shape");
                    reconciliation.channel = "shape";
                }
                catch (Lib3MF::ELib3MFException const &)
                {
                    levelSet->SetChannelName("result");
                    reconciliation.channel = "result";
                }
                reconciliation.action = VolumetricOutputChainAction::Rebound;
                reconciliation.levelSetId
                  = modelResourceIdToResourceId(levelSet->GetModelResourceID());
                reconciliation.success = true;
                if (!isPlacedAsBuildItem(model, levelSet->GetModelResourceID()))
                {
                    Lib3MF::sTransform identity;
                    setTransformToIdentity(identity);
                    model->AddBuildItem(levelSet.get(), identity);
                }
                if (logger)
                {
                    logger->logInfo("Level set "
                                    + std::to_string(reconciliation.levelSetId)
                                    + " rebound to function "
                                    + std::to_string(function->GetModelResourceID()));
                }
                return reconciliation;
            }
        }
        catch (Lib3MF::ELib3MFException const & e)
        {
            reconciliation.error = std::string("lib3mf error: ") + e.what();
            return reconciliation;
        }

        auto const created = createVolumetricOutputChain(model, function, minPoint, maxPoint, logger);
        reconciliation.action = VolumetricOutputChainAction::Created;
        static_cast<VolumetricScaffoldResult &>(reconciliation) = created;
        return reconciliation;
    }

    VolumetricScaffoldResult createVolumetricOutputChain(Lib3MF::PModel const & model,
                                                        Lib3MF::PFunction const & function,
                                                        std::array<float, 3> const & minPoint,
                                                        std::array<float, 3> const & maxPoint,
                                                        events::SharedLogger const & logger)
    {
        VolumetricScaffoldResult result;
        if (!model || !function)
        {
            result.error = "A 3MF model and a function resource are required";
            return result;
        }
        if (minPoint[0] >= maxPoint[0] || minPoint[1] >= maxPoint[1] || minPoint[2] >= maxPoint[2])
        {
            result.error = "The evaluation domain is empty";
            return result;
        }

        try
        {
            auto mesh = model->AddMeshObject();
            for (auto const & corner : kUnitCubeCorners)
            {
                auto const x = minPoint[0] + (corner[0] + 1.0f) * 0.5f * (maxPoint[0] - minPoint[0]);
                auto const y = minPoint[1] + (corner[1] + 1.0f) * 0.5f * (maxPoint[1] - minPoint[1]);
                auto const z = minPoint[2] + (corner[2] + 1.0f) * 0.5f * (maxPoint[2] - minPoint[2]);
                mesh->AddVertex({x, y, z});
            }
            for (auto const & triangle : kUnitCubeTriangles)
            {
                mesh->AddTriangle({triangle[0], triangle[1], triangle[2]});
            }
            mesh->SetName("Bounding Box");
            result.domainMeshId = modelResourceIdToResourceId(mesh->GetModelResourceID());

            auto levelSet = model->AddLevelSet();
            levelSet->SetFunction(function);
            levelSet->SetMesh(mesh.get());
            levelSet->SetMeshBBoxOnly(true);
            levelSet->SetMinFeatureSize(0.1f);
            levelSet->SetFallBackValue(0.0f);

            // The root function's shape output is what the renderer reads; graphs that only produce
            // a "result" port still render, so fall back rather than fail the whole chain.
            try
            {
                levelSet->SetChannelName("shape");
                result.channel = "shape";
            }
            catch (Lib3MF::ELib3MFException const &)
            {
                levelSet->SetChannelName("result");
                result.channel = "result";
            }

            Lib3MF::sTransform identity;
            setTransformToIdentity(identity);
            model->AddBuildItem(levelSet.get(), identity);

            result.levelSetId = modelResourceIdToResourceId(levelSet->GetModelResourceID());
            result.success = true;
            if (logger)
            {
                logger->logInfo("Volumetric output chain created for function " +
                             std::to_string(function->GetModelResourceID()) + " (level set " +
                             std::to_string(result.levelSetId) + ", domain " +
                             std::to_string(result.domainMeshId) + ")");
            }
            return result;
        }
        catch (Lib3MF::ELib3MFException const & e)
        {
            result.error = std::string("lib3mf error: ") + e.what();
            return result;
        }
        catch (std::exception const & e)
        {
            result.error = e.what();
            return result;
        }
    }
}
