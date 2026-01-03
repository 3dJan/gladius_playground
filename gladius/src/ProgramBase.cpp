#include "ProgramBase.h"

#include "Profiling.h"
#include <fmt/format.h>
#include <string>

#include "exceptions.h"

namespace gladius
{
    ProgramBase::ProgramBase(SharedComputeContext context, const SharedResources resources)
        : m_ComputeContext(context)
        , m_programFront(std::make_unique<CLProgram>(context))
        , m_resources(resources)
    {
        if (m_logger)
        {
            m_programFront->setLogger(m_logger);
        }

        // Base source files that are always required for kernels
        // PNanoVDB_OpenCL.h is conditionally added in the (re)compile methods when VDB is enabled
        m_sourceFiles = {"arguments.h",
                         "types.h",
                         "sdf.h",
                         "sampler.h",
                         "rendering.h",
                         "sdf_generator.h",
                         //"PNanoVDB_OpenCL.h",
                         "mesh_sdf.cl",
                         "sdf.cl",
                         "rendering.cl",
                         "sdf_generator.cl"};
    }

    void ProgramBase::swapProgramsIfNeeded()
    {

        if (m_programSwapRequired)
        {
            m_programSwapRequired = false;
            m_onProgramSwapCallBack();
        }
    }

    void ProgramBase::waitForCompilation() const
    {
        ProfileFunction if (!m_ComputeContext->isValid())
        {
            return;
        }
        m_programFront->finishCompilation();
    }

    void ProgramBase::dumpSource(std::filesystem::path const & path) const
    {
        ProfileFunction m_programFront->dumpSource(path);
    }

    void ProgramBase::recompileNonBlocking()
    {
        ProfileFunction
        try
        {
            if (m_modelKernel.empty())
            {
                if (m_logger)
                {
                    m_logger->logInfo(
                      "Aborting compilation attempt: No model source has been set yet");
                }
                return;
            }

            // Clear the model kernel changed flag since we're now compiling with the new kernel
            m_modelKernelChanged = false;

            // Configure optional features (like VDB) and include headers accordingly
            if (m_enableVdb)
            {
                m_programFront->addSymbol("ENABLE_VDB");
                // Ensure PNanoVDB OpenCL headers are present when VDB is enabled
                // Order: PNanoVDB_OpenCL.h -> PNanoVDB.h -> PNanoVDB_OpenCL_Helpers.h
                auto hasNano =
                  std::find(m_sourceFiles.begin(), m_sourceFiles.end(), "PNanoVDB_OpenCL.h") !=
                  m_sourceFiles.end();
                if (!hasNano)
                {
                    // Insert before kernels to keep headers first (insert in reverse order)
                    auto insertPos =
                      std::find(m_sourceFiles.begin(), m_sourceFiles.end(), std::string("sdf.cl"));
                    m_sourceFiles.insert(insertPos, "PNanoVDB_OpenCL_Helpers.h");
                    insertPos = std::find(m_sourceFiles.begin(),
                                          m_sourceFiles.end(),
                                          std::string("PNanoVDB_OpenCL_Helpers.h"));
                    m_sourceFiles.insert(insertPos, "PNanoVDB.h");
                    insertPos = std::find(
                      m_sourceFiles.begin(), m_sourceFiles.end(), std::string("PNanoVDB.h"));
                    m_sourceFiles.insert(insertPos, "PNanoVDB_OpenCL.h");
                }
            }
            else
            {
                m_programFront->removeSymbol("ENABLE_VDB");
                // Remove PNanoVDB headers when VDB is disabled to improve compatibility
                auto it =
                  std::find(m_sourceFiles.begin(), m_sourceFiles.end(), "PNanoVDB_OpenCL.h");
                if (it != m_sourceFiles.end())
                {
                    m_sourceFiles.erase(it);
                }
                it = std::find(m_sourceFiles.begin(), m_sourceFiles.end(), "PNanoVDB.h");
                if (it != m_sourceFiles.end())
                {
                    m_sourceFiles.erase(it);
                }
                it = std::find(
                  m_sourceFiles.begin(), m_sourceFiles.end(), "PNanoVDB_OpenCL_Helpers.h");
                if (it != m_sourceFiles.end())
                {
                    m_sourceFiles.erase(it);
                }
            }

            m_buildFinishedCallBack = [&]() { m_programSwapRequired = true; };
            m_programFront->clearSources();

            // Mark compilation as started
            m_compilationProgress.store(0.1f, std::memory_order_release);
            m_compilationSucceeded.store(false, std::memory_order_release);

            if (m_isFirstBuild)
            {
                m_isFirstBuild = false;
                waitForCompilation();

                swapProgramsIfNeeded();

                m_programFront->buildFromSourceAndLinkWithLib(
                  m_sourceFiles, m_modelKernel, m_buildFinishedCallBack);
                m_programSwapRequired = true;

                // Mark as complete after blocking build
                m_compilationProgress.store(1.0f, std::memory_order_release);
                m_compilationSucceeded.store(m_programFront->isValid(), std::memory_order_release);
            }
            else
            {
                // Wrap callback to track completion
                auto originalCallback = m_buildFinishedCallBack;
                m_buildFinishedCallBack = [this, originalCallback]()
                {
                    if (originalCallback && originalCallback.has_value())
                    {
                        (*originalCallback)();
                    }
                    m_compilationProgress.store(1.0f, std::memory_order_release);
                    m_compilationSucceeded.store(m_programFront->isValid(),
                                                 std::memory_order_release);
                };

                m_programFront->buildFromSourceAndLinkWithLibNonBlocking(
                  m_sourceFiles, m_modelKernel, m_buildFinishedCallBack);
            }
        }
        catch (OpenCLError & e)
        {
            m_ComputeContext->invalidate("OpenCL error during compilation in ProgramBase");
            throw e;
        }
    }

