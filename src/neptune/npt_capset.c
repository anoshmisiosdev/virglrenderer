/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

/* The Neptune capset is a tiny, self-contained protocol descriptor that the
 * proxy front-end advertises to the guest.  It is kept out of npt_renderer.c
 * -- and free of any renderer-backend linkage -- so a proxy-only client build
 * of the library (neptune-mode=client) can report the capset without pulling
 * in the renderer.  That is what lets libvirglrenderer be built natively
 * (e.g. arm64) while the actual rendering runs in an out-of-tree x86_64
 * render server. */

#include "npt_common.h"

#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune_hw.h"
#include "npt_renderer.h"

size_t
npt_get_capset(void *capset, UNUSED uint32_t flags)
{
   struct virgl_renderer_capset_neptune *c = capset;
   if (c) {
      memset(c, 0, sizeof(*c));
      c->wire_format_version = NPT_PROTOCOL_WIRE_VERSION;
   }

   return sizeof(struct virgl_renderer_capset_neptune);
}
