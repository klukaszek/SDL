// File: /webgpu/SDL_gpu_webgpu.c
// Author: Kyle Lukaszek
// Email: kylelukaszek [at] gmail [dot] com
// License: Zlib
// Description: WebGPU driver for SDL_gpu using the emscripten WebGPU implementation
// Note: Compiling SDL GPU programs using emscripten will require -sUSE_WEBGPU=1 -sASYNCIFY=1

#include "../SDL_sysgpu.h"
#include "SDL_internal.h"
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_oldnames.h>
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

#define TRACK_RESOURCE(resource, type, array, count, capacity) \
    Uint32 i;                                                  \
                                                               \
    for (i = 0; i < commandBuffer->count; i += 1) {            \
        if (commandBuffer->array[i] == resource) {             \
            return;                                            \
        }                                                      \
    }                                                          \
                                                               \
    if (commandBuffer->count == commandBuffer->capacity) {     \
        commandBuffer->capacity += 1;                          \
        commandBuffer->array = SDL_realloc(                    \
            commandBuffer->array,                              \
            commandBuffer->capacity * sizeof(type));           \
    }                                                          \
    commandBuffer->array[commandBuffer->count] = resource;     \
    commandBuffer->count += 1;                                 \
    SDL_AtomicIncRef(&resource->referenceCount);

// Enums
typedef enum WebGPUBindingType
{
    WGPUBindingType_Undefined = 0x00000000,
    WGPUBindingType_Buffer = 0x00000001,
    WGPUBindingType_Sampler = 0x00000002,
    WGPUBindingType_Texture = 0x00000003,
    WGPUBindingType_StorageTexture = 0x00000004,

    // Buffer sub-types
    WGPUBindingType_UniformBuffer = 0x00000011,
    WGPUBindingType_StorageBuffer = 0x00000012,
    WGPUBindingType_ReadOnlyStorageBuffer = 0x00000013,

    // Sampler sub-types
    WGPUBindingType_FilteringSampler = 0x00000022,
    WGPUBindingType_NonFilteringSampler = 0x00000023,
    WGPUBindingType_ComparisonSampler = 0x00000024,

    // Texture sub-types
    WGPUBindingType_FilteringTexture = 0x00000033,
    WGPUBindingType_NonFilteringTexture = 0x00000034,

    // Storage Texture sub-types (access modes)
    WGPUBindingType_WriteOnlyStorageTexture = 0x00000044,
    WGPUBindingType_ReadOnlyStorageTexture = 0x00000045,
    WGPUBindingType_ReadWriteStorageTexture = 0x00000046,
} WebGPUBindingType;

typedef enum WebGPUBufferType
{
    WEBGPU_BUFFER_TYPE_GPU,
    WEBGPU_BUFFER_TYPE_UNIFORM,
    WEBGPU_BUFFER_TYPE_TRANSFER
} WebGPUBufferType;

// Shader Reflection Definitions:
// ---------------------------------------------------
typedef struct ReflectedBinding
{
    uint32_t set;
    uint32_t binding;
    WebGPUBindingType type;
    WGPUShaderStageFlags stages;
} ReflectedBinding;

typedef struct ReflectedBindGroup
{
    ReflectedBinding *bindings;
    uint32_t bindingCount;
} ReflectedBindGroup;

// WebGPU Structures
// ---------------------------------------------------
typedef struct WebGPUBuffer WebGPUBuffer;
typedef struct WebGPUBufferContainer WebGPUBufferContainer;
typedef struct WebGPUTexture WebGPUTexture;
typedef struct WebGPUTextureContainer WebGPUTextureContainer;

typedef struct WebGPUShader
{
    char *wgslSource;
    WGPUShaderModule shaderModule;
    const char *entrypoint;
    Uint32 samplerCount;
    Uint32 storageTextureCount;
    Uint32 storageBufferCount;
    Uint32 uniformBufferCount;
    SDL_AtomicInt referenceCount;
} WebGPUShader;

typedef struct WebGPUViewport
{
    float x;
    float y;
    float width;
    float height;
    float minDepth;
    float maxDepth;
} WebGPUViewport;

typedef struct WebGPURect
{
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} WebGPURect;

// Buffer structures

#define MAX_BIND_GROUPS 4

typedef struct WebGPUBufferHandle
{
    WebGPUBuffer *webgpuBuffer;
    WebGPUBufferContainer *container;
} WebGPUBufferHandle;

typedef struct WebGPUBuffer
{
    WGPUBuffer buffer;
    uint64_t size;
    WebGPUBufferType type;
    SDL_GPUBufferUsageFlags usageFlags;
    SDL_AtomicInt referenceCount;
    WebGPUBufferHandle *handle;
    bool transitioned;
    Uint8 markedForDestroy;
    bool isMapped;
    void *mappedData;
    SDL_AtomicInt mappingComplete;
} WebGPUBuffer;

typedef struct WebGPUBufferContainer
{
    WebGPUBufferHandle *activeBufferHandle;
    Uint32 bufferCapacity;
    Uint32 bufferCount;
    WebGPUBufferHandle **bufferHandles;
    char *debugName;
} WebGPUBufferContainer;

// Bind Group definitions

typedef struct WebGPUBindGroupEntry
{
    WebGPUBindingType type;
    union
    {
        struct
        {
            WebGPUBufferHandle *bufferHandle;
            uint32_t offset;
            uint32_t size;
        } buffer;
        WGPUTextureView textureView;
        WGPUSampler sampler;
    };
} WebGPUBindGroupEntry;

typedef struct WebGPUBindGroup
{
    WGPUBindGroup bindGroup;
    WebGPUBindGroupEntry *entries;
    uint32_t entryCount;
} WebGPUBindGroup;

typedef struct WebGPUBindGroupLayout
{
    WGPUBindGroupLayout layout;
    WGPUShaderStageFlags *stageFlags;
    WebGPUBindingType *types;
    uint32_t entryCount;
} WebGPUBindGroupLayout;

// Graphics Pipeline definitions
typedef struct WebGPUPipelineResourceLayout
{
    WGPUPipelineLayout pipelineLayout;
    WebGPUBindGroupLayout bindGroupLayouts[MAX_BIND_GROUPS];
    uint32_t bindGroupLayoutCount;
} WebGPUPipelineResourceLayout;

typedef struct WebGPUGraphicsPipeline
{
    WGPURenderPipeline pipeline;
    SDL_GPUPrimitiveType primitiveType;
    WebGPUPipelineResourceLayout *resourceLayout;
    WebGPUShader *vertexShader;
    WebGPUShader *fragmentShader;
    WGPURenderPipelineDescriptor pipelineDesc;
    SDL_AtomicInt referenceCount;
} WebGPUGraphicsPipeline;

typedef struct WebGPUPipelineResources
{
    WebGPUBindGroup bindGroups[MAX_BIND_GROUPS];
    uint32_t bindGroupCount;
} WebGPUPipelineResources;

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
    SDL_GPUTextureUsageFlags usage;

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

// Renderer's command buffer structure
typedef struct WebGPUCommandBuffer
{
    CommandBufferCommonHeader common;
    WebGPURenderer *renderer;

    WGPUCommandEncoder commandEncoder;
    WGPURenderPassEncoder renderPassEncoder;
    WGPUComputePassEncoder computePassEncoder;

    // WebGPUComputePipeline *currentComputePipeline;
    WebGPUGraphicsPipeline *currentGraphicsPipeline;

    WebGPUPipelineResources currentResources;
    bool resourcesDirty;

    WebGPUViewport currentViewport;
    WebGPURect currentScissor;

    WebGPUGraphicsPipeline **usedGraphicsPipelines;
    Uint32 usedGraphicsPipelineCount;
    Uint32 usedGraphicsPipelineCapacity;

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
} WebGPURenderer;

// ---------------------------------------------------

// Conversion Functions:
// ---------------------------------------------------