    void ProgramBase::recompileBlocking()
    {
        ProfileFunction if (m_modelKernel.empty())
        {
            if (m_logger)
            {
                m_logger->logInfo("Aborting compilation attempt: No model source has been set yet");
            }
            return;
        }

        // Clear the model kernel changed flag since we're now compiling with the new kernel
        m_modelKernelChanged = false;

        if (m_enableVdb)
        {
            m_programFront->addSymbol("ENABLE_VDB");
            auto hasNano =
              std::find(m_sourceFiles.begin(), m_sourceFiles.end(), "PNanoVDB_OpenCL.h") !=
              m_sourceFiles.end();
            if (!hasNano)
            {
                auto insertPos =
                  std::find(m_sourceFiles.begin(), m_sourceFiles.end(), std::string("sdf.cl"));
                m_sourceFiles.insert(insertPos, "PNanoVDB_OpenCL_Helpers.h");
                insertPos = std::find(m_sourceFiles.begin(),
                                      m_sourceFiles.end(),
                                      std::string("PNanoVDB_OpenCL_Helpers.h"));
                m_sourceFiles.insert(insertPos, "PNanoVDB.h");
                insertPos =
                  std::find(m_sourceFiles.begin(), m_sourceFiles.end(), std::string("PNanoVDB.h"));
                m_sourceFiles.insert(insertPos, "PNanoVDB_OpenCL.h");
            }
        }
        else
        {
            m_programFront->removeSymbol("ENABLE_VDB");
            auto it = std::find(m_sourceFiles.begin(), m_sourceFiles.end(), "PNanoVDB_OpenCL.h");
            if (it != m_sourceFiles.end())
            {
                m_sourceFiles.erase(it);
            }
            it = std::find(m_sourceFiles.begin(), m_sourceFiles.end(), "PNanoVDB.h");
            if (it != m_sourceFiles.end())
            {
                m_sourceFiles.erase(it);
            }
            it = std::find(m_sourceFiles.begin(), m_sourceFiles.end(), "PNanoVDB_OpenCL_Helpers.h");
            if (it != m_sourceFiles.end())
            {
                m_sourceFiles.erase(it);
            }
        }

        m_programFront->clearSources();
        m_programFront->buildFromSourceAndLinkWithLib(
          m_sourceFiles, m_modelKernel, m_buildFinishedCallBack);
        m_programSwapRequired = true;
        swapProgramsIfNeeded();
        m_isFirstBuild = false;
    }

