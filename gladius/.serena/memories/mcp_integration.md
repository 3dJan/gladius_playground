# MCP Integration Architecture

## Overview
Gladius includes a comprehensive Model Context Protocol (MCP) server implementation that enables AI agent control and automation of the application.

## Architecture Components

### 1. MCPApplicationInterface (Header-only)
**File**: `src/mcp/MCPApplicationInterface.h`
- Clean, minimal interface for MCP integration
- No heavy dependencies to avoid build conflicts
- Essential methods: `getVersion()`, `isRunning()`, `getStatus()`

### 2. MCPServer (Core Implementation)
**Files**: `src/mcp/MCPServer.{h,cpp}`
- Complete HTTP server with JSON-RPC 2.0 protocol
- Built on cpp-httplib for HTTP handling
- Dual constructor pattern: accepts both shared_ptr and raw pointer
- CORS support for web-based AI agents
- Health endpoints for monitoring

### 3. ApplicationMCPAdapter (Bridge)
**Files**: `src/mcp/ApplicationMCPAdapter.{h,cpp}`
- Adapter pattern bridging Application class to MCP interface
- Isolates Application dependencies from MCP server
- Uses raw pointer to avoid circular dependencies

## Integration Pattern

```cpp
// In Application.h
std::unique_ptr<mcp::MCPServer> m_mcpServer;
std::unique_ptr<mcp::ApplicationMCPAdapter> m_mcpAdapter;

// In Application::enableMCPServer()
m_mcpAdapter = std::make_unique<mcp::ApplicationMCPAdapter>(this);
m_mcpServer = std::make_unique<mcp::MCPServer>(m_mcpAdapter.get());
bool success = m_mcpServer->start(port);
```

## Available Tools

The adapter-registered tool set (44 tools as of this writing) is what `tools/list` returns:
document lifecycle, function authoring, evaluation/validation, rendering, build items, library
management, change tracking, and - UI mode only - the UI interaction tools. Inspect the live
list with:

```bash
curl -s -X POST http://localhost:8080/ -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}'
```

There are no `ping`, `list_tools` or `test_computation` tools: connectivity is checked with
`GET /health`, enumeration with the `tools/list` method, and state with the `get_status` tool.

### Tool Extension
New tools can be added by:
1. Implementing tool logic in ApplicationMCPAdapter
2. Registering tool in MCPServer constructor
3. Following JSON schema for tool definitions

## HTTP Endpoints

### MCP Protocol
- `POST /` - JSON-RPC 2.0 endpoint (the only JSON-RPC route; `POST /mcp` returns 404)
- `OPTIONS /.*` - CORS preflight support

### Monitoring
- `GET /health` - Health check endpoint

The server supports only `initialize`, `tools/list` and `tools/call`. Any other method is
answered with JSON-RPC error `-32601` over an HTTP `200` response. It is stateless: no
`mcp-session-id` header and no SSE streaming.

`listen("localhost", port)` binds the IPv6 loopback only, so clients must use
`http://localhost:8080/` rather than `http://127.0.0.1:8080/`.

## Usage Modes

### 1. HTTP Mode
```bash
./gladiusmcp --mcp-server 8080            # Gladius window + MCP server
./gladiusmcp --mcp-server 8080 --headless # background server, no window
```
- Web-accessible for browser-based AI agents
- CORS enabled for cross-origin requests
- RESTful health endpoints
- A UI-mode server exits when its window is closed

### 2. STDIO Mode
```bash
./gladiusmcp --mcp-stdio
```
- Direct stdin/stdout communication
- For embedded MCP client integration
- No HTTP overhead

There is no `--enable-mcp` or `--port` flag; `--mcp-server [port]` is the HTTP switch.

## AI Agent Integration

### Copilot / VS Code
Configure in the repository-root `.mcp.json` (portable `mcpServers` schema):
```json
{
  "mcpServers": {
    "gladius": {
      "type": "http",
      "url": "http://localhost:8080/",
      "tools": ["*"]
    },
    "gladius-stdio": {
      "type": "stdio",
      "command": "/path/to/gladiusmcp",
      "args": ["--mcp-stdio", "--headless"],
      "tools": ["*"],
      "env": { "DISPLAY": ":0" }
    }
  }
}
```
A client never launches an `http` entry: start the server with the VS Code tasks
**"Start Gladius MCP Server (HTTP)"** or **"Start Gladius MCP Server (HTTP, headless)"**.
Only `stdio` entries are launched and auto-relaunched by the MCP host.

## Testing and Compliance

### Protocol Compliance
- JSON-RPC 2.0 specification compliance
- Advertises MCP protocol version `2024-11-05` and ignores the client's requested version
- Schema validation for requests/responses

### Testing Tools
```bash
# Manual testing
curl -X POST http://localhost:8080/ \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}'

# Automated transport tests
cd out/build/linux-releaseWithDebug/tests/apitests
./gladius_apitest --gtest_filter='MCPHTTPTransportTest.*'
```

## Benefits for AI Agents

### Capabilities
- **Real-time application control**: Start/stop, status monitoring
- **3D model manipulation**: Import/export, processing operations
- **Computation access**: OpenCL-based 3D computations
- **File system integration**: 3MF file operations
- **Workflow automation**: Multi-step 3D processing pipelines

### Use Cases
- Automated 3D model processing workflows
- AI-driven 3D design assistance
- Batch processing of 3MF files
- Integration with AI coding assistants
- Automated testing and validation

## Security Considerations

### Current Implementation
- No authentication (development mode)
- CORS enabled for development
- Local-only binding recommended

### Production Enhancements (Future)
- Authentication tokens
- Rate limiting
- Secure HTTPS endpoints
- Access control for sensitive operations

## Future Extensibility

### Planned Tool Categories
1. **Document Operations**: Open, save, import, export 3MF files
2. **3D Manipulation**: Transform, boolean operations, mesh processing
3. **Rendering Control**: Camera, lighting, visualization settings
4. **Computation Tools**: Custom OpenCL kernel execution
5. **Workflow Management**: Save/restore application state

### Plugin Architecture
The MCP system is designed to support plugin-based tool registration for easy extension by third-party developers.