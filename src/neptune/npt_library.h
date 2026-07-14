/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef NPT_LIBRARY_H
#define NPT_LIBRARY_H

#include "npt_common.h"
#include "neptune-protocol/npt_protocol_host_dispatch_types.h"

/*
 * Loads the host D3D backend libraries.  Path lookup: NPT_*_LIBRARY_PATH
 * env var if set, otherwise the built-in default name.
 */

/* Host backend workaround flag bits, host side.  Derived at backend-library
 * load from the loaded backend's needs and written once -- ORed with
 * NPT_WA_FLAGS_PRESENT so the guest can tell the word was written -- into the
 * ring blob at npt_cmd_create_ring.workaround_offset.  Each bit names a
 * shader/cap patch the guest applies ONLY when the bit is set; a backend that
 * needs none leaves them all clear.  These compensate for a specific host
 * backend's defects, not general correctness.  The guest defines the same wire
 * values in its own npt_workaround.h; the two are matched by review, not by a
 * shared header. */
#define NPT_WA_FLAGS_PRESENT                     (1u << 31) /* host wrote the word */
#define NPT_WA_WIDEN_SCALAR_VS_INPUT_MASK        (1u << 0)  /* scalar .x IA VS input -> .xy */
#define NPT_WA_TYPE_VS_INPUT_FROM_VERTEX_FORMAT  (1u << 1)  /* VS ISGN comp type from bound format */
#define NPT_WA_LINEARIZE_NOPERSPECTIVE_PS_INPUT  (1u << 2)  /* dcl_input_ps noperspective -> linear */
#define NPT_WA_SYNTHESIZE_IO_SIGNATURE_FROM_SHDR (1u << 3)  /* empty GS ISGN/OSGN from SHDR DCLs */
struct npt_d3d_library {
   void *d3d11_module;
   void *dxgi_module;
   void *d3d12_module;

   PFN_D3D11CreateDevice pfn_D3D11CreateDevice;
   PFN_D3D11On12CreateDevice pfn_D3D11On12CreateDevice;

   PFN_CreateDXGIFactory1 pfn_CreateDXGIFactory1;

   PFN_D3D12CreateDevice pfn_D3D12CreateDevice;

   /* NPT_WA_* bits describing the workarounds the loaded backend needs, set in
    * npt_library_init. Reported to the guest per-context (via the ring blob) so
    * Triton gates host-backend-specific shader/cap patches on them. */
   uint32_t workaround_flags;
};

bool
npt_library_init(struct npt_d3d_library *lib);

void
npt_library_fini(struct npt_d3d_library *lib);

#endif /* NPT_LIBRARY_H */
