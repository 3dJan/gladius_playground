# Building the WebGPU browser target

The browser target is an additional Emscripten build. The native desktop and
native `linux-webgpu-releaseWithDebug` targets are unchanged.

## Prerequisites

Install and activate the Emscripten SDK, then ensure `EMSDK` points to the SDK
root. The preset uses the SDK's CMake toolchain directly.

## Configure and build

From `gladius/`, configure the `web-release` preset and build the `gladius`
target. This is the full Gladius application source graph, not a standalone
WebGPU demo. The output is written to `out/build/web-release/web/` and includes
a self-contained `gladius.html` with the packaged runtime data.

The generated page embeds the Wasm and virtual filesystem so it can be opened
directly from `file://` during development. Serving it over HTTP is still
recommended for deployment. WebGPU also requires a browser with WebGPU enabled
and a compatible adapter.

## Browser limitations

The browser build uses the real GLFW/ImGui application and WebGPU backend.
WebGPU adapter and device acquisition is asynchronous in the browser, and the
application defers compute-dependent UI setup until the device is ready.
Browser file pickers and persistent settings still need browser-specific
adapters; native file dialogs, desktop persistence, MCP, OpenCL, OpenVDB/NanoVDB,
and desktop packaging remain excluded. A local `.3mf` can be opened by dragging
it onto the Gladius canvas. The browser stages the file in its session-local
virtual filesystem, and `Download 3MF` downloads the edited Gladius project,
including its graph and resources, as a new `.3mf` file. Imported files keep
their original filename when possible; new documents use `gladius-model.3mf`.
The virtual filesystem is not persistent across page reloads, and browser
Save As does not provide a custom filename picker yet.

Native-only features remain outside this target: OpenCL, OpenVDB/NanoVDB, MCP,
desktop file dialogs, native filesystem persistence, and desktop packaging.

## Opening a shared 3MF URL

The browser build can open a 3MF file during startup when the page URL contains
the `url` query parameter. For example:

`https://example.org/gladius.html?url=https%3A%2F%2Fmodels.example.org%2Fsample.3mf`

Encode the complete target URL as one query-parameter value. This is important
when the target URL contains its own query string or fragment, because characters
such as `&` and `#` otherwise become part of the Gladius page URL instead of the
download URL.

The target must be an HTTP(S) URL. Same-origin files work without additional
configuration. A file hosted on another origin must allow browser reads with an
appropriate `Access-Control-Allow-Origin` response header; this also applies to
the final response after redirects. Downloads are made without credentials, so
authenticated URLs and server-side proxying are not supported by this feature.

The URL pathname is used for the imported filename when possible, but it does not
need to end in `.3mf`; the downloaded bytes are validated when Gladius opens the
file. Download, HTTP, and CORS errors are shown in the browser, while invalid
3MF files use Gladius's normal loading/error UI. Dragging a local `.3mf` onto the
canvas remains available as a fallback.