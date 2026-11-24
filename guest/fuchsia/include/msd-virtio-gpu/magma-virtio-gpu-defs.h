// Copyright 2024 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_DRIVERS_MSD_VIRTIO_GPU_INCLUDE_MAGMA_VIRTIO_GPU_DEFS_H_
#define SRC_GRAPHICS_DRIVERS_MSD_VIRTIO_GPU_INCLUDE_MAGMA_VIRTIO_GPU_DEFS_H_

#include <lib/magma/magma_common_defs.h>
#include <stdint.h>

#define MAGMA_VENDOR_VERSION_VIRTIO 1

enum MagmaVirtioGpuQuery {
  // Bits 32..47 indicate the capset id (from virtio spec), bits 48..63 indicate the version.
  // Returns buffer result.
  kMagmaVirtioGpuQueryCapset = MAGMA_QUERY_VENDOR_PARAM_0 + 10000,

  // Allocates a device-wide unique virtio-gpu resource id. Returns value result.
  // Ids come from a range the display driver doesn't use.
  kMagmaVirtioGpuQueryAllocResourceId = MAGMA_QUERY_VENDOR_PARAM_0 + 10001,

  // Bits 32..63 hold a resource id created with a mappable RESOURCE_CREATE_BLOB.
  // The MSD picks an offset in the host-visible region, sends
  // VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB, and returns a VMO slice covering the blob.
  // Returns buffer result. Call magma_connection_flush() first, so the
  // RESOURCE_CREATE_BLOB inline command has been processed.
  kMagmaVirtioGpuQueryMapBlob = MAGMA_QUERY_VENDOR_PARAM_0 + 10002,
};

// Inline commands (magma_connection_execute_inline_commands) carry one raw
// virtio-gpu control command each. The MSD stamps the context id into the
// header, clears fence flags, and forwards it. Allowed commands:
//   RESOURCE_UNREF, RESOURCE_CREATE_BLOB (host blobs only, no guest pages),
//   CTX_ATTACH_RESOURCE, SUBMIT_3D.
// The MSD itself owns CTX_CREATE/CTX_DESTROY and RESOURCE_MAP/UNMAP_BLOB.

#define MAGMA_VIRTIO_GPU_MAX_INLINE_COMMAND_SIZE (60 * 1024)

// Returns the 2D resource (a gfxstream host ColorBuffer, handle = resource id)
// that backs a sysmem buffer, and takes a reference on it. The reference is
// dropped when the context is destroyed. The MSD handles this request itself.
//
// Submitted with magma_connection_execute_command() on a context:
// - The descriptor has exactly one command buffer, and no semaphores.
// - The command buffer points at a magma_virtio_gpu_color_buffer_request
//   inside a magma buffer created by the client. The MSD writes the response
//   fields back into that buffer; read them after magma_connection_flush().
// - The sysmem VMO is imported with magma_connection_import_buffer() and listed
//   in the descriptor's resources at `vmo_resource_index`.
//
// The display driver shares the same resource for the buffer, and stops
// uploading the buffer's guest pages to it. With `create` == 0, fails with
// ZX_ERR_NOT_FOUND when no resource exists yet.
struct magma_virtio_gpu_color_buffer_request {
  // Index of the sysmem VMO in the descriptor's resources.
  uint32_t vmo_resource_index;
  // Geometry used if the resource is created. `pixel_format` is a
  // fuchsia.images2.PixelFormat value (B8G8R8A8 or R8G8B8A8).
  uint32_t width;
  uint32_t height;
  uint32_t pixel_format;
  uint32_t create;
  // Out.
  uint32_t resource_id;
  // Out: a zx_status_t.
  int32_t status;
};

enum MagmaVirtioGpuCommand {
  kMagmaVirtioGpuCmdResourceUnref = 0x0102,
  kMagmaVirtioGpuCmdResourceCreateBlob = 0x010c,
  kMagmaVirtioGpuCmdCtxCreate = 0x0200,
  kMagmaVirtioGpuCmdCtxDestroy = 0x0201,
  kMagmaVirtioGpuCmdCtxAttachResource = 0x0202,
  kMagmaVirtioGpuCmdSubmit3d = 0x0207,
  kMagmaVirtioGpuCmdResourceMapBlob = 0x0208,
  kMagmaVirtioGpuCmdResourceUnmapBlob = 0x0209,

  kMagmaVirtioGpuRespOkNoData = 0x1100,
  kMagmaVirtioGpuRespOkMapInfo = 0x1106,
};

#define MAGMA_VIRTIO_GPU_FLAG_FENCE (1u << 0)
#define MAGMA_VIRTIO_GPU_FLAG_INFO_RING_IDX (1u << 1)

#define MAGMA_VIRTIO_GPU_BLOB_MEM_HOST3D 0x2
#define MAGMA_VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE 0x1

// Wire layouts from the virtio spec (5.7.6), little endian.
struct magma_virtio_gpu_ctrl_hdr {
  uint32_t type;
  uint32_t flags;
  uint64_t fence_id;
  uint32_t ctx_id;
  uint8_t ring_idx;
  uint8_t padding[3];
};

struct magma_virtio_gpu_resource_unref {
  struct magma_virtio_gpu_ctrl_hdr hdr;
  uint32_t resource_id;
  uint32_t padding;
};

struct magma_virtio_gpu_resource_create_blob {
  struct magma_virtio_gpu_ctrl_hdr hdr;
  uint32_t resource_id;
  uint32_t blob_mem;
  uint32_t blob_flags;
  uint32_t nr_entries;
  uint64_t blob_id;
  uint64_t size;
};

struct magma_virtio_gpu_ctx_create {
  struct magma_virtio_gpu_ctrl_hdr hdr;
  uint32_t nlen;
  uint32_t context_init;  // Low 8 bits: capset id.
  char debug_name[64];
};

struct magma_virtio_gpu_ctx_resource {
  struct magma_virtio_gpu_ctrl_hdr hdr;
  uint32_t resource_id;
  uint32_t padding;
};

// Followed by `size` bytes of context-specific command stream.
struct magma_virtio_gpu_cmd_submit {
  struct magma_virtio_gpu_ctrl_hdr hdr;
  uint32_t size;
  uint32_t num_in_fences;
};

struct magma_virtio_gpu_resource_map_blob {
  struct magma_virtio_gpu_ctrl_hdr hdr;
  uint32_t resource_id;
  uint32_t padding;
  uint64_t offset;
};

struct magma_virtio_gpu_resource_unmap_blob {
  struct magma_virtio_gpu_ctrl_hdr hdr;
  uint32_t resource_id;
  uint32_t padding;
};

#ifdef __cplusplus
static_assert(sizeof(magma_virtio_gpu_ctrl_hdr) == 24, "virtio-gpu header layout");
static_assert(sizeof(magma_virtio_gpu_resource_create_blob) == 56, "create_blob layout");
static_assert(sizeof(magma_virtio_gpu_ctx_create) == 96, "ctx_create layout");
static_assert(sizeof(magma_virtio_gpu_cmd_submit) == 32, "submit_3d layout");
static_assert(sizeof(magma_virtio_gpu_resource_map_blob) == 40, "map_blob layout");
static_assert(sizeof(magma_virtio_gpu_color_buffer_request) == 28, "color buffer request layout");
#endif

#endif  // SRC_GRAPHICS_DRIVERS_MSD_VIRTIO_GPU_INCLUDE_MAGMA_VIRTIO_GPU_DEFS_H_
