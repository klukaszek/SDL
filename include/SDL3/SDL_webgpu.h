/*
  // Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

/**
 * # CategoryWebGPU
**/

// wgpu-native and Dawn install as `include/` and `lib/`. `include/` will contain a directory `webgpu/`
#ifdef SDL_WEBGPU_DAWN
#include "../../wgpu/include/dawn/dawn.h"
#endif

#ifdef SDL_WEBGPU_WGPU
#include "../../wgpu/include/webgpu/wgpu.h"
#endif
 
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_video.h>

extern SDL_DECLSPEC bool SDLCALL SDL_WebGPU_CreateSurface(SDL_Window *window,
                                                          WGPUInstance instance,
                                                          WGPUSurface *retSurface);