static WGPUBufferUsageFlags SDLToWGPUBufferUsageFlags(SDL_GPUBufferUsageFlags usageFlags)
{
    WGPUBufferUsageFlags wgpuFlags = WGPUBufferUsage_None;
    if (usageFlags & SDL_GPU_BUFFERUSAGE_VERTEX)
        wgpuFlags |= WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
    if (usageFlags & SDL_GPU_BUFFERUSAGE_INDEX)
        wgpuFlags |= WGPUBufferUsage_Index;
    if (usageFlags & SDL_GPU_BUFFERUSAGE_INDIRECT)
        wgpuFlags |= WGPUBufferUsage_Indirect;
    return wgpuFlags;
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

static WGPUPrimitiveTopology SDLToWGPUPrimitiveTopology(SDL_GPUPrimitiveType topology)
{
    switch (topology) {
    case SDL_GPU_PRIMITIVETYPE_POINTLIST:
        return WGPUPrimitiveTopology_PointList;
    case SDL_GPU_PRIMITIVETYPE_LINELIST:
        return WGPUPrimitiveTopology_LineList;
    case SDL_GPU_PRIMITIVETYPE_LINESTRIP:
        return WGPUPrimitiveTopology_LineStrip;
    case SDL_GPU_PRIMITIVETYPE_TRIANGLELIST:
        return WGPUPrimitiveTopology_TriangleList;
    case SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP:
        return WGPUPrimitiveTopology_TriangleStrip;
    default:
        SDL_Log("SDL_GPU: Invalid primitive type %d", topology);
        return WGPUPrimitiveTopology_TriangleList;
    }
}

static WGPUFrontFace SDLToWGPUFrontFace(SDL_GPUFrontFace frontFace)
{
    switch (frontFace) {
    case SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE:
        return WGPUFrontFace_CCW;
    case SDL_GPU_FRONTFACE_CLOCKWISE:
        return WGPUFrontFace_CW;
    default:
        SDL_Log("SDL_GPU: Invalid front face %d. Using CCW.", frontFace);
        return WGPUFrontFace_CCW;
    }
}

static WGPUCullMode SDLToWGPUCullMode(SDL_GPUCullMode cullMode)
{
    switch (cullMode) {
    case SDL_GPU_CULLMODE_NONE:
        return WGPUCullMode_None;
    case SDL_GPU_CULLMODE_FRONT:
        return WGPUCullMode_Front;
    case SDL_GPU_CULLMODE_BACK:
        return WGPUCullMode_Back;
    default:
        SDL_Log("SDL_GPU: Invalid cull mode %d. Using None.", cullMode);
        return WGPUCullMode_None;
    }
}

static WGPUTextureFormat SDLToWGPUTextureFormat(SDL_GPUTextureFormat sdlFormat)
{
    switch (sdlFormat) {
    case SDL_GPU_TEXTUREFORMAT_R8_UNORM:
        return WGPUTextureFormat_R8Unorm;
    case SDL_GPU_TEXTUREFORMAT_R8G8_UNORM:
        return WGPUTextureFormat_RG8Unorm;
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM:
        return WGPUTextureFormat_RGBA8Unorm;
    case SDL_GPU_TEXTUREFORMAT_R16_UNORM:
        return WGPUTextureFormat_R16Uint; // Note: WebGPU doesn't have R16Unorm
    case SDL_GPU_TEXTUREFORMAT_R16G16_UNORM:
        return WGPUTextureFormat_RG16Uint; // Note: WebGPU doesn't have RG16Unorm
    case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM:
        return WGPUTextureFormat_RGBA16Uint; // Note: WebGPU doesn't have RGBA16Unorm
    case SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM:
        return WGPUTextureFormat_RGB10A2Unorm;
    case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM:
        return WGPUTextureFormat_BGRA8Unorm;
    case SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM:
        return WGPUTextureFormat_BC1RGBAUnorm;
    case SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM:
        return WGPUTextureFormat_BC2RGBAUnorm;
    case SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM:
        return WGPUTextureFormat_BC3RGBAUnorm;
    case SDL_GPU_TEXTUREFORMAT_BC4_R_UNORM:
        return WGPUTextureFormat_BC4RUnorm;
    case SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM:
        return WGPUTextureFormat_BC5RGUnorm;
    case SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM:
        return WGPUTextureFormat_BC7RGBAUnorm;
    case SDL_GPU_TEXTUREFORMAT_BC6H_RGB_FLOAT:
        return WGPUTextureFormat_BC6HRGBFloat;
    case SDL_GPU_TEXTUREFORMAT_BC6H_RGB_UFLOAT:
        return WGPUTextureFormat_BC6HRGBUfloat;
    case SDL_GPU_TEXTUREFORMAT_R8_SNORM:
        return WGPUTextureFormat_R8Snorm;
    case SDL_GPU_TEXTUREFORMAT_R8G8_SNORM:
        return WGPUTextureFormat_RG8Snorm;
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_SNORM:
        return WGPUTextureFormat_RGBA8Snorm;
    case SDL_GPU_TEXTUREFORMAT_R16_FLOAT:
        return WGPUTextureFormat_R16Float;
    case SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT:
        return WGPUTextureFormat_RG16Float;
    case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT:
        return WGPUTextureFormat_RGBA16Float;
    case SDL_GPU_TEXTUREFORMAT_R32_FLOAT:
        return WGPUTextureFormat_R32Float;
    case SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT:
        return WGPUTextureFormat_RG32Float;
    case SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT:
        return WGPUTextureFormat_RGBA32Float;
    case SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT:
        return WGPUTextureFormat_RG11B10Ufloat;
    case SDL_GPU_TEXTUREFORMAT_R8_UINT:
        return WGPUTextureFormat_R8Uint;
    case SDL_GPU_TEXTUREFORMAT_R8G8_UINT:
        return WGPUTextureFormat_RG8Uint;
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UINT:
        return WGPUTextureFormat_RGBA8Uint;
    case SDL_GPU_TEXTUREFORMAT_R16_UINT:
        return WGPUTextureFormat_R16Uint;
    case SDL_GPU_TEXTUREFORMAT_R16G16_UINT:
        return WGPUTextureFormat_RG16Uint;
    case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UINT:
        return WGPUTextureFormat_RGBA16Uint;
    case SDL_GPU_TEXTUREFORMAT_R8_INT:
        return WGPUTextureFormat_R8Sint;
    case SDL_GPU_TEXTUREFORMAT_R8G8_INT:
        return WGPUTextureFormat_RG8Sint;
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_INT:
        return WGPUTextureFormat_RGBA8Sint;
    case SDL_GPU_TEXTUREFORMAT_R16_INT:
        return WGPUTextureFormat_R16Sint;
    case SDL_GPU_TEXTUREFORMAT_R16G16_INT:
        return WGPUTextureFormat_RG16Sint;
    case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_INT:
        return WGPUTextureFormat_RGBA16Sint;
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB:
        return WGPUTextureFormat_RGBA8UnormSrgb;
    case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB:
        return WGPUTextureFormat_BGRA8UnormSrgb;
    case SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM_SRGB:
        return WGPUTextureFormat_BC1RGBAUnormSrgb;
    case SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM_SRGB:
        return WGPUTextureFormat_BC2RGBAUnormSrgb;
    case SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM_SRGB:
        return WGPUTextureFormat_BC3RGBAUnormSrgb;
    case SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM_SRGB:
        return WGPUTextureFormat_BC7RGBAUnormSrgb;
    case SDL_GPU_TEXTUREFORMAT_D16_UNORM:
        return WGPUTextureFormat_Depth16Unorm;
    case SDL_GPU_TEXTUREFORMAT_D24_UNORM:
        return WGPUTextureFormat_Depth24Plus;
    case SDL_GPU_TEXTUREFORMAT_D32_FLOAT:
        return WGPUTextureFormat_Depth32Float;
    case SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT:
        return WGPUTextureFormat_Depth24PlusStencil8;
    case SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT:
        return WGPUTextureFormat_Depth32FloatStencil8;
    default:
        return WGPUTextureFormat_Undefined;
    }
}

static WGPUTextureUsageFlags SDLToWGPUTextureUsageFlags(SDL_GPUTextureUsageFlags usageFlags)
{
    WGPUTextureUsageFlags wgpuFlags;
    switch (usageFlags) {
    case SDL_GPU_TEXTUREUSAGE_SAMPLER:
        wgpuFlags = WGPUTextureUsage_TextureBinding;
        break;
    case SDL_GPU_TEXTUREUSAGE_COLOR_TARGET:
    case SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET:
        wgpuFlags = WGPUTextureUsage_RenderAttachment;
        break;
    case SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ:
        wgpuFlags = WGPUTextureUsage_StorageBinding;
        break;
    case SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ:
        wgpuFlags = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_CopySrc;
        break;
    case SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE:
        wgpuFlags = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_CopyDst;
        break;
    case SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE:
        wgpuFlags = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst;
        break;
    default:
        wgpuFlags = WGPUTextureUsage_None;
        break;
    }

    return wgpuFlags;
}

static WGPUTextureDimension SDLToWGPUTextureDimension(SDL_GPUTextureType type)
{
    switch (type) {
    case SDL_GPU_TEXTURETYPE_2D:
    case SDL_GPU_TEXTURETYPE_2D_ARRAY:
    // Cubemaps in WebGPU are treated as 2D textures so we set the dimension to 2D
    case SDL_GPU_TEXTURETYPE_CUBE:
    case SDL_GPU_TEXTURETYPE_CUBE_ARRAY:
        return WGPUTextureDimension_2D;
    case SDL_GPU_TEXTURETYPE_3D:
        return WGPUTextureDimension_3D;
    default:
        SDL_Log("SDL_GPU: Invalid texture type %d. Using 2D.", type);
        return WGPUTextureDimension_2D;
    }
}

static WGPUTextureViewDimension SDLToWGPUTextureViewDimension(SDL_GPUTextureType type)
{
    switch (type) {
    case SDL_GPU_TEXTURETYPE_2D:
        return WGPUTextureViewDimension_2D;
    case SDL_GPU_TEXTURETYPE_2D_ARRAY:
        return WGPUTextureViewDimension_2DArray;
    case SDL_GPU_TEXTURETYPE_CUBE:
        return WGPUTextureViewDimension_Cube;
    case SDL_GPU_TEXTURETYPE_CUBE_ARRAY:
        return WGPUTextureViewDimension_CubeArray;
    case SDL_GPU_TEXTURETYPE_3D:
        return WGPUTextureViewDimension_3D;
    default:
        SDL_Log("SDL_GPU: Invalid texture type %d. Using 2D.", type);
        return WGPUTextureViewDimension_2D;
    }
}

static SDL_GPUTextureFormat WGPUToSDLTextureFormat(WGPUTextureFormat wgpuFormat)
{
    switch (wgpuFormat) {
    case WGPUTextureFormat_R8Unorm:
        return SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    case WGPUTextureFormat_RG8Unorm:
        return SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
    case WGPUTextureFormat_RGBA8Unorm:
        return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    case WGPUTextureFormat_R16Uint:
        return SDL_GPU_TEXTUREFORMAT_R16_UINT; // Note: This is an approximation
    case WGPUTextureFormat_RG16Uint:
        return SDL_GPU_TEXTUREFORMAT_R16G16_UINT; // Note: This is an approximation
    case WGPUTextureFormat_RGBA16Uint:
        return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UINT; // Note: This is an approximation
    case WGPUTextureFormat_RGB10A2Unorm:
        return SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM;
    case WGPUTextureFormat_BGRA8Unorm:
        return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    case WGPUTextureFormat_BC1RGBAUnorm:
        return SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
    case WGPUTextureFormat_BC2RGBAUnorm:
        return SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM;
    case WGPUTextureFormat_BC3RGBAUnorm:
        return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
    case WGPUTextureFormat_BC4RUnorm:
        return SDL_GPU_TEXTUREFORMAT_BC4_R_UNORM;
    case WGPUTextureFormat_BC5RGUnorm:
        return SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM;
    case WGPUTextureFormat_BC7RGBAUnorm:
        return SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM;
    case WGPUTextureFormat_BC6HRGBFloat:
        return SDL_GPU_TEXTUREFORMAT_BC6H_RGB_FLOAT;
    case WGPUTextureFormat_BC6HRGBUfloat:
        return SDL_GPU_TEXTUREFORMAT_BC6H_RGB_UFLOAT;
    case WGPUTextureFormat_R8Snorm:
        return SDL_GPU_TEXTUREFORMAT_R8_SNORM;
    case WGPUTextureFormat_RG8Snorm:
        return SDL_GPU_TEXTUREFORMAT_R8G8_SNORM;
    case WGPUTextureFormat_RGBA8Snorm:
        return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_SNORM;
    case WGPUTextureFormat_R16Float:
        return SDL_GPU_TEXTUREFORMAT_R16_FLOAT;
    case WGPUTextureFormat_RG16Float:
        return SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
    case WGPUTextureFormat_RGBA16Float:
        return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    case WGPUTextureFormat_R32Float:
        return SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    case WGPUTextureFormat_RG32Float:
        return SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT;
    case WGPUTextureFormat_RGBA32Float:
        return SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    case WGPUTextureFormat_RG11B10Ufloat:
        return SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT;
    case WGPUTextureFormat_R8Uint:
        return SDL_GPU_TEXTUREFORMAT_R8_UINT;
    case WGPUTextureFormat_RG8Uint:
        return SDL_GPU_TEXTUREFORMAT_R8G8_UINT;
    case WGPUTextureFormat_RGBA8Uint:
        return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UINT;
    case WGPUTextureFormat_R8Sint:
        return SDL_GPU_TEXTUREFORMAT_R8_INT;
    case WGPUTextureFormat_RG8Sint:
        return SDL_GPU_TEXTUREFORMAT_R8G8_INT;
    case WGPUTextureFormat_RGBA8Sint:
        return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_INT;
    case WGPUTextureFormat_R16Sint:
        return SDL_GPU_TEXTUREFORMAT_R16_INT;
    case WGPUTextureFormat_RG16Sint:
        return SDL_GPU_TEXTUREFORMAT_R16G16_INT;
    case WGPUTextureFormat_RGBA16Sint:
        return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_INT;
    case WGPUTextureFormat_RGBA8UnormSrgb:
        return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
    case WGPUTextureFormat_BGRA8UnormSrgb:
        return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
    case WGPUTextureFormat_BC1RGBAUnormSrgb:
        return SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM_SRGB;
    case WGPUTextureFormat_BC2RGBAUnormSrgb:
        return SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM_SRGB;
    case WGPUTextureFormat_BC3RGBAUnormSrgb:
        return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM_SRGB;
    case WGPUTextureFormat_BC7RGBAUnormSrgb:
        return SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM_SRGB;
    case WGPUTextureFormat_Depth16Unorm:
        return SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    case WGPUTextureFormat_Depth24Plus:
        return SDL_GPU_TEXTUREFORMAT_D24_UNORM;
    case WGPUTextureFormat_Depth32Float:
        return SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    case WGPUTextureFormat_Depth24PlusStencil8:
        return SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    case WGPUTextureFormat_Depth32FloatStencil8:
        return SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
    default:
        return SDL_GPU_TEXTUREFORMAT_INVALID;
    }
}

static uint32_t SDLToWGPUSampleCount(SDL_GPUSampleCount samples)
{
    switch (samples) {
    // WGPU only supports 1, and 4x MSAA
    case SDL_GPU_SAMPLECOUNT_1:
        return 1;
    case SDL_GPU_SAMPLECOUNT_2:
    case SDL_GPU_SAMPLECOUNT_4:
    case SDL_GPU_SAMPLECOUNT_8:
        return 4;
    default:
        return 1;
    }
}

static WGPUBlendFactor SDLToWGPUBlendFactor(SDL_GPUBlendFactor sdlFactor)
{
    switch (sdlFactor) {
    case SDL_GPU_BLENDFACTOR_ZERO:
        return WGPUBlendFactor_Zero;
    case SDL_GPU_BLENDFACTOR_ONE:
        return WGPUBlendFactor_One;
    case SDL_GPU_BLENDFACTOR_SRC_COLOR:
        return WGPUBlendFactor_Src;
    case SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR:
        return WGPUBlendFactor_OneMinusSrc;
    case SDL_GPU_BLENDFACTOR_DST_COLOR:
        return WGPUBlendFactor_Dst;
    case SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR:
        return WGPUBlendFactor_OneMinusDst;
    case SDL_GPU_BLENDFACTOR_SRC_ALPHA:
        return WGPUBlendFactor_SrcAlpha;
    case SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA:
        return WGPUBlendFactor_OneMinusSrcAlpha;
    case SDL_GPU_BLENDFACTOR_DST_ALPHA:
        return WGPUBlendFactor_DstAlpha;
    case SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA:
        return WGPUBlendFactor_OneMinusDstAlpha;
    case SDL_GPU_BLENDFACTOR_CONSTANT_COLOR:
        return WGPUBlendFactor_Constant;
    case SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR:
        return WGPUBlendFactor_OneMinusConstant;
    case SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE:
        return WGPUBlendFactor_SrcAlphaSaturated;
    default:
        return WGPUBlendFactor_Undefined;
    }
}

static WGPUBlendOperation SDLToWGPUBlendOperation(SDL_GPUBlendOp sdlOp)
{
    switch (sdlOp) {
    case SDL_GPU_BLENDOP_ADD:
        return WGPUBlendOperation_Add;
    case SDL_GPU_BLENDOP_SUBTRACT:
        return WGPUBlendOperation_Subtract;
    case SDL_GPU_BLENDOP_REVERSE_SUBTRACT:
        return WGPUBlendOperation_ReverseSubtract;
    case SDL_GPU_BLENDOP_MIN:
        return WGPUBlendOperation_Min;
    case SDL_GPU_BLENDOP_MAX:
        return WGPUBlendOperation_Max;
    default:
        return WGPUBlendOperation_Undefined;
    }
}

static WGPUStencilOperation SDLToWGPUStencilOperation(SDL_GPUStencilOp op)
{
    switch (op) {
    case SDL_GPU_STENCILOP_KEEP:
        return WGPUStencilOperation_Keep;
    case SDL_GPU_STENCILOP_ZERO:
        return WGPUStencilOperation_Zero;
    case SDL_GPU_STENCILOP_REPLACE:
        return WGPUStencilOperation_Replace;
    case SDL_GPU_STENCILOP_INVERT:
        return WGPUStencilOperation_Invert;
    case SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP:
        return WGPUStencilOperation_IncrementClamp;
    case SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP:
        return WGPUStencilOperation_DecrementClamp;
    case SDL_GPU_STENCILOP_INCREMENT_AND_WRAP:
        return WGPUStencilOperation_IncrementWrap;
    case SDL_GPU_STENCILOP_DECREMENT_AND_WRAP:
        return WGPUStencilOperation_DecrementWrap;
    default:
        return WGPUStencilOperation_Keep;
    }
}

static WGPUColorWriteMask SDLToWGPUColorWriteMask(SDL_GPUColorComponentFlags mask)
{
    WGPUColorWriteMask wgpuMask = WGPUColorWriteMask_None;
    if (mask & SDL_GPU_COLORCOMPONENT_R) {
        wgpuMask |= WGPUColorWriteMask_Green;
    }
    if (mask & SDL_GPU_COLORCOMPONENT_G) {
        wgpuMask |= WGPUColorWriteMask_Blue;
    }
    if (mask & SDL_GPU_COLORCOMPONENT_B) {
        wgpuMask |= WGPUColorWriteMask_Alpha;
    }
    if (mask & SDL_GPU_COLORCOMPONENT_A) {
        wgpuMask |= WGPUColorWriteMask_Red;
    }
    return wgpuMask;
}

static WGPUCompareFunction SDLToWGPUCompareFunction(SDL_GPUCompareOp compareOp)
{
    switch (compareOp) {
    case SDL_GPU_COMPAREOP_NEVER:
        return WGPUCompareFunction_Never;
    case SDL_GPU_COMPAREOP_LESS:
        return WGPUCompareFunction_Less;
    case SDL_GPU_COMPAREOP_EQUAL:
        return WGPUCompareFunction_Equal;
    case SDL_GPU_COMPAREOP_LESS_OR_EQUAL:
        return WGPUCompareFunction_LessEqual;
    case SDL_GPU_COMPAREOP_GREATER:
        return WGPUCompareFunction_Greater;
    case SDL_GPU_COMPAREOP_NOT_EQUAL:
        return WGPUCompareFunction_NotEqual;
    case SDL_GPU_COMPAREOP_GREATER_OR_EQUAL:
        return WGPUCompareFunction_GreaterEqual;
    case SDL_GPU_COMPAREOP_ALWAYS:
        return WGPUCompareFunction_Always;
    default:
        return WGPUCompareFunction_Never;
    }
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

static WGPUPresentMode SDLToWGPUPresentMode(SDL_GPUPresentMode presentMode)
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

static WGPUVertexStepMode SDLToWGPUInputStepMode(SDL_GPUVertexInputRate inputRate)
{
    switch (inputRate) {
    case SDL_GPU_VERTEXINPUTRATE_VERTEX:
        return WGPUVertexStepMode_Vertex;
    case SDL_GPU_VERTEXINPUTRATE_INSTANCE:
        return WGPUVertexStepMode_Instance;
    default:
        return WGPUVertexStepMode_Undefined;
    }
}

static WGPUVertexFormat SDLToWGPUVertexFormat(SDL_GPUVertexElementFormat format)
{
    switch (format) {
    case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT:
        return WGPUVertexFormat_Float32;
    case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2:
        return WGPUVertexFormat_Float32x2;
    case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3:
        return WGPUVertexFormat_Float32x3;
    case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4:
        return WGPUVertexFormat_Float32x4;
    case SDL_GPU_VERTEXELEMENTFORMAT_INT:
        return WGPUVertexFormat_Sint32;
    case SDL_GPU_VERTEXELEMENTFORMAT_INT2:
        return WGPUVertexFormat_Sint32x2;
    case SDL_GPU_VERTEXELEMENTFORMAT_INT3:
        return WGPUVertexFormat_Sint32x3;
    case SDL_GPU_VERTEXELEMENTFORMAT_INT4:
        return WGPUVertexFormat_Sint32x4;
    case SDL_GPU_VERTEXELEMENTFORMAT_UINT:
        return WGPUVertexFormat_Uint32;
    case SDL_GPU_VERTEXELEMENTFORMAT_UINT2:
        return WGPUVertexFormat_Uint32x2;
    case SDL_GPU_VERTEXELEMENTFORMAT_UINT3:
        return WGPUVertexFormat_Uint32x3;
    case SDL_GPU_VERTEXELEMENTFORMAT_UINT4:
        return WGPUVertexFormat_Uint32x4;
    case SDL_GPU_VERTEXELEMENTFORMAT_BYTE2_NORM:
        return WGPUVertexFormat_Snorm8x2;
    case SDL_GPU_VERTEXELEMENTFORMAT_BYTE4_NORM:
        return WGPUVertexFormat_Snorm8x4;
    case SDL_GPU_VERTEXELEMENTFORMAT_UBYTE2_NORM:
        return WGPUVertexFormat_Unorm8x2;
    case SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM:
        return WGPUVertexFormat_Unorm8x4;
    case SDL_GPU_VERTEXELEMENTFORMAT_SHORT2:
        return WGPUVertexFormat_Sint16x2;
    case SDL_GPU_VERTEXELEMENTFORMAT_SHORT4:
        return WGPUVertexFormat_Sint16x4;
    case SDL_GPU_VERTEXELEMENTFORMAT_USHORT2:
        return WGPUVertexFormat_Uint16x2;
    case SDL_GPU_VERTEXELEMENTFORMAT_USHORT4:
        return WGPUVertexFormat_Uint16x4;
    default:
        return WGPUVertexFormat_Undefined;
    }
}

// WGPU Bind Group Layout Functions
// ---------------------------------------------------
static void WebGPU_CreateBindingLayout(WGPUBindGroupLayoutEntry *entry, ReflectedBinding *binding)
{
    switch (binding->type) {
    case WGPUBindingType_Buffer:
    case WGPUBindingType_UniformBuffer:
        entry->buffer = (WGPUBufferBindingLayout){
            .type = WGPUBufferBindingType_Uniform,
            .hasDynamicOffset = false,
            .minBindingSize = 0,
        };
        break;
    case WGPUBindingType_StorageBuffer:
        entry->buffer = (WGPUBufferBindingLayout){
            .type = WGPUBufferBindingType_Storage,
            .hasDynamicOffset = false,
            .minBindingSize = 0,
        };
        break;
    case WGPUBindingType_ReadOnlyStorageBuffer:
        entry->buffer = (WGPUBufferBindingLayout){
            .type = WGPUBufferBindingType_ReadOnlyStorage,
            .hasDynamicOffset = false,
            .minBindingSize = 0,
        };
        break;
    case WGPUBindingType_Texture:
    case WGPUBindingType_FilteringTexture:
    case WGPUBindingType_NonFilteringTexture:
        entry->texture = (WGPUTextureBindingLayout){
            .sampleType = WGPUTextureSampleType_Float,
            .viewDimension = WGPUTextureViewDimension_2D,
            .multisampled = false,
        };
        break;
    case WGPUBindingType_StorageTexture:
    case WGPUBindingType_WriteOnlyStorageTexture:
        entry->storageTexture = (WGPUStorageTextureBindingLayout){
            .access = WGPUStorageTextureAccess_WriteOnly,
            .format = WGPUTextureFormat_RGBA8Unorm,
            .viewDimension = WGPUTextureViewDimension_2D,
        };
        break;
    case WGPUBindingType_ReadOnlyStorageTexture:
        entry->storageTexture = (WGPUStorageTextureBindingLayout){
            .access = WGPUStorageTextureAccess_ReadOnly,
            .format = WGPUTextureFormat_RGBA8Unorm,
            .viewDimension = WGPUTextureViewDimension_2D,
        };
        break;
    case WGPUBindingType_ReadWriteStorageTexture:
        entry->storageTexture = (WGPUStorageTextureBindingLayout){
            .access = WGPUStorageTextureAccess_ReadWrite,
            .format = WGPUTextureFormat_RGBA8Unorm,
            .viewDimension = WGPUTextureViewDimension_2D,
        };
        break;
    case WGPUBindingType_Sampler:
    case WGPUBindingType_FilteringSampler:
        entry->sampler = (WGPUSamplerBindingLayout){
            .type = WGPUSamplerBindingType_Filtering,
        };
        break;
    case WGPUBindingType_NonFilteringSampler:
        entry->sampler = (WGPUSamplerBindingLayout){
            .type = WGPUSamplerBindingType_NonFiltering,
        };
        break;
    case WGPUBindingType_ComparisonSampler:
        entry->sampler = (WGPUSamplerBindingLayout){
            .type = WGPUSamplerBindingType_Comparison,
        };
        break;
    default:
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Invalid binding type %d", binding->type);
        break;
    };
}

static WebGPUPipelineResourceLayout *CreateResourceLayoutFromReflection(
    WebGPURenderer *renderer,
    ReflectedBindGroup *vertexBindGroups,
    uint32_t vertexBindGroupCount,
    ReflectedBindGroup *fragmentBindGroups,
    uint32_t fragmentBindGroupCount)
{
    WebGPUPipelineResourceLayout *layout = SDL_malloc(sizeof(WebGPUPipelineResourceLayout));
    layout->bindGroupLayoutCount = SDL_max(vertexBindGroupCount, fragmentBindGroupCount);

    WGPUBindGroupLayoutEntry *entries = NULL;

    SDL_Log("Vertex bind group count: %d", vertexBindGroupCount);
    SDL_Log("Fragment bind group count: %d", fragmentBindGroupCount);

    for (uint32_t i = 0; i < layout->bindGroupLayoutCount; i++) {
        SDL_Log("Creating bind group layout %d", i);
        uint32_t entryCount = 0;
        // Combine vertex and fragment bindings
        if (i < vertexBindGroupCount) {
            SDL_Log("Processing vertex bindgroup %d, with %u bindings", i, vertexBindGroups[i].bindingCount);
            for (uint32_t j = 0; j < vertexBindGroups[i].bindingCount; j++) {
                ReflectedBindGroup *bindGroup = &vertexBindGroups[i];
                SDL_Log("Retrieving binding %d", j);
                ReflectedBinding *binding = &bindGroup->bindings[j];
                SDL_Log("Binding %d, set %d, binding %d, type %d, stages %d", j, binding->set, binding->binding, binding->type, binding->stages);
                entries = SDL_realloc(entries, (entryCount + 1) * sizeof(WGPUBindGroupLayoutEntry));
                entries[entryCount] = (WGPUBindGroupLayoutEntry){
                    .binding = binding->binding,
                    .visibility = binding->stages,
                };

                // Assign the correct binding layout type to the BindingLayoutEntry
                // This can be a buffer, texture, sampler, etc.
                WebGPU_CreateBindingLayout(&entries[entryCount], binding);

                entryCount++;
            }
        }

        if (i < fragmentBindGroupCount) {
            for (uint32_t j = 0; j < fragmentBindGroups[i].bindingCount; j++) {
                ReflectedBinding *binding = &fragmentBindGroups[i].bindings[j];
                // Check if this binding already exists from vertex stage
                bool found = false;
                for (uint32_t k = 0; k < entryCount; k++) {
                    if (entries[k].binding == binding->binding) {
                        entries[k].visibility |= binding->stages;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    entries = SDL_realloc(entries, (entryCount + 1) * sizeof(WGPUBindGroupLayoutEntry));
                    entries[entryCount] = (WGPUBindGroupLayoutEntry){
                        .binding = binding->binding,
                        .visibility = binding->stages,
                    };

                    // Assign the correct binding layout type to the BindingLayoutEntry
                    // This can be a buffer, texture, sampler, etc.
                    WebGPU_CreateBindingLayout(&entries[entryCount], binding);

                    entryCount++;
                }
            }
        }

        WGPUBindGroupLayoutDescriptor bindGroupLayoutDesc = {
            .entryCount = entryCount,
            .entries = entries
        };

        layout->bindGroupLayouts[i].layout = wgpuDeviceCreateBindGroupLayout(renderer->device, &bindGroupLayoutDesc);
        layout->bindGroupLayouts[i].entryCount = entryCount;
        layout->bindGroupLayouts[i].stageFlags = SDL_malloc(entryCount * sizeof(WGPUShaderStageFlags));

        for (uint32_t j = 0; j < entryCount; j++) {
            layout->bindGroupLayouts[i].stageFlags[j] = entries[j].visibility;
        }

        SDL_free(entries);
    }

    return layout;
}

// WebGPU Functions:
// ---------------------------------------------------

// Simple Error Callback for WebGPU
static void WebGPU_ErrorCallback(WGPUErrorType type, const char *message, void *userdata)
{
    const char *errorTypeStr;
    switch (type) {
    case WGPUErrorType_Validation:
        errorTypeStr = "Validation Error";
        break;
    case WGPUErrorType_OutOfMemory:
        errorTypeStr = "Out of Memory Error";
        break;
    case WGPUErrorType_Unknown:
        errorTypeStr = "Unknown Error";
        break;
    case WGPUErrorType_DeviceLost:
        errorTypeStr = "Device Lost Error";
        break;
    default:
        errorTypeStr = "Unhandled Error Type";
        break;
    }
    // Output the error information to the console
    SDL_Log("[%s]: %s\n", errorTypeStr, message);
}

// Device Request Callback for when the device is requested from the adapter
static void WebGPU_RequestDeviceCallback(WGPURequestDeviceStatus status, WGPUDevice device, const char *message, void *userdata)
{
    WebGPURenderer *renderer = (WebGPURenderer *)userdata;
    if (status == WGPURequestDeviceStatus_Success) {
        renderer->device = device;
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
static bool WebGPU_INTERNAL_OnWindowResize(void *userdata, SDL_Event *event)
{
    SDL_Window *window = (SDL_Window *)userdata;
    // Event watchers will pass any event, but we only care about window resize events
    if (event->type != SDL_EVENT_WINDOW_RESIZED) {
        return false;
    }

    WindowData *windowData = WebGPU_INTERNAL_FetchWindowData(window);
    if (windowData) {
        windowData->needsSwapchainRecreate = true;
    }

    SDL_Log("Window resized, recreating swapchain");

    return true;
}

static SDL_GPUCommandBuffer *WebGPU_AcquireCommandBuffer(SDL_GPURenderer *driverData)
{
    WebGPURenderer *renderer = (WebGPURenderer *)driverData;
    WebGPUCommandBuffer *commandBuffer = SDL_malloc(sizeof(WebGPUCommandBuffer));
    memset(commandBuffer, '\0', sizeof(WebGPUCommandBuffer));
    commandBuffer->renderer = renderer;
    int width, height;
    width = renderer->claimedWindows[0]->window->w;
    height = renderer->claimedWindows[0]->window->h;
    commandBuffer->currentViewport = (WebGPUViewport){ 0, 0, width, height, 0.0, 1.0 };
    commandBuffer->currentScissor = (WebGPURect){ 0, 0, width, height };

    WGPUCommandEncoderDescriptor commandEncoderDesc = {
        .label = "SDL_GPU Command Encoder",
    };

    commandBuffer->commandEncoder = wgpuDeviceCreateCommandEncoder(renderer->device, &commandEncoderDesc);

    return (SDL_GPUCommandBuffer *)commandBuffer;
}

static bool WebGPU_Submit(SDL_GPUCommandBuffer *commandBuffer)
{
    WebGPUCommandBuffer *webgpuCommandBuffer = (WebGPUCommandBuffer *)commandBuffer;
    WebGPURenderer *renderer = webgpuCommandBuffer->renderer;

    WGPUCommandBufferDescriptor commandBufferDesc = {
        .label = "SDL_GPU Command Buffer",
    };

    WGPUCommandBuffer commandHandle = wgpuCommandEncoderFinish(webgpuCommandBuffer->commandEncoder, &commandBufferDesc);
    if (!commandHandle) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Failed to finish command buffer");
        return false;
    }
    wgpuQueueSubmit(renderer->queue, 1, &commandHandle);

    // Release the actual command buffer followed by the SDL command buffer
    wgpuCommandBufferRelease(commandHandle);
    wgpuCommandEncoderRelease(webgpuCommandBuffer->commandEncoder);
    SDL_free(webgpuCommandBuffer);

    return true;
}

static void WebGPU_BeginRenderPass(SDL_GPUCommandBuffer *commandBuffer,
                                   const SDL_GPUColorTargetInfo *colorAttachmentInfos,
                                   Uint32 colorAttachmentCount,
                                   const SDL_GPUDepthStencilTargetInfo *depthStencilAttachmentInfo)
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
            .loadOp = SDLToWGPULoadOp(colorAttachmentInfos[i].load_op),
            .storeOp = SDLToWGPUStoreOp(colorAttachmentInfos[i].store_op),
            .clearValue = (WGPUColor){
                .r = colorAttachmentInfos[i].clear_color.r,
                .g = colorAttachmentInfos[i].clear_color.g,
                .b = colorAttachmentInfos[i].clear_color.b,
                .a = colorAttachmentInfos[i].clear_color.a,
            },
        };

        // If we have an MSAA texture, we need to make sure the resolve target is not NULL
        if (texture->isMSAAColorTarget) {
            colorAttachments[i].resolveTarget = texture->fullView;
        }
    }

    // Set color attachments for the render pass
    WGPURenderPassDescriptor renderPassDesc = {
        .label = "SDL_GPU Render Pass",
        .colorAttachmentCount = colorAttachmentCount,
        .colorAttachments = colorAttachments,
    };

    // Set depth stencil attachment if provided
    if (depthStencilAttachmentInfo != NULL) {
        // Get depth texture as WebGPUTexture
        WebGPUTextureContainer *textureContainer = (WebGPUTextureContainer *)depthStencilAttachmentInfo->texture;
        texture = textureContainer->activeTextureHandle->webgpuTexture;
        WGPURenderPassDepthStencilAttachment depthStencilAttachment = {
            .view = NULL,
            .depthLoadOp = SDLToWGPULoadOp(depthStencilAttachmentInfo->load_op),
            .depthStoreOp = SDLToWGPUStoreOp(depthStencilAttachmentInfo->store_op),
            .depthClearValue = depthStencilAttachmentInfo->clear_depth,
            .stencilLoadOp = SDLToWGPULoadOp(depthStencilAttachmentInfo->stencil_load_op),
            .stencilStoreOp = SDLToWGPUStoreOp(depthStencilAttachmentInfo->stencil_store_op),
            .stencilClearValue = depthStencilAttachmentInfo->clear_stencil,
        };

        renderPassDesc.depthStencilAttachment = &depthStencilAttachment;
    }

    // Begin the render pass
    webgpuCommandBuffer->renderPassEncoder = wgpuCommandEncoderBeginRenderPass(webgpuCommandBuffer->commandEncoder, &renderPassDesc);
}

static void WebGPU_EndRenderPass(SDL_GPUCommandBuffer *commandBuffer)
{
    WebGPUCommandBuffer *webgpuCommandBuffer = (WebGPUCommandBuffer *)commandBuffer;
    wgpuRenderPassEncoderEnd(webgpuCommandBuffer->renderPassEncoder);
    wgpuRenderPassEncoderRelease(webgpuCommandBuffer->renderPassEncoder);
}

static void WebGPU_BeginCopyPass(SDL_GPUCommandBuffer *commandBuffer)
{
    WebGPUCommandBuffer *webgpuCommandBuffer = (WebGPUCommandBuffer *)commandBuffer;
    WGPUCommandEncoderDescriptor commandEncoderDesc = {
        .label = "SDL_GPU Copy Encoder",
    };
    webgpuCommandBuffer->commandEncoder = wgpuDeviceCreateCommandEncoder(webgpuCommandBuffer->renderer->device, &commandEncoderDesc);
}

static void WebGPU_EndCopyPass(SDL_GPUCommandBuffer *commandBuffer)
{
    (void)commandBuffer;
    // No need to do anything here, everything is handled in Submit for WGPU
}

// Swapchain & Window Related Functions
// ---------------------------------------------------
bool WebGPU_INTERNAL_CreateSurface(WebGPURenderer *renderer, WindowData *windowData)
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

    SDL_assert(WebGPU_INTERNAL_CreateSurface(renderer, windowData));

    WGPUSurfaceDescriptorFromCanvasHTMLSelector canvas_desc = {
        .chain.sType = WGPUSType_SurfaceDescriptorFromCanvasHTMLSelector,
        .selector = "#canvas",
    };
    WGPUSurfaceDescriptor surf_desc = {
        .nextInChain = &canvas_desc.chain,
    };
    windowData->swapchainData->surface = wgpuInstanceCreateSurface(renderer->instance, &surf_desc);

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
        .sampleCount = swapchainData->sampleCount != 0 ? swapchainData->sampleCount : 1,
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

static SDL_GPUTextureFormat WebGPU_GetSwapchainTextureFormat(
    SDL_GPURenderer *driverData,
    SDL_Window *window)
{
    WindowData *windowData = WebGPU_INTERNAL_FetchWindowData(window);
    WebGPUSwapchainData *swapchainData = windowData->swapchainData;

    return WGPUToSDLTextureFormat(swapchainData->format);
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

static bool WebGPU_AcquireSwapchainTexture(
    SDL_GPUCommandBuffer *commandBuffer,
    SDL_Window *window,
    SDL_GPUTexture **ret_texture,
    Uint32 *ret_width,
    Uint32 *ret_height)
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
            SDL_LogError(SDL_LOG_CATEGORY_GPU, "Failed to recreate swapchain");
            SDL_SetError("Failed to recreate swapchain");
            return false;
        }
    }

    // Get the current texture view from the swapchain
    WGPUTextureView currentView = wgpuSwapChainGetCurrentTextureView(swapchainData->swapchain);
    if (currentView == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Failed to acquire texture view from swapchain");
        SDL_SetError("Failed to acquire texture view from swapchain");
        return false;
    }

    // Create a temporary WebGPUTexture to return
    WebGPUTexture *texture = SDL_calloc(1, sizeof(WebGPUTexture));
    if (!texture) {
        SDL_OutOfMemory();
        return false;
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
    texture->usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;

    // For MSAA, we'll return the MSAA texture instead of the swapchain texture
    if (swapchainData->sampleCount > 1) {
        texture->texture = swapchainData->msaaTexture;
        texture->fullView = swapchainData->msaaView;
    }

    *ret_texture = (SDL_GPUTexture *)texture;

    // It is important to release these textures when they are no longer needed
    return true;
}

