#include <webgpu/webgpu.h>
#include <webgpu/webgpu_cpp.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>

namespace gladius
{
    wgpu::SurfaceSourceMetalLayer createMetalSurfaceSource(GLFWwindow* glfwWindow)
    {
        wgpu::SurfaceSourceMetalLayer surfaceSource;

        NSWindow* window = static_cast<NSWindow*>(glfwGetCocoaWindow(glfwWindow));
        if (!window)
        {
            return surfaceSource;
        }

        NSView* view = [window contentView];
        if (!view)
        {
            return surfaceSource;
        }

        [view setWantsLayer:YES];

        CAMetalLayer* metalLayer = [CAMetalLayer layer];
        [view setLayer:metalLayer];

        // The surface is configured with glfwGetFramebufferSize(), which already
        // reports physical pixels on Cocoa, so the layer must not scale again.
        // Dawn adjusts drawableSize itself when the surface is configured.
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(glfwWindow, &framebufferWidth, &framebufferHeight);
        metalLayer.contentsScale = 1.0;
        metalLayer.drawableSize = CGSizeMake(std::max(framebufferWidth, 1), std::max(framebufferHeight, 1));

        surfaceSource.layer = metalLayer;
        return surfaceSource;
    }
}
