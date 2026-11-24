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
#include <lib/zx/vmo.h>
#include <msd-virtio-gpu/magma-virtio-gpu-defs.h>
#include <os_dirent.h>
#include <services/service_connector.h>
#include <zircon/syscalls.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "FuchsiaVirtGpu.h"
#include "FuchsiaVirtGpuColorBuffer.h"

FuchsiaVirtGpuDevice::FuchsiaVirtGpuDevice(enum VirtGpuCapset capset, magma_device_t device)
    : VirtGpuDevice(capset), device_(device) {
    memset(&mCaps, 0, sizeof(struct VirtGpuCaps));

    // Hard-coded values that may be assumed on Fuchsia.
    mCaps.params[kParam3D] = 1;
    mCaps.params[kParamCapsetFix] = 1;
    mCaps.params[kParamResourceBlob] = 1;
    mCaps.params[kParamHostVisible] = 1;
    mCaps.params[kParamCrossDevice] = 0;
    mCaps.params[kParamContextInit] = 1;
    mCaps.params[kParamSupportedCapsetIds] = 0;
    mCaps.params[kParamExplicitDebugName] = 0;
    mCaps.params[kParamCreateGuestHandle] = 0;

    if (capset == kCapsetGfxStreamVulkan) {
        uint64_t query_id = kMagmaVirtioGpuQueryCapset;
        query_id |= static_cast<uint64_t>(kCapsetGfxStreamVulkan) << 32;
        constexpr uint16_t kVersion = 0;
        query_id |= static_cast<uint64_t>(kVersion) << 16;

        magma_handle_t buffer;
        magma_status_t status = magma_device_query(device_, query_id, &buffer, nullptr);
        if (status == MAGMA_STATUS_OK) {
            zx::vmo capset_info(buffer);
            zx_status_t status =
                capset_info.read(&mCaps.vulkanCapset, /*offset=*/0, sizeof(struct vulkanCapset));
            ALOGD("Got capset result, read status %d", status);
        } else {
            ALOGE("Query(%lu) failed: status %d, expected buffer result", query_id, status);
        }

        // We always need an ASG blob in some cases, so always define blobAlignment
        if (!mCaps.vulkanCapset.blobAlignment) {
            mCaps.vulkanCapset.blobAlignment = 4096;
        }
    }

    // One magma connection and context per device. The MSD creates a virtio-gpu
    // 3D context with the gfxstream capset for the magma context.
    magma_status_t status = magma_device_create_connection(device_, &connection_);
    if (status != MAGMA_STATUS_OK) {
        ALOGE("magma_device_create_connection failed: %d", status);
        connection_ = 0;
        return;
    }
    status = magma_connection_create_context2(connection_, MAGMA_PRIORITY_MEDIUM, &contextId_);
    if (status != MAGMA_STATUS_OK) {
        ALOGE("magma_connection_create_context failed: %d", status);
        magma_connection_release(connection_);
        connection_ = 0;
        return;
    }
}

FuchsiaVirtGpuDevice::~FuchsiaVirtGpuDevice() {
    if (connection_) {
        if (requestBuffer_) {
            magma_connection_release_buffer(connection_, requestBuffer_);
        }
        magma_connection_release_context(connection_, contextId_);
        magma_connection_release(connection_);
    }
    magma_device_release(device_);
}

int64_t FuchsiaVirtGpuDevice::getDeviceHandle(void) { return device_; }

int FuchsiaVirtGpuDevice::sendCommand(const void* command, size_t size) {
    if (!isReady()) return -1;
    if (size > MAGMA_VIRTIO_GPU_MAX_INLINE_COMMAND_SIZE) {
        ALOGE("%s: command too large: %zu", __func__, size);
        return -1;
    }
    magma_inline_command_buffer buffer = {};
    buffer.data = const_cast<void*>(command);
    buffer.size = size;
    std::lock_guard lock(mutex_);
    magma_status_t status =
        magma_connection_execute_inline_commands(connection_, contextId_, 1, &buffer);
    if (status != MAGMA_STATUS_OK) {
        ALOGE("%s: execute_inline_commands failed: %d", __func__, status);
        return -1;
    }
    return 0;
}

