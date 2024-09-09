// File: /webgpu/SDL_gpu_webgpu.c
// Author: Kyle Lukaszek
// Email: kylelukaszek at gmail dot com
// License: Zlib
// Description: WebGPU driver for SDL_gpu using the emscripten WebGPU implementation
// Note: Compiling SDL GPU programs using emscripten will require -sUSE_WEBGPU=1

#include "../SDL_sysgpu.h"
#include "SDL_internal.h"
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_stdinc.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <stdint.h>
#include <webgpu/webgpu.h>

#define MAX_UBO_SECTION_SIZE          4096 // 4   KiB
#define DESCRIPTOR_POOL_STARTING_SIZE 128
#define WINDOW_PROPERTY_DATA          "SDL_GPUWebGPUWindowPropertyData"

#define EXPAND_ELEMENTS_IF_NEEDED(arr, initialValue, type) \
    if (arr->count == arr->capacity) {                     \
        if (arr->capacity == 0) {                          \
            arr->capacity = initialValue;                  \
        } else {                                           \
            arr->capacity *= 2;                            \
        }                                                  \
        arr->elements = (type *)SDL_realloc(               \
            arr->elements,                                 \
            arr->capacity * sizeof(type));                 \
    }

#define EXPAND_ARRAY_IF_NEEDED(arr, elementType, newCount, capacity, newCapacity) \
    if (newCount >= capacity) {                                                   \
        capacity = newCapacity;                                                   \
        arr = (elementType *)SDL_realloc(                                         \
            arr,                                                                  \
            sizeof(elementType) * capacity);                                      \
    }

#define MOVE_ARRAY_CONTENTS_AND_RESET(i, dstArr, dstCount, srcArr, srcCount) \
    for (i = 0; i < srcCount; i += 1) {                                      \
        dstArr[i] = srcArr[i];                                               \
    }                                                                        \
    dstCount = srcCount;                                                     \
    srcCount = 0;

// Structures

typedef struct WebGPUBuffer WebGPUBuffer;
typedef struct WebGPUBufferContainer WebGPUBufferContainer;
typedef struct WebGPUTexture WebGPUTexture;
typedef struct WebGPUTextureContainer WebGPUTextureContainer;

// Buffer structures

typedef struct WebGPUBufferHandle
{
    WebGPUBuffer *webgpuBuffer;
    WebGPUBufferContainer *container;
} WebGPUBufferHandle;

typedef enum WebGPUBufferType
{
    WEBGPU_BUFFER_TYPE_GPU,
    WEBGPU_BUFFER_TYPE_UNIFORM,
    WEBGPU_BUFFER_TYPE_TRANSFER
} WebGPUBufferType;

struct WebGPUBuffer
{
    WGPUBuffer buffer;
    uint64_t size;

    WebGPUBufferType type;
    SDL_GPUBufferUsageFlags usageFlags;

    SDL_AtomicInt referenceCount;

    WebGPUBufferHandle *handle;

    bool transitioned;
    Uint8 markedForDestroy;
};

struct WebGPUBufferContainer
{
    WebGPUBufferHandle *activeBufferHandle;

    Uint32 bufferCapacity;
    Uint32 bufferCount;
    WebGPUBufferHandle **bufferHandles;

    char *debugName;
};

// Texture structures
typedef struct WebGPUTextureHandle
{
    WebGPUTexture *webgpuTexture;
    WebGPUTextureContainer *container;
} WebGPUTextureHandle;

typedef struct WebGPUTextureSubresource
{
    WebGPUTexture *parent;
    Uint32 layer;
    Uint32 level;

    WGPUTextureView *renderTargetViews;
    WGPUTextureView computeWriteView;
    WGPUTextureView depthStencilView;

    WebGPUTextureHandle *msaaTexHandle;

    bool transitioned;
} WebGPUTextureSubresource;

struct WebGPUTexture
{
    WGPUTexture texture;
    WGPUTextureView fullView;
    WGPUExtent3D dimensions;

    SDL_GPUTextureType type;
    Uint8 isMSAAColorTarget;

