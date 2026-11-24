#pragma once

#include <chrono>
#include <filesystem>
#include <string>

namespace gladius
{
    class Document;
}

namespace gladius::design
{
    /// @brief Outcome of seeding a document with the volumetric project template.
    struct TemplateSeedResult
    {
        bool success{};
        std::filesystem::path path;
        std::string error;
    };

    /**
     * @brief Replaces the document's content with examples/template.3mf.
     * @details The template is what makes a document renderable from the first turn: it carries the
     *          domain object, the volumetric data, the level set on the root function, and the build
     *          item. A blank document instead starts as a valid void whose root output is a constant,
     *          so everything the agent writes is unreachable and nothing renders. Both the interactive
     *          agent and the benchmark seed this way so what is measured matches what the user sees.
     * @param timeout Bounds the wait for the asynchronous load. The load runs on a worker; this call
     *                only polls, so it must never be used from a render loop.
     */
    [[nodiscard]] TemplateSeedResult seedVolumetricTemplate(
      Document * document,
      std::chrono::milliseconds timeout = std::chrono::minutes(2));
}
