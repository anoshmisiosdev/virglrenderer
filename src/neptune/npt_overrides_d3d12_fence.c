/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D12 fence dispatch overrides.  Both Signal entry points feed the
 * feedback substrate so the guest can read a fence's value out of a
 * shared slot instead of over the wire: ID3D12CommandQueue::Signal is
 * the GPU-timeline advance, ID3D12Fence::Signal the CPU-side one.
 */

#include "npt_com.h"
#include "npt_context.h"
#include "npt_event.h"
#include "npt_feedback.h"
#include "npt_overrides.h"

#include "neptune-protocol/npt_protocol_host_dispatch_types.h"
#include "neptune-protocol/npt_protocol_host_id3d12commandqueue.h"
#include "neptune-protocol/npt_protocol_host_id3d12fence.h"

/* ---- GATE_WAIT bridge ---------------------------------------------------- */

void
npt_d3d12_gate_addref(void *fence)
{
   npt_com_add_ref(fence);
}

void
npt_d3d12_gate_release(void *fence)
{
   npt_com_release(fence);
}

bool
npt_d3d12_gate_seoc(void *fence, uint64_t value, void *signal_handle)
{
   /* signal_handle follows the same convention as the event-proxy path:
    * whatever npt_event's event_fd_signal_handle produces for this
    * platform. */
   PFN_ID3D12Fence_SetEventOnCompletion seoc = NPT_COM_VTBL_FUNC(
      PFN_ID3D12Fence_SetEventOnCompletion, npt_com_vtable(fence),
      NPT_VTBL_ID3D12Fence_SetEventOnCompletion);
   return NPT_SUCCEEDED(seoc(fence, value, (HANDLE)signal_handle));
}

bool
npt_d3d12_gate_reached(void *fence, uint64_t value)
{
   PFN_ID3D12Fence_GetCompletedValue get = NPT_COM_VTBL_FUNC(
      PFN_ID3D12Fence_GetCompletedValue, npt_com_vtable(fence),
      NPT_VTBL_ID3D12Fence_GetCompletedValue);
   return get(fence) >= value;
}

static HRESULT
npt_override_ID3D12CommandQueue_Signal(
   struct npt_dispatch_context *dctx,
   struct npt_command_ID3D12CommandQueue_Signal *args,
   PFN_ID3D12CommandQueue_Signal original)
{
   args->ret = original(args->_self, args->pFence, args->Value);

   if (NPT_SUCCEEDED(args->ret) && args->pFence) {
      npt_feedback_fence_mark_signal(npt_context_from_dispatch(dctx),
                                     args->pFence, args->Value);
   }
   return args->ret;
}

static HRESULT
npt_override_ID3D12Fence_Signal(
   struct npt_dispatch_context *dctx,
   struct npt_command_ID3D12Fence_Signal *args,
   PFN_ID3D12Fence_Signal original)
{
   args->ret = original(args->_self, args->Value);

   if (NPT_SUCCEEDED(args->ret)) {
      npt_feedback_fence_mark_signal(npt_context_from_dispatch(dctx),
                                     args->_self, args->Value);
   }
   return args->ret;
}

struct npt_dispatch_id3d12commandqueue_overrides
npt_id3d12commandqueue_overrides = {
   .Signal = npt_override_ID3D12CommandQueue_Signal,
};

struct npt_dispatch_id3d12fence_overrides npt_id3d12fence_overrides = {
   .Signal = npt_override_ID3D12Fence_Signal,
};