static bool WebGPU_SupportsTextureFormat(SDL_GPURenderer *driverData,
                                         SDL_GPUTextureFormat format,
                                         SDL_GPUTextureType type,
                                         SDL_GPUTextureUsageFlags usage)
{
    WebGPURenderer *renderer = (WebGPURenderer *)driverData;
    WGPUTextureFormat wgpuFormat = SDLToWGPUTextureFormat(format);
    WGPUTextureUsageFlags wgpuUsage = SDLToWGPUTextureUsageFlags(usage);
    WGPUTextureDimension dimension = WGPUTextureDimension_Undefined;
    if (type == SDL_GPU_TEXTURETYPE_2D || type == SDL_GPU_TEXTURETYPE_2D_ARRAY) {
        dimension = WGPUTextureDimension_2D;
    } else if (type == SDL_GPU_TEXTURETYPE_3D || type == SDL_GPU_TEXTURETYPE_CUBE_ARRAY) {
        dimension = WGPUTextureDimension_3D;
    }

    // Verify that the format, usage, and dimension are considered valid
    if (wgpuFormat == WGPUTextureFormat_Undefined) {
        return false;
    }
    if (wgpuUsage == WGPUTextureUsage_None) {
        return false;
    }
    if (dimension == WGPUTextureDimension_Undefined) {
        return false;
    }

    // Texture format is valid.
    return true;
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

            SDL_AddEventWatch(WebGPU_INTERNAL_OnWindowResize, window);
            return true;
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_GPU, "Could not create swapchain, failed to claim window!");
            SDL_free(windowData);
            return false;
        }
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Window already claimed!");
        return false;
    }
}

