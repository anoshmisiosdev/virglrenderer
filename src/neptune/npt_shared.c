/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D11 shared / presentable textures over virtio-gpu blob resources.
 * See npt_shared.h for the model.
 */

#include "npt_shared.h"

#include <string.h>
#include <time.h>
#include <unistd.h>

#include <dxvk_shared_resource.h>

#include "c11/threads.h"

#include "npt_context.h"
#include "npt_transport_defs.h"

#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_host_dispatch_types.h"

/* D3D11_BIND_SHADER_RESOURCE (d3d11.h); consumers sample the texture. */
#define NPT_D3D11_BIND_SHADER_RESOURCE 0x8u

/* OPEN_RES arrives on the consumer's ring thread; the resource fd
 * arrives on the dispatch thread via the proxy's attach-forwarding
 * (triggered by the guest KMD's CTX_ATTACH_RESOURCE, a virtio ctrl
 * command that is not ordered against ring commands).  Bounded poll
 * bridges the race; the budget stays well under dxgkrnl's ~2 s TDR
 * so a missing attach fails the open instead of wedging the ring. */
#define NPT_SHARED_ATTACH_WAIT_MS   1000
#define NPT_SHARED_ATTACH_POLL_MS   2

HRESULT
npt_shared_export_blob(struct npt_context *ctx, uint64_t texture_id,
                       uint64_t blob_id, uint32_t data_res_id,
                       uint32_t data_off)
{
   if (!texture_id || !blob_id)
      return NPT_E_INVALIDARG;

   void *texture = npt_context_lookup_object(ctx, NULL, texture_id,
                                             NPT_OBJECT_TYPE_ID3D11TEXTURE2D);
   if (!texture) {
      npt_log("shared: export: texture id 0x%016" PRIx64 " not found",
              texture_id);
      return NPT_E_INVALIDARG;
   }

   /* Export the texture's shared descriptor.  The descriptor and its
    * fd are owned by the texture; only copies leave this frame. */
   void *dxgi_res = NULL;
   if (NPT_FAILED(npt_com_query_interface(texture, &NPT_IID_IDXGIResource,
                                          &dxgi_res)) || !dxgi_res) {
      npt_log("shared: export: blob_id %" PRIu64 " has no IDXGIResource",
              blob_id);
      return NPT_E_FAIL;
   }

   PFN_IDXGIResource_GetSharedHandle get_shared =
      NPT_COM_VTBL_FUNC(PFN_IDXGIResource_GetSharedHandle,
                        npt_com_vtable(dxgi_res),
                        NPT_VTBL_IDXGIResource_GetSharedHandle);
   HANDLE handle = 0;
   HRESULT hr = get_shared(dxgi_res, &handle);
   npt_com_release(dxgi_res);

   if (NPT_FAILED(hr) || !handle) {
      npt_log("shared: export: blob_id %" PRIu64
              " GetSharedHandle failed (hr=0x%x)", blob_id, hr);
      return NPT_FAILED(hr) ? hr : NPT_E_FAIL;
   }

   const struct DxvkSharedTextureDescriptor *desc =
      (const struct DxvkSharedTextureDescriptor *)(uintptr_t)handle;
   if (desc->magic != DXVK_SHARED_DESCRIPTOR_TEXTURE ||
       desc->structSize != sizeof(*desc) || desc->fd < 0 ||
       desc->planeCount < 1 ||
       desc->planeCount > NPT_BLOB_EXPORT_MAX_PLANES) {
      npt_log("shared: export: blob_id %" PRIu64 " bad descriptor", blob_id);
      return NPT_E_FAIL;
   }

   if (!(desc->meta.BindFlags & NPT_D3D11_BIND_SHADER_RESOURCE))
      npt_log("shared: export: blob_id %" PRIu64 " texture lacks "
              "SHADER_RESOURCE bind (0x%x); consumers cannot sample it",
              blob_id, desc->meta.BindFlags);

   /* Publish the dmabuf-level facts into the exporter's shmem window. */
   struct npt_resource *data_res = npt_context_get_resource(ctx, data_res_id);
   if (!data_res || data_res->fd_type != VIRGL_RESOURCE_FD_SHM ||
       !data_res->u.data) {
      npt_log("shared: export: data resource %u not found", data_res_id);
      return NPT_E_INVALIDARG;
   }

   struct npt_blob_export_info info;
   memset(&info, 0, sizeof(info));
   info.modifier = desc->drmFormatModifier;
   info.allocation_size = desc->allocationSize;
   info.plane_count = desc->planeCount;
   info.texture_layout = desc->meta.TextureLayout;
   for (uint32_t i = 0; i < desc->planeCount; i++) {
      info.planes[i].offset = desc->planes[i].offset;
      info.planes[i].pitch = desc->planes[i].pitch;
   }

   /* data_off is guest-supplied: bound the write to the mapping. */
   if ((uint64_t)data_off + sizeof(info) > data_res->size) {
      npt_log("shared: export: data_off=%u overruns res size=%zu",
              data_off, data_res->size);
      return NPT_E_INVALIDARG;
   }
   memcpy((uint8_t *)data_res->u.data + data_off, &info, sizeof(info));

   /* Stage the pending blob the guest KMD claims via
    * RESOURCE_CREATE_BLOB(HOST3D, blob_id).  The table takes fd
    * ownership; the texture keeps its own. */
   int fd = dup(desc->fd);
   if (fd < 0) {
      npt_log("shared: export: blob_id %" PRIu64 " dup failed", blob_id);
      return NPT_E_FAIL;
   }
   if (!npt_context_register_pending_blob(ctx, blob_id,
                                          VIRGL_RESOURCE_FD_DMABUF, fd,
                                          desc->allocationSize)) {
      close(fd);
      return NPT_E_FAIL;
   }

   npt_log("shared: exported blob_id=%" PRIu64 " %ux%u fmt=%u mod=0x%016"
           PRIx64 " pitch=%" PRIu64 " (ctx %u)", blob_id, desc->meta.Width,
           desc->meta.Height, desc->meta.Format, desc->drmFormatModifier,
           desc->planes[0].pitch, ctx->ctx_id);
   return NPT_S_OK;
}

