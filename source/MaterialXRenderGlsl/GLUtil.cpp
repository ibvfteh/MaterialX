//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXRenderGlsl/External/Glad/glad.h>

#include <MaterialXRenderGlsl/GLUtil.h>

#if !defined(_WIN32)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif

#include <iostream>

MATERIALX_NAMESPACE_BEGIN

void checkGlErrors(const string& context)
{
    for (GLenum error = glGetError(); error; error = glGetError())
    {
        std::cerr << "OpenGL error " << context << ": " << std::to_string(error) << std::endl;
    }
}

#if !defined(_WIN32)

namespace
{
using PFN_eglQueryDevicesEXT = EGLBoolean (*)(EGLint, EGLDeviceEXT*, EGLint*);
using PFN_eglGetPlatformDisplayEXT = EGLDisplay (*)(EGLenum, void*, const EGLint*);
}

bool createEglHeadlessContext(EglHeadlessContext& ctx, int pbufferWidth, int pbufferHeight)
{
    PFN_eglQueryDevicesEXT queryDevices = reinterpret_cast<PFN_eglQueryDevicesEXT>(eglGetProcAddress("eglQueryDevicesEXT"));
    PFN_eglGetPlatformDisplayEXT getPlatformDisplay = reinterpret_cast<PFN_eglGetPlatformDisplayEXT>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (!queryDevices || !getPlatformDisplay)
    {
        std::cerr << "EGL_EXT_device_base not available" << std::endl;
        return false;
    }

    EGLDeviceEXT devices[8];
    EGLint numDevices = 0;
    if (queryDevices(8, devices, &numDevices) != EGL_TRUE || numDevices < 1)
    {
        std::cerr << "eglQueryDevicesEXT returned no devices" << std::endl;
        return false;
    }

    EGLDisplay display = getPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devices[0], nullptr);
    if (display == EGL_NO_DISPLAY)
    {
        std::cerr << "eglGetPlatformDisplayEXT failed" << std::endl;
        return false;
    }

    if (eglInitialize(display, nullptr, nullptr) != EGL_TRUE)
    {
        std::cerr << "eglInitialize failed" << std::endl;
        return false;
    }

    if (eglBindAPI(EGL_OPENGL_API) != EGL_TRUE)
    {
        std::cerr << "eglBindAPI(EGL_OPENGL_API) failed" << std::endl;
        eglTerminate(display);
        return false;
    }

    const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config = nullptr;
    EGLint numConfigs = 0;
    if (eglChooseConfig(display, configAttribs, &config, 1, &numConfigs) != EGL_TRUE || numConfigs < 1)
    {
        std::cerr << "eglChooseConfig failed" << std::endl;
        eglTerminate(display);
        return false;
    }

    const EGLint pbufferAttribs[] = {
        EGL_WIDTH, pbufferWidth,
        EGL_HEIGHT, pbufferHeight,
        EGL_NONE,
    };
    EGLSurface surface = eglCreatePbufferSurface(display, config, pbufferAttribs);
    if (surface == EGL_NO_SURFACE)
    {
        std::cerr << "eglCreatePbufferSurface failed" << std::endl;
        eglTerminate(display);
        return false;
    }

    const EGLint ctxAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 4,
        EGL_CONTEXT_MINOR_VERSION, 5,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctxAttribs);
    if (context == EGL_NO_CONTEXT)
    {
        std::cerr << "eglCreateContext failed" << std::endl;
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return false;
    }

    if (eglMakeCurrent(display, surface, surface, context) != EGL_TRUE)
    {
        std::cerr << "eglMakeCurrent failed" << std::endl;
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return false;
    }

    ctx.display = display;
    ctx.context = context;
    ctx.surface = surface;
    return true;
}

void destroyEglHeadlessContext(EglHeadlessContext& ctx)
{
    EGLDisplay display = reinterpret_cast<EGLDisplay>(ctx.display);
    EGLSurface surface = reinterpret_cast<EGLSurface>(ctx.surface);
    EGLContext context = reinterpret_cast<EGLContext>(ctx.context);

    if (display)
    {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (context)
    {
        eglDestroyContext(display, context);
    }
    if (surface)
    {
        eglDestroySurface(display, surface);
    }
    if (display)
    {
        eglTerminate(display);
    }

    ctx.display = nullptr;
    ctx.context = nullptr;
    ctx.surface = nullptr;
}

#else

bool createEglHeadlessContext(EglHeadlessContext&, int, int)
{
    std::cerr << "EGL headless backend not available on this platform/build" << std::endl;
    return false;
}

void destroyEglHeadlessContext(EglHeadlessContext&)
{
}

#endif

MATERIALX_NAMESPACE_END
