#include "agent/providers/ProviderError.h"

#include <gtest/gtest.h>
#include <string>

namespace gladius::agent::tests
{
    TEST(ProviderError, ClassifyHttpError_Status401WithKey_ReportsInvalidApiKey)
    {
        // Arrange
        char const * body = "{\"error\":{\"message\":\"Incorrect API key provided\"}}";

        // Act
        auto const err = classifyHttpError(401, body, false, "openai/gpt-4o");

        // Assert
        EXPECT_EQ(err.kind, ProviderErrorKind::InvalidApiKey);
        EXPECT_NE(err.message.find("rejected the API key"), std::string::npos);
        EXPECT_FALSE(err.hint.empty());
        EXPECT_TRUE(err.isSettingsFixable());
    }

    TEST(ProviderError, ClassifyHttpError_Status403WithoutKey_ReportsMissingApiKey)
    {
        // Act
        auto const err = classifyHttpError(403, "", true);

        // Assert
        EXPECT_EQ(err.kind, ProviderErrorKind::MissingApiKey);
        EXPECT_NE(err.message.find("No API key"), std::string::npos);
    }

    TEST(ProviderError, ClassifyHttpError_Status404_ReportsModelNotFound)
    {
        // Act
        auto const err = classifyHttpError(404, "{\"error\":\"model_not_found\"}", false);

        // Assert
        EXPECT_EQ(err.kind, ProviderErrorKind::ModelNotFound);
        EXPECT_TRUE(err.isSettingsFixable());
    }

    TEST(ProviderError, ClassifyHttpError_Status429_ReportsRateLimited)
    {
        // Act
        auto const err = classifyHttpError(429, "Too Many Requests", false);

        // Assert
        EXPECT_EQ(err.kind, ProviderErrorKind::RateLimited);
        EXPECT_FALSE(err.isSettingsFixable());
    }

    TEST(ProviderError, ClassifyHttpError_Status402_ReportsQuotaExceeded)
    {
        // Act
        auto const err = classifyHttpError(402, "insufficient credits", false);

        // Assert
        EXPECT_EQ(err.kind, ProviderErrorKind::QuotaExceeded);
    }

    TEST(ProviderError, ClassifyErrorText_CurlTransportFailure_ReportsNetwork)
    {
        // Act
        auto const err = classifyErrorText("Could not resolve host: api.openai.com", false);

        // Assert
        EXPECT_EQ(err.kind, ProviderErrorKind::Network);
        EXPECT_NE(err.message.find("could not be reached"), std::string::npos);
    }

    TEST(ProviderError, ClassifyErrorText_CurlTimeout_ReportsTimeout)
    {
        // Act
        auto const err = classifyErrorText("Operation timed out after 120001 milliseconds", false);

        // Assert
        EXPECT_EQ(err.kind, ProviderErrorKind::Timeout);
    }

    TEST(ProviderError, ClassifyErrorText_EmptyResponse_ReportsMalformedResponse)
    {
        // Act
        auto const err = classifyErrorText("Missing choices in OpenAI response", false);

        // Assert
        EXPECT_EQ(err.kind, ProviderErrorKind::MalformedResponse);
    }

    TEST(ProviderError, ClassifyErrorText_NoProvidersConfigured_ReportsNotConfigured)
    {
        // Act
        auto const err = classifyErrorText("No AI providers configured", true);

        // Assert
        EXPECT_EQ(err.kind, ProviderErrorKind::NotConfigured);
        EXPECT_TRUE(err.isSettingsFixable());
    }

    TEST(ProviderError, ClassifyErrorText_RecoveredHttpStatus_StillClassifies)
    {
        // Act: the loop only ever sees the joined text, the status is embedded in it
        auto const err = classifyErrorText("openai/gpt-4o: HTTP 429: slow down", false);

        // Assert
        EXPECT_EQ(err.kind, ProviderErrorKind::RateLimited);
    }

    TEST(ProviderError, ClassifyErrorText_CancelledFlag_ReportsCancelled)
    {
        // Act
        auto const err = classifyErrorText("Request cancelled", false, true);

        // Assert
        EXPECT_EQ(err.kind, ProviderErrorKind::Cancelled);
    }

    TEST(ProviderError, ClassifyHttpError_LargeVendorBody_TruncatesDetail)
    {
        // Arrange: vendor error bodies can run to many kilobytes of HTML
        std::string const huge(5000, 'x');

        // Act
        auto const err = classifyHttpError(599, huge, false);

        // Assert: diagnostics keep a tail, never the whole body
        EXPECT_EQ(err.kind, ProviderErrorKind::Network);
        EXPECT_LT(err.detail.size(), 300u);
    }

    TEST(ProviderError, ClassifyHttpError_UnknownZeroStatusWithText_ReportsUnknown)
    {
        // Act
        auto const err = classifyHttpError(0, "something the classifier has never seen", false);

        // Assert
        EXPECT_EQ(err.kind, ProviderErrorKind::Unknown);
        EXPECT_NE(err.detail.find("never seen"), std::string::npos);
    }

    TEST(ProviderError, TrimProviderBody_LongBody_CapsAtDiagnosticLength)
    {
        // Act
        auto const trimmed = trimProviderBody(std::string(2000, 'e'));

        // Assert
        EXPECT_LT(trimmed.size(), 300u);
        EXPECT_NE(trimmed.find("..."), std::string::npos);
    }

    TEST(ProviderError, DescribeEmptyCompletion_TruncatedStopReason_MentionsOutputLimit)
    {
        // Act
        auto const err = describeEmptyCompletion("max_tokens", "local model");

        // Assert
        EXPECT_EQ(err.kind, ProviderErrorKind::MalformedResponse);
        EXPECT_NE(err.message.find("output limit"), std::string::npos);
        EXPECT_NE(err.hint.find("max output tokens"), std::string::npos);
        EXPECT_EQ(err.detail, "max_tokens");
    }

    TEST(ProviderError, DescribeEmptyCompletion_StopWithoutReason_FallsBackToGenericLabel)
    {
        // Act
        auto const err = describeEmptyCompletion("");

        // Assert
        EXPECT_EQ(err.kind, ProviderErrorKind::MalformedResponse);
        EXPECT_NE(err.message.find("The model answered with nothing"), std::string::npos);
        EXPECT_FALSE(err.hint.empty());
    }

    TEST(ProviderError, Summarised_MessageWithHint_JoinsBoth)
    {
        // Arrange
        ProviderError err{ProviderErrorKind::RateLimited, "Rate limited.", "Wait and retry.", ""};

        // Act / Assert
        EXPECT_EQ(err.summarised(), "Rate limited. Wait and retry.");
        EXPECT_TRUE(err.isSet());
    }
} // namespace gladius::agent::tests
