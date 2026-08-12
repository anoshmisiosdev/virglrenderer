/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef NPT_LIBRARY_NAMES_H
#define NPT_LIBRARY_NAMES_H

/*
 * Env-var names and default names of the host D3D backend libraries.
 * Kept free of includes and separate from npt_library.h because the
 * capset probe needs them in proxy-only builds, which must not link the
 * renderer.
 *
 * On darwin all three slots name one backend umbrella, dlopened and
 * dlsym'd at runtime: libd3dmetal-native on the x86_64 slice,
 * libdxmt-native on the arm64 slice.  DXMT has no D3D12 entry point, so
 * that slot's dlsym fails and the D3D12 paths degrade cleanly.
 */

#define NPT_D3D11_LIBRARY_ENV "NPT_D3D11_LIBRARY_PATH"
#define NPT_DXGI_LIBRARY_ENV  "NPT_DXGI_LIBRARY_PATH"
#define NPT_D3D12_LIBRARY_ENV "NPT_D3D12_LIBRARY_PATH"

#if defined(__APPLE__) && defined(__aarch64__)
#define NPT_D3D11_LIBRARY_DEFAULT "libdxmt-native.dylib"
#define NPT_DXGI_LIBRARY_DEFAULT  "libdxmt-native.dylib"
#define NPT_D3D12_LIBRARY_DEFAULT "libdxmt-native.dylib"
#elif defined(__APPLE__)
#define NPT_D3D11_LIBRARY_DEFAULT "libd3dmetal-native.dylib"
#define NPT_DXGI_LIBRARY_DEFAULT  "libd3dmetal-native.dylib"
#define NPT_D3D12_LIBRARY_DEFAULT "libd3dmetal-native.dylib"
#else
#define NPT_D3D11_LIBRARY_DEFAULT "libd3d11.so"
#define NPT_DXGI_LIBRARY_DEFAULT  "libdxgi.so"
#define NPT_D3D12_LIBRARY_DEFAULT "libvkd3d-proton-d3d12.so"
#endif

#endif /* NPT_LIBRARY_NAMES_H */
