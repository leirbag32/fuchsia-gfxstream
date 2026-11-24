/*
 * Copyright 2023 Google LLC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cutils/log.h>
#include <fbl/no_destructor.h>
#include <fidl/fuchsia.logger/cpp/wire.h>
#include <lib/syslog/structured_backend/cpp/log_buffer.h>
#include <lib/syslog/structured_backend/cpp/logger.h>
#include <lib/syslog/structured_backend/fuchsia_syslog.h>
#include <lib/zx/channel.h>
#include <lib/zx/socket.h>
#include <lib/zxio/zxio.h>
#include <unistd.h>
#include <zircon/process.h>
#include <cstdarg>

#include "ResourceTracker.h"
#include "TraceProviderFuchsia.h"
#include "gfxstream_vk_entrypoints.h"
#include "gfxstream_vk_private.h"
#include "services/service_connector.h"

namespace {

fbl::NoDestructor<fuchsia_logging::Logger> logger;

typedef VkResult(VKAPI_PTR* PFN_vkOpenInNamespaceAddr)(const char* pName, uint32_t handle);

PFN_vkOpenInNamespaceAddr g_vulkan_connector;

zx_koid_t GetKoid(zx_handle_t handle) {
    zx_info_handle_basic_t info;
    zx_status_t status =
        zx_object_get_info(handle, ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
    return status == ZX_OK ? info.koid : ZX_KOID_INVALID;
}

static zx_koid_t pid = GetKoid(zx_process_self());

static thread_local zx_koid_t tid = GetKoid(zx_thread_self());

std::optional<std::string_view> CStringToStringView(const char* cstr) {
    if (!cstr) {
        return std::nullopt;
    }
    return cstr;
}

const char* StripDots(const char* path) {
    while (strncmp(path, "../", 3) == 0) {
        path += 3;
    }
    return path;
}

const char* StripPath(const char* path) {
    auto p = strrchr(path, '/');
    if (p) {
        return p + 1;
    } else {
        return path;
    }
}

const char* StripFile(const char* file, FuchsiaLogSeverity severity) {
    return severity > FUCHSIA_LOG_INFO ? StripDots(file) : StripPath(file);
}

extern "C" void gfxstream_fuchsia_log(int8_t severity, const char* tag, const char* file, int line,
                                      const char* format, va_list va) {
    if (!logger->IsValid()) {
        // No syslog connection in this process.
        return;
    }
    fuchsia_logging::LogBuffer buffer;
    constexpr size_t kFormatStringLength = 1024;
    char fmt_string[kFormatStringLength];
    fmt_string[kFormatStringLength - 1] = 0;
    int n = kFormatStringLength;
    // Format
    // Number of bytes written not including null terminator
    int count = vsnprintf(fmt_string, n, format, va) + 1;
    if (count < 0) {
        // No message to write.
        return;
    }

    if (count >= n) {
        // truncated
        constexpr char kEllipsis[] = "...";
        constexpr size_t kEllipsisSize = sizeof(kEllipsis);
        snprintf(fmt_string + kFormatStringLength - 1 - kEllipsisSize, kEllipsisSize, kEllipsis);
    }

    if (file) {
        file = StripFile(file, severity);
    }
    buffer.BeginRecord(severity, CStringToStringView(file), line, fmt_string, 0, pid, tid);
    if (tag) {
        buffer.WriteKeyValue("tag", tag);
    }
    logger->FlushBuffer(buffer);
}

zx_handle_t LocalConnectToServiceFunction(const char* pName) {
    zx::channel remote_endpoint, local_endpoint;
    zx_status_t status;
    if ((status = zx::channel::create(0, &remote_endpoint, &local_endpoint)) != ZX_OK) {
        ALOGE("zx::channel::create failed: %d", status);
        return ZX_HANDLE_INVALID;
    }
    if ((status = g_vulkan_connector(pName, remote_endpoint.release())) != ZX_OK) {
        ALOGE("vulkan_connector failed: %d", status);
        return ZX_HANDLE_INVALID;
    }
    return local_endpoint.release();
}

}  // namespace

class VulkanDevice {
   public:
    VulkanDevice() : mHostSupportsGoldfish(IsAccessible(QEMU_PIPE_PATH)) {
        InitTraceProvider();
        gfxstream::vk::ResourceTracker::get();
    }

    static void InitLogger();

    static bool IsAccessible(const char* name) {
        zx_handle_t handle = GetConnectToServiceFunction()(name);
        if (handle == ZX_HANDLE_INVALID) return false;

        zxio_storage_t io_storage;
        if (zx_status_t status = zxio_create(handle, &io_storage); status != ZX_OK) {
            return false;
        }

        zxio_destroy(&io_storage.io);

        return true;
    }

    static VulkanDevice& GetInstance() {
        static VulkanDevice g_instance;
        return g_instance;
    }

   private:
    void InitTraceProvider();

    TraceProviderFuchsia mTraceProvider;
    const bool mHostSupportsGoldfish;
};

void VulkanDevice::InitLogger() {
    auto result = fuchsia_logging::Logger::Create(fuchsia_logging::RawLogSettings{
        .log_sink = GetConnectToServiceFunction()("/svc/fuchsia.logger.LogSink"),
    });
    if (result.is_error()) {
        return;
    }
    *logger = *std::move(result);
}

void VulkanDevice::InitTraceProvider() {
    if (!mTraceProvider.Initialize()) {
        ALOGE("Trace provider failed to initialize");
    }
}

extern "C" __attribute__((visibility("default"))) void vk_icdInitializeOpenInNamespaceCallback(
    PFN_vkOpenInNamespaceAddr callback) {
    g_vulkan_connector = callback;
    SetConnectToServiceFunction(&LocalConnectToServiceFunction);

    VulkanDevice::InitLogger();

    ALOGV("Gfxstream on Fuchsia initialized");
}

// Entry points for the VK_FUCHSIA_* extensions.
//
// codegen/.../cereal/functable.py lists these in RESOURCE_TRACKER_ENTRIES, but
// the checked-in vulkan_enc/func_table.cpp was generated without the Fuchsia
// extensions. Without these definitions the weak references in
// gfxstream_vk_entrypoints.h stay NULL, and the device dispatch table hands
// NULL function pointers to apps (Scenic jumps to PC 0). They follow the
// pattern of the generated wrappers: unwrap gfxstream handles, then forward to
// ResourceTracker. VkDeviceMemory and VkBufferCollectionFUCHSIA aren't wrapped
// by gfxstream; VkDevice and VkSemaphore are.

VkResult gfxstream_vk_GetMemoryZirconHandleFUCHSIA(
    VkDevice device, const VkMemoryGetZirconHandleInfoFUCHSIA* pGetZirconHandleInfo,
    zx_handle_t* pZirconHandle) {
    VK_FROM_HANDLE(gfxstream_vk_device, gfxstream_device, device);
    auto vkEnc = gfxstream::vk::ResourceTracker::getThreadLocalEncoder();
    return gfxstream::vk::ResourceTracker::get()->on_vkGetMemoryZirconHandleFUCHSIA(
        vkEnc, VK_SUCCESS, gfxstream_device->internal_object, pGetZirconHandleInfo,
        pZirconHandle);
}

VkResult gfxstream_vk_GetMemoryZirconHandlePropertiesFUCHSIA(
    VkDevice device, VkExternalMemoryHandleTypeFlagBits handleType, zx_handle_t zirconHandle,
    VkMemoryZirconHandlePropertiesFUCHSIA* pMemoryZirconHandleProperties) {
    VK_FROM_HANDLE(gfxstream_vk_device, gfxstream_device, device);
    auto vkEnc = gfxstream::vk::ResourceTracker::getThreadLocalEncoder();
    return gfxstream::vk::ResourceTracker::get()->on_vkGetMemoryZirconHandlePropertiesFUCHSIA(
        vkEnc, VK_SUCCESS, gfxstream_device->internal_object, handleType, zirconHandle,
        pMemoryZirconHandleProperties);
}

VkResult gfxstream_vk_GetSemaphoreZirconHandleFUCHSIA(
    VkDevice device, const VkSemaphoreGetZirconHandleInfoFUCHSIA* pGetZirconHandleInfo,
    zx_handle_t* pZirconHandle) {
    VK_FROM_HANDLE(gfxstream_vk_device, gfxstream_device, device);
    auto vkEnc = gfxstream::vk::ResourceTracker::getThreadLocalEncoder();
    VkSemaphoreGetZirconHandleInfoFUCHSIA internal_info = *pGetZirconHandleInfo;
    VK_FROM_HANDLE(gfxstream_vk_semaphore, gfxstream_semaphore, internal_info.semaphore);
    internal_info.semaphore = gfxstream_semaphore->internal_object;
    return gfxstream::vk::ResourceTracker::get()->on_vkGetSemaphoreZirconHandleFUCHSIA(
        vkEnc, VK_SUCCESS, gfxstream_device->internal_object, &internal_info, pZirconHandle);
}

VkResult gfxstream_vk_ImportSemaphoreZirconHandleFUCHSIA(
    VkDevice device,
    const VkImportSemaphoreZirconHandleInfoFUCHSIA* pImportSemaphoreZirconHandleInfo) {
    VK_FROM_HANDLE(gfxstream_vk_device, gfxstream_device, device);
    auto vkEnc = gfxstream::vk::ResourceTracker::getThreadLocalEncoder();
    VkImportSemaphoreZirconHandleInfoFUCHSIA internal_info = *pImportSemaphoreZirconHandleInfo;
    VK_FROM_HANDLE(gfxstream_vk_semaphore, gfxstream_semaphore, internal_info.semaphore);
    internal_info.semaphore = gfxstream_semaphore->internal_object;
    return gfxstream::vk::ResourceTracker::get()->on_vkImportSemaphoreZirconHandleFUCHSIA(
        vkEnc, VK_SUCCESS, gfxstream_device->internal_object, &internal_info);
}

VkResult gfxstream_vk_CreateBufferCollectionFUCHSIA(
    VkDevice device, const VkBufferCollectionCreateInfoFUCHSIA* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkBufferCollectionFUCHSIA* pCollection) {
    VK_FROM_HANDLE(gfxstream_vk_device, gfxstream_device, device);
    auto vkEnc = gfxstream::vk::ResourceTracker::getThreadLocalEncoder();
    return gfxstream::vk::ResourceTracker::get()->on_vkCreateBufferCollectionFUCHSIA(
        vkEnc, VK_SUCCESS, gfxstream_device->internal_object, pCreateInfo, pAllocator,
        pCollection);
}

void gfxstream_vk_DestroyBufferCollectionFUCHSIA(VkDevice device,
                                                 VkBufferCollectionFUCHSIA collection,
                                                 const VkAllocationCallbacks* pAllocator) {
    VK_FROM_HANDLE(gfxstream_vk_device, gfxstream_device, device);
    auto vkEnc = gfxstream::vk::ResourceTracker::getThreadLocalEncoder();
    gfxstream::vk::ResourceTracker::get()->on_vkDestroyBufferCollectionFUCHSIA(
        vkEnc, VK_SUCCESS, gfxstream_device->internal_object, collection, pAllocator);
}

VkResult gfxstream_vk_SetBufferCollectionBufferConstraintsFUCHSIA(
    VkDevice device, VkBufferCollectionFUCHSIA collection,
    const VkBufferConstraintsInfoFUCHSIA* pBufferConstraintsInfo) {
    VK_FROM_HANDLE(gfxstream_vk_device, gfxstream_device, device);
    auto vkEnc = gfxstream::vk::ResourceTracker::getThreadLocalEncoder();
    return gfxstream::vk::ResourceTracker::get()->on_vkSetBufferCollectionBufferConstraintsFUCHSIA(
        vkEnc, VK_SUCCESS, gfxstream_device->internal_object, collection, pBufferConstraintsInfo);
}

VkResult gfxstream_vk_SetBufferCollectionImageConstraintsFUCHSIA(
    VkDevice device, VkBufferCollectionFUCHSIA collection,
    const VkImageConstraintsInfoFUCHSIA* pImageConstraintsInfo) {
    VK_FROM_HANDLE(gfxstream_vk_device, gfxstream_device, device);
    auto vkEnc = gfxstream::vk::ResourceTracker::getThreadLocalEncoder();
    return gfxstream::vk::ResourceTracker::get()->on_vkSetBufferCollectionImageConstraintsFUCHSIA(
        vkEnc, VK_SUCCESS, gfxstream_device->internal_object, collection, pImageConstraintsInfo);
}

VkResult gfxstream_vk_GetBufferCollectionPropertiesFUCHSIA(
    VkDevice device, VkBufferCollectionFUCHSIA collection,
    VkBufferCollectionPropertiesFUCHSIA* pProperties) {
    VK_FROM_HANDLE(gfxstream_vk_device, gfxstream_device, device);
    auto vkEnc = gfxstream::vk::ResourceTracker::getThreadLocalEncoder();
    return gfxstream::vk::ResourceTracker::get()->on_vkGetBufferCollectionPropertiesFUCHSIA(
        vkEnc, VK_SUCCESS, gfxstream_device->internal_object, collection, pProperties);
}
