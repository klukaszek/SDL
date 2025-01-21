# SDL WebGPU Driver Roadmap / Checklist

These methods are tested by the example suite:
- https://github.com/klukaszek/SDL3-WebGPU-Examples

Getting my fork of the example suite merged with the original shouldn't be too much work but it needs to tested first.

The original example suite can be found at:
- https://github.com/TheSpydog/SDL_gpu_examples/

Currently, the WebGPU backend only supports Emscripten as the compilation target, though I've added Elie Michel's cross-platform surface configuration logic to the backend. I have not tested it with any native implementations however. 

# Checklist

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
- [ ] PushFragmentUniformData (NEEDS TO BE REVISITED)

## Vertex Stage
- [x] BindVertexBuffers
- [x] BindIndexBuffer
- [x] BindVertexSamplers
- [ ] BindVertexStorageTextures
- [ ] BindVertexStorageBuffers
- [ ] PushVertexUniformData (NEEDS TO BE REVISITED)

## Rendering States
- [x] SetViewport
- [x] SetScissor
- [ ] SetBlendConstants
- [x] SetStencilReference

## Composition
- [ ] Blit (Bug: Example "Blit2DArray" has a sampler issue where the RHS is not downsampled)