int FuchsiaVirtGpuDevice::flush() {
    if (!isReady()) return -1;
    std::lock_guard lock(mutex_);
    magma_status_t status = magma_connection_flush(connection_);
    if (status != MAGMA_STATUS_OK) {
        // An error from an earlier inline command closes the connection and shows up here.
        ALOGE("%s: magma_connection_flush failed: %d", __func__, status);
        return -1;
    }
    return 0;
}

zx::vmo FuchsiaVirtGpuDevice::mapBlob(uint32_t resourceId) {
    if (flush() != 0) return zx::vmo();
    uint64_t query_id = kMagmaVirtioGpuQueryMapBlob | (static_cast<uint64_t>(resourceId) << 32);
    magma_handle_t buffer;
    magma_status_t status = magma_device_query(device_, query_id, &buffer, nullptr);
    if (status != MAGMA_STATUS_OK) {
        ALOGE("%s: map query for resource %u failed: %d", __func__, resourceId, status);
        return zx::vmo();
    }
    return zx::vmo(buffer);
}

zx_status_t FuchsiaVirtGpuDevice::importColorBuffer(zx_handle_t vmo, uint32_t width,
                                                    uint32_t height, uint32_t sysmemPixelFormat,
                                                    bool create, uint32_t* colorBuffer) {
    if (!isReady()) return ZX_ERR_BAD_STATE;
    std::lock_guard lock(mutex_);

    if (!requestBuffer_) {
        uint64_t size = 0;
        magma_status_t status = magma_connection_create_buffer(
            connection_, zx_system_get_page_size(), &size, &requestBuffer_, &requestBufferId_);
        if (status != MAGMA_STATUS_OK) {
            ALOGE("%s: magma_connection_create_buffer failed: %d", __func__, status);
            requestBuffer_ = 0;
            return ZX_ERR_NO_MEMORY;
        }
        magma_handle_t handle;
        status = magma_buffer_get_handle(requestBuffer_, &handle);
        if (status != MAGMA_STATUS_OK) {
            ALOGE("%s: magma_buffer_get_handle failed: %d", __func__, status);
            magma_connection_release_buffer(connection_, requestBuffer_);
            requestBuffer_ = 0;
            return ZX_ERR_INTERNAL;
        }
        requestVmo_.reset(handle);
    }

    // The MSD reads the sysmem VMO from a magma buffer, which only lives for
    // this request; the MSD and the display driver keep their own handles.
    zx_handle_t vmoCopy;
    zx_status_t zxStatus = zx_handle_duplicate(vmo, ZX_RIGHT_SAME_RIGHTS, &vmoCopy);
    if (zxStatus != ZX_OK) {
        ALOGE("%s: zx_handle_duplicate failed: %d", __func__, zxStatus);
        return zxStatus;
    }
    uint64_t vmoSize = 0;
    magma_buffer_t vmoBuffer = 0;
    magma_buffer_id_t vmoBufferId = 0;
    magma_status_t status =
        magma_connection_import_buffer(connection_, vmoCopy, &vmoSize, &vmoBuffer, &vmoBufferId);
    if (status != MAGMA_STATUS_OK) {
        ALOGE("%s: magma_connection_import_buffer failed: %d", __func__, status);
        return ZX_ERR_INTERNAL;
    }

    magma_virtio_gpu_color_buffer_request request = {};
    request.vmo_resource_index = 1;
    request.width = width;
    request.height = height;
    request.pixel_format = sysmemPixelFormat;
    request.create = create ? 1 : 0;
    request.status = ZX_ERR_INTERNAL;

    zxStatus = requestVmo_.write(&request, 0, sizeof(request));
    if (zxStatus == ZX_OK) {
        magma_exec_resource resources[2] = {
            {.buffer_id = requestBufferId_, .offset = 0, .length = sizeof(request)},
            {.buffer_id = vmoBufferId, .offset = 0, .length = vmoSize},
        };
        magma_exec_command_buffer commandBuffer = {.resource_index = 0, .start_offset = 0};
        magma_command_descriptor_t descriptor = {};
        descriptor.resource_count = 2;
        descriptor.command_buffer_count = 1;
        descriptor.resources = resources;
        descriptor.command_buffers = &commandBuffer;
        status = magma_connection_execute_command(connection_, contextId_, &descriptor);
        if (status == MAGMA_STATUS_OK) {
            // The MSD writes its response before the flush returns.
            status = magma_connection_flush(connection_);
        }
        if (status == MAGMA_STATUS_OK) {
            zxStatus = requestVmo_.read(&request, 0, sizeof(request));
        } else {
            ALOGE("%s: color buffer request failed: %d", __func__, status);
            zxStatus = ZX_ERR_INTERNAL;
        }
    }
    magma_connection_release_buffer(connection_, vmoBuffer);

    if (zxStatus != ZX_OK) return zxStatus;
    if (request.status != ZX_OK) {
        if (request.status != ZX_ERR_NOT_FOUND) {
            ALOGE("%s: MSD failed to import a %ux%u color buffer (format %u): %d", __func__,
                  width, height, sysmemPixelFormat, request.status);
        }
        return request.status;
    }
    *colorBuffer = request.resource_id;
    return ZX_OK;
}

