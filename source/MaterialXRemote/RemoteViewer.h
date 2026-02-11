//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#ifndef MATERIALXREMOTE_REMOTEVIEWER_H
#define MATERIALXREMOTE_REMOTEVIEWER_H

#if !defined(MATERIALX_REMOTE_EGL_ONLY)
#include <MaterialXView/Viewer.h>
#include <MaterialXRender/Util.h>
#include <MaterialXFormat/File.h>
#include <MaterialXCore/Library.h>
#else
#include <MaterialXRemote/RemoteViewerEgl.h>
#include <MaterialXRenderGlsl/GLUtil.h>
#endif
#include <MaterialXRemote/Types.h>
#include <json/json.h>

#include <string>
#include <utility>
#include <vector>
#include <memory>

namespace mx = MaterialX;

MATERIALX_NAMESPACE_BEGIN

#if defined(MATERIALX_REMOTE_EGL_ONLY)

using RemoteViewer = RemoteViewerEgl;

#else

class RemoteViewer : public ::Viewer
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
        int gpuIndex = 0;
        // Headless flag: false -> windowed, true -> windowless. EGL builds ignore and run headless.
        bool headless = true;
    };

    explicit RemoteViewer(const Options& options);
    ~RemoteViewer() override = default;

    void initializeRemote();

  // Apply a shader package (vertex/fragment stage sources) to the currently
  // selected material. This compiles the provided stages into the material's
  // program on the render thread and returns a JSON diagnostics object.
  Json::Value applyShaderPackage(const ShaderPackage& pkg);

  /// Perform a stateless render: compile/apply the provided shader package,
  /// apply the provided uniform overrides (JSON array of {path,value}), render
  /// frames. This is intended for single-call renders; uniforms now persist
  /// after the call.
  std::pair<Json::Value, std::vector<std::string>> renderStateless(const ShaderPackage& candidatePkg,
                                    const Json::Value& uniformsPayload,
                                    unsigned int frames,
                                    unsigned int width,
                                    unsigned int height,
                                    unsigned int warmup);

    bool isHeadless() const;

  private:
    Options _options;
};

#endif

  using RemoteViewerPtr = std::shared_ptr<RemoteViewer>;

  MATERIALX_NAMESPACE_END

  #endif
