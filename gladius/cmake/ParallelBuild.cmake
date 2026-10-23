# ParallelBuild.cmake - scale build concurrency to the host machine.
#
# Hard-coded job counts are always wrong somewhere: they throttle a 32-thread
# workstation to a trickle and OOM a small laptop. Both limits are therefore
# derived from the host at configure time:
#
#   * logical cores   - upper bound on useful concurrency
#   * physical memory - MSVC keeps far more state per cl.exe instance than the
#                       core count suggests, so RAM is usually the real limit
#
# Every value can still be pinned explicitly with a cache variable (0 = auto).

include_guard(GLOBAL)

# Rough peak resident set of one C++ compiler instance on this codebase.
# /Zm200 caps MSVC's internal heap, but template-heavy TUs still reach ~2 GiB.
set(GLADIUS_GIB_PER_COMPILE_JOB "3" CACHE STRING
    "Physical GiB assumed per concurrent compiler instance, used for auto-detection")

set(GLADIUS_BUILD_PARALLEL_JOBS "0" CACHE STRING
    "Total concurrent build jobs for 'cmake --build' (0 = auto-detect)")

set(GLADIUS_MSVC_COMPILE_JOB_POOL "0" CACHE STRING
    "Concurrent MSVC compile jobs (0 = auto-detect)")

set(GLADIUS_MSVC_HEAVY_COMPILE_JOB_POOL "0" CACHE STRING
    "Concurrent MSVC jobs for the template-heavy sources (0 = auto-detect)")

set(GLADIUS_MAX_HEAVY_COMPILE_JOBS "8" CACHE STRING
    "Upper bound on concurrent template-heavy MSVC compile jobs")

#[[
Detect logical CPUs. ProcessorCount() is unreliable on Windows, so fall back to
the NUMBER_OF_PROCESSORS environment variable that every Windows shell exports.
]]
function(gladius_detect_logical_cores outVar)
    include(ProcessorCount)
    ProcessorCount(coreCount)

    if(coreCount LESS 1 AND DEFINED ENV{NUMBER_OF_PROCESSORS})
        set(coreCount "$ENV{NUMBER_OF_PROCESSORS}")
    endif()

    if(NOT coreCount MATCHES "^[0-9]+$")
        set(coreCount 0)
    endif()

    set(${outVar} ${coreCount} PARENT_SCOPE)
endfunction()

#[[
Detect installed physical memory in GiB. Returns 0 when the host cannot be
queried, which disables the memory-based limit rather than failing the configure.

Uses CMAKE_HOST_WIN32 rather than WIN32: the limit bounds compiler processes
running on the build machine, so the host matters even when cross-compiling.
]]
function(gladius_detect_total_memory_gib outVar)
    set(memoryGiB 0)

    if(CMAKE_HOST_WIN32)
        find_program(GLADIUS_POWERSHELL_EXECUTABLE NAMES pwsh powershell)
        if(GLADIUS_POWERSHELL_EXECUTABLE)
            execute_process(
                COMMAND "${GLADIUS_POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive
                        -Command "[math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory/1GB)"
                OUTPUT_VARIABLE output
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
                RESULT_VARIABLE status
                TIMEOUT 15)
            if(status EQUAL 0 AND output MATCHES "^[0-9]+$")
                set(memoryGiB ${output})
            endif()
        endif()
    elseif(EXISTS "/proc/meminfo")
        file(STRINGS "/proc/meminfo" memTotalLine LIMIT 1)
        if(memTotalLine MATCHES "^MemTotal:[ \t]*([0-9]+) kB")
            math(EXPR memoryGiB "${CMAKE_MATCH_1} / 1048576")
        endif()
    elseif(APPLE)
        execute_process(
            COMMAND sysctl -n hw.memsize
            OUTPUT_VARIABLE output
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE status)
        if(status EQUAL 0 AND output MATCHES "^[0-9]+$")
            math(EXPR memoryGiB "${output} / 1073741824")
        endif()
    endif()

    set(${outVar} ${memoryGiB} PARENT_SCOPE)
endfunction()

#[[
Clamp a computed job count to at least one job. A zero or undetectable limit
must never stall the build, so an unusable constraint simply drops out.
]]
function(gladius_clamp_jobs outVar requestedJobs cores memoryGiB gibPerJob)
    set(jobs ${requestedJobs})

    if(cores GREATER 0 AND jobs GREATER cores)
        set(jobs ${cores})
    endif()

    if(memoryGiB GREATER 0 AND gibPerJob GREATER 0)
        math(EXPR memoryLimitedJobs "${memoryGiB} / ${gibPerJob}")
        if(memoryLimitedJobs LESS 1)
            set(memoryLimitedJobs 1)
        endif()
        if(jobs GREATER memoryLimitedJobs)
            set(jobs ${memoryLimitedJobs})
        endif()
    endif()

    if(jobs LESS 1)
        set(jobs 1)
    endif()

    set(${outVar} ${jobs} PARENT_SCOPE)