    Uint32 depth;
    Uint32 layerCount;
    Uint32 levelCount;
    WGPUTextureFormat format;
    SDL_GPUTextureUsageFlags usageFlags;

    Uint32 subresourceCount;
    WebGPUTextureSubresource *subresources;

    WebGPUTextureHandle *handle;

    Uint8 markedForDestroy;
    SDL_AtomicInt referenceCount;
};

struct WebGPUTextureContainer
{
    TextureCommonHeader header;

    WebGPUTextureHandle *activeTextureHandle;

    Uint32 textureCapacity;
    Uint32 textureCount;
    WebGPUTextureHandle **textureHandles;

    Uint8 canBeCycled;

    char *debugName;
};

// Swapchain structures

typedef struct SwapchainSupportDetails
{
    // should just call wgpuSurfaceGetPreferredFormat
    WGPUTextureFormat *formats;
    Uint32 formatsLength;
    WGPUPresentMode *presentModes;
    Uint32 presentModesLength;
} SwapchainSupportDetails;

/*typedef struct WebGPUSwapchainData*/
/*{*/
/*    // Surface*/
/*    WGPUSurface surface;*/
/*    // Swapchain for emscripten surface*/
/*    WGPUSwapChain swapchain;*/
/*    WGPUTextureFormat format;*/
/*    WGPUPresentMode presentMode;*/
/*    // Swapchain textures*/
/*    WebGPUTextureContainer *textureContainers;*/
/*    uint32_t textureCount;*/
/*    // Synchronization primitives*/
/*    uint32_t currentTextureIndex;*/
/*    uint32_t frameCounter;*/
/*    // Configuration*/
/*    WGPUSwapChainDescriptor swapchainDesc;*/
/*} WebGPUSwapchainData;*/

typedef struct WebGPUSwapchainData
{
    WGPUSurface surface;
    WGPUSwapChain swapchain;
    WGPUTextureFormat format;
    WGPUPresentMode presentMode;

    WGPUTexture depthStencilTexture;
    WGPUTextureView depthStencilView;

    WGPUTexture msaaTexture;
    WGPUTextureView msaaView;

    uint32_t width;
    uint32_t height;
    uint32_t sampleCount;

    uint32_t frameCounter;
} WebGPUSwapchainData;

typedef struct WindowData
{
    SDL_Window *window;
    SDL_GPUSwapchainComposition swapchainComposition;
    SDL_GPUPresentMode presentMode;
    WebGPUSwapchainData *swapchainData;
    bool needsSwapchainRecreate;
} WindowData;

// Renderer Structure

typedef struct WebGPURenderer WebGPURenderer;

typedef struct WebGPUCommandBuffer
{
    CommandBufferCommonHeader common;
    WebGPURenderer *renderer;

    WGPUCommandEncoder commandEncoder;
    WGPURenderPassEncoder renderPassEncoder;
    WGPUComputePassEncoder computePassEncoder;

    // ... (other fields as needed)

} WebGPUCommandBuffer;

typedef struct WebGPURenderer
{
    bool debugMode;
    bool preferLowPower;

    WGPUInstance instance;
    WGPUAdapter adapter;
    WGPUDevice device;
    WGPUQueue queue;

    WindowData **claimedWindows;
    Uint32 claimedWindowCount;
    Uint32 claimedWindowCapacity;

    SDL_Semaphore *adapter_semaphore;
} WebGPURenderer;

// Simple Error Callback for WebGPU
static void WebGPU_ErrorCallback(WGPUErrorType type, const char *message, void *userdata)
{
    SDL_SetError("WebGPU error: %s", message);
}

// Device Request Callback for when the device is requested from the adapter
static void WebGPU_RequestDeviceCallback(WGPURequestDeviceStatus status, WGPUDevice device, const char *message, void *userdata)
{
    WebGPURenderer *renderer = (WebGPURenderer *)userdata;
    if (status == WGPURequestDeviceStatus_Success) {
        renderer->device = device;
        SDL_SignalSemaphore(renderer->adapter_semaphore);
        SDL_Log("WebGPU device requested successfully");
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Failed to request WebGPU device: %s", message);
    }
}

