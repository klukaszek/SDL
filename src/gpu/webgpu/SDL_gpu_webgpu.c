// File: /webgpu/SDL_gpu_webgpu.c
// Author: Kyle Lukaszek
// Email: kylelukaszek at gmail dot com
// License: MIT
// Description: WebGPU driver for SDL_gpu using the emscripten WebGPU implementation
// Note: Compiling SDL GPU programs using emscripten will require -sUSE_WEBGPU=1
//
// TODO:
// - Implement WebGPU_ClaimWindow and manually assign the pointer to the driver so that we can try to use the html canvas

#include "../SDL_sysgpu.h"
#include "SDL_internal.h"
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_mutex.h>
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
    SDL_Semaphore *adapter_semaphore;
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

static void WebGPU_RequestDeviceCallback(WGPURequestDeviceStatus status, WGPUDevice device, const char *message, void *userdata)
{
    WebGPU_GPURenderer *renderer = (WebGPU_GPURenderer *)userdata;
    if (status == WGPURequestDeviceStatus_Success) {
        renderer->device = device;
        SDL_SignalSemaphore(renderer->adapter_semaphore);
        SDL_Log("WebGPU device requested successfully");
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Failed to request WebGPU device: %s", message);
    }
}

static void WebGPU_RequestAdapterCallback(WGPURequestAdapterStatus status, WGPUAdapter adapter, const char *message, void *userdata)
{
    WebGPU_GPURenderer *renderer = (WebGPU_GPURenderer *)userdata;
    if (status != WGPURequestAdapterStatus_Success) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Failed to request WebGPU adapter: %s", message);
    } else {
        renderer->adapter = adapter;
        SDL_Log("WebGPU adapter requested successfully");

        // Request device from adapter
        WGPUFeatureName requiredFeatures[1] = {
            WGPUFeatureName_Depth32FloatStencil8
        };
        WGPUDeviceDescriptor dev_desc = {
            .requiredFeatureCount = 1,
            .requiredFeatures = requiredFeatures,
        };
        wgpuAdapterRequestDevice(renderer->adapter, &dev_desc, WebGPU_RequestDeviceCallback, renderer);
    }
}

static bool WebGPU_PrepareDriver(SDL_VideoDevice *_this)
{
    /*WebGPU_GPURenderer *renderer = (WebGPU_GPURenderer *)malloc(sizeof(WebGPU_GPURenderer));*/
    /*if (!renderer) {*/
    /*    SDL_OutOfMemory();*/
    /*    return false;*/
    /*}*/
    /**/
    /*SDL_memset(renderer, '\0', sizeof(WebGPU_GPURenderer));*/
    /**/
    /*renderer->instance = wgpuCreateInstance(NULL);*/
    /*if (!renderer->instance) {*/
    /*    SDL_LogError(SDL_LOG_CATEGORY_GPU, "Failed to create WebGPU instance");*/
    /*    SDL_free(renderer);*/
    /*    return false;*/
    /*}*/
    /**/
    /*SDL_LogInfo(SDL_LOG_CATEGORY_GPU, "Preparing SDL_GPU Driver: WebGPU");*/
    /*SDL_Log("WebGPU instance created successfully");*/
    /**/
    /*renderer->adapter_semaphore = SDL_CreateSemaphore(0);*/
    /**/
    /*// Request adapter*/
    /*WGPURequestAdapterOptions adapter_options = { 0 };*/
    /**/
    /*wgpuInstanceRequestAdapter(renderer->instance, &adapter_options, WebGPU_RequestAdapterCallback, renderer);*/
    /**/
    /*// Wait for the adapter semaphore to be posted*/
    /*SDL_WaitSemaphore(renderer->adapter_semaphore);*/
    /**/
    /*SDL_Log("Adapter semaphore posted");*/
    /**/
    /*SDL_DestroySemaphore(renderer->adapter_semaphore);*/
    /**/
    /*// Since we've reached this point, we can assume that the driver is ready*/
    /*// and we can free the renderer and return true*/
    /*// We don't have to worry about unloading libraries or anything like that*/
    /*SDL_free(renderer);*/
    return true;
}

static SDL_GPUDevice *WebGPU_CreateDevice(SDL_bool debug, bool preferLowPower, SDL_PropertiesID props)
{

    WebGPU_GPURenderer *renderer;
    SDL_GPUDevice *result = NULL;

    // Allocate memory for the renderer and device
    renderer = (WebGPU_GPURenderer *)SDL_malloc(sizeof(WebGPU_GPURenderer));
    memset(renderer, '\0', sizeof(WebGPU_GPURenderer));
    renderer->debugMode = debug;
    renderer->preferLowPower = preferLowPower;

    // Initialize WebGPU instance so that we can request an adapter and then device
    renderer->instance = wgpuCreateInstance(NULL);
    if (!renderer->instance) {
        SDL_SetError("Failed to create WebGPU instance");
        SDL_free(renderer);
        return NULL;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_GPU, "SDL_GPU Driver: WebGPU");

    renderer->adapter_semaphore = SDL_CreateSemaphore(0);

    // Request adapter using the instance and then the device using the adapter
    WGPURequestAdapterOptions adapter_options = { 0 };
    wgpuInstanceRequestAdapter(renderer->instance, &adapter_options, WebGPU_RequestAdapterCallback, renderer);

    // This seems to be necessary to ensure that the device is created before continuing
    // This should probably be tested on all browsers to ensure that it works as expected
    // but Chrome's Dawn WebGPU implementation needs this to work
    while (!renderer->device) {
        emscripten_sleep(1);
    }

    // I was trying to use SDL_WaitSemaphore here, but it really doesn't seem to work as expected
    // with emscripten, so I'm using emscripten_sleep instead
    SDL_WaitSemaphore(renderer->adapter_semaphore);

    /*// Set our error callback for emscripten*/
    wgpuDeviceSetUncapturedErrorCallback(renderer->device, WebGPU_ErrorCallback, NULL);

    result = (SDL_GPUDevice *)SDL_malloc(sizeof(SDL_GPUDevice));
    /*
    TODO: Ensure that all function signatures for the driver are correct so that the following line compiles
          This will attach all of the driver's functions to the SDL_GPUDevice struct

          i.e. result->CreateTexture = WebGPU_CreateTexture;
               result->DestroyDevice = WebGPU_DestroyDevice;
               ... etc.
    */
    /*ASSIGN_DRIVER(WebGPU)*/
    result->driverData = (SDL_GPURenderer *)renderer;

    SDL_Log("WebGPU driver created successfully");

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
