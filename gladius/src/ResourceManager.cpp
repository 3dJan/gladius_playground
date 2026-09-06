#include "ResourceManager.h"

#include "BeamLatticeResource.h"
#include "ImageStackResource.h"
#include "Profiling.h"
#if defined(GLADIUS_ENABLE_OPENCL)
#include "ResourceContext.h"
#endif
#include "SpatialMeshResource.h"
#if defined(GLADIUS_ENABLE_OPENVDB)
#include "StlResource.h"
#include "VdbImporter.h"
#include "VdbResource.h"
#include "MeshResourceVdb.h"
#endif

#include <fmt/format.h>
#include <lodepng.h>

#include <algorithm>

namespace gladius
{
#if defined(GLADIUS_ENABLE_OPENVDB)
    void StlResource::loadImpl()
    {
        m_payloadData.meta.clear();
        m_payloadData.data.clear();
        vdb::VdbImporter reader;
        reader.loadStl(getFilename());
        reader.writeMesh(m_payloadData);
    }
#endif

    ResourceManager::ResourceManager(SharedResources resourceContext,
                                     std::filesystem::path assemblyDir)
        : m_resourceContext(std::move(resourceContext))
        , m_assemblyDir(std::move(assemblyDir))
    {
    }

    void ResourceManager::addResource(std::filesystem::path const & filename)
    {
        if (m_resources.find(ResourceKey{filename}) != m_resources.end())
        {
            return;
        }

        if (!is_regular_file(filename))
        {
            throw std::runtime_error(
              fmt::format("Loading {} failed, the file does not exist", filename.string()));
        }

#if defined(GLADIUS_ENABLE_OPENVDB)
        if (filename.extension() == ".stl" || filename.extension() == ".STL")
        {
            m_resources[ResourceKey{filename}] =
              std::make_unique<StlResource>(ResourceKey{filename});
        }
#else
        if (filename.extension() == ".stl" || filename.extension() == ".STL")
        {
            throw std::runtime_error(
              fmt::format("Loading {} failed, STL import requires a build with "
                          "GLADIUS_ENABLE_OPENVDB enabled",
                          filename.string()));
        }
#endif
    }

#if defined(GLADIUS_ENABLE_OPENVDB)
    void ResourceManager::addResource(ResourceKey key, vdb::TriangleMesh && mesh)
    {
        m_resources[key] = std::make_unique<MeshResourceVdb>(key, std::move(mesh));
    }

    void ResourceManager::addResource(ResourceKey key, openvdb::GridBase::Ptr && grid)
    {
        m_resources[key] = std::make_unique<VdbResource>(key, std::move(grid));
    }
#endif

    void ResourceManager::addResource(ResourceKey key, io::ImageStack && stack)
    {
        m_resources[key] = std::make_unique<ImageStackResource>(key, std::move(stack));
    }

    void ResourceManager::addResource(ResourceKey key,
                                      std::unique_ptr<BeamLatticeResource> && resource)
    {
        m_resources[key] = std::move(resource);
    }

    void ResourceManager::addResource(ResourceKey key, SpatialMeshData && spatialData)
    {
        m_resources[key] = std::make_unique<SpatialMeshResource>(key, std::move(spatialData));
    }

    void ResourceManager::addResource(ResourceKey key,
                                      SpatialMeshData && spatialData,
                                      MeshSdfEvaluationConfig const & evaluationConfig,
                                      NanoVdbBuildPolicy const & nanovdbBuildPolicy)
    {
        m_resources[key] = std::make_unique<SpatialMeshResource>(
          key, std::move(spatialData), evaluationConfig, nanovdbBuildPolicy);
    }

    void ResourceManager::loadResources()
    {
        for (auto & [filename, res] : m_resources)
        {
            if (res->isInUse())
            {
                m_bufferChanged |= res->load();
            }
        }
    }

    void ResourceManager::writeResources(Primitives & primitives)
    {
        for (auto & [filename, res] : m_resources)
        {
            res->write(primitives);
        }
        if (primitives.data.getSize() > 0)
        {
            primitives.write();
        }
    }

    void ResourceManager::clear()
    {
        m_textures.clear();
#if defined(GLADIUS_ENABLE_OPENCL)
        m_resourceContext->clearImageStacks();
#endif
        m_nameCounter = 0;
    }

    void ResourceManager::increaseImageNumber()
    {
        ++m_nameCounter;
    }

    IResource & ResourceManager::getResource(ResourceKey const & key) const
    {
        return *m_resources.at(key);
    }

    IResource * ResourceManager::getResourcePtr(ResourceKey const & key)
    {
        auto iter = m_resources.find(key);
        if (iter == m_resources.end())
        {
            return nullptr;
        }
        return iter->second.get();
    }

    ResourceMap const & ResourceManager::getResourceMap() const
    {
        return m_resources;
    }

    bool ResourceManager::hasResource(ResourceKey const & key) const
    {
        return m_resources.find(key) != m_resources.end();
    }

    void ResourceManager::deleteResource(ResourceKey const & key)
    {
        // find the resource
        auto iter = m_resources.find(key);
        if (iter == m_resources.end())
        {
            return;
        }

        // remove the resource
        m_resources.erase(iter);
    }
    
    std::vector<MeshVoxelGridBuildParams> ResourceManager::collectVoxelGridBuildParams() const
    {
        std::vector<MeshVoxelGridBuildParams> params;
        params.reserve(m_resources.size());  // Upper bound estimate
        
        for (auto const & [key, resource] : m_resources)
        {
            // Fast path: skip non-mesh resources without RTTI
            if (key.getResourceType() != ResourceType::Mesh)
            {
                continue;
            }
            
            auto* spatialMesh = dynamic_cast<SpatialMeshResource*>(resource.get());
            if (spatialMesh != nullptr && spatialMesh->needsVoxelGridBuild())
            {
                auto buildParams = spatialMesh->getVoxelGridBuildParams();
                if (buildParams.has_value())
                {
                    params.push_back(buildParams.value());
                }
            }
        }
        
        return params;
    }