// Callback for requesting an adapter from the WebGPU instance
// This will then request a device from the adapter once the adapter is successfully requested
static void WebGPU_RequestAdapterCallback(WGPURequestAdapterStatus status, WGPUAdapter adapter, const char *message, void *userdata)
{
    WebGPURenderer *renderer = (WebGPURenderer *)userdata;
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

// Fetch the necessary PropertiesID for the WindowData for a browser window
static WindowData *WebGPU_INTERNAL_FetchWindowData(SDL_Window *window)
{
    SDL_PropertiesID properties = SDL_GetWindowProperties(window);
    return (WindowData *)SDL_GetPointerProperty(properties, WINDOW_PROPERTY_DATA, NULL);
}

// Callback for when the window is resized
static SDL_bool WebGPU_INTERNAL_OnWindowResize(void *userdata, SDL_Event *event)
{
    SDL_Window *window = (SDL_Window *)userdata;
    WindowData *windowData = WebGPU_INTERNAL_FetchWindowData(window);
    if (windowData) {
        windowData->needsSwapchainRecreate = true;
    }

    SDL_Log("Window resized, recreating swapchain");

    return true;
}

static EM_BOOL emsc_window_resize_callback(int eventType, const EmscriptenUiEvent *uiEvent, void *userData)
{
    (void)eventType;
    (void)uiEvent;
    SDL_bool result = WebGPU_INTERNAL_OnWindowResize(userData, NULL);
    return (EM_BOOL)result;
}

static SDL_GPUTextureFormat SwapchainCompositionToSDLFormat(
    SDL_GPUSwapchainComposition composition,
    bool usingFallback)
{
    switch (composition) {
    case SDL_GPU_SWAPCHAINCOMPOSITION_SDR:
        return usingFallback ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM : SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    case SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR:
        return usingFallback ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB : SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
    case SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR:
        return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    case SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2048:
        return SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM;
    default:
        return SDL_GPU_TEXTUREFORMAT_INVALID;
    }
}

static WGPUPresentMode
SDLToWGPUPresentMode(SDL_GPUPresentMode presentMode)
{
    switch (presentMode) {
    case SDL_GPU_PRESENTMODE_IMMEDIATE:
        SDL_Log("WebGPU: Immediate present mode.");
        return WGPUPresentMode_Immediate;
    case SDL_GPU_PRESENTMODE_MAILBOX:
        SDL_Log("WebGPU: Mailbox present mode.");
        return WGPUPresentMode_Mailbox;
    case SDL_GPU_PRESENTMODE_VSYNC:
        SDL_Log("WebGPU: VSYNC/FIFO present mode.");
        return WGPUPresentMode_Fifo;
    default:
        SDL_Log("WebGPU: Defaulting to VSYNC/FIFO present mode.");
        return WGPUPresentMode_Fifo;
    }
}

SDL_GPUCommandBuffer *WebGPU_AcquireCommandBuffer(SDL_GPURenderer *driverData)
{
    WebGPURenderer *renderer = (WebGPURenderer *)driverData;
    WebGPUCommandBuffer *commandBuffer = SDL_malloc(sizeof(WebGPUCommandBuffer));
    memset(commandBuffer, '\0', sizeof(WebGPUCommandBuffer));
    commandBuffer->renderer = renderer;

    WGPUCommandEncoderDescriptor commandEncoderDesc = {
        .label = "SDL_GPU Command Encoder",
    };

    commandBuffer->commandEncoder = wgpuDeviceCreateCommandEncoder(renderer->device, &commandEncoderDesc);

    return (SDL_GPUCommandBuffer *)commandBuffer;
}

void WebGPU_Submit(SDL_GPUCommandBuffer *commandBuffer)
{
    WebGPUCommandBuffer *webgpuCommandBuffer = (WebGPUCommandBuffer *)commandBuffer;
    WebGPURenderer *renderer = webgpuCommandBuffer->renderer;

    WGPUCommandBufferDescriptor commandBufferDesc = {
        .label = "SDL_GPU Command Buffer",
    };

    WGPUCommandBuffer commandHandle = wgpuCommandEncoderFinish(webgpuCommandBuffer->commandEncoder, &commandBufferDesc);
    if (!commandHandle) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Failed to finish command buffer");
        return;
    }
    wgpuQueueSubmit(renderer->queue, 1, &commandHandle);

    // Release the actual command buffer followed by the SDL command buffer
    wgpuCommandBufferRelease(commandHandle);
    wgpuCommandEncoderRelease(webgpuCommandBuffer->commandEncoder);
    SDL_free(webgpuCommandBuffer);
}

