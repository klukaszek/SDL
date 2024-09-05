// File: /webgpu/SDL_gpu_webgpu.c

#include "../SDL_sysgpu.h"
#include "SDL_internal.h"
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_stdinc.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <webgpu/webgpu.h>

typedef struct WebGPU_GPURenderer
{
    WGPUInstance instance;
    WGPUAdapter adapter;
    WGPUDevice device;
    WGPUSurface surface;
    WGPUSwapChain swapchain;
    WGPUTextureFormat render_format;
    WGPUTexture depth_stencil_tex;
    WGPUTextureView depth_stencil_view;
    WGPUTexture msaa_tex;
    WGPUTextureView msaa_view;
    uint32_t width;
    uint32_t height;
    uint32_t sample_count;
    bool debugMode;
    bool preferLowPower;
} WebGPU_GPURenderer;

typedef struct WebGPU_GPUTexture
{
    TextureCommonHeader common;
    WGPUTexture texture;
    WGPUTextureView view;
} WebGPU_GPUTexture;

typedef struct WebGPU_GPUBuffer
{
    WGPUBuffer buffer;
    Uint64 size;
    WGPUBufferUsage usage;
} WebGPU_GPUBuffer;

typedef struct WebGPU_GPUCommandBuffer
{
    CommandBufferCommonHeader common;
    WGPUCommandEncoder encoder;
    WGPUCommandBuffer cmd_buffer;
} WebGPU_GPUCommandBuffer;

static void WebGPU_ErrorCallback(WGPUErrorType type, const char *message, void *userdata)
{
    SDL_SetError("WebGPU error: %s", message);
}

static void WebGPU_RequestAdapterCallback(WGPURequestAdapterStatus status, WGPUAdapter adapter, const char *message, void *userdata)
{
    WebGPU_GPURenderer *renderer = (WebGPU_GPURenderer *)userdata;
    if (status == WGPURequestAdapterStatus_Success) {
        renderer->adapter = adapter;
    } else {
        SDL_SetError("Failed to request WebGPU adapter: %s", message);
    }
}

static void WebGPU_RequestDeviceCallback(WGPURequestDeviceStatus status, WGPUDevice device, const char *message, void *userdata)
{
    WebGPU_GPURenderer *renderer = (WebGPU_GPURenderer *)userdata;
    if (status == WGPURequestDeviceStatus_Success) {
        renderer->device = device;
    } else {
        SDL_SetError("Failed to request WebGPU device: %s", message);
    }
}

static SDL_GPUDevice *WebGPU_CreateDevice(SDL_bool debug, bool preferLowPower, SDL_PropertiesID props)
{

    WebGPU_GPURenderer *renderer;
    SDL_GPUDevice *result;

    // Allocate memory for the renderer and device
    renderer = (WebGPU_GPURenderer *)SDL_malloc(sizeof(WebGPU_GPURenderer));
    memset(renderer, '\0', sizeof(WebGPU_GPURenderer));
    renderer->debugMode = debug;
    renderer->preferLowPower = preferLowPower;

    // Initialize WebGPU
    renderer->instance = wgpuCreateInstance(NULL);
    if (!renderer->instance) {
        SDL_SetError("Failed to create WebGPU instance");
        SDL_free(renderer);
        return NULL;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_GPU, "SDL_GPU Driver: WebGPU");
    SDL_LogInfo(
        SDL_LOG_CATEGORY_GPU,
        ""
    )

    // Request adapter
    wgpuInstanceRequestAdapter(renderer->instance, NULL, WebGPU_RequestAdapterCallback, renderer);
    if (!renderer->adapter) {
        SDL_SetError("Failed to get WebGPU adapter");
        wgpuInstanceRelease(renderer->instance);
        SDL_free(renderer);
        return NULL;
    }

    // Request device from adapter
    // TODO: Set up device descriptor according to the properties passed
    WGPUDeviceDescriptor device_desc = { 0 };
    wgpuAdapterRequestDevice(renderer->adapter, &device_desc, WebGPU_RequestDeviceCallback, renderer);
    if (!renderer->device) {
        SDL_SetError("Failed to create WebGPU device");
        wgpuAdapterRelease(renderer->adapter);
        wgpuInstanceRelease(renderer->instance);
        SDL_free(renderer);
        return NULL;
    }

    // Set our error callback for emscripten
    wgpuDeviceSetUncapturedErrorCallback(renderer->device, WebGPU_ErrorCallback, NULL);

    // Set up function pointers
    // TODO: Implement these functions
    // result->DestroyDevice = WebGPU_DestroyDevice;
    // result->CreateTexture = WebGPU_CreateTexture;
    // ... (other function pointers)

    return result;
}

// TODO: Implement other necessary functions like WebGPU_DestroyDevice, WebGPU_CreateTexture, etc.

SDL_GPUBootstrap WebGPUDriver = {
    "webgpu",
    SDL_GPU_DRIVER_WEBGPU,
    SDL_GPU_SHADERFORMAT_SPIRV,
    WebGPU_PrepareDriver,
    WebGPU_CreateDevice,
};
