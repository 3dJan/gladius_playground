#pragma once

/// @file MeshSdfSettings.h
/// @brief Persistent, change-notifying configuration wrapper around
///        @ref ConfigManager for mesh SDF evaluation and repair.

#include "MeshRepair.h"
#include "MeshSdfMethod.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace gladius
{
    class ConfigManager;

    /// Bitset describing which subsystems must react to a settings change.
    enum class MeshSdfSettingsChange : std::uint8_t
    {
        None        = 0,
        /// Repair toggles changed → re-import meshes.
        Repair      = 1 << 0,
        /// Evaluation method or rebuild-trigger parameters changed →
        /// recreate acceleration structures on every mesh resource.
        Method      = 1 << 1,
        /// Pure runtime knobs (inflation, early-exit toggle) changed.
        RuntimeOnly = 1 << 2,
    };

    inline MeshSdfSettingsChange operator|(MeshSdfSettingsChange a,
                                           MeshSdfSettingsChange b) noexcept
    {
        return static_cast<MeshSdfSettingsChange>(static_cast<std::uint8_t>(a) |
                                                  static_cast<std::uint8_t>(b));
    }
    inline MeshSdfSettingsChange & operator|=(MeshSdfSettingsChange & a,
                                              MeshSdfSettingsChange b) noexcept
    {
        a = a | b;
        return a;
    }
    inline bool any(MeshSdfSettingsChange v) noexcept
    {
        return static_cast<std::uint8_t>(v) != 0u;
    }
    inline bool has(MeshSdfSettingsChange v, MeshSdfSettingsChange flag) noexcept
    {
        return (static_cast<std::uint8_t>(v) & static_cast<std::uint8_t>(flag)) != 0u;
    }

    /// Persistent mesh SDF configuration.
    ///
    /// Wraps a @ref ConfigManager (sections `meshSdf.repair` and
    /// `meshSdf.evaluation`). All mutators broadcast the appropriate
    /// @ref MeshSdfSettingsChange bitset to subscribed listeners. Listeners
    /// are responsible for performing reload/rebuild work.
    class MeshSdfSettings
    {
      public:
        using ChangeCallback = std::function<void(MeshSdfSettingsChange)>;
        using ListenerHandle = std::size_t;

        /// Construct a settings object backed by @p config. Values are loaded
        /// immediately. If @p config is nullptr, in-memory defaults are used.
        explicit MeshSdfSettings(ConfigManager * config);

        /// Default constructor used by tests when no ConfigManager exists.
        MeshSdfSettings();

        ~MeshSdfSettings() = default;

        /// Bind a ConfigManager after default construction. Reloads values from
        /// the new backing store and replaces any previous one. Pass nullptr to
        /// detach (subsequent mutations are in-memory only).
        void attachConfigManager(ConfigManager * config);

        // -- accessors ---------------------------------------------------------

        mesh_repair::MeshRepairConfig const & repairConfig() const noexcept
        {
            return m_repair;
        }
        MeshSdfEvaluationConfig const & evaluationConfig() const noexcept
        {
            return m_evaluation;
        }

        // -- mutators ----------------------------------------------------------

        /// Replace the repair configuration. Persists, notifies listeners with
        /// @ref MeshSdfSettingsChange::Repair if it differs from the current
        /// config.
        void setRepairConfig(mesh_repair::MeshRepairConfig const & cfg);

        /// Replace the evaluation configuration. Notifies with the appropriate
        /// flags depending on whether the change requires a rebuild.
        void setEvaluationConfig(MeshSdfEvaluationConfig const & cfg);

        // -- listener API ------------------------------------------------------

        ListenerHandle subscribe(ChangeCallback cb);
        void unsubscribe(ListenerHandle handle);

      private:
        void load();
        void save();
        void notify(MeshSdfSettingsChange change);

        ConfigManager * m_config = nullptr;
        mesh_repair::MeshRepairConfig m_repair{};
        MeshSdfEvaluationConfig m_evaluation{};

        struct Subscription
        {
            ListenerHandle handle;
            ChangeCallback callback;
        };
        std::vector<Subscription> m_listeners;
        ListenerHandle m_nextHandle = 1u;
        mutable std::mutex m_mutex;
    };

} // namespace gladius