static WGPULoadOp SDLToWGPULoadOp(SDL_GPULoadOp loadOp)
{
    switch (loadOp) {
    case SDL_GPU_LOADOP_LOAD:
        return WGPULoadOp_Load;
    case SDL_GPU_LOADOP_CLEAR:
        return WGPULoadOp_Clear;
    default:
        return WGPULoadOp_Clear;
    }
}

static WGPUStoreOp SDLToWGPUStoreOp(SDL_GPUStoreOp storeOp)
{
    switch (storeOp) {
    case SDL_GPU_STOREOP_STORE:
        return WGPUStoreOp_Store;
    case SDL_GPU_STOREOP_DONT_CARE:
        return WGPUStoreOp_Discard;
    default:
        return WGPUStoreOp_Store;
    }
}

void WebGPU_BeginRenderPass(SDL_GPUCommandBuffer *commandBuffer,
                            SDL_GPUColorAttachmentInfo *colorAttachmentInfos,
                            Uint32 colorAttachmentCount,
                            SDL_GPUDepthStencilAttachmentInfo *depthStencilAttachmentInfo)
{
    WebGPUCommandBuffer *webgpuCommandBuffer = (WebGPUCommandBuffer *)commandBuffer;
    WebGPUTexture *texture = NULL;

    if (colorAttachmentCount == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "No color attachments provided for render pass");
        return;
    }

    // Build WGPU color attachments from the SDL color attachment structs
    WGPURenderPassColorAttachment *colorAttachments = SDL_malloc(sizeof(WGPURenderPassColorAttachment) * colorAttachmentCount);
    for (Uint32 i = 0; i < colorAttachmentCount; i += 1) {
        texture = (WebGPUTexture *)colorAttachmentInfos[i].texture;
        colorAttachments[i] = (WGPURenderPassColorAttachment){
            .view = texture->fullView,
            .depthSlice = ~0u,
            .loadOp = SDLToWGPULoadOp(colorAttachmentInfos[i].loadOp),
            .storeOp = SDLToWGPUStoreOp(colorAttachmentInfos[i].storeOp),
            .clearValue = (WGPUColor){
                .r = colorAttachmentInfos[i].clearColor.r,
                .g = colorAttachmentInfos[i].clearColor.g,
                .b = colorAttachmentInfos[i].clearColor.b,
                .a = colorAttachmentInfos[i].clearColor.a,
            },
        };

        // If we have an MSAA texture, we need to make sure the resolve target is not NULL
        if (texture->isMSAAColorTarget) {
            colorAttachments[i].resolveTarget = texture->fullView;
        }
    }

    // Set color attachments for the render pass
    WGPURenderPassDescriptor renderPassDesc = {
        .colorAttachmentCount = colorAttachmentCount,
        .colorAttachments = colorAttachments,
    };

    // Set depth stencil attachment if provided
    if (depthStencilAttachmentInfo != NULL) {
        // Get depth texture as WebGPUTexture
        texture = (WebGPUTexture *)depthStencilAttachmentInfo->texture;
        WGPURenderPassDepthStencilAttachment depthStencilAttachment = {
            .view = texture->fullView,
            .depthLoadOp = SDLToWGPULoadOp(depthStencilAttachmentInfo->stencilLoadOp),
            .depthStoreOp = SDLToWGPUStoreOp(depthStencilAttachmentInfo->stencilStoreOp),
            .depthClearValue = depthStencilAttachmentInfo->depthStencilClearValue.depth,
            .stencilLoadOp = SDLToWGPULoadOp(depthStencilAttachmentInfo->stencilLoadOp),
            .stencilStoreOp = SDLToWGPUStoreOp(depthStencilAttachmentInfo->stencilStoreOp),
            .stencilClearValue = depthStencilAttachmentInfo->depthStencilClearValue.stencil,
        };

        renderPassDesc.depthStencilAttachment = &depthStencilAttachment;
    }

    // Begin the render pass
    webgpuCommandBuffer->renderPassEncoder = wgpuCommandEncoderBeginRenderPass(webgpuCommandBuffer->commandEncoder, &renderPassDesc);
}

