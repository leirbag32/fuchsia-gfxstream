/*
 * Copyright 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

// Fuchsia virtio-gpu replacement for the goldfish ControlDevice ColorBuffer
// calls. A sysmem buffer is rendered through the virtio-gpu 2D resource that
// the display driver shares for it. Under gfxstream, that resource is a host
// ColorBuffer whose handle is the resource id, so it's imported with
// VkImportColorBufferGOOGLE.

#include <stdint.h>
#include <zircon/types.h>

#include "VirtGpu.h"

// Returns the ColorBuffer handle for the sysmem buffer `vmo` (not consumed) in
// `*colorBuffer`, and keeps a reference on it for the life of the device's
// connection. If the buffer has no ColorBuffer yet and `create` is true,
// creates one with the given geometry; `sysmemPixelFormat` is a
// fuchsia.images2.PixelFormat value. Returns ZX_ERR_NOT_FOUND if the buffer has
// no ColorBuffer and `create` is false.
zx_status_t fuchsiaVirtGpuImportColorBuffer(VirtGpuDevice* device, zx_handle_t vmo,
                                            uint32_t width, uint32_t height,
                                            uint32_t sysmemPixelFormat, bool create,
                                            uint32_t* colorBuffer);
