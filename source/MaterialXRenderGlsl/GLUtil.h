//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#ifndef MATERIALX_GLUTIL_H
#define MATERIALX_GLUTIL_H

/// @file
/// OpenGL utilities

#include <MaterialXRenderGlsl/Export.h>

#include <MaterialXCore/Library.h>

MATERIALX_NAMESPACE_BEGIN

MX_RENDERGLSL_API void checkGlErrors(const string& context);

// Minimal EGL headless context wrapper (desktop GL via EGLDevice/Pbuffer).
// Not tied to window systems; sized Pbuffer is typically 1x1.
struct EglHeadlessContext
{
	void* display = nullptr;
	void* context = nullptr;
	void* surface = nullptr;
};

// Create a headless EGL desktop GL context with a tiny Pbuffer surface.
// Returns true on success; leaves context current.
MX_RENDERGLSL_API bool createEglHeadlessContext(EglHeadlessContext& ctx,
								int pbufferWidth = 1,
								int pbufferHeight = 1,
								int deviceIndex = 0);

// Destroy the EGL headless context (safe to call on a default-initialized ctx).
MX_RENDERGLSL_API void destroyEglHeadlessContext(EglHeadlessContext& ctx);

MATERIALX_NAMESPACE_END

#endif
