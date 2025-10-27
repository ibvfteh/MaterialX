//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXRemote/RemoteViewer.h>

#include <MaterialXFormat/Util.h>
#include <MaterialXRenderGlsl/GlslMaterial.h>
#include <MaterialXRender/ShaderRenderer.h>

#include <nanogui/window.h>

MATERIALX_NAMESPACE_BEGIN
namespace
{
const std::string DEFAULT_MATERIAL = "resources/Materials/Examples/StandardSurface/standard_surface_default.mtlx";
const std::string DEFAULT_MESH = "resources/Geometry/shaderball.glb";
const std::string DEFAULT_ENV = "resources/Lights/san_giuseppe_bridge_split.hdr";
const mx::FilePathVec DEFAULT_LIBRARY_FOLDERS = { mx::FilePath("libraries") };
}

RemoteViewer::RemoteViewer(const Options& options) :
    Viewer(options.materialFilename.empty() ? DEFAULT_MATERIAL : options.materialFilename,
           options.meshFilename.empty() ? DEFAULT_MESH : options.meshFilename,
           options.envRadianceFilename.empty() ? DEFAULT_ENV : options.envRadianceFilename,
           options.searchPath.isEmpty() ? mx::getDefaultDataSearchPath() : options.searchPath,
           options.libraryFolders.empty() ? DEFAULT_LIBRARY_FOLDERS : options.libraryFolders,
           options.screenWidth,
           options.screenHeight,
           options.screenColor),
    _options(options)
{
    if (_options.materialFilename.empty())
    {
        _options.materialFilename = DEFAULT_MATERIAL;
    }
    if (_options.meshFilename.empty())
    {
        _options.meshFilename = DEFAULT_MESH;
    }
    if (_options.envRadianceFilename.empty())
    {
        _options.envRadianceFilename = DEFAULT_ENV;
    }
    if (_options.searchPath.isEmpty())
    {
        _options.searchPath = mx::getDefaultDataSearchPath();
    }
    if (_options.libraryFolders.empty())
    {
        _options.libraryFolders = DEFAULT_LIBRARY_FOLDERS;
    }

    if (_options.headless)
    {
        set_visible(false);
    }
}

void RemoteViewer::initializeRemote()
{
    initialize();

    if (_options.headless)
    {
        if (auto window = getWindow())
        {
            window->set_visible(false);
        }
        set_visible(false);
    }
    else
    {
        set_visible(true);
    }
}

Json::Value RemoteViewer::applyShaderPackage(const ShaderPackage& pkg)
{
    Json::Value out(Json::objectValue);

    // Ensure we have a selected material
    mx::MaterialPtr material = getSelectedMaterial();
    if (!material)
    {
        out["error"] = "No selected material available";
        return out;
    }

    // Only GLSL pipeline supported here
    try
    {
        // Attempt to cast to a GLSL material; otherwise we cannot apply GLSL stages
        auto glslMaterial = std::dynamic_pointer_cast<mx::GlslMaterial>(material);
        if (!glslMaterial)
        {
            out["error"] = "Selected material is not a GLSL material";
            return out;
        }

        // Use the new helper API to set program stages (will throw on compile/link failure)
        glslMaterial->setProgramStages(pkg.vertex, pkg.fragment);
        out["status"] = "ok";
    }
    catch (const mx::ExceptionRenderError& e)
    {
        Json::Value errors(Json::arrayValue);
        for (const std::string& err : e.errorLog())
        {
            errors.append(err);
        }
        out["status"] = "error";
        out["errors"] = errors;
        return out;
    }
    catch (const std::exception& e)
    {
        out["error"] = e.what();
    }

    return out;
}

MATERIALX_NAMESPACE_END