    std::vector<MeshSignCacheBuildParams> ResourceManager::collectSignCacheBuildParams() const
    {
        GLADIUS_FWN_PREP_SCOPE("ResourceManager::collectSignCacheBuildParams");
        std::vector<MeshSignCacheBuildParams> params;
        params.reserve(m_resources.size());  // Upper bound estimate

        for (auto const & [key, resource] : m_resources)
        {
            if (key.getResourceType() != ResourceType::Mesh)
            {
                continue;
            }

            auto * spatialMesh = dynamic_cast<SpatialMeshResource *>(resource.get());
            if (spatialMesh != nullptr &&
                !spatialMesh->needsFwnAggregateBuild() &&
                spatialMesh->needsSignCacheBuild() &&
                spatialMesh->usesFwnSignCache())
            {
                auto buildParams = spatialMesh->getSignCacheBuildParams();
                if (buildParams.has_value())
                {
                    params.push_back(buildParams.value());
                }
            }
        }

        GLADIUS_FWN_PREP_LOG_IF(!params.empty(),
                                "ResourceManager::collectSignCacheBuildParams collected=" +
                                    std::to_string(params.size()));
        return params;
    }

    std::vector<MeshFwnAggregateBuildParams> ResourceManager::collectFwnAggregateBuildParams() const
    {
        GLADIUS_FWN_PREP_SCOPE("ResourceManager::collectFwnAggregateBuildParams");
        std::vector<MeshFwnAggregateBuildParams> params;
        params.reserve(m_resources.size());

        for (auto const & [key, resource] : m_resources)
        {
            if (key.getResourceType() != ResourceType::Mesh)
            {
                continue;
            }

            auto * spatialMesh = dynamic_cast<SpatialMeshResource *>(resource.get());
            if (spatialMesh != nullptr &&
                spatialMesh->needsFwnAggregateBuild())
            {
                auto buildParams = spatialMesh->getFwnAggregateBuildParams();
                if (buildParams.has_value())
                {
                    buildParams->resourceKey = key;
                    params.push_back(buildParams.value());
                }
            }
        }

        GLADIUS_FWN_PREP_LOG_IF(!params.empty(),
                                "ResourceManager::collectFwnAggregateBuildParams collected=" +
                                    std::to_string(params.size()));
        return params;
    }
    
    void ResourceManager::markVoxelGridsBuilt()
    {
        for (auto & [key, resource] : m_resources)
        {
            if (key.getResourceType() != ResourceType::Mesh)
            {
                continue;
            }
            
            auto* spatialMesh = dynamic_cast<SpatialMeshResource*>(resource.get());
            if (spatialMesh != nullptr && spatialMesh->needsVoxelGridBuild())
            {
                spatialMesh->markVoxelGridBuilt();
            }
        }
    }

    void ResourceManager::markFwnAggregatesBuilt(std::vector<MeshFwnAggregateBuildParams> const & buildParams,
                                                 size_t builtCount)
    {
        GLADIUS_FWN_PREP_SCOPE_IF("ResourceManager::markFwnAggregatesBuilt", !buildParams.empty());
        GLADIUS_FWN_PREP_LOG_IF(!buildParams.empty(),
                                "ResourceManager::markFwnAggregatesBuilt built=" +
                                    std::to_string(builtCount) +
                                    " requested=" + std::to_string(buildParams.size()));
        size_t const count = std::min(builtCount, buildParams.size());
        for (size_t i = 0; i < count; ++i)
        {
            auto const & params = buildParams[i];
            if (params.resourceKey.has_value())
            {
                auto * resource = getResourcePtr(params.resourceKey.value());
                auto * spatialMesh = dynamic_cast<SpatialMeshResource *>(resource);
                if (spatialMesh != nullptr)
                {
                    spatialMesh->markFwnAggregatesBuilt();
                }
                continue;
            }

            // Compatibility fallback for tests or direct callers that construct
            // MeshFwnAggregateBuildParams without ResourceManager metadata.
            for (auto & [key, resource] : m_resources)
            {
                if (key.getResourceType() != ResourceType::Mesh)
                {
                    continue;
                }

                auto * spatialMesh = dynamic_cast<SpatialMeshResource *>(resource.get());
                auto build = spatialMesh != nullptr ? spatialMesh->getFwnAggregateBuildParams() : std::nullopt;
                if (build.has_value() && build->fwnAggregatesOffset == params.fwnAggregatesOffset)
                {
                    spatialMesh->markFwnAggregatesBuilt();
                    break;
                }
            }
        }
    }

    void ResourceManager::markSignCacheBuildProgress(std::vector<MeshSignCacheBuildParams> const & buildParams,
                                                     size_t queuedCount)
    {
        size_t const count = std::min(queuedCount, buildParams.size());
        for (size_t i = 0; i < count; ++i)
        {
            auto const & params = buildParams[i];
            for (auto & [key, resource] : m_resources)
            {
                if (key.getResourceType() != ResourceType::Mesh)
                {
                    continue;
                }

                auto * spatialMesh = dynamic_cast<SpatialMeshResource *>(resource.get());
                if (spatialMesh != nullptr &&
                    spatialMesh->needsSignCacheBuild() &&
                    spatialMesh->usesFwnSignCache() &&
                    spatialMesh->signCacheReadyHostOffset() == params.signCacheReadyOffset)
                {
                    spatialMesh->markSignCacheBuildQueued(params);
                    break;
                }
            }
        }
    }
}
