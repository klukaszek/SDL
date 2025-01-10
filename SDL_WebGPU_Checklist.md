# SDL WebGPU Driver Function Checklist

## General
- [x] DestroyDevice
- [x] SupportsPresentMode
- [x] ClaimWindow
- [x] ReleaseWindow

## Swapchains
- [x] SetSwapchainParameters
- [x] SupportsTextureFormat
- [x] SupportsSampleCount
- [x] SupportsSwapchainComposition

## Command Buffers and Fences
- [x] AcquireCommandBuffer
- [x] AcquireSwapchainTexture
- [x] GetSwapchainTextureFormat
- [x] Submit
- [x] SubmitAndAcquireFence (Should just call Submit)
- [x] Cancel (Should be no-op for WebGPU)
- [x] Wait (Should be no-op for WebGPU)
- [x] WaitForFences (Should be no-op for WebGPU)
- [x] QueryFence (Should be no-op for WebGPU)
- [x] ReleaseFence (Should be no-op for WebGPU)

Note: WebGPU has no exposed fence API.

## Buffers
- [x] CreateBuffer
- [x] ReleaseBuffer
- [x] SetBufferName
- [x] CreateTransferBuffer
- [x] ReleaseTransferBuffer
- [x] MapTransferBuffer
- [x] UnmapTransferBuffer
- [x] UploadToBuffer
- [x] DownloadFromBuffer
- [x] CopyBufferToBuffer

Note: All functions are implemented but have not been fully tested for memory correctness. These functions simply worked as expected within the SDL_GPU example suite. Once native support is implemented, I can start running the program through Valgrind.

## Textures
- [x] CreateTexture
- [x] ReleaseTexture
- [x] SetTextureName
- [x] UploadToTexture
- [x] DownloadFromTexture (needs to be tested)
- [x] CopyTextureToTexture (needs to be tested)
- [ ] GenerateMipmaps

## Samplers
- [x] CreateSampler
- [x] ReleaseSampler

## Debugging
- [ ] InsertDebugLabel
- [ ] PushDebugGroup
- [ ] PopDebugGroup

## Graphics Pipelines
- [x] CreateGraphicsPipeline
- [x] BindGraphicsPipeline
- [x] ReleaseGraphicsPipeline

## Compute Pipelines
- [ ] CreateComputePipeline
- [ ] BindComputePipeline
- [ ] ReleaseComputePipeline

## Shaders
- [x] CreateShader
- [x] ReleaseShader

## Rendering
- [x] BeginRenderPass
- [x] EndRenderPass
- [ ] DrawPrimitivesIndirect
- [x] DrawPrimitives
- [x] DrawIndexedPrimitives
- [ ] DrawIndexedPrimitivesIndirect

## Copy Passes
- [x] BeginCopyPass
- [x] EndCopyPass

## Compute Passes
- [ ] BeginComputePass
- [ ] EndComputePass
- [ ] DispatchCompute
- [ ] DispatchComputeIndirect
- [ ] BindComputeSamplers
- [ ] BindComputeStorageTextures
- [ ] BindComputeStorageBuffers
- [ ] PushComputeUniformData

## Fragment Stage
- [x] BindFragmentSamplers
- [ ] BindFragmentStorageTextures
- [ ] BindFragmentStorageBuffers
- [ ] PushFragmentUniformData (IN PROGRESS)

## Vertex Stage
- [x] BindVertexBuffers
- [x] BindIndexBuffer
- [x] BindVertexSamplers
- [ ] BindVertexStorageTextures
- [ ] BindVertexStorageBuffers
- [ ] PushVertexUniformData (IN PROGRESS)

## Rendering States
- [x] SetViewport
- [x] SetScissor
- [ ] SetBlendConstants
- [x] SetStencilReference

## Composition
- [ ] Blit (IN PROGRESS: Blit2DArray has a sampeler issue where the RHS is not downsampled)
