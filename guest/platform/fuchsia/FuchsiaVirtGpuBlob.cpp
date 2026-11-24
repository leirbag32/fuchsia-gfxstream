/*
 * Copyright 2023 The Android Open Source Project
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

#include <cutils/log.h>
#include <lib/zx/vmar.h>
#include <msd-virtio-gpu/magma-virtio-gpu-defs.h>
#include <zircon/syscalls.h>

#include "FuchsiaVirtGpu.h"

namespace {

uint64_t pageRoundUp(uint64_t size) {
    const uint64_t pageSize = zx_system_get_page_size();
    return (size + pageSize - 1) & ~(pageSize - 1);
}

}  // namespace

FuchsiaVirtGpuResource::FuchsiaVirtGpuResource(FuchsiaVirtGpuDevice* device, uint32_t resourceId,
                                               uint64_t size, bool mappable)
    : device_(device), resourceId_(resourceId), size_(size), mappable_(mappable) {}

FuchsiaVirtGpuResource::~FuchsiaVirtGpuResource() {
    // The MSD unmaps the blob first if it's mapped.
    magma_virtio_gpu_resource_unref unref = {};
    unref.hdr.type = kMagmaVirtioGpuCmdResourceUnref;
    unref.resource_id = resourceId_;
    device_->sendCommand(&unref, sizeof(unref));
}

uint32_t FuchsiaVirtGpuResource::getResourceHandle() const { return resourceId_; }

// There are no GEM handles on Fuchsia; the resource id doubles as the handle.
uint32_t FuchsiaVirtGpuResource::getBlobHandle() const { return resourceId_; }

int FuchsiaVirtGpuResource::wait() { return device_->flush(); }

int FuchsiaVirtGpuResource::exportBlob(struct VirtGpuExternalHandle& handle) {
    ALOGE("%s: unimplemented", __func__);
    return -1;
}

int FuchsiaVirtGpuResource::transferFromHost(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    ALOGE("%s: unimplemented", __func__);
    return -1;
}

int FuchsiaVirtGpuResource::transferToHost(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    ALOGE("%s: unimplemented", __func__);
    return -1;
}

VirtGpuResourceMappingPtr FuchsiaVirtGpuResource::createMapping(void) {
    if (!mappable_) {
        ALOGE("%s: resource %u is not mappable", __func__, resourceId_);
        return nullptr;
    }
    zx::vmo vmo = device_->mapBlob(resourceId_);
    if (!vmo.is_valid()) return nullptr;

    uint64_t mapSize = pageRoundUp(size_);
    zx_vaddr_t addr = 0;
    zx_status_t status = zx::vmar::root_self()->map(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, vmo, 0,
                                                    mapSize, &addr);
    if (status != ZX_OK) {
        ALOGE("%s: vmar map of resource %u (0x%lx bytes) failed: %d", __func__, resourceId_,
              mapSize, status);
        return nullptr;
    }
    return std::make_shared<FuchsiaVirtGpuResourceMapping>(shared_from_this(),
                                                           reinterpret_cast<uint8_t*>(addr),
                                                           mapSize);
}