static void WebGPU_ReleaseWindow(SDL_GPURenderer *driverData, SDL_Window *window)
{
    WebGPURenderer *renderer = (WebGPURenderer *)driverData;

    if (renderer->claimedWindowCount == 0) {
        return;
    }

    WindowData *windowData = WebGPU_INTERNAL_FetchWindowData(window);

    if (windowData == NULL) {
        return;
    }

    // Destroy the swapchain
    if (windowData->swapchainData) {
        WebGPU_DestroySwapchain(windowData->swapchainData);
    }

    // Eliminate the window from the claimed windows
    for (Uint32 i = 0; i < renderer->claimedWindowCount; i += 1) {
        if (renderer->claimedWindows[i]->window == window) {
            renderer->claimedWindows[i] = renderer->claimedWindows[renderer->claimedWindowCount - 1];
            renderer->claimedWindowCount -= 1;
            break;
        }
    }

    // Cleanup
    SDL_free(windowData);
    SDL_ClearProperty(SDL_GetWindowProperties(window), WINDOW_PROPERTY_DATA);
    SDL_RemoveEventWatch(WebGPU_INTERNAL_OnWindowResize, window);
}

// Buffer Management Functions
// ---------------------------------------------------
static SDL_GPUBuffer *WebGPU_INTERNAL_CreateGPUBuffer(SDL_GPURenderer *driverData,
                                                      void *usageFlags,
                                                      Uint32 size, WebGPUBufferType type)
{
    WebGPUBuffer *buffer = SDL_calloc(1, sizeof(WebGPUBuffer));
    buffer->size = size;

    WGPUBufferUsageFlags wgpuUsage = 0;
    if (type == WEBGPU_BUFFER_TYPE_TRANSFER) {
        // VERIFY ALL OF THESE IF THERE ARE BUGS. I BELIEVE THIS IS CORRECT HOWEVER
        SDL_GPUTransferBufferUsage sdlFlags = *((SDL_GPUTransferBufferUsage *)usageFlags);
        if (sdlFlags == SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD) {
            SDL_Log("Creating upload transfer buffer");
            wgpuUsage = WGPUBufferUsage_MapWrite | WGPUBufferUsage_CopySrc;
        } else if (sdlFlags == SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD) {
            SDL_Log("Creating download transfer buffer");
            wgpuUsage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
        }
    } else {
        SDL_Log("Creating GPU buffer");
        wgpuUsage = SDLToWGPUBufferUsageFlags(*(SDL_GPUBufferUsageFlags *)usageFlags);
        wgpuUsage |= WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc;
    }

    buffer->usageFlags = *((SDL_GPUBufferUsageFlags *)usageFlags);
    SDL_SetAtomicInt(&buffer->mappingComplete, 0);

    SDL_Log("Creating buffer with usage flags: %d", wgpuUsage);
    WGPUBufferDescriptor bufferDesc = {
        .usage = wgpuUsage,
        .size = size,
        .mappedAtCreation = false, // The programmer will map the buffer when they need to write to it
    };

    buffer->buffer = wgpuDeviceCreateBuffer(((WebGPURenderer *)driverData)->device, &bufferDesc);
    buffer->type = type;
    buffer->markedForDestroy = false;
    buffer->isMapped = false;

    return (SDL_GPUBuffer *)buffer;
}