HRESULT
npt_shared_open_res(struct npt_context *ctx, uint64_t device_id,
                    const struct npt_cmd_shared_open_res *cmd)
{
   if (!device_id || !cmd->res_id || !cmd->mint_object_id)
      return NPT_E_INVALIDARG;
   if (cmd->export_info.plane_count < 1 ||
       cmd->export_info.plane_count > NPT_BLOB_EXPORT_MAX_PLANES)
      return NPT_E_INVALIDARG;

   void *device = npt_context_lookup_object(ctx, NULL, device_id,
                                            NPT_OBJECT_TYPE_ID3D11DEVICE);
   if (!device) {
      npt_log("shared: open: device id 0x%016" PRIx64 " not found", device_id);
      return NPT_E_INVALIDARG;
   }

   /* Wait for the attach-forwarded resource.  Same-context opens hit
    * immediately (the blob create recorded it). */
   struct npt_resource *res = NULL;
   for (int waited_ms = 0;; waited_ms += NPT_SHARED_ATTACH_POLL_MS) {
      res = npt_context_get_resource(ctx, cmd->res_id);
      if (res || waited_ms >= NPT_SHARED_ATTACH_WAIT_MS)
         break;
      thrd_sleep(&(struct timespec){
                    .tv_nsec = NPT_SHARED_ATTACH_POLL_MS * 1000000L }, NULL);
   }
   if (!res || res->fd_type != VIRGL_RESOURCE_FD_DMABUF || res->u.fd < 0) {
      npt_log("shared: open: res_id %u not attached (found=%d type=%d)",
              cmd->res_id, res != NULL, res ? (int)res->fd_type : -1);
      return NPT_E_INVALIDARG;
   }

   /* Rebuild the exporter's descriptor around our own fd reference. */
   struct DxvkSharedTextureDescriptor desc;
   memset(&desc, 0, sizeof(desc));
   desc.magic = DXVK_SHARED_DESCRIPTOR_TEXTURE;
   desc.version = DXVK_SHARED_DESCRIPTOR_VERSION;
   desc.structSize = sizeof(desc);
   desc.meta.Width = cmd->width;
   desc.meta.Height = cmd->height;
   desc.meta.MipLevels = cmd->mip_levels;
   desc.meta.ArraySize = cmd->array_size;
   desc.meta.Format = cmd->format;
   desc.meta.SampleDesc.Count = cmd->sample_count;
   desc.meta.SampleDesc.Quality = 0;
   desc.meta.Usage = cmd->usage;
   desc.meta.BindFlags = cmd->bind_flags;
   desc.meta.CPUAccessFlags = cmd->cpu_access_flags;
   desc.meta.MiscFlags = cmd->misc_flags;
   desc.meta.TextureLayout = cmd->export_info.texture_layout;
   desc.drmFormatModifier = cmd->export_info.modifier;
   desc.planeCount = cmd->export_info.plane_count;
   for (uint32_t i = 0; i < desc.planeCount; i++) {
      desc.planes[i].offset = cmd->export_info.planes[i].offset;
      desc.planes[i].pitch = cmd->export_info.planes[i].pitch;
   }
   desc.allocationSize = cmd->export_info.allocation_size;

   /* The import dup()s the fd internally; hold our own reference so a
    * concurrent resource destroy can't invalidate res->u.fd mid-call. */
   desc.fd = dup(res->u.fd);
   if (desc.fd < 0)
      return NPT_E_FAIL;

   PFN_ID3D11Device_OpenSharedResource open_shared =
      NPT_COM_VTBL_FUNC(PFN_ID3D11Device_OpenSharedResource,
                        npt_com_vtable(device),
                        NPT_VTBL_ID3D11Device_OpenSharedResource);

   void *texture = NULL;
   HRESULT hr = open_shared(device, (HANDLE)(uintptr_t)&desc,
                            &NPT_IID_ID3D11Texture2D, &texture);
   close(desc.fd);

   if (NPT_FAILED(hr) || !texture) {
      npt_log("shared: open: res_id %u import failed (hr=0x%x)",
              cmd->res_id, hr);
      return NPT_FAILED(hr) ? hr : NPT_E_FAIL;
   }

   /* The freshly imported texture carries one reference; the object
    * table registration is what the guest's minted id releases. */
   npt_context_register_object(ctx, cmd->mint_object_id, texture,
                               NPT_OBJECT_TYPE_ID3D11TEXTURE2D);

   npt_log("shared: opened res_id=%u -> id 0x%016" PRIx64 " (ctx %u)",
           cmd->res_id, cmd->mint_object_id, ctx->ctx_id);
   return NPT_S_OK;
}
