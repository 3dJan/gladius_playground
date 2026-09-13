#include <iostream>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <GLFW/glfw3.h>
#include <webgpu/webgpu_cpp.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_wgpu.h"

static GLFWwindow* g_window = nullptr;
static wgpu::Instance g_instance;
static wgpu::Adapter g_adapter;
static wgpu::Device g_device;
static wgpu::Queue g_queue;
static wgpu::Surface g_surface;
static wgpu::TextureFormat g_surfaceFormat = wgpu::TextureFormat::Undefined;
static uint32_t g_width = 1280;
static uint32_t g_height = 720;
static bool g_ready = false;

static void updateStatus(const char* msg)
{
    EM_ASM({
        var s = document.getElementById('gladius-status');
        if (s) {
            s.textContent = UTF8ToString($0);
            s.style.display = 'block';
            s.style.opacity = '1';
        }
        console.log('[Demo]', UTF8ToString($0));
    }, msg);
}

static void resizeSurface(uint32_t width, uint32_t height)
{
    if (!g_surface || !g_device || width == 0 || height == 0)
        return;

    g_width = width;
    g_height = height;

    wgpu::SurfaceConfiguration config = {};
    config.device = g_device;
    config.format = g_surfaceFormat;
    config.usage = wgpu::TextureUsage::RenderAttachment;
    config.width = g_width;
    config.height = g_height;
    config.presentMode = wgpu::PresentMode::Fifo;
    config.alphaMode = wgpu::CompositeAlphaMode::Auto;
    g_surface.Configure(&config);
}

static void mainLoop()
{
    if (!g_ready)
        return;

    glfwPollEvents();

    int w = 0, h = 0;
    glfwGetFramebufferSize(g_window, &w, &h);
    if (w > 0 && h > 0 && (static_cast<uint32_t>(w) != g_width || static_cast<uint32_t>(h) != g_height))
    {
        resizeSurface(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    }

    wgpu::SurfaceTexture surfaceTexture;
    g_surface.GetCurrentTexture(&surfaceTexture);
    if (surfaceTexture.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
        surfaceTexture.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal)
    {
        return;
    }

    wgpu::TextureView targetView = surfaceTexture.texture.CreateView();

    ImGui_ImplWGPU_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // Show a simple test window
    ImGui::Begin("WebGPU ImGui Minimal Demo");
    ImGui::Text("Hello from Dear ImGui running on WebGPU in Emscripten!");
    static float f = 0.5f;
    static int counter = 0;
    ImGui::SliderFloat("Slider", &f, 0.0f, 1.0f);
    if (ImGui::Button("Click Me"))
    {
        counter++;
    }
    ImGui::SameLine();
    ImGui::Text("Counter: %d", counter);
    ImGui::Text("Framerate: %.1f FPS (%.2f ms/frame)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
    ImGui::End();

    static bool showDemo = true;
    if (showDemo)
    {
        ImGui::ShowDemoWindow(&showDemo);
    }

    ImGui::Render();

    wgpu::CommandEncoder encoder = g_device.CreateCommandEncoder();
    wgpu::RenderPassColorAttachment colorAttachment = {};
    colorAttachment.view = targetView;
    colorAttachment.loadOp = wgpu::LoadOp::Clear;
    colorAttachment.storeOp = wgpu::StoreOp::Store;
    colorAttachment.clearValue = { 0.15, 0.18, 0.22, 1.0 };

    wgpu::RenderPassDescriptor renderPassDesc = {};
    renderPassDesc.colorAttachmentCount = 1;
    renderPassDesc.colorAttachments = &colorAttachment;

    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&renderPassDesc);
    ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass.Get());
    pass.End();

    wgpu::CommandBuffer cmd = encoder.Finish();
    g_queue.Submit(1, &cmd);
}