zx_status_t fuchsiaVirtGpuImportColorBuffer(VirtGpuDevice* device, zx_handle_t vmo,
                                            uint32_t width, uint32_t height,
                                            uint32_t sysmemPixelFormat, bool create,
                                            uint32_t* colorBuffer) {
    if (!device) return ZX_ERR_BAD_STATE;
    // FuchsiaVirtGpuDevice is the only VirtGpuDevice on Fuchsia.
    return static_cast<FuchsiaVirtGpuDevice*>(device)->importColorBuffer(
        vmo, width, height, sysmemPixelFormat, create, colorBuffer);
}

VirtGpuResourcePtr FuchsiaVirtGpuDevice::createBlob(const struct VirtGpuCreateBlob& blobCreate) {
    if (!isReady()) return nullptr;

    uint32_t blobMem = static_cast<uint32_t>(blobCreate.blobMem);
    uint32_t blobFlags = static_cast<uint32_t>(blobCreate.flags);
    if (blobMem != MAGMA_VIRTIO_GPU_BLOB_MEM_HOST3D) {
        ALOGE("%s: only host3d blobs are supported (blobMem %u)", __func__, blobMem);
        return nullptr;
    }

    uint64_t resourceId = 0;
    magma_status_t status =
        magma_device_query(device_, kMagmaVirtioGpuQueryAllocResourceId, nullptr, &resourceId);
    if (status != MAGMA_STATUS_OK) {
        ALOGE("%s: resource id query failed: %d", __func__, status);
        return nullptr;
    }

    magma_virtio_gpu_resource_create_blob create = {};
    create.hdr.type = kMagmaVirtioGpuCmdResourceCreateBlob;
    create.resource_id = static_cast<uint32_t>(resourceId);
    create.blob_mem = blobMem;
    create.blob_flags = blobFlags;
    create.nr_entries = 0;
    create.blob_id = blobCreate.blobId;
    create.size = blobCreate.size;
    if (sendCommand(&create, sizeof(create)) != 0) return nullptr;

    magma_virtio_gpu_ctx_resource attach = {};
    attach.hdr.type = kMagmaVirtioGpuCmdCtxAttachResource;
    attach.resource_id = create.resource_id;
    if (sendCommand(&attach, sizeof(attach)) != 0) return nullptr;

    bool mappable = (blobFlags & MAGMA_VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE) != 0;
    return std::make_shared<FuchsiaVirtGpuResource>(this, create.resource_id, blobCreate.size,
                                                    mappable);
}

