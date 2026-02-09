//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#ifndef MATERIALXREMOTE_REMOTEVIEWEREGL_H
#define MATERIALXREMOTE_REMOTEVIEWEREGL_H

#include <MaterialXCore/Library.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXFormat/File.h>
#include <MaterialXGenGlsl/GlslShaderGenerator.h>
#include <MaterialXRenderGlsl/GlslProgram.h>
#include <MaterialXRenderGlsl/GLFramebuffer.h>
#include <MaterialXRenderGlsl/GLTextureHandler.h>
#include <MaterialXRender/Camera.h>
#include <MaterialXRender/GeometryHandler.h>
#include <MaterialXRender/LightHandler.h>
#include <MaterialXRemote/Types.h>
#include <MaterialXRenderGlsl/GLUtil.h>

#include <memory>
#include <string>
#include <vector>

namespace mx = MaterialX;

MATERIALX_NAMESPACE_BEGIN

/// Minimal headless EGL-only viewer for remote rendering.
class RemoteViewerEgl
{
  public:
    struct Options
    {
        std::string materialFilename;
        std::string meshFilename;
        std::string envRadianceFilename;
        mx::FileSearchPath searchPath;
        mx::FilePathVec libraryFolders;
        int screenWidth = 1280;
        int screenHeight = 960;
        mx::Color3 screenColor = mx::DEFAULT_SCREEN_COLOR_SRGB;
    };

    explicit RemoteViewerEgl(const Options& options);
    ~RemoteViewerEgl();

    void initializeRemote();
    void loadDocumentFromFile(const mx::FilePath& filename);

    Json::Value applyShaderPackage(const ShaderPackage& pkg);
    std::pair<Json::Value, std::vector<std::string>> renderAndCapture(const ShaderPackage& pkg,
                                                                      unsigned int frames,
                                                                      unsigned int width,
                                                                      unsigned int height,
                                                                      unsigned int warmup);
    std::pair<Json::Value, std::vector<std::string>> renderStateless(const ShaderPackage& candidatePkg,
                                                                     const Json::Value& uniformsPayload,
                                                                     unsigned int frames,
                                                                     unsigned int width,
                                                                     unsigned int height,
                                                                     unsigned int warmup);

    mx::MaterialPtr getSelectedMaterial();
    mx::ShaderPtr getShader() const { return _shader; }

    bool isHeadless() const { return true; }

  private:
    bool ensureShader(const ShaderPackage& pkg);
    void resizeFramebuffer(unsigned int width, unsigned int height);
    void renderFrame();
    bool captureFrame(std::string& outBytes, Json::Value& frameDesc);

  private:
    Options _options;
    bool _initialized = false;
    EglHeadlessContext _eglCtx;

    // MaterialX state
    mx::GenContext _genContext;
    mx::DocumentPtr _stdlib;
    mx::DocumentPtr _doc;
    mx::MaterialPtr _material;
    mx::ShaderPtr _shader;

    mx::GlslProgramPtr _program;
    mx::GLFramebufferPtr _framebuffer;
    mx::GLTextureHandlerPtr _imageHandler;
    mx::GeometryHandlerPtr _geometryHandler;
    mx::LightHandlerPtr _lightHandler;
    mx::CameraPtr _camera;

    unsigned int _width = 0;
    unsigned int _height = 0;
};

MATERIALX_NAMESPACE_END

#endif
