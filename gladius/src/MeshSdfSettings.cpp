/// @file MeshSdfSettings.cpp
/// @brief Implementation of @ref gladius::MeshSdfSettings.

#include "MeshSdfSettings.h"

#include "ConfigManager.h"

#include <algorithm>
#include <string>

namespace gladius
{
    namespace
    {
        constexpr char const * const SECTION_REPAIR     = "meshSdf.repair";
        constexpr char const * const SECTION_EVALUATION = "meshSdf.evaluation";

        bool repairEqual(mesh_repair::MeshRepairConfig const & a,
                         mesh_repair::MeshRepairConfig const & b) noexcept
        {
            return a.weld == b.weld && a.weldEpsilon == b.weldEpsilon &&
                   a.removeDegenerate == b.removeDegenerate &&
                   a.areaEpsilon == b.areaEpsilon &&
                   a.orientConsistently == b.orientConsistently &&
                   a.fillHoles == b.fillHoles && a.maxHolePerimeter == b.maxHolePerimeter;
        }

        bool evalEqual(MeshSdfEvaluationConfig const & a,
                       MeshSdfEvaluationConfig const & b) noexcept
        {
            return a.method == b.method && a.useEarlyExit == b.useEarlyExit &&
                   a.inflationDistance == b.inflationDistance &&
                   a.voxelGridResolution == b.voxelGridResolution &&
                   a.fwnBeta == b.fwnBeta &&
                   a.fwnFarFieldFactor == b.fwnFarFieldFactor &&
                   a.fwnUseSignCache == b.fwnUseSignCache &&
                   a.nanovdbVoxelSize_mm == b.nanovdbVoxelSize_mm;
        }
    } // namespace

    MeshSdfSettings::MeshSdfSettings() = default;

    MeshSdfSettings::MeshSdfSettings(ConfigManager * config)
        : m_config(config)
    {
        load();
    }