VirtGpuResourcePtr FuchsiaVirtGpuDevice::createResource(uint32_t width, uint32_t height,
                                                        uint32_t stride, uint32_t size,
                                                        uint32_t virglFormat, uint32_t target,
                                                        uint32_t bind) {
    ALOGE("%s: unimplemented", __func__);
    return nullptr;
}

VirtGpuResourcePtr FuchsiaVirtGpuDevice::importBlob(const struct VirtGpuExternalHandle& handle) {
    ALOGE("%s: unimplemented", __func__);
    return nullptr;
}

int FuchsiaVirtGpuDevice::execBuffer(struct VirtGpuExecBuffer& execbuffer,
                                     const VirtGpuResource* blob) {
    if (!isReady()) return -1;

    // Fences and ring indices aren't supported yet, so the flags are ignored.
    // Callers that wait use VirtGpuResource::wait(), which flushes.
    std::vector<uint8_t> buffer(sizeof(magma_virtio_gpu_cmd_submit) + execbuffer.command_size);
    auto* submit = reinterpret_cast<magma_virtio_gpu_cmd_submit*>(buffer.data());
    submit->hdr.type = kMagmaVirtioGpuCmdSubmit3d;
    submit->size = execbuffer.command_size;
    submit->num_in_fences = 0;
    memcpy(buffer.data() + sizeof(*submit), execbuffer.command, execbuffer.command_size);
    return sendCommand(buffer.data(), buffer.size());
}

struct VirtGpuCaps FuchsiaVirtGpuDevice::getCaps(void) { return mCaps; }

VirtGpuDevice* createPlatformVirtGpuDevice(enum VirtGpuCapset capset, int fd) {
    // We don't handle the VirtioGpuPipeStream case.
    if (fd >= 0) {
        ALOGE("Fuchsia: fd not handled");
        abort();
        return nullptr;
    }

    // The Vulkan loader exposes magma devices as instances of the
    // fuchsia.gpu.magma.Service service under svc/magma. Each instance
    // directory holds the device connection in its "device" member. The
    // loader no longer serves the legacy devfs "class/gpu" directory.
    const char kMagmaServiceDir[] = "/loader-gpu-devices/svc/magma";

    // The Vulkan loader's open callback only opens a /loader-gpu-devices path
    // as a directory if the path ends with '/' (or is one of the legacy
    // /class/<name> paths). Any other path is connected to as a service, and
    // can't be enumerated. See OpenAsDirectory() in the Vulkan loader's
    // loader_extensions_fuchsia.cc.
    const char kMagmaServiceDirToList[] = "/loader-gpu-devices/svc/magma/";

    struct os_dirent* de;
    os_dir_t* dir = os_opendir(kMagmaServiceDirToList);
    if (!dir) {
        ALOGE("Error opening %s", kMagmaServiceDir);
        return nullptr;
    }

    ALOGD("Opened dir %s", kMagmaServiceDir);

    VirtGpuDevice* gpu_device = nullptr;

    while ((de = os_readdir(dir)) != NULL) {
        ALOGD("Got name %s", de->d_name);

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        // Room for "<dir>/<instance>/device" plus the null terminator.
        char name[sizeof(kMagmaServiceDir) + 1 + sizeof(de->d_name) + sizeof("/device")];
        snprintf(name, sizeof(name), "%s/%s/device", kMagmaServiceDir, de->d_name);

        zx_handle_t device_channel = GetConnectToServiceFunction()(name);
        if (device_channel == ZX_HANDLE_INVALID) {
            ALOGE("Failed to open device: %s", name);
            continue;
        }

        magma_device_t magma_device;
        magma_status_t status = magma_device_import(device_channel, &magma_device);
        if (status != MAGMA_STATUS_OK) {
            ALOGE("magma_device_import failed: %d", status);
            continue;
        }

        gpu_device = new FuchsiaVirtGpuDevice(capset, magma_device);
        break;
    }
    os_closedir(dir);

    return gpu_device;
}
