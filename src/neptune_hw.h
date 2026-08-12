/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef NEPTUNE_HW_H
#define NEPTUNE_HW_H

#include <stdint.h>

struct virgl_renderer_capset_neptune {
   uint32_t wire_format_version;
   /* VIRGL_RENDERER_CAPSET_NEPTUNE_CAP_* bits.  Guests predating the
    * field ignore it; hosts predating it zero the whole struct, so a
    * clear bit always means "not supported". */
   uint32_t caps_flags;
   uint32_t pad[13]; /* reserved for future use */
};

/* The host has a D3D12 backend, so D3D12CreateDevice can succeed.  A
 * clear bit means the guest must fail device creation locally rather
 * than discover it through a failed wire call. */
#define VIRGL_RENDERER_CAPSET_NEPTUNE_CAP_D3D12 (1u << 0)

#endif /* NEPTUNE_HW_H */