    void MeshSdfSettings::attachConfigManager(ConfigManager * config)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_config = config;
        load();
    }

    void MeshSdfSettings::load()
    {
        if (m_config == nullptr)
        {
            return;
        }
        // Repair section
        m_repair.weld =
            m_config->getValue<bool>(SECTION_REPAIR, "weld", m_repair.weld);
        m_repair.weldEpsilon =
            m_config->getValue<float>(SECTION_REPAIR, "weldEpsilon", m_repair.weldEpsilon);
        m_repair.removeDegenerate =
            m_config->getValue<bool>(SECTION_REPAIR, "removeDegenerate", m_repair.removeDegenerate);
        m_repair.areaEpsilon =
            m_config->getValue<float>(SECTION_REPAIR, "areaEpsilon", m_repair.areaEpsilon);
        m_repair.orientConsistently = m_config->getValue<bool>(
            SECTION_REPAIR, "orientConsistently", m_repair.orientConsistently);
        m_repair.fillHoles =
            m_config->getValue<bool>(SECTION_REPAIR, "fillHoles", m_repair.fillHoles);
        m_repair.maxHolePerimeter = m_config->getValue<float>(
            SECTION_REPAIR, "maxHolePerimeter", m_repair.maxHolePerimeter);

        // Evaluation section
        std::string const methodName = m_config->getValue<std::string>(
            SECTION_EVALUATION, "method", std::string{toString(m_evaluation.method)});
        m_evaluation.method = parseMeshSdfMethod(methodName);
        m_evaluation.useEarlyExit = m_config->getValue<bool>(
            SECTION_EVALUATION, "useEarlyExit", m_evaluation.useEarlyExit);
        m_evaluation.inflationDistance = m_config->getValue<float>(
            SECTION_EVALUATION, "inflationDistance", m_evaluation.inflationDistance);
        m_evaluation.voxelGridResolution = m_config->getValue<int>(
            SECTION_EVALUATION, "voxelGridResolution", m_evaluation.voxelGridResolution);
        m_evaluation.fwnBeta = m_config->getValue<float>(
            SECTION_EVALUATION, "fwnBeta", m_evaluation.fwnBeta);
        m_evaluation.fwnFarFieldFactor = m_config->getValue<float>(
            SECTION_EVALUATION, "fwnFarFieldFactor", m_evaluation.fwnFarFieldFactor);
        m_evaluation.fwnUseSignCache = m_config->getValue<bool>(
            SECTION_EVALUATION, "fwnUseSignCache", m_evaluation.fwnUseSignCache);
        m_evaluation.nanovdbVoxelSize_mm = m_config->getValue<float>(
            SECTION_EVALUATION, "nanovdbVoxelSize_mm", m_evaluation.nanovdbVoxelSize_mm);
    }

    void MeshSdfSettings::save()
    {
        if (m_config == nullptr)
        {
            return;
        }
        m_config->setValue<bool>(SECTION_REPAIR, "weld", m_repair.weld);
        m_config->setValue<float>(SECTION_REPAIR, "weldEpsilon", m_repair.weldEpsilon);
        m_config->setValue<bool>(SECTION_REPAIR, "removeDegenerate", m_repair.removeDegenerate);
        m_config->setValue<float>(SECTION_REPAIR, "areaEpsilon", m_repair.areaEpsilon);
        m_config->setValue<bool>(
            SECTION_REPAIR, "orientConsistently", m_repair.orientConsistently);
        m_config->setValue<bool>(SECTION_REPAIR, "fillHoles", m_repair.fillHoles);
        m_config->setValue<float>(
            SECTION_REPAIR, "maxHolePerimeter", m_repair.maxHolePerimeter);

        m_config->setValue<std::string>(
            SECTION_EVALUATION, "method", std::string{toString(m_evaluation.method)});
        m_config->setValue<bool>(
            SECTION_EVALUATION, "useEarlyExit", m_evaluation.useEarlyExit);
        m_config->setValue<float>(
            SECTION_EVALUATION, "inflationDistance", m_evaluation.inflationDistance);
        m_config->setValue<int>(
            SECTION_EVALUATION, "voxelGridResolution", m_evaluation.voxelGridResolution);
        m_config->setValue<float>(
            SECTION_EVALUATION, "fwnBeta", m_evaluation.fwnBeta);
        m_config->setValue<float>(
            SECTION_EVALUATION, "fwnFarFieldFactor", m_evaluation.fwnFarFieldFactor);
        m_config->setValue<bool>(
            SECTION_EVALUATION, "fwnUseSignCache", m_evaluation.fwnUseSignCache);
        m_config->setValue<float>(
            SECTION_EVALUATION, "nanovdbVoxelSize_mm", m_evaluation.nanovdbVoxelSize_mm);
        m_config->save();
    }

    void MeshSdfSettings::setRepairConfig(mesh_repair::MeshRepairConfig const & cfg)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (repairEqual(m_repair, cfg))
            {
                return;
            }
            m_repair = cfg;
            // Persist while still holding the lock so a concurrent mutator
            // cannot interleave its own snapshot into the file.
            save();
        }
        notify(MeshSdfSettingsChange::Repair);
    }

    void MeshSdfSettings::setEvaluationConfig(MeshSdfEvaluationConfig const & cfg)
    {
        MeshSdfSettingsChange change = MeshSdfSettingsChange::None;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (evalEqual(m_evaluation, cfg))
            {
                return;
            }
            if (requiresMeshRebuild(m_evaluation, cfg))
            {
                change |= MeshSdfSettingsChange::Method;
            }
            if (m_evaluation.useEarlyExit != cfg.useEarlyExit ||
                m_evaluation.inflationDistance != cfg.inflationDistance ||
                m_evaluation.fwnBeta != cfg.fwnBeta ||
                m_evaluation.fwnFarFieldFactor != cfg.fwnFarFieldFactor ||
                m_evaluation.fwnUseSignCache != cfg.fwnUseSignCache)
            {
                change |= MeshSdfSettingsChange::RuntimeOnly;
            }
            m_evaluation = cfg;
            save();
        }
        if (any(change))
        {
            notify(change);
        }
    }

    MeshSdfSettings::ListenerHandle MeshSdfSettings::subscribe(ChangeCallback cb)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ListenerHandle const handle = m_nextHandle++;
        m_listeners.push_back({handle, std::move(cb)});
        return handle;
    }

    void MeshSdfSettings::unsubscribe(ListenerHandle handle)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto const it = std::remove_if(
            m_listeners.begin(),
            m_listeners.end(),
            [handle](Subscription const & s) { return s.handle == handle; });
        m_listeners.erase(it, m_listeners.end());
    }

    void MeshSdfSettings::notify(MeshSdfSettingsChange change)
    {
        // Snapshot listeners under the lock, then invoke without holding it
        // to avoid deadlocks if a callback reaches back into us.
        std::vector<ChangeCallback> callbacks;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            callbacks.reserve(m_listeners.size());
            for (auto const & s : m_listeners)
            {
                callbacks.push_back(s.callback);
            }
        }
        for (auto const & cb : callbacks)
        {
            cb(change);
        }
    }

} // namespace gladius