void WebGPU_EndRenderPass(SDL_GPUCommandBuffer *commandBuffer)
{
    WebGPUCommandBuffer *webgpuCommandBuffer = (WebGPUCommandBuffer *)commandBuffer;
    wgpuRenderPassEncoderEnd(webgpuCommandBuffer->renderPassEncoder);
    wgpuRenderPassEncoderRelease(webgpuCommandBuffer->renderPassEncoder);
}

static bool WebGPU_CreateSurface(WebGPURenderer *renderer, WindowData *windowData)
{
    WGPUSurfaceDescriptorFromCanvasHTMLSelector canvas_desc = {
        .chain.sType = WGPUSType_SurfaceDescriptorFromCanvasHTMLSelector,
        .selector = "#canvas",
    };
    WGPUSurfaceDescriptor surf_desc = {
        .nextInChain = &canvas_desc.chain,
    };
    windowData->swapchainData->surface = wgpuInstanceCreateSurface(renderer->instance, &surf_desc);
    return windowData->swapchainData->surface != NULL;
}

static void WebGPU_CreateSwapchain(WebGPURenderer *renderer, WindowData *windowData)
{
    windowData->swapchainData = SDL_calloc(1, sizeof(WebGPUSwapchainData));

    SDL_assert(WebGPU_CreateSurface(renderer, windowData));

    WebGPUSwapchainData *swapchainData = windowData->swapchainData;
    swapchainData->format = wgpuSurfaceGetPreferredFormat(swapchainData->surface, renderer->adapter);
    swapchainData->presentMode = SDLToWGPUPresentMode(windowData->presentMode);

    // Swapchain should be the size of whatever SDL_Window it is attached to
    swapchainData->width = windowData->window->w;
    swapchainData->height = windowData->window->h;

    // Emscripten WebGPU swapchain
    WGPUSwapChainDescriptor swapchainDesc = {
        .usage = WGPUTextureUsage_RenderAttachment,
        .format = swapchainData->format,
        .width = swapchainData->width,
        .height = swapchainData->height,
        .presentMode = swapchainData->presentMode,
    };

    swapchainData->swapchain = wgpuDeviceCreateSwapChain(renderer->device, swapchainData->surface, &swapchainDesc);

    // Depth/stencil texture for swapchain
    WGPUTextureDescriptor depthDesc = {
        .usage = WGPUTextureUsage_RenderAttachment,
        .dimension = WGPUTextureDimension_2D,
        .size = {
            .width = swapchainData->width,
            .height = swapchainData->height,
            .depthOrArrayLayers = 1,
        },
        .format = WGPUTextureFormat_Depth32FloatStencil8,
        .mipLevelCount = 1,
        .sampleCount = swapchainData->sampleCount,
    };
    swapchainData->depthStencilTexture = wgpuDeviceCreateTexture(renderer->device, &depthDesc);
    swapchainData->depthStencilView = wgpuTextureCreateView(swapchainData->depthStencilTexture, NULL);

    // MSAA texture for swapchain
    if (swapchainData->sampleCount > 1) {
        WGPUTextureDescriptor msaaDesc = {
            .usage = WGPUTextureUsage_RenderAttachment,
            .dimension = WGPUTextureDimension_2D,
            .size = {
                .width = swapchainData->width,
                .height = swapchainData->height,
                .depthOrArrayLayers = 1,
            },
            .format = swapchainData->format,
            .mipLevelCount = 1,
            .sampleCount = swapchainData->sampleCount,
        };
        swapchainData->msaaTexture = wgpuDeviceCreateTexture(renderer->device, &msaaDesc);
        swapchainData->msaaView = wgpuTextureCreateView(swapchainData->msaaTexture, NULL);
    }

    SDL_Log("WebGPU: Created swapchain %p of size %dx%d", swapchainData->swapchain, swapchainData->width, swapchainData->height);
}