static void onDeviceAcquired(wgpu::Device device)
{
    g_device = std::move(device);
    g_queue = g_device.GetQueue();

    updateStatus("Creating WebGPU surface...");

    wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector canvasDesc;
    canvasDesc.selector = "#gladius-canvas";

    wgpu::SurfaceDescriptor surfaceDesc;
    surfaceDesc.nextInChain = &canvasDesc;
    g_surface = g_instance.CreateSurface(&surfaceDesc);
    if (!g_surface)
    {
        updateStatus("Failed to create WebGPU surface");
        return;
    }

    wgpu::SurfaceCapabilities capabilities;
    if (!g_surface.GetCapabilities(g_adapter, &capabilities) || capabilities.formatCount == 0)
    {
        updateStatus("Failed to get surface capabilities");
        return;
    }

    g_surfaceFormat = capabilities.formats[0];

    int w = 0, h = 0;
    glfwGetFramebufferSize(g_window, &w, &h);
    if (w <= 0 || h <= 0) { w = 1280; h = 720; }
    resizeSurface(static_cast<uint32_t>(w), static_cast<uint32_t>(h));

    updateStatus("Initializing Dear ImGui...");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
#ifdef IMGUI_HAS_DOCK
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
#endif

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOther(g_window, true);
    ImGui_ImplGlfw_InstallEmscriptenCallbacks(g_window, "#gladius-canvas");

    ImGui_ImplWGPU_InitInfo initInfo = {};
    initInfo.Device = g_device.Get();
    initInfo.NumFramesInFlight = 3;
    initInfo.RenderTargetFormat = static_cast<WGPUTextureFormat>(g_surfaceFormat);
    initInfo.DepthStencilFormat = WGPUTextureFormat_Undefined;
    if (!ImGui_ImplWGPU_Init(&initInfo))
    {
        updateStatus("Failed to initialize ImGui_ImplWGPU");
        return;
    }

    g_ready = true;
    updateStatus("Running...");

    emscripten_set_main_loop(mainLoop, 0, false);
}

static void requestDevice()
{
    updateStatus("Requesting WebGPU device...");
    wgpu::DeviceDescriptor desc = {};
    g_adapter.RequestDevice(
        &desc,
        wgpu::CallbackMode::AllowSpontaneous,
        [](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView message)
        {
            if (status != wgpu::RequestDeviceStatus::Success || !device)
            {
                std::string err = message.data ? std::string(message.data, message.length) : "unknown error";
                updateStatus(("RequestDevice failed: " + err).c_str());
                return;
            }
            onDeviceAcquired(std::move(device));
        });
}

static void requestAdapter()
{
    updateStatus("Requesting WebGPU adapter...");
    wgpu::RequestAdapterOptions options = {};
    options.powerPreference = wgpu::PowerPreference::HighPerformance;

    g_instance.RequestAdapter(
        &options,
        wgpu::CallbackMode::AllowSpontaneous,
        [](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView message)
        {
            if (status != wgpu::RequestAdapterStatus::Success || !adapter)
            {
                std::string err = message.data ? std::string(message.data, message.length) : "unknown error";
                updateStatus(("RequestAdapter failed: " + err).c_str());
                return;
            }
            g_adapter = std::move(adapter);
            requestDevice();
        });
}

int main()
{
    updateStatus("Initializing GLFW...");
    glfwSetErrorCallback([](int error, const char* desc) {
        std::cerr << "GLFW Error " << error << ": " << desc << std::endl;
    });

    if (!glfwInit())
    {
        updateStatus("glfwInit() failed");
        return 1;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    g_window = glfwCreateWindow(1280, 720, "ImGui WebGPU Demo", nullptr, nullptr);
    if (!g_window)
    {
        updateStatus("glfwCreateWindow() failed");
        return 1;
    }

    updateStatus("Creating WebGPU instance...");
    g_instance = wgpu::CreateInstance();
    if (!g_instance)
    {
        updateStatus("CreateInstance() failed");
        return 1;
    }

    requestAdapter();
    return 0;
}