endfunction()

#[[
Resolve every concurrency limit for the host.

Publishes the results as internal cache variables so the Ninja job pools and
CTEST_PARALLEL_LEVEL can be wired up wherever they are consumed:
  GLADIUS_RESOLVED_BUILD_JOBS      - value to use for 'cmake --build --parallel'
  GLADIUS_RESOLVED_COMPILE_POOL    - concurrent cl.exe compiles
  GLADIUS_RESOLVED_HEAVY_POOL      - concurrent template-heavy compiles
]]
function(gladius_configure_build_parallelism)
    gladius_detect_logical_cores(logicalCores)
    gladius_detect_total_memory_gib(memoryGiB)

    # Total jobs: an explicit request wins, otherwise cores and RAM both vote.
    gladius_clamp_jobs(autoBuildJobs "${logicalCores}" "${logicalCores}"
                       "${memoryGiB}" "${GLADIUS_GIB_PER_COMPILE_JOB}")
    if(GLADIUS_BUILD_PARALLEL_JOBS GREATER 0)
        set(buildJobs ${GLADIUS_BUILD_PARALLEL_JOBS})
        set(buildJobsSource "explicit")
    else()
        set(buildJobs ${autoBuildJobs})
        set(buildJobsSource "auto")
    endif()

    # ctest reads this cache variable directly, so it is safe to publish.
    set(CTEST_PARALLEL_LEVEL ${buildJobs} CACHE INTERNAL
        "Concurrent ctest jobs, scaled to the host")
    set(GLADIUS_RESOLVED_BUILD_JOBS ${buildJobs} CACHE INTERNAL
        "Concurrent build jobs scaled to the host")

    message(STATUS "Gladius build concurrency: ${buildJobs} jobs "
                   "(${buildJobsSource}; ${logicalCores} logical cores, ${memoryGiB} GiB RAM, "
                   "${GLADIUS_GIB_PER_COMPILE_JOB} GiB/compiler)")

    if(NOT MSVC)
        return()
    endif()

    # Ninja only: job pools are how Ninja bounds cl.exe instances. Without this
    # the pools below are unused and Ninja's own default already scales to cores.
    if(NOT CMAKE_GENERATOR MATCHES "Ninja")
        return()
    endif()

    gladius_clamp_jobs(autoCompileJobs "${buildJobs}" "${logicalCores}"
                       "${memoryGiB}" "${GLADIUS_GIB_PER_COMPILE_JOB}")
    if(GLADIUS_MSVC_COMPILE_JOB_POOL GREATER 0)
        set(compileJobs ${GLADIUS_MSVC_COMPILE_JOB_POOL})
    else()
        set(compileJobs ${autoCompileJobs})
    endif()

    # The heavy sources (MCP tool TUs) blow past the normal per-job estimate, so
    # only a fraction of the pool may run them at once.
    math(EXPR autoHeavyJobs "${compileJobs} / 4")
    if(autoHeavyJobs LESS 1)
        set(autoHeavyJobs 1)
    endif()
    if(GLADIUS_MAX_HEAVY_COMPILE_JOBS GREATER 0 AND
       autoHeavyJobs GREATER GLADIUS_MAX_HEAVY_COMPILE_JOBS)
        set(autoHeavyJobs ${GLADIUS_MAX_HEAVY_COMPILE_JOBS})
    endif()
    if(GLADIUS_MSVC_HEAVY_COMPILE_JOB_POOL GREATER 0)
        set(heavyJobs ${GLADIUS_MSVC_HEAVY_COMPILE_JOB_POOL})
    else()
        set(heavyJobs ${autoHeavyJobs})
    endif()

    set(GLADIUS_RESOLVED_COMPILE_POOL ${compileJobs} CACHE INTERNAL
        "Concurrent MSVC compile jobs scaled to the host")
    set(GLADIUS_RESOLVED_HEAVY_POOL ${heavyJobs} CACHE INTERNAL
        "Concurrent template-heavy MSVC compile jobs scaled to the host")

    message(STATUS "Gladius MSVC job pools: ${compileJobs} compile, ${heavyJobs} heavy compile")
endfunction()