static void WebGPU_DestroySwapchain(WebGPUSwapchainData *swapchainData)
{
    if (swapchainData->msaaView) {
        wgpuTextureViewRelease(swapchainData->msaaView);
        swapchainData->msaaView = NULL;
    }
    if (swapchainData->msaaTexture) {
        wgpuTextureRelease(swapchainData->msaaTexture);
        swapchainData->msaaTexture = NULL;
    }
    if (swapchainData->depthStencilView) {
        wgpuTextureViewRelease(swapchainData->depthStencilView);
        swapchainData->depthStencilView = NULL;
    }
    if (swapchainData->depthStencilTexture) {
        wgpuTextureRelease(swapchainData->depthStencilTexture);
        swapchainData->depthStencilTexture = NULL;
    }
    if (swapchainData->swapchain) {
        wgpuSwapChainRelease(swapchainData->swapchain);
        swapchainData->swapchain = NULL;
    }
    if (swapchainData->surface) {
        wgpuSurfaceRelease(swapchainData->surface);
        swapchainData->surface = NULL;
    }
}

static void WebGPU_RecreateSwapchain(WebGPURenderer *renderer, WindowData *windowData)
{
    WebGPU_DestroySwapchain(windowData->swapchainData);
    WebGPU_CreateSwapchain(renderer, windowData);
    windowData->needsSwapchainRecreate = false;
}

static SDL_GPUTexture *WebGPU_AcquireSwapchainTexture(
    SDL_GPUCommandBuffer *commandBuffer,
    SDL_Window *window,
    Uint32 *pWidth,
    Uint32 *pHeight)
{
    WebGPUCommandBuffer *webgpuCommandBuffer = (WebGPUCommandBuffer *)commandBuffer;
    WebGPURenderer *renderer = webgpuCommandBuffer->renderer;
    WindowData *windowData = WebGPU_INTERNAL_FetchWindowData(window);
    WebGPUSwapchainData *swapchainData = windowData->swapchainData;

    // Check if the swapchain needs to be recreated
    if (windowData->needsSwapchainRecreate) {
        WebGPU_RecreateSwapchain(renderer, windowData);
        swapchainData = windowData->swapchainData;

        if (swapchainData == NULL) {
            SDL_SetError("Failed to recreate swapchain");
            return NULL;
        }
    }

    // Get the current texture view from the swapchain
    WGPUTextureView currentView = wgpuSwapChainGetCurrentTextureView(swapchainData->swapchain);
    if (currentView == NULL) {
        SDL_SetError("Failed to acquire texture view from swapchain");
        return NULL;
    }

    // Create a temporary WebGPUTexture to return
    WebGPUTexture *texture = SDL_calloc(1, sizeof(WebGPUTexture));
    if (!texture) {
        SDL_OutOfMemory();
        return NULL;
    }

    texture->texture = wgpuSwapChainGetCurrentTexture(swapchainData->swapchain);
    texture->fullView = currentView;
    texture->dimensions = (WGPUExtent3D){
        .width = swapchainData->width,
        .height = swapchainData->height,
        .depthOrArrayLayers = 1,
    };
    texture->type = SDL_GPU_TEXTURETYPE_2D;
    texture->isMSAAColorTarget = swapchainData->sampleCount > 1;
    texture->format = swapchainData->format;
    texture->usageFlags = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;

    // For MSAA, we'll return the MSAA texture instead of the swapchain texture
    if (swapchainData->sampleCount > 1) {
        texture->texture = swapchainData->msaaTexture;
        texture->fullView = swapchainData->msaaView;
    }

    // Set the width and height if provided
    if (pWidth) {
        *pWidth = swapchainData->width;
    }
    if (pHeight) {
        *pHeight = swapchainData->height;
    }

    // It is important to release these textures when they are no longer needed
    return (SDL_GPUTexture *)texture;
}

