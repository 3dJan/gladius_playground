/**
 * @file MCP_HTTP_transport_tests.cpp
 * @brief Tests for the MCP HTTP transport.
 *
 * These tests exercise the real cpp-httplib routing, the /health endpoint and the
 * JSON-RPC method allow-list, which the in-process processJSONRPCRequest() tests skip.
 */

#include "MockMCPApplication.h"
#include "mcp/MCPServer.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace gladius::tests
{
    namespace
    {
        /// @brief Reserve a port that nothing else is listening on.
        /// @return Port number on localhost, or 0 when no port could be reserved.
        /// @note Must use the same host string as MCPServer::startHTTP() so the reserved
        /// port belongs to the address family (IPv4 or IPv6) the server will bind to.
        /// @note bind_to_any_port() already calls listen() but never marks the server as
        /// running, so stop() would be a no-op and the listening socket would leak for the
        /// whole process. cpp-httplib sets SO_REUSEPORT, which makes the kernel load-balance
        /// connections between that leaked socket and the real server, so requests would
        /// randomly be accepted by a socket with no acceptor and time out. Listening and
        /// stopping normally releases the socket and makes the port reusable.
        int reserveFreePort()
        {
            httplib::Server probe;
            int const port = probe.bind_to_any_port("localhost");
            if (port <= 0)
            {
                return 0;
            }

            std::thread listener([&probe]() { probe.listen_after_bind(); });
            probe.wait_until_ready();
            probe.stop();
            listener.join();
            return port;
        }
    } // namespace

    /// Fixture that runs a real MCP HTTP server on a freshly reserved port.
    class MCPHTTPTransportTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_mockApp = std::make_unique<testing::NiceMock<MockMCPApplication>>();
            m_server = std::make_unique<mcp::MCPServer>(m_mockApp.get());

            int const freePort = reserveFreePort();
            ASSERT_GT(freePort, 0) << "No free localhost port available";
            ASSERT_TRUE(m_server->startHTTP(freePort));
            ASSERT_TRUE(m_server->isRunning());

            m_client = std::make_unique<httplib::Client>("localhost", m_server->getPort());
            m_client->set_connection_timeout(5);
            m_client->set_read_timeout(10);

            // listen() runs on its own thread; wait until the socket actually answers.
            for (int attempt = 0; attempt < 40; ++attempt)
            {
                if (m_client->Get("/health"))
                {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            FAIL() << "MCP HTTP server never became reachable on port " << m_server->getPort();
        }

        void TearDown() override
        {
            m_client.reset();
            m_server.reset();
            m_mockApp.reset();
        }

        /// @brief POST a JSON-RPC request to the single JSON-RPC endpoint.
        httplib::Result postJson(json const & request)
        {
            return m_client->Post("/", request.dump(), "application/json");
        }

        std::unique_ptr<testing::NiceMock<MockMCPApplication>> m_mockApp;
        std::unique_ptr<mcp::MCPServer> m_server;
        std::unique_ptr<httplib::Client> m_client;
    };

    TEST_F(MCPHTTPTransportTest, HealthEndpoint_ServerStarted_ReportsRunningAndToolCount)
    {
        // Arrange / Act
        auto response = m_client->Get("/health");

        // Assert
        ASSERT_TRUE(response != nullptr) << httplib::to_string(response.error());
        ASSERT_EQ(response->status, 200);
        json body = json::parse(response->body);
        EXPECT_EQ(body["status"], "ok");
        EXPECT_EQ(body["server"], "Gladius MCP Server");
        EXPECT_EQ(body["running"], true);
        EXPECT_EQ(body["tools_count"], m_server->getRegisteredTools().size());
        EXPECT_GT(body["tools_count"].get<std::size_t>(), 0u);
    }

    TEST_F(MCPHTTPTransportTest, Initialize_OverHttp_ReturnsProtocolVersionAndServerInfo)
    {
        // Arrange
        json request = {{"jsonrpc", "2.0"},
                        {"id", 1},
                        {"method", "initialize"},
                        {"params",
                         {{"protocolVersion", "2025-06-18"},
                          {"capabilities", json::object()},
                          {"clientInfo", {{"name", "probe"}, {"version", "0"}}}}}};

        // Act
        auto response = postJson(request);

        // Assert
        ASSERT_TRUE(response != nullptr) << httplib::to_string(response.error());
        ASSERT_EQ(response->status, 200);
        EXPECT_EQ(response->get_header_value("Content-Type"), "application/json");
        json body = json::parse(response->body);
        EXPECT_EQ(body["jsonrpc"], "2.0");
        EXPECT_EQ(body["id"], 1);
        EXPECT_EQ(body["result"]["protocolVersion"], "2024-11-05");
        EXPECT_TRUE(body["result"]["capabilities"].contains("tools"));
        EXPECT_EQ(body["result"]["serverInfo"]["name"], "Gladius MCP Server");
        EXPECT_FALSE(body["result"].contains("sessionId"));
    }

    TEST_F(MCPHTTPTransportTest, ListTools_OverHttp_ReturnsSchemasForAllRegisteredTools)
    {
        // Arrange / Act
        auto response = postJson({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});

        // Assert
        ASSERT_TRUE(response != nullptr) << httplib::to_string(response.error());
        ASSERT_EQ(response->status, 200);
        json body = json::parse(response->body);
        ASSERT_TRUE(body["result"].contains("tools"));
        EXPECT_EQ(body["result"]["tools"].size(), m_server->getRegisteredTools().size());
        EXPECT_EQ(body["id"], 2);

        std::vector<std::string> names;
        for (auto const & tool : body["result"]["tools"])
        {
            names.push_back(tool["name"].get<std::string>());
            EXPECT_TRUE(tool.contains("inputSchema"));
        }
        EXPECT_THAT(names, ::testing::Contains("get_status"));
        EXPECT_THAT(names, ::testing::Contains("create_document"));
    }

    TEST_F(MCPHTTPTransportTest, CallTool_OverHttp_ReturnsToolResultAsTextContent)
    {
        // Arrange
        EXPECT_CALL(*m_mockApp, getApplicationName()).WillOnce(::testing::Return("Gladius"));
        EXPECT_CALL(*m_mockApp, getVersion()).WillOnce(::testing::Return("1.2.3"));
        EXPECT_CALL(*m_mockApp, getStatus()).WillOnce(::testing::Return("Idle"));
        EXPECT_CALL(*m_mockApp, hasActiveDocument()).WillOnce(::testing::Return(false));
        EXPECT_CALL(*m_mockApp, isHeadlessMode()).WillOnce(::testing::Return(true));

        json request = {{"jsonrpc", "2.0"},
                        {"id", 3},
                        {"method", "tools/call"},
                        {"params", {{"name", "get_status"}, {"arguments", json::object()}}}};

        // Act
        auto response = postJson(request);

        // Assert
        ASSERT_TRUE(response != nullptr) << httplib::to_string(response.error());
        ASSERT_EQ(response->status, 200);
        json body = json::parse(response->body);
        EXPECT_EQ(body["id"], 3);
        ASSERT_TRUE(body["result"].contains("content"));
        EXPECT_FALSE(body["result"].value("isError", false));
        ASSERT_EQ(body["result"]["content"][0]["type"], "text");
        json toolResult = json::parse(body["result"]["content"][0]["text"].get<std::string>());
        EXPECT_EQ(toolResult["application"], "Gladius");
        EXPECT_EQ(toolResult["version"], "1.2.3");
        EXPECT_EQ(toolResult["status"], "Idle");
        EXPECT_EQ(toolResult["headless"], true);
    }

    TEST_F(MCPHTTPTransportTest, CallTool_UnknownToolName_ReturnsMethodNotFoundWithToolList)
    {
        // Arrange / Act
        auto response = postJson({{"jsonrpc", "2.0"},
                                  {"id", 4},
                                  {"method", "tools/call"},
                                  {"params", {{"name", "does_not_exist"}}}});

        // Assert
        ASSERT_TRUE(response != nullptr) << httplib::to_string(response.error());
        ASSERT_EQ(response->status, 200);
        json body = json::parse(response->body);
        ASSERT_TRUE(body.contains("error"));
        EXPECT_EQ(body["error"]["code"], -32601);
        EXPECT_THAT(body["error"]["message"].get<std::string>(),
                    ::testing::HasSubstr("Tool not found: does_not_exist"));
        EXPECT_THAT(body["error"]["message"].get<std::string>(),
                    ::testing::HasSubstr("get_status"));
    }

    TEST_F(MCPHTTPTransportTest, UnsupportedMethod_OverHttp_ReturnsMethodNotFoundError)
    {
        // MCP clients send notifications/initialized right after initialize; the server
        // answers with a JSON-RPC error over HTTP 200 instead of dropping the request.
        for (std::string const method : {"notifications/initialized", "ping", "resources/list"})
        {
            // Act
            auto response = postJson({{"jsonrpc", "2.0"}, {"id", 5}, {"method", method}});

            // Assert
            ASSERT_TRUE(response != nullptr) << method << ": " << httplib::to_string(response.error());
            EXPECT_EQ(response->status, 200) << method;
            json body = json::parse(response->body);
            EXPECT_EQ(body["error"]["code"], -32601) << method;
            EXPECT_THAT(body["error"]["message"].get<std::string>(),
                        ::testing::HasSubstr("Method not found: " + method))
              << method;
        }
    }

    TEST_F(MCPHTTPTransportTest, MalformedRequestBody_OverHttp_ReturnsParseError)
    {
        // Act
        auto response = m_client->Post("/", "{ not json", "application/json");

        // Assert
        ASSERT_TRUE(response != nullptr) << httplib::to_string(response.error());
        ASSERT_EQ(response->status, 200);
        EXPECT_EQ(json::parse(response->body)["error"]["code"], -32700);
    }

    TEST_F(MCPHTTPTransportTest, MissingMethodField_OverHttp_ReturnsInvalidRequestError)
    {
        // Act
        auto response = postJson({{"jsonrpc", "2.0"}, {"id", 6}});

        // Assert
        ASSERT_TRUE(response != nullptr) << httplib::to_string(response.error());
        ASSERT_EQ(response->status, 200);
        EXPECT_EQ(json::parse(response->body)["error"]["code"], -32600);
    }

    TEST_F(MCPHTTPTransportTest, UnknownRoute_OverHttp_ReturnsNotFound)
    {
        // The server exposes the JSON-RPC entry point at /, there is no /mcp route.
        auto response = m_client->Post("/mcp", "{}", "application/json");
        ASSERT_TRUE(response != nullptr) << httplib::to_string(response.error());
        EXPECT_EQ(response->status, 404);
    }

    TEST_F(MCPHTTPTransportTest, CorsPreflight_OverHttp_IsAcceptedWithCorsHeaders)
    {
        // Act
        auto preflight = m_client->Options("/health");
        auto health = m_client->Get("/health");

        // Assert
        ASSERT_TRUE(preflight != nullptr) << httplib::to_string(preflight.error());
        ASSERT_TRUE(health != nullptr) << httplib::to_string(health.error());
        EXPECT_EQ(health->get_header_value("Access-Control-Allow-Origin"), "*");
        EXPECT_THAT(health->get_header_value("Access-Control-Allow-Methods"),
                    ::testing::HasSubstr("POST"));
    }

    TEST_F(MCPHTTPTransportTest, Stop_AfterHttpStart_FreesPortAndClearsRunningState)
    {
        // Act
        m_server->stop();

        // Assert
        EXPECT_FALSE(m_server->isRunning());
        EXPECT_EQ(m_server->getPort(), 0);
        EXPECT_EQ(m_client->Get("/health"), nullptr);
    }
}