static SDL_GPUBuffer *WebGPU_CreateGPUBuffer(SDL_GPURenderer *driverData,
                                             SDL_GPUBufferUsageFlags usageFlags,
                                             Uint32 size)
{
    return WebGPU_INTERNAL_CreateGPUBuffer(driverData, (void *)&usageFlags, size, WEBGPU_BUFFER_TYPE_GPU);
}

static void WebGPU_ReleaseBuffer(SDL_GPURenderer *driverData, SDL_GPUBuffer *buffer)
{
    WebGPUBuffer *webgpuBuffer = (WebGPUBuffer *)buffer;
    if (webgpuBuffer->buffer) {
        wgpuBufferRelease(webgpuBuffer->buffer);
    }
    SDL_free(webgpuBuffer);
}

static SDL_GPUTransferBuffer *WebGPU_CreateTransferBuffer(
    SDL_GPURenderer *driverData,
    SDL_GPUTransferBufferUsage usage, // ignored on Vulkan
    Uint32 size)
{
    return (SDL_GPUTransferBuffer *)WebGPU_INTERNAL_CreateGPUBuffer(driverData, &usage, size, WEBGPU_BUFFER_TYPE_TRANSFER);
}

static void WebGPU_ReleaseTransferBuffer(SDL_GPURenderer *driverData, SDL_GPUTransferBuffer *transferBuffer)
{
    WebGPUBuffer *webgpuBuffer = (WebGPUBuffer *)transferBuffer;
    if (webgpuBuffer->buffer) {
        wgpuBufferRelease(webgpuBuffer->buffer);
    }
    SDL_free(webgpuBuffer);
}

static void WebGPU_INTERNAL_MapTransferBuffer(WGPUBufferMapAsyncStatus status, void *userdata)
{
    WebGPUBuffer *buffer = (WebGPUBuffer *)userdata;

    if (status != WGPUBufferMapAsyncStatus_Success) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Failed to map buffer: status %d", status);
        buffer->mappedData = NULL;
        buffer->isMapped = false;
    } else {
        buffer->isMapped = true;
    }

    // Signal that the mapping operation is complete
    SDL_SetAtomicInt(&buffer->mappingComplete, 1);
}

static void *WebGPU_MapTransferBuffer(
    SDL_GPURenderer *driverData,
    SDL_GPUTransferBuffer *transferBuffer,
    bool cycle)
{
    WebGPUBuffer *buffer = (WebGPUBuffer *)transferBuffer;

    (void)cycle;

    if (!buffer || !buffer->buffer) {
        SDL_SetError("Invalid buffer");
        return NULL;
    }

    if (buffer->type != WEBGPU_BUFFER_TYPE_TRANSFER) {
        SDL_SetError("Buffer is not a transfer buffer");
        return NULL;
    }

    const Uint32 TIMEOUT = 1000;
    Uint32 startTime = SDL_GetTicks();

    // Reset mapped state
    buffer->isMapped = false;
    buffer->mappedData = NULL;
    SDL_SetAtomicInt(&buffer->mappingComplete, 0);

    WGPUMapMode mapMode = buffer->usageFlags == SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD ? WGPUMapMode_Write : WGPUMapMode_Read;

    // Start async mapping
    wgpuBufferMapAsync(buffer->buffer, mapMode, 0, buffer->size,
                       WebGPU_INTERNAL_MapTransferBuffer, buffer);

    // Poll for completion
    while (SDL_GetAtomicInt(&buffer->mappingComplete) != 1) {
        if (SDL_GetTicks() - startTime > TIMEOUT) {
            SDL_SetError("Buffer mapping timed out");
            return NULL;
        }

        emscripten_sleep(1);
        SDL_Log("Waiting for buffer mapping to complete");
    }

    if (!buffer->isMapped) {
        SDL_SetError("Failed to map buffer");
        return NULL;
    }

    if (mapMode == WGPUMapMode_Read) {
        buffer->mappedData = (void *)wgpuBufferGetConstMappedRange(buffer->buffer, 0, buffer->size);
    } else {
        buffer->mappedData = wgpuBufferGetMappedRange(buffer->buffer, 0, buffer->size);
    }

    SDL_Log("Mapped buffer %p to %p", buffer->buffer, buffer->mappedData);

    return buffer->mappedData;
}

static void WebGPU_UnmapTransferBuffer(
    SDL_GPURenderer *driverData,
    SDL_GPUTransferBuffer *transferBuffer)
{
    WebGPUBuffer *buffer = (WebGPUBuffer *)transferBuffer;

    if (buffer && buffer->buffer) {
        wgpuBufferUnmap(buffer->buffer);
        buffer->isMapped = false;
        buffer->mappedData = NULL;
        SDL_SetAtomicInt(&buffer->mappingComplete, 0);
    }

    (void)driverData;
}

static void WebGPU_UploadToBuffer(SDL_GPUCommandBuffer *commandBuffer,
                                  const SDL_GPUTransferBufferLocation *source,
                                  const SDL_GPUBufferRegion *destination,
                                  bool cycle)
{
    if (!commandBuffer || !source || !destination) {
        SDL_SetError("Invalid parameters for buffer upload");
        return;
    }

    (void)cycle;

    WebGPUCommandBuffer *webgpuCmdBuffer = (WebGPUCommandBuffer *)commandBuffer;
    WebGPUBuffer *srcBuffer = (WebGPUBuffer *)source->transfer_buffer;
    WebGPUBuffer *dstBuffer = (WebGPUBuffer *)destination->buffer;

    if (!srcBuffer || !srcBuffer->buffer || !dstBuffer || !dstBuffer->buffer) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Invalid buffer");
        return;
    }

    if ((uint64_t)source->offset + destination->size > srcBuffer->size ||
        (uint64_t)destination->offset + destination->size > dstBuffer->size) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Invalid buffer region");
        return;
    }

    SDL_Log("Uploading %u bytes from buffer %p to buffer %p", destination->size, srcBuffer->buffer, dstBuffer->buffer);

    wgpuCommandEncoderCopyBufferToBuffer(
        webgpuCmdBuffer->commandEncoder,
        srcBuffer->buffer,
        source->offset,
        dstBuffer->buffer,
        destination->offset,
        destination->size);

    SDL_Log("Uploaded %u bytes from buffer %p to buffer %p", destination->size, srcBuffer->buffer, dstBuffer->buffer);
}

static void WebGPU_DownloadFromBuffer(
    SDL_GPUCommandBuffer *commandBuffer,
    const SDL_GPUBufferRegion *source,
    const SDL_GPUTransferBufferLocation *destination)
{
    if (!commandBuffer || !source || !destination) {
        SDL_SetError("Invalid parameters for buffer download");
        return;
    }

    WebGPUCommandBuffer *webgpuCmdBuffer = (WebGPUCommandBuffer *)commandBuffer;
    WebGPUBuffer *srcBuffer = (WebGPUBuffer *)source->buffer;
    WebGPUBuffer *dstBuffer = (WebGPUBuffer *)destination->transfer_buffer;

    if (!srcBuffer || !srcBuffer->buffer || !dstBuffer || !dstBuffer->buffer) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Invalid buffer");
        return;
    }

    if (source->offset + source->size > srcBuffer->size ||
        destination->offset + source->size > dstBuffer->size) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Invalid buffer region");
        return;
    }

    WGPUCommandEncoder encoder = webgpuCmdBuffer->commandEncoder;

    wgpuCommandEncoderCopyBufferToBuffer(
        encoder,
        srcBuffer->buffer,
        source->offset,
        dstBuffer->buffer,
        destination->offset,
        source->size);

    SDL_Log("Downloaded %u bytes from buffer %p to buffer %p", source->size, srcBuffer->buffer, dstBuffer->buffer);
}

