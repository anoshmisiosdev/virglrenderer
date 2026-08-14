/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * ID3D12Device::CheckFeatureSupport override.
 *
 * Root cause of the long-standing hang: CheckFeatureSupport's payload
 * (pFeatureSupportData/FeatureSupportDataSize) is marshalled generically
 * as an opaque byte blob -- correct for the vast majority of
 * D3D12_FEATURE_DATA_* structs, which are flat (no pointer members).
 * Three of them are NOT flat though:
 *
 *   - D3D12_FEATURE_DATA_FEATURE_LEVELS::pFeatureLevelsRequested
 *   - D3D12_FEATURE_DATA_QUERY_META_COMMAND::pQueryInputData / pQueryOutputData
 *   - D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_TYPES::pTypes
 *
 * The generic blob transport copies the STRUCT's own bytes verbatim,
 * which includes these pointer fields as raw 8-byte values -- but a
 * pointer is only meaningful in the address space it was written in.
 * The host's generic dispatch (npt_dispatch_ID3D12Device_CheckFeatureSupport
 * in npt_protocol_host_id3d12device.h) hands that struct straight to
 * the real vkd3d-proton CheckFeatureSupport, which for
 * D3D12_FEATURE_FEATURE_LEVELS dereferences pFeatureLevelsRequested to
 * read the caller's requested level array -- except that pointer is a
 * *guest* virtual address, meaningless in the host render_server
 * process. This was confirmed directly: instrumented dispatch showed
 * CheckFeatureSupport entering the real call and never returning
 * (guest hang), while `journalctl -k` for the same moment showed
 *
 *   npt-ring-N[pid]: segfault at 7ffffe2ffdf0 ip ... in
 *   libvkd3d-proton-d3d12core.so ... error 4
 *
 * -- a read fault at exactly the guest stack address the test's
 * D3D_FEATURE_LEVEL array lived at, inside vkd3d-proton's own
 * CheckFeatureSupport implementation. The render_server worker thread
 * servicing that ring dies without ever writing a reply, so the
 * guest's blocking wait for the reply spins forever: a hang, not a
 * crash, from the guest's point of view.
 *
 * Fix for FEATURE_LEVELS (the one real games/runtimes actually call,
 * usually right after device creation): reroute through a matching
 * guest-side override (npt_overrides_d3d12_device.c) that repacks the
 * request into a self-contained wire blob -- a small header
 * (NumFeatureLevels + MaxSupportedFeatureLevel) followed by the
 * requested D3D_FEATURE_LEVEL array *inline*, with no pointer at all.
 * This host override unpacks that blob, builds a real
 * D3D12_FEATURE_DATA_FEATURE_LEVELS whose pFeatureLevelsRequested
 * points at the array embedded in the SAME host-local buffer (a valid
 * host address), calls the real entry point, then writes the result
 * back into the blob's header so the existing generic reply-encode
 * path (unmodified) carries it back to the guest.
 *
 * QUERY_META_COMMAND and PROTECTED_RESOURCE_SESSION_TYPES get the same
 * treatment in spirit but not in full: both are exotic corners of the
 * API (vendor meta-commands; protected/DRM playback session
 * enumeration) that nothing in this project's test surface exercises,
 * so rather than build full array marshalling for two features that
 * may never be called, this override fails them safely with
 * E_INVALIDARG *before* reaching the real entry point -- closing off
 * the same crash class without pretending they're implemented. If a
 * real workload needs either, extend this file using the
 * FEATURE_LEVELS case as the template.
 */

#include <string.h>

#include "npt_cs.h"
#include "npt_overrides.h"

#include "neptune-protocol/npt_protocol_host_id3d12device.h"

/* Must match the guest-side layout exactly -- see
 * npt_overrides_d3d12_device.c (mesa) struct
 * npt_wire_feature_levels_hdr. */
struct npt_wire_feature_levels_hdr {
   UINT NumFeatureLevels;
   D3D_FEATURE_LEVEL MaxSupportedFeatureLevel;
   /* D3D_FEATURE_LEVEL levels[NumFeatureLevels] follows inline */
};

HRESULT
npt_d3d12_device_CheckFeatureSupport_override(
   struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12Device_CheckFeatureSupport *args,
   PFN_ID3D12Device_CheckFeatureSupport original)
{
   (void)ctx;

   if (args->Feature == D3D12_FEATURE_QUERY_META_COMMAND ||
       args->Feature == D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPES) {
      /* See file header: pointer-bearing feature-data structs we
       * don't marshal yet. Fail before touching pFeatureSupportData
       * at all -- never call `original` with the raw guest-blob
       * struct for these. */
      args->ret = NPT_E_INVALIDARG;
      return args->ret;
   }

   if (args->Feature != D3D12_FEATURE_FEATURE_LEVELS) {
      /* Every other D3D12_FEATURE_DATA_* struct in current use is
       * flat (no pointer members) -- safe to forward the blob as-is,
       * unchanged from the pre-existing generic behavior. */
      args->ret = original(args->_self, args->Feature,
                           args->pFeatureSupportData,
                           args->FeatureSupportDataSize);
      return args->ret;
   }

   struct npt_wire_feature_levels_hdr *wire = args->pFeatureSupportData;
   if (!wire || args->FeatureSupportDataSize < sizeof(*wire)) {
      args->ret = NPT_E_INVALIDARG;
      return args->ret;
   }

   const UINT n = wire->NumFeatureLevels;
   const size_t expect_size =
      sizeof(*wire) + (size_t)n * sizeof(D3D_FEATURE_LEVEL);
   if (n == 0 || args->FeatureSupportDataSize < expect_size) {
      args->ret = NPT_E_INVALIDARG;
      return args->ret;
   }

   D3D12_FEATURE_DATA_FEATURE_LEVELS real;
   memset(&real, 0, sizeof(real));
   real.NumFeatureLevels = n;
   real.pFeatureLevelsRequested = (const D3D_FEATURE_LEVEL *)(wire + 1);

   args->ret = original(args->_self, D3D12_FEATURE_FEATURE_LEVELS,
                        &real, sizeof(real));
   /* Written back into the same buffer the generic reply-encode path
    * (npt_encode_ID3D12Device_CheckFeatureSupport_reply) echoes to
    * the guest unmodified -- no protocol/decoder changes needed. */
   wire->MaxSupportedFeatureLevel = real.MaxSupportedFeatureLevel;
   return args->ret;
}

/* npt_id3d12device_overrides itself (the struct instance) is defined
 * in npt_overrides_shared_handle.c alongside the shared-HANDLE reject
 * overrides -- that designated initializer wires this function in as
 * its .CheckFeatureSupport slot, keeping the single point of truth
 * for that global's contents. */