static bool WebGPU_ClaimWindow(
    SDL_GPURenderer *driverData,
    SDL_Window *window)
{
    WebGPURenderer *renderer = (WebGPURenderer *)driverData;
    WindowData *windowData = WebGPU_INTERNAL_FetchWindowData(window);

    if (windowData == NULL) {
        windowData = SDL_malloc(sizeof(WindowData));
        windowData->window = window;
        windowData->presentMode = SDL_GPU_PRESENTMODE_VSYNC;
        windowData->swapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;

        WebGPU_CreateSwapchain(renderer, windowData);

        if (windowData->swapchainData != NULL) {
            SDL_SetPointerProperty(SDL_GetWindowProperties(window), WINDOW_PROPERTY_DATA, windowData);

            if (renderer->claimedWindowCount >= renderer->claimedWindowCapacity) {
                renderer->claimedWindowCapacity *= 2;
                renderer->claimedWindows = SDL_realloc(
                    renderer->claimedWindows,
                    renderer->claimedWindowCapacity * sizeof(WindowData *));
            }

            renderer->claimedWindows[renderer->claimedWindowCount] = windowData;
            renderer->claimedWindowCount += 1;

            /*SDL_AddEventWatch(WebGPU_INTERNAL_OnWindowResize, window);*/
            emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, windowData, true, emsc_window_resize_callback);

            return true;
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_GPU, "Could not create swapchain, failed to claim window!");
            SDL_free(windowData);
            return 0;
        }
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_GPU, "Window already claimed!");
        return 0;
    }
}

static bool WebGPU_PrepareDriver(SDL_VideoDevice *_this)
{
    // Realistically, we should check if the browser supports WebGPU here and return false if it doesn't
    // For now, we'll just return true because it'll simply crash if the browser doesn't support WebGPU anyways
    return true;
}

static SDL_GPUDevice *WebGPU_CreateDevice(SDL_bool debug, bool preferLowPower, SDL_PropertiesID props)
{
    WebGPURenderer *renderer;
    SDL_GPUDevice *result = NULL;

    // Allocate memory for the renderer and device
    renderer = (WebGPURenderer *)SDL_malloc(sizeof(WebGPURenderer));
    memset(renderer, '\0', sizeof(WebGPURenderer));
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

    WGPURequestAdapterOptions adapter_options = {
        .powerPreference = WGPUPowerPreference_HighPerformance,
        .backendType = WGPUBackendType_WebGPU
    };

    // Request adapter using the instance and then the device using the adapter (this is done in the callback)
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
    wgpuDeviceSetUncapturedErrorCallback(renderer->device, WebGPU_ErrorCallback, 0);
    wgpuDevicePushErrorScope(renderer->device, WGPUErrorFilter_Validation);

    // Acquire the queue from the device
    renderer->queue = wgpuDeviceGetQueue(renderer->device);

    result = (SDL_GPUDevice *)SDL_malloc(sizeof(SDL_GPUDevice));
    /*
    TODO: Ensure that all function signatures for the driver are correct so that the following line compiles
          This will attach all of the driver's functions to the SDL_GPUDevice struct

          i.e. result->CreateTexture = WebGPU_CreateTexture;
               result->DestroyDevice = WebGPU_DestroyDevice;
               ... etc.
    */
    /*ASSIGN_DRIVER(WebGPU)*/
    result->ClaimWindow = WebGPU_ClaimWindow;
    result->driverData = (SDL_GPURenderer *)renderer;
    result->AcquireCommandBuffer = WebGPU_AcquireCommandBuffer;
    result->AcquireSwapchainTexture = WebGPU_AcquireSwapchainTexture;
    result->Submit = WebGPU_Submit;
    result->BeginRenderPass = WebGPU_BeginRenderPass;
    result->EndRenderPass = WebGPU_EndRenderPass;

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