    void ProgramBase::buildKernelLib() const
    {
        ProfileFunction m_programFront->clearSources();

        // Build the header list based on current VDB setting
        auto sourceFiles = m_sourceFiles;
        if (m_enableVdb)
        {
            if (std::find(sourceFiles.begin(), sourceFiles.end(), "PNanoVDB_OpenCL.h") ==
                sourceFiles.end())
            {
                auto insertPos =
                  std::find(sourceFiles.begin(), sourceFiles.end(), std::string("sdf.cl"));
                sourceFiles.insert(insertPos, "PNanoVDB_OpenCL_Helpers.h");
                insertPos = std::find(
                  sourceFiles.begin(), sourceFiles.end(), std::string("PNanoVDB_OpenCL_Helpers.h"));
                sourceFiles.insert(insertPos, "PNanoVDB.h");
                insertPos =
                  std::find(sourceFiles.begin(), sourceFiles.end(), std::string("PNanoVDB.h"));
                sourceFiles.insert(insertPos, "PNanoVDB_OpenCL.h");
            }
        }
        else
        {
            auto it = std::find(sourceFiles.begin(), sourceFiles.end(), "PNanoVDB_OpenCL.h");
            if (it != sourceFiles.end())
            {
                sourceFiles.erase(it);
            }
            it = std::find(sourceFiles.begin(), sourceFiles.end(), "PNanoVDB.h");
            if (it != sourceFiles.end())
            {
                sourceFiles.erase(it);
            }
            it = std::find(sourceFiles.begin(), sourceFiles.end(), "PNanoVDB_OpenCL_Helpers.h");
            if (it != sourceFiles.end())
            {
                sourceFiles.erase(it);
            }
        }

        m_programFront->loadAndCompileLib(sourceFiles);
    }

    void ProgramBase::setOnProgramSwapCallBack(const std::function<void()> & callBack)
    {
        m_onProgramSwapCallBack = callBack;
    }

    bool ProgramBase::isCompilationInProgress() const
    {
        return m_programFront->isCompilationInProgress();
    }

    bool ProgramBase::isValid() const
    {
        if (!m_programFront)
        {
            return false;
        }

        // If model kernel changed, need recompilation
        if (m_modelKernelChanged)
        {
            return false;
        }

        return m_programFront->isValid();
    }

    void ProgramBase::setModelKernel(const std::string & newModelKernelSource)
    {
        // If model kernel changed, mark for recompilation
        if (m_modelKernel != newModelKernelSource)
        {
            m_modelKernel = newModelKernelSource;
            m_modelKernelChanged = true;
        }
    }

    void ProgramBase::setEnableVdb(bool enableVdb)
    {
        m_enableVdb = enableVdb;
    }

    void ProgramBase::setLogger(events::SharedLogger logger)
    {
        m_logger = std::move(logger);
        if (m_programFront)
        {
            m_programFront->setLogger(m_logger);
        }
    }

    void ProgramBase::setCacheDirectory(const std::filesystem::path & path)
    {
        if (m_logger)
        {
            m_logger->logInfo("ProgramBase::setCacheDirectory called with path: " + path.string());
        }
        if (m_programFront)
        {
            if (m_logger)
            {
                m_logger->logInfo("ProgramBase: Calling CLProgram setCacheDirectory");
            }
            m_programFront->setCacheDirectory(path);
        }
        else if (m_logger)
        {
            m_logger->logWarning("ProgramBase: m_programFront is null!");
        }
    }

    void ProgramBase::clearCache()
    {
        if (m_programFront)
        {
            m_programFront->clearCache();
        }
    }

    void ProgramBase::setCacheEnabled(bool enabled)
    {
        if (m_programFront)
        {
            m_programFront->setCacheEnabled(enabled);
        }
        else if (m_logger)
        {
            m_logger->logWarning(
              "ProgramBase: m_programFront is null, cannot set cache enabled state!");
        }
    }

    bool ProgramBase::isCacheEnabled() const
    {
        if (m_programFront)
        {
            return m_programFront->isCacheEnabled();
        }
        return true; // Default value when program is not available
    }

    float ProgramBase::getCompilationProgress() const noexcept
    {
        return m_compilationProgress.load(std::memory_order_relaxed);
    }

    bool ProgramBase::compilationSucceeded() const noexcept
    {
        return m_compilationSucceeded.load(std::memory_order_relaxed);
    }
}
