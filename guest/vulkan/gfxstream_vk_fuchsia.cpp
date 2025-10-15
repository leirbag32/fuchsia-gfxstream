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
        abort();
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
    auto logger = fuchsia_logging::Logger::Create(fuchsia_logging::RawLogSettings{
        .log_sink = GetConnectToServiceFunction()("/svc/fuchsia.logger.LogSink"),
    });
    if (logger.is_error()) {
        return;
    }
    *logger = *std::move(logger);
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