static void WebGPU_BindVertexBuffers(
    SDL_GPUCommandBuffer *commandBuffer,
    Uint32 firstSlot,
    const SDL_GPUBufferBinding *bindings,
    Uint32 numBindings)
{
    if (!commandBuffer || !bindings || numBindings == 0) {
        SDL_SetError("Invalid parameters for binding vertex buffers");
        return;
    }

    WebGPUCommandBuffer *webgpuCmdBuffer = (WebGPUCommandBuffer *)commandBuffer;

    // Ensure we're inside a render pass
    if (!webgpuCmdBuffer->renderPassEncoder) {
        SDL_SetError("Cannot bind vertex buffers outside of a render pass");
        return;
    }

    // WebGPU requires us to set vertex buffers individually
    for (Uint32 i = 0; i < numBindings; i++) {
        const SDL_GPUBufferBinding *binding = &bindings[i];
        WebGPUBuffer *buffer = (WebGPUBuffer *)binding->buffer;

        if (!buffer || !buffer->buffer) {
            SDL_SetError("Invalid buffer at binding %u", i);
            continue;
        }

        /*SDL_Log("Binding vertex buffer %p to slot %u", buffer->buffer, firstSlot + i);*/

        wgpuRenderPassEncoderSetVertexBuffer(
            webgpuCmdBuffer->renderPassEncoder,
            firstSlot + i,                                     // slot
            buffer->buffer,                                    // buffer
            binding->offset,                                   // offset
            buffer->size == 0 ? WGPU_WHOLE_SIZE : buffer->size // size
        );
    }
}

// Shader Functions
// ---------------------------------------------------
static SDL_GPUShader *WebGPU_CreateShader(
    SDL_GPURenderer *driverData,
    const SDL_GPUShaderCreateInfo *shaderCreateInfo)
{
    SDL_assert(driverData && "Driver data must not be NULL when creating a shader");
    SDL_assert(shaderCreateInfo && "Shader create info must not be NULL when creating a shader");

    WebGPURenderer *renderer = (WebGPURenderer *)driverData;
    WebGPUShader *shader = SDL_calloc(1, sizeof(WebGPUShader));

    const char *wgsl = (const char *)shaderCreateInfo->code;
    WGPUShaderModuleWGSLDescriptor wgsl_desc = {
        .chain = {
            .sType = WGPUSType_ShaderModuleWGSLDescriptor,
            .next = NULL,
        },
        .code = wgsl,
    };

    WGPUShaderModuleDescriptor shader_desc = {
        .nextInChain = (WGPUChainedStruct *)&wgsl_desc,
        .label = "SDL_GPU WebGPU WGSL Cross-Compiled Shader",
    };

    // Create a WebGPUShader object to cast to SDL_GPUShader *
    uint32_t entryPointNameLength = SDL_strlen(shaderCreateInfo->entrypoint) + 1;
    shader->wgslSource = SDL_malloc(SDL_strlen(wgsl) + 1);
    SDL_strlcpy((char *)shader->wgslSource, wgsl, SDL_strlen(wgsl) + 1);
    shader->entrypoint = SDL_malloc(entryPointNameLength);
    SDL_utf8strlcpy((char *)shader->entrypoint, shaderCreateInfo->entrypoint, entryPointNameLength);
    shader->samplerCount = shaderCreateInfo->num_samplers;
    shader->storageBufferCount = shaderCreateInfo->num_storage_buffers;
    shader->uniformBufferCount = shaderCreateInfo->num_uniform_buffers;
    shader->storageTextureCount = shaderCreateInfo->num_storage_textures;
    shader->shaderModule = wgpuDeviceCreateShaderModule(renderer->device, &shader_desc);

    SDL_Log("Shader Created Successfully:");
    SDL_Log("entry: %s\n", shader->entrypoint);
    SDL_Log("sampler count: %u\n", shader->samplerCount);
    SDL_Log("storageBufferCount: %u\n", shader->storageBufferCount);
    SDL_Log("uniformBufferCount: %u\n", shader->uniformBufferCount);

    // Set our shader referenceCount to 0 at creation
    SDL_SetAtomicInt(&shader->referenceCount, 0);

    return (SDL_GPUShader *)shader;
}

static void WebGPU_ReleaseShader(
    SDL_GPURenderer *driverData,
    SDL_GPUShader *shader)
{
    SDL_assert(driverData && "Driver data must not be NULL when destroying a shader");
    SDL_assert(shader && "Shader must not be NULL when destroying a shader");

    WebGPUShader *wgpuShader = (WebGPUShader *)shader;

    // Free entry function string
    SDL_free((void *)wgpuShader->entrypoint);

    // Release the shader module
    wgpuShaderModuleRelease(wgpuShader->shaderModule);

    SDL_free(shader);
}

static void WebGPU_INTERNAL_TrackGraphicsPipeline(WebGPUCommandBuffer *commandBuffer,
                                                  WebGPUGraphicsPipeline *graphicsPipeline)
{
    TRACK_RESOURCE(
        graphicsPipeline,
        WebGPUGraphicsPipeline *,
        usedGraphicsPipelines,
        usedGraphicsPipelineCount,
        usedGraphicsPipelineCapacity)
}

// When building a graphics pipeline, we need to create the VertexState which is comprised of a shader module, an entry,
// and vertex buffer layouts. Using the existing SDL_GPUVertexInputState, we can create the vertex buffer layouts and
// pass them to the WGPUVertexState.
static WGPUVertexBufferLayout *SDL_WGPU_INTERNAL_CreateVertexBufferLayouts(const SDL_GPUVertexInputState *vertexInputState)
{
    if (vertexInputState == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Vertex input state must not be NULL when creating vertex buffer layouts");
        return NULL;
    }

    // Allocate memory for the vertex buffer layouts if needed.
    // Otherwise, early return NULL if there are no vertex buffers to create layouts for.
    WGPUVertexBufferLayout *vertexBufferLayouts;
    if (vertexInputState->num_vertex_buffers != 0) {
        vertexBufferLayouts = SDL_malloc(sizeof(WGPUVertexBufferLayout) * vertexInputState->num_vertex_buffers);
        if (vertexBufferLayouts == NULL) {
            SDL_OutOfMemory();
            return NULL;
        }
    } else {
        // This is not a bad thing. Just means we have no vertex buffers to create layouts for.
        return NULL;
    }

    // Allocate memory for the vertex attributes
    WGPUVertexAttribute *attributes = SDL_malloc(sizeof(WGPUVertexAttribute) * vertexInputState->num_vertex_attributes);
    if (attributes == NULL) {
        SDL_OutOfMemory();
        return NULL;
    }

    // Iterate through the vertex attributes and build the WGPUVertexAttribute array.
    // We also determine where each attribute belongs. This is used to build the vertex buffer layouts.
    Uint32 attribute_buffer_indices[vertexInputState->num_vertex_attributes];
    for (Uint32 i = 0; i < vertexInputState->num_vertex_attributes; i += 1) {
        const SDL_GPUVertexAttribute *vertexAttribute = &vertexInputState->vertex_attributes[i];
        attributes[i] = (WGPUVertexAttribute){
            .format = SDLToWGPUVertexFormat(vertexAttribute->format),
            .offset = vertexAttribute->offset,
            .shaderLocation = vertexAttribute->location,
        };
        attribute_buffer_indices[i] = vertexAttribute->buffer_slot;
    }

    // Iterate through the vertex buffers and build the WGPUVertexBufferLayouts using our attributes array.
    for (Uint32 i = 0; i < vertexInputState->num_vertex_buffers; i += 1) {
        Uint32 numAttributes = 0;
        // Not incredibly efficient but for now this will build the attributes for each vertex buffer
        for (Uint32 j = 0; j < vertexInputState->num_vertex_attributes; j += 1) {
            if (attribute_buffer_indices[j] == i) {
                numAttributes += 1;
            }
        }

        // Build the attributes for the current iteration's vertex buffer
        WGPUVertexAttribute *buffer_attributes;
        if (numAttributes == 0) {
            buffer_attributes = NULL;
            SDL_Log("No attributes found for vertex buffer %d", i);
        } else {
            buffer_attributes = SDL_malloc(sizeof(WGPUVertexAttribute) * numAttributes);
            if (buffer_attributes == NULL) {
                SDL_OutOfMemory();
                return NULL;
            }

            int count = 0;
            // Iterate through the vertex attributes and populate the attributes array
            for (Uint32 j = 0; j < vertexInputState->num_vertex_attributes; j += 1) {
                if (attribute_buffer_indices[j] == i) {
                    // We need to make an explicit copy of the attribute to avoid issues with the original being freed
                    memcpy(&buffer_attributes[count], &attributes[j], sizeof(WGPUVertexAttribute));
                    count += 1;
                }
            } // End attribute iteration
        }

        // Build the vertex buffer layout for the current vertex buffer using the attributes list (can be NULL)
        // This is then passed to the vertex state for the render pipeline
        const SDL_GPUVertexBufferDescription *vertexBuffer = &vertexInputState->vertex_buffer_descriptions[i];
        vertexBufferLayouts[i] = (WGPUVertexBufferLayout){
            .arrayStride = vertexBuffer->pitch,
            .stepMode = SDLToWGPUInputStepMode(vertexBuffer->input_rate),
            .attributeCount = numAttributes,
            .attributes = buffer_attributes,
        };
    }

    // Print the vertex buffer layouts for debugging purposes
    for (Uint32 i = 0; i < vertexInputState->num_vertex_buffers; i += 1) {
        SDL_Log("Vertex Buffer Layout %d:", i);
        SDL_Log("  Array Stride: %llu", vertexBufferLayouts[i].arrayStride);
        SDL_Log("  Step Mode: %d", vertexBufferLayouts[i].stepMode);
        for (Uint32 j = 0; j < vertexBufferLayouts[i].attributeCount; j += 1) {
            SDL_Log("  Attribute %d:", j);
            SDL_Log("    Format: %d", vertexBufferLayouts[i].attributes[j].format);
            SDL_Log("    Offset: %llu", vertexBufferLayouts[i].attributes[j].offset);
            SDL_Log("    Shader Location: %u", vertexBufferLayouts[i].attributes[j].shaderLocation);
        }
    }

    // Free the initial attributes array
    SDL_free(attributes);

    // Return a pointer to the head of the vertex buffer layouts
    return vertexBufferLayouts;
}

