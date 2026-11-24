#include "TemplateSeed.h"

#include "../Document.h"
#include "../FileSystemUtils.h"

#include <thread>

namespace gladius::design
{
    TemplateSeedResult seedVolumetricTemplate(Document * document,
                                              std::chrono::milliseconds timeout)
    {
        TemplateSeedResult result;
        if (!document)
        {
            result.error = "No document to seed";
            return result;
        }

        result.path = getAppDir() / "examples" / "template.3mf";
        if (!std::filesystem::exists(result.path))
        {
            result.error = "Volumetric project template not found at " + result.path.string();
            return result;
        }

        // The load runs on a worker thread; the agent may only start once the resources exist,
        // otherwise its first snippet races the import and the doc stays half-initialised.
        document->loadNonBlocking(result.path);

        auto const deadline = std::chrono::steady_clock::now() + timeout;
        while (document->isLoadingInProgress() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (document->isLoadingInProgress())
        {
            result.error = "Loading " + result.path.string() + " timed out";
            return result;
        }

        result.error = document->getLoadingError();
        result.success = result.error.empty();
        return result;
    }
}
