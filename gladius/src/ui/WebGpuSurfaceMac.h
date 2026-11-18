#pragma once

#include <webgpu/webgpu.h>

struct GLFWwindow;

namespace gladius
{
#if defined(__APPLE__)
    /**
     * @brief Create a WebGPU Metal surface source for the given GLFW window.
     *
     * The GLFW window is created with GLFW_NO_API, so no CAMetalLayer exists
     * yet. This helper enables layer backing, installs a freshly created
     * CAMetalLayer and hands it to wgpu::SurfaceSourceMetalLayer.
     *
     * The layer is created with contentsScale 1.0 because the WebGPU surface is
     * configured with glfwGetFramebufferSize(), which already reports physical
     * device pixels on Cocoa. Dawn rewrites the layer's drawableSize on
     * surface configuration.
     *
     * @param glfwWindow GLFW window to attach the layer to.
     * @return Surface source; its layer is null when the window has no view.
     */
    wgpu::SurfaceSourceMetalLayer createMetalSurfaceSource(GLFWwindow* glfwWindow);
#endif
}