static SDL_GPUGraphicsPipeline *WebGPU_CreateGraphicsPipeline(
    SDL_GPURenderer *driverData,
    const SDL_GPUGraphicsPipelineCreateInfo *pipelineCreateInfo)
{
    SDL_assert(driverData && "Driver data must not be NULL when creating a graphics pipeline");
    SDL_assert(pipelineCreateInfo && "Pipeline create info must not be NULL when creating a graphics pipeline");

    WebGPURenderer *renderer = (WebGPURenderer *)driverData;
    WebGPUGraphicsPipeline *pipeline = SDL_calloc(1, sizeof(WebGPUGraphicsPipeline));
    if (!pipeline) {
        SDL_OutOfMemory();
        return NULL;
    }

    WebGPUShader *vertShader = (WebGPUShader *)pipelineCreateInfo->vertex_shader;
    WebGPUShader *fragShader = (WebGPUShader *)pipelineCreateInfo->fragment_shader;

    // TODO: Find a new way to reflect bindings now that we are using WGSL instead of SPIR-V
    uint32_t vertexBindGroupCount = 0;
    uint32_t fragmentBindGroupCount = 0;
    ReflectedBindGroup *vertexBindGroups = NULL;
    ReflectedBindGroup *fragmentBindGroups = NULL;

    // TODO: For now this will be a placeholder until we can reflect bindings from WGSL
    WebGPUPipelineResourceLayout *resourceLayout = CreateResourceLayoutFromReflection(renderer,
                                                                                      vertexBindGroups,
                                                                                      vertexBindGroupCount,
                                                                                      fragmentBindGroups,
                                                                                      fragmentBindGroupCount);

    wgpuDeviceSetUncapturedErrorCallback(renderer->device, WebGPU_ErrorCallback, renderer);

    // Create the pipeline layout
    WGPUPipelineLayoutDescriptor layoutDesc = {
        .label = "SDL_GPU WebGPU Pipeline Layout",
        .bindGroupLayoutCount = resourceLayout->bindGroupLayoutCount,
        .bindGroupLayouts = SDL_malloc(sizeof(WGPUBindGroupLayout) * resourceLayout->bindGroupLayoutCount),
    };

    // Assign the bind group layouts to the pipeline layout
    for (uint32_t i = 0; i < resourceLayout->bindGroupLayoutCount; i++) {
        SDL_memcpy(layoutDesc.bindGroupLayouts[i], &resourceLayout->bindGroupLayouts[i].layout, sizeof(WGPUBindGroupLayout));
    }

    // Create the pipeline layout from the descriptor
    WGPUPipelineLayout pipelineLayout = wgpuDeviceCreatePipelineLayout(renderer->device, &layoutDesc);

    resourceLayout->pipelineLayout = pipelineLayout;

    const SDL_GPUVertexInputState *vertexInputState = &pipelineCreateInfo->vertex_input_state;

    // Get the vertex buffer layouts for the vertex state if they exist
    WGPUVertexBufferLayout *vertexBufferLayouts = SDL_WGPU_INTERNAL_CreateVertexBufferLayouts(vertexInputState);

    // Create the vertex state for the render pipeline
    WGPUVertexState vertexState = {
        .module = vertShader->shaderModule,
        .entryPoint = vertShader->entrypoint,
        .bufferCount = pipelineCreateInfo->vertex_input_state.num_vertex_buffers,
        .buffers = vertexBufferLayouts,
        .constantCount = 0, // Leave as 0 as the Vulkan backend does not support push constants either
        .constants = NULL,  // Leave as NULL as the Vulkan backend does not support push constants either
    };

    // Build the color targets for the render pipeline'
    const SDL_GPUGraphicsPipelineTargetInfo *targetInfo = &pipelineCreateInfo->target_info;
    WGPUColorTargetState *colorTargets = SDL_malloc(sizeof(WGPUColorTargetState) * targetInfo->num_color_targets);
    for (Uint32 i = 0; i < targetInfo->num_color_targets; i += 1) {
        const SDL_GPUColorTargetDescription *colorAttachment = &targetInfo->color_target_descriptions[i];
        SDL_GPUColorTargetBlendState blendState = colorAttachment->blend_state;
        colorTargets[i] = (WGPUColorTargetState){
            .format = SDLToWGPUTextureFormat(colorAttachment->format),
            .blend = blendState.enable_blend == false
                         ? 0
                         : &(WGPUBlendState){
                               .color = {
                                   .srcFactor = SDLToWGPUBlendFactor(blendState.src_color_blendfactor),
                                   .dstFactor = SDLToWGPUBlendFactor(blendState.dst_color_blendfactor),
                                   .operation = SDLToWGPUBlendOperation(blendState.color_blend_op),
                               },
                               .alpha = {
                                   .srcFactor = SDLToWGPUBlendFactor(blendState.src_alpha_blendfactor),
                                   .dstFactor = SDLToWGPUBlendFactor(blendState.dst_alpha_blendfactor),
                                   .operation = SDLToWGPUBlendOperation(blendState.alpha_blend_op),
                               },
                           },
            .writeMask = blendState.enable_blend == true ? SDLToWGPUColorWriteMask(blendState.color_write_mask) : WGPUColorWriteMask_All
        };
    }

    // Create the fragment state for the render pipeline
    WGPUFragmentState fragmentState = {
        .module = fragShader->shaderModule,
        .entryPoint = fragShader->entrypoint,
        .constantCount = 0,
        .constants = NULL,
        .targetCount = targetInfo->num_color_targets,
        .targets = colorTargets,
    };

    WGPUDepthStencilState depthStencil;
    if (pipelineCreateInfo->target_info.has_depth_stencil_target) {
        const SDL_GPUDepthStencilState *state = &pipelineCreateInfo->depth_stencil_state;

        depthStencil.format = SDLToWGPUTextureFormat(pipelineCreateInfo->target_info.depth_stencil_format);
        depthStencil.depthWriteEnabled = state->enable_depth_write;
        depthStencil.depthCompare = SDLToWGPUCompareFunction(state->compare_op);

        depthStencil.stencilReadMask = state->compare_mask != 0 ? state->compare_mask : 0xFF;
        depthStencil.stencilWriteMask = state->write_mask;

        // If the stencil test is enabled, we need to set up the stencil state for the front and back faces
        if (state->enable_stencil_test) {
            depthStencil.stencilFront = (WGPUStencilFaceState){
                .compare = SDLToWGPUCompareFunction(state->front_stencil_state.compare_op),
                .failOp = SDLToWGPUStencilOperation(state->front_stencil_state.fail_op),
                .depthFailOp = SDLToWGPUStencilOperation(state->front_stencil_state.depth_fail_op),
                .passOp = SDLToWGPUStencilOperation(state->front_stencil_state.pass_op),
            };

            depthStencil.stencilBack = (WGPUStencilFaceState){
                .compare = SDLToWGPUCompareFunction(state->back_stencil_state.compare_op),
                .failOp = SDLToWGPUStencilOperation(state->back_stencil_state.fail_op),
                .depthFailOp = SDLToWGPUStencilOperation(state->back_stencil_state.depth_fail_op),
                .passOp = SDLToWGPUStencilOperation(state->back_stencil_state.pass_op),
            };
        }
    }

    // Create the render pipeline descriptor
    WGPURenderPipelineDescriptor pipelineDesc = {
        .nextInChain = NULL,
        .label = "SDL_GPU WebGPU Render Pipeline",
        .layout = pipelineLayout,
        .vertex = vertexState,
        .primitive = {
            .topology = SDLToWGPUPrimitiveTopology(pipelineCreateInfo->primitive_type),
            .stripIndexFormat = WGPUIndexFormat_Undefined, // TODO: Support strip index format Uint16 or Uint32
            .frontFace = SDLToWGPUFrontFace(pipelineCreateInfo->rasterizer_state.front_face),
            .cullMode = SDLToWGPUCullMode(pipelineCreateInfo->rasterizer_state.cull_mode),
        },
        // Needs to be set up
        .depthStencil = pipelineCreateInfo->target_info.has_depth_stencil_target ? &depthStencil : NULL,
        .multisample = {
            .count = pipelineCreateInfo->multisample_state.sample_count == 0 ? 1 : pipelineCreateInfo->multisample_state.sample_count,
            .mask = pipelineCreateInfo->multisample_state.sample_mask == 0 ? 0xFFFF : pipelineCreateInfo->multisample_state.sample_mask,
            .alphaToCoverageEnabled = false,
        },
        .fragment = &fragmentState,
    };

    // Create the WebGPU render pipeline from the descriptor
    WGPURenderPipeline wgpuPipeline = wgpuDeviceCreateRenderPipeline(renderer->device, &pipelineDesc);

    pipeline->pipeline = wgpuPipeline;
    pipeline->pipelineDesc = pipelineDesc;
    pipeline->vertexShader = vertShader;
    pipeline->fragmentShader = fragShader;
    pipeline->primitiveType = pipelineCreateInfo->primitive_type;
    pipeline->resourceLayout = resourceLayout;

    SDL_SetAtomicInt(&pipeline->referenceCount, 1);

    // Clean up
    SDL_free(colorTargets);
    SDL_free(((void *)layoutDesc.bindGroupLayouts));
    wgpuPipelineLayoutRelease(pipelineLayout);
    SDL_free(resourceLayout);

    // Iterate through the VertexBufferLayouts and free the attributes, then free the layout.
    // This can be done since everything has already been copied to the final render pipeline.
    for (Uint32 i = 0; i < vertexInputState->num_vertex_buffers; i += 1) {
        WGPUVertexBufferLayout *bufferLayout = &vertexBufferLayouts[i];
        if (bufferLayout->attributes != NULL) {
            SDL_free((void *)bufferLayout->attributes);
        }
        SDL_free(bufferLayout);
    }

    SDL_Log("Graphics Pipeline Created Successfully");
    return (SDL_GPUGraphicsPipeline *)pipeline;
}

static void WebGPU_ReleaseGraphicsPipeline(SDL_GPURenderer *driverData,
                                           SDL_GPUGraphicsPipeline *graphicsPipeline)
{
    WebGPUGraphicsPipeline *pipeline = (WebGPUGraphicsPipeline *)graphicsPipeline;
    SDL_AtomicDecRef(&pipeline->referenceCount);

    if (pipeline->pipeline) {
        if (SDL_GetAtomicInt(&pipeline->referenceCount) == 0) {
            wgpuRenderPipelineRelease(pipeline->pipeline);
            pipeline->pipeline = NULL;
        }
    }
}

// Helper function to create or update a bind group
static void WebGPU_INTERNAL_CreateOrUpdateBindGroup(
    WebGPUCommandBuffer *commandBuffer,
    WebGPUGraphicsPipeline *pipeline,
    uint32_t bindGroupIndex)
{
    WebGPUBindGroup *bindGroup = &commandBuffer->currentResources.bindGroups[bindGroupIndex];
    WebGPUBindGroupLayout *layout = &pipeline->resourceLayout->bindGroupLayouts[bindGroupIndex];

    WGPUBindGroupEntry *entries = SDL_malloc(sizeof(WGPUBindGroupEntry) * layout->entryCount);

    for (uint32_t i = 0; i < layout->entryCount; i++) {
        entries[i].binding = i;
        entries[i].sampler = NULL;
        entries[i].textureView = NULL;
        entries[i].buffer = NULL;
        entries[i].offset = 0;
        entries[i].size = 0;

        switch (layout->types[i]) {
        case WGPUBindingType_Buffer:
        case WGPUBindingType_UniformBuffer:
        case WGPUBindingType_StorageBuffer:
        case WGPUBindingType_ReadOnlyStorageBuffer:
            entries[i].buffer = bindGroup->entries[i].buffer.bufferHandle->webgpuBuffer->buffer;
            entries[i].offset = bindGroup->entries[i].buffer.offset;
            entries[i].size = bindGroup->entries[i].buffer.size;
            break;
        case WGPUBindingType_Sampler:
        case WGPUBindingType_FilteringSampler:
        case WGPUBindingType_NonFilteringSampler:
        case WGPUBindingType_ComparisonSampler:
            entries[i].sampler = bindGroup->entries[i].sampler;
            break;
        case WGPUBindingType_FilteringTexture:
        case WGPUBindingType_NonFilteringTexture:
            entries[i].textureView = bindGroup->entries[i].textureView;
            break;
            // Add cases for other binding types as needed
        case WGPUBindingType_Undefined:
        default:
            SDL_LogError(SDL_LOG_CATEGORY_GPU, "Undefined binding type in bind group layout");
            break;
        }
    }

    WGPUBindGroupDescriptor bindGroupDescriptor = {
        .layout = layout->layout,
        .entryCount = layout->entryCount,
        .entries = entries
    };

    if (bindGroup->bindGroup != NULL) {
        wgpuBindGroupRelease(bindGroup->bindGroup);
    }

    bindGroup->bindGroup = wgpuDeviceCreateBindGroup(commandBuffer->renderer->device, &bindGroupDescriptor);

    SDL_free(entries);
}

// Texture Functions
// ---------------------------------------------------

