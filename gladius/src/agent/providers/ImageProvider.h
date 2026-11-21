#pragma once

#include "HttpClient.h"
#include "IAiProvider.h"
#include "../Conversation.h"
#include "../../io/CancellationToken.h"

#include <coro/coro.hpp>
#include <memory>
#include <string>

namespace gladius::agent
{
    struct ImageGenerationRequest
    {
        std::string prompt;
        std::string size = "1024x1024";
    };

    struct ImageGenerationResponse
    {
        bool success = true;
        ImageContent image;
        std::string error;
    };

    /// @brief Abstract interface for reference image generation
    class IImageProvider
    {
      public:
        virtual ~IImageProvider() = default;

        [[nodiscard]] virtual coro::task<ImageGenerationResponse>
        generate(ImageGenerationRequest const & request,
                 io::CancellationToken const & cancellation) = 0;
    };

    /// @brief OpenAI-compatible /v1/images/generations provider
    class OpenAiImageProvider final : public IImageProvider
    {
      public:
        explicit OpenAiImageProvider(ProviderConfig config,
                                     std::shared_ptr<IHttpTransport> transport = nullptr);

        [[nodiscard]] coro::task<ImageGenerationResponse>
        generate(ImageGenerationRequest const & request,
                 io::CancellationToken const & cancellation) override;

      private:
        ProviderConfig m_config;
        std::shared_ptr<IHttpTransport> m_transport;
    };

    /// @brief Decode a standard Base64 string to raw bytes
    [[nodiscard]] std::vector<std::byte> decodeBase64(std::string const & encoded);
} // namespace gladius::agent
