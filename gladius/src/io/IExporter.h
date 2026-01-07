#pragma once
#include <filesystem>

namespace gladius
{
    class ComputeCore;
}

namespace gladius::io
{
    class CancellationToken;

    class IExporter
    {
      public:
        virtual ~IExporter() = default;

        virtual void beginExport(const std::filesystem::path & fileName,
                                 ComputeCore & generator) = 0;

        virtual bool advanceExport(ComputeCore & generator) = 0;

        virtual void finalize() = 0;

        [[nodiscard]] virtual double getProgress() const = 0;

        /// @brief Set the cancellation token for cooperative cancellation
        /// @param token Pointer to a CancellationToken, or nullptr to disable cancellation
        ///
        /// The token must remain valid for the lifetime of the export operation.
        /// Implementations should check isCancellationRequested() periodically
        /// and abort the export early when cancellation is requested.
        virtual void setCancellationToken(CancellationToken * token)
        {
            m_cancellationToken = token;
        }

        /// @brief Check if cancellation has been requested
        /// @return true if cancellation was requested via the token, false otherwise
        [[nodiscard]] virtual bool isCancellationRequested() const;

      protected:
        CancellationToken * m_cancellationToken = nullptr;
    };
}