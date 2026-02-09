//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#ifndef MATERIALXREMOTE_REMOTEVIEWER_H
#define MATERIALXREMOTE_REMOTEVIEWER_H

#include <MaterialXView/Viewer.h>
#include <MaterialXRender/Util.h>
#include <MaterialXFormat/File.h>
#include <MaterialXCore/Library.h>
#include <MaterialXRemote/Types.h>
#include <json/json.h>

#include <string>

#include <memory>

namespace mx = MaterialX;

MATERIALX_NAMESPACE_BEGIN

class RemoteViewer : public ::Viewer
{
  public:
    struct Options
    {
      enum class Backend
      {
        GLFWWindowed,
        GLFWWindowless,
        EGLHeadless
      };

        std::string materialFilename;
        std::string meshFilename;
        std::string envRadianceFilename;
        mx::FileSearchPath searchPath;
        mx::FilePathVec libraryFolders;
        int screenWidth = 1280;
        int screenHeight = 960;
        mx::Color3 screenColor = mx::DEFAULT_SCREEN_COLOR_SRGB;
      Backend backend = Backend::GLFWWindowless;
    };

    explicit RemoteViewer(const Options& options);
    ~RemoteViewer() override = default;

    void initializeRemote();

  // Apply a shader package (vertex/fragment stage sources) to the currently
  // selected material. This compiles the provided stages into the material's
  // program on the render thread and returns a JSON diagnostics object.
  Json::Value applyShaderPackage(const ShaderPackage& pkg);

  /// Render using the current session shader package (merged with generated stages if needed),
  /// capture `frames` frames at `width` x `height`, perform `warmup` initial frames (not timed),
  /// and return a pair of JSON metadata and a vector of PNG byte strings (one per frame).
  std::pair<Json::Value, std::vector<std::string>> renderAndCapture(const ShaderPackage& pkg,
                                    unsigned int frames,
                                    unsigned int width,
                                    unsigned int height,
                                    unsigned int warmup);

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

    Options::Backend getBackend() const { return _options.backend; }
    bool isHeadless() const { return _options.backend != Options::Backend::GLFWWindowed; }

  private:
    Options _options;
};

using RemoteViewerPtr = std::shared_ptr<RemoteViewer>;

MATERIALX_NAMESPACE_END

#endif
