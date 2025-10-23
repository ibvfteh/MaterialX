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

#include <string>

#include <memory>

namespace mx = MaterialX;

MATERIALX_NAMESPACE_BEGIN

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
        bool headless = true;
    };

    explicit RemoteViewer(const Options& options);
    ~RemoteViewer() override = default;

    void initializeRemote();

    bool isHeadless() const { return _options.headless; }

  private:
    Options _options;
};

using RemoteViewerPtr = std::shared_ptr<RemoteViewer>;

MATERIALX_NAMESPACE_END

#endif