static SDL_GPUTexture *WebGPU_CreateTexture(
    SDL_GPURenderer *driverData,
    const SDL_GPUTextureCreateInfo *textureCreateInfo)
{
    SDL_assert(driverData && "Driver data must not be NULL when creating a texture");
    SDL_assert(textureCreateInfo && "Texture create info must not be NULL when creating a texture");

    WebGPURenderer *renderer = (WebGPURenderer *)driverData;
    WebGPUTexture *texture = (WebGPUTexture *)SDL_calloc(1, sizeof(WebGPUTexture));
    if (!texture) {
        SDL_OutOfMemory();
        return NULL;
    }

    WGPUTextureDescriptor textureDesc = {
        .label = "SDL_GPU WebGPU Texture",
        .size = (WGPUExtent3D){
            .width = textureCreateInfo->width,
            .height = textureCreateInfo->height,
            .depthOrArrayLayers = textureCreateInfo->layer_count_or_depth,
        },
        .mipLevelCount = 1,
        .sampleCount = SDLToWGPUSampleCount(textureCreateInfo->sample_count),
        .dimension = SDLToWGPUTextureDimension(textureCreateInfo->type),
        .format = SDLToWGPUTextureFormat(textureCreateInfo->format),
        .usage = SDLToWGPUTextureUsageFlags(textureCreateInfo->usage),
    };

    WGPUTexture wgpuTexture = wgpuDeviceCreateTexture(renderer->device, &textureDesc);
    if (wgpuTexture == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Failed to create texture");
        SDL_free(texture);
        SDL_OutOfMemory();
        return NULL;
    }

    // Create the WebGPUTexture object
    texture->texture = wgpuTexture;
    texture->usage = textureDesc.usage;
    texture->format = textureDesc.format;
    texture->dimensions = textureDesc.size;
    texture->layerCount = textureCreateInfo->layer_count_or_depth;
    texture->type = textureCreateInfo->type;

    // Create Texture View for the texture
    WGPUTextureViewDescriptor viewDesc = {
        .label = "SDL_GPU WebGPU Texture View",
        .format = textureDesc.format,
        .dimension = SDLToWGPUTextureViewDimension(textureCreateInfo->type),
        .baseMipLevel = 0,
        .mipLevelCount = 1,
        .baseArrayLayer = 0,
        .arrayLayerCount = textureCreateInfo->layer_count_or_depth,
    };

    // Create the texture view
    texture->fullView = wgpuTextureCreateView(texture->texture, &viewDesc);
    if (texture->fullView == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Failed to create texture view");
        SDL_free(texture);
        SDL_OutOfMemory();
        return NULL;
    }

    // Create a handle pointer for our texture and its container.
    WebGPUTextureHandle *textureHandle = SDL_malloc(sizeof(WebGPUTextureHandle));
    if (textureHandle == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Failed to create texture handle");
        SDL_free(texture);
        SDL_OutOfMemory();
        return NULL;
    }
    textureHandle->webgpuTexture = texture;
    textureHandle->container = NULL;

    // Assign the texture handle to the texture object
    texture->handle = textureHandle;

    // Create a texture container for the texture handle
    WebGPUTextureContainer *container = SDL_malloc(sizeof(WebGPUTextureContainer));
    if (container == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_GPU, "Failed to create texture container");
        SDL_free(texture);
        SDL_free(textureHandle);
        SDL_OutOfMemory();
        return NULL;
    }

    // Assign the container to the texture handle
    textureHandle->container = container;

    // Configure the texture container
    container->header.info = *textureCreateInfo;
    container->activeTextureHandle = textureHandle;
    container->textureCapacity = 1;
    container->textureCount = 1;
    container->textureHandles = SDL_malloc(sizeof(WebGPUTextureHandle *) * container->textureCapacity);
    container->textureHandles[0] = textureHandle;
    container->debugName = NULL;

    SDL_Log("Created texture handle %p, with texture container %p, and texture %p", textureHandle, container, texture);
    return (SDL_GPUTexture *)container;
}

void WebGPU_SetViewport(SDL_GPUCommandBuffer *renderPass, const SDL_GPUViewport *viewport)
{
    if (renderPass == NULL) {
        return;
    }

    WebGPUCommandBuffer *commandBuffer = (WebGPUCommandBuffer *)renderPass;

    uint32_t window_width = commandBuffer->renderer->claimedWindows[0]->swapchainData->width;
    uint32_t window_height = commandBuffer->renderer->claimedWindows[0]->swapchainData->height;
    WebGPUViewport *wgpuViewport = &commandBuffer->currentViewport;

    float max_viewport_width = (float)window_width - viewport->x;
    float max_viewport_height = (float)window_height - viewport->y;

    wgpuViewport = &(WebGPUViewport){
        .x = viewport->x,
        .y = viewport->y,
        .width = viewport->w > max_viewport_width ? max_viewport_width : viewport->w,
        .height = viewport->h > max_viewport_height ? max_viewport_height : viewport->h,
        .minDepth = viewport->min_depth > 0.0f ? viewport->min_depth : 0.0f,
        .maxDepth = viewport->max_depth > wgpuViewport->minDepth ? viewport->max_depth : wgpuViewport->minDepth,
    };

    // Set the viewport
    wgpuRenderPassEncoderSetViewport(commandBuffer->renderPassEncoder, wgpuViewport->x, wgpuViewport->y, wgpuViewport->width, wgpuViewport->height, wgpuViewport->minDepth, wgpuViewport->maxDepth);
}

void WebGPU_SetScissorRect(SDL_GPUCommandBuffer *renderPass, const SDL_Rect *scissorRect)
{
    if (renderPass == NULL) {
        return;
    }

    WebGPUCommandBuffer *commandBuffer = (WebGPUCommandBuffer *)renderPass;

    uint32_t window_width = commandBuffer->renderer->claimedWindows[0]->swapchainData->width;
    uint32_t window_height = commandBuffer->renderer->claimedWindows[0]->swapchainData->height;

    uint32_t max_scissor_width = window_width - scissorRect->x;
    uint32_t max_scissor_height = window_height - scissorRect->y;

    uint32_t clamped_width = (scissorRect->w > max_scissor_width) ? max_scissor_width : scissorRect->w;
    uint32_t clamped_height = (scissorRect->h > max_scissor_height) ? max_scissor_height : scissorRect->h;

    commandBuffer->currentScissor = (WebGPURect){
        .x = scissorRect->x,
        .y = scissorRect->y,
        .width = clamped_width,
        .height = clamped_height,
    };

    wgpuRenderPassEncoderSetScissorRect(commandBuffer->renderPassEncoder, scissorRect->x, scissorRect->y, clamped_width, clamped_height);
}

static void WebGPU_SetStencilReference(SDL_GPUCommandBuffer *commandBuffer,
                                       Uint8 reference)
{
    // no-op (pass)
}

    static void WebGPU_BindGraphicsPipeline(
        SDL_GPUCommandBuffer *commandBuffer,
        SDL_GPUGraphicsPipeline *graphicsPipeline)
{
    WebGPUCommandBuffer *webgpuCommandBuffer = (WebGPUCommandBuffer *)commandBuffer;
    WebGPUGraphicsPipeline *pipeline = (WebGPUGraphicsPipeline *)graphicsPipeline;

    // Bind the pipeline
    wgpuRenderPassEncoderSetPipeline(webgpuCommandBuffer->renderPassEncoder, pipeline->pipeline);

    // Set the current pipeline
    webgpuCommandBuffer->currentGraphicsPipeline = pipeline;

    // Track the pipeline (you may need to implement this function)
    WebGPU_INTERNAL_TrackGraphicsPipeline(webgpuCommandBuffer, pipeline);

    /*// TODO: For now, this is commenrted out as it has some issues that need to be resolved*/
    /*// Bind resources based on the pipeline's resource layout*/
    /*for (uint32_t i = 0; i < pipeline->resourceLayout->bindGroupLayoutCount; i++) {*/
    /*    WebGPUBindGroup *bindGroup = &webgpuCommandBuffer->currentResources.bindGroups[i];*/
    /**/
    /*    // Check if we need to create or update the bind group*/
    /*    if (bindGroup->bindGroup == NULL || webgpuCommandBuffer->resourcesDirty) {*/
    /*        WebGPU_INTERNAL_CreateOrUpdateBindGroup(webgpuCommandBuffer, pipeline, i);*/
    /*    }*/
    /**/
    /*    // Set the bind group*/
    /*    wgpuRenderPassEncoderSetBindGroup(webgpuCommandBuffer->renderPassEncoder, i, bindGroup->bindGroup, 0, NULL);*/
    /*}*/

    // Mark resources as clean
    webgpuCommandBuffer->resourcesDirty = false;
}

static void WebGPU_DrawPrimitives(
    SDL_GPUCommandBuffer *commandBuffer,
    Uint32 vertexCount,
    Uint32 instanceCount,
    Uint32 firstVertex,
    Uint32 firstInstance)
{
    WebGPUCommandBuffer *wgpuCommandBuffer = (WebGPUCommandBuffer *)commandBuffer;
    wgpuRenderPassEncoderDraw(wgpuCommandBuffer->renderPassEncoder, vertexCount, instanceCount, firstVertex, firstInstance);
}

static bool WebGPU_PrepareDriver(SDL_VideoDevice *_this)
{
    // Realistically, we should check if the browser supports WebGPU here and return false if it doesn't
    // For now, we'll just return true because it'll simply crash if the browser doesn't support WebGPU anyways
    return true;
}

static void WebGPU_DestroyDevice(SDL_GPUDevice *device)
{
    WebGPURenderer *renderer = (WebGPURenderer *)device->driverData;

    // Destroy all claimed windows
    for (Uint32 i = 0; i < renderer->claimedWindowCount; i += 1) {
        WebGPU_ReleaseWindow((SDL_GPURenderer *)renderer, renderer->claimedWindows[i]->window);
    }

    /*// Destroy the device*/
    /*wgpuDeviceRelease(renderer->device);*/
    /*wgpuAdapterRelease(renderer->adapter);*/
    /*wgpuInstanceRelease(renderer->instance);*/

    // Free the renderer
    SDL_free(renderer);
}

static SDL_GPUDevice *WebGPU_CreateDevice(bool debug, bool preferLowPower, SDL_PropertiesID props)
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

    WGPURequestAdapterOptions adapter_options = {
        .powerPreference = WGPUPowerPreference_HighPerformance,
        .backendType = WGPUBackendType_WebGPU,
    };

    // Request adapter using the instance and then the device using the adapter (this is done in the callback)
    wgpuInstanceRequestAdapter(renderer->instance, &adapter_options, WebGPU_RequestAdapterCallback, renderer);

    // This seems to be necessary to ensure that the device is created before continuing
    // This should probably be tested on all browsers to ensure that it works as expected
    // but Chrome's Dawn WebGPU implementation needs this to work
    while (!renderer->device) {
        emscripten_sleep(1);
    }

    /*// Set our error callback for emscripten*/
    wgpuDeviceSetUncapturedErrorCallback(renderer->device, WebGPU_ErrorCallback, renderer);

    /*emscripten_set_fullscreenchange_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, true, emsc_fullscreen_callback);*/

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
    result->driverData = (SDL_GPURenderer *)renderer;
    result->DestroyDevice = WebGPU_DestroyDevice;
    result->ClaimWindow = WebGPU_ClaimWindow;
    result->ReleaseWindow = WebGPU_ReleaseWindow;

    result->AcquireCommandBuffer = WebGPU_AcquireCommandBuffer;
    result->AcquireSwapchainTexture = WebGPU_AcquireSwapchainTexture;
    result->GetSwapchainTextureFormat = WebGPU_GetSwapchainTextureFormat;
    result->SupportsTextureFormat = WebGPU_SupportsTextureFormat;

    result->CreateBuffer = WebGPU_CreateGPUBuffer;
    result->ReleaseBuffer = WebGPU_ReleaseBuffer;
    result->CreateTransferBuffer = WebGPU_CreateTransferBuffer;
    result->ReleaseTransferBuffer = WebGPU_ReleaseTransferBuffer;
    result->MapTransferBuffer = WebGPU_MapTransferBuffer;
    result->UnmapTransferBuffer = WebGPU_UnmapTransferBuffer;
    result->UploadToBuffer = WebGPU_UploadToBuffer;
    result->DownloadFromBuffer = WebGPU_DownloadFromBuffer;

    result->CreateTexture = WebGPU_CreateTexture;

    result->BindVertexBuffers = WebGPU_BindVertexBuffers;

    result->CreateShader = WebGPU_CreateShader;
    result->ReleaseShader = WebGPU_ReleaseShader;

    // TODO START (finish the implementation of these functions)

    result->CreateGraphicsPipeline = WebGPU_CreateGraphicsPipeline;
    result->BindGraphicsPipeline = WebGPU_BindGraphicsPipeline;
    result->ReleaseGraphicsPipeline = WebGPU_ReleaseGraphicsPipeline;
    result->DrawPrimitives = WebGPU_DrawPrimitives;

    // TODO END

    result->SetScissor = WebGPU_SetScissorRect;
    result->SetViewport = WebGPU_SetViewport;
    result->SetStencilReference = WebGPU_SetStencilReference;

    result->Submit = WebGPU_Submit;
    result->BeginRenderPass = WebGPU_BeginRenderPass;
    result->EndRenderPass = WebGPU_EndRenderPass;
    result->BeginCopyPass = WebGPU_BeginCopyPass;
    result->EndCopyPass = WebGPU_EndCopyPass;

    return result;
}

// TODO: Implement other necessary functions like WebGPU_DestroyDevice, WebGPU_CreateTexture, etc.

SDL_GPUBootstrap WebGPUDriver = {
    "webgpu",
    SDL_GPU_SHADERFORMAT_WGSL,
    WebGPU_PrepareDriver,
    WebGPU_CreateDevice,
};
