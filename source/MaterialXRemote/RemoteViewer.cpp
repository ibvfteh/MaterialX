//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXRemote/RemoteViewer.h>
#include <MaterialXRemote/RemoteViewerCommon.h>

#if defined(MATERIALX_REMOTE_EGL_ONLY)

// EGL-only implementation lives in RemoteViewerEgl.{h,cpp}.

#else

#include <MaterialXFormat/Util.h>
#include <MaterialXRenderGlsl/GlslMaterial.h>
#include <MaterialXRender/ShaderRenderer.h>

#include <GLFW/glfw3.h>

#include <nanogui/window.h>
// For timing
#include <chrono>
#include <iostream>

MATERIALX_NAMESPACE_BEGIN
namespace
{
const std::string DEFAULT_MATERIAL = "resources/Materials/Examples/StandardSurface/standard_surface_default.mtlx";
const std::string DEFAULT_MESH = "resources/Geometry/shaderball.glb";
const std::string DEFAULT_ENV = "resources/Lights/san_giuseppe_bridge_split.hdr";
const mx::FilePathVec DEFAULT_LIBRARY_FOLDERS = { mx::FilePath("libraries") };
}

bool RemoteViewer::isHeadless() const
{
    return _options.headless;
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
}

void RemoteViewer::initializeRemote()
{
    initialize();

    // Force 1:1 pixel ratio so requested sizes aren't multiplied by monitor DPI.
    m_pixel_ratio = 1.0f;
    if (GLFWwindow* win = glfw_window())
    {
        glfwSetWindowContentScaleCallback(win, nullptr);
    }

    if (isHeadless())
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
    catch (const std::exception& e)
    {
        out["error"] = e.what();
    }

    return out;
}

std::pair<Json::Value, std::vector<std::string>> RemoteViewer::renderStateless(const ShaderPackage& candidatePkg,
                                                                               const Json::Value& uniformsPayload,
                                                                               unsigned int frames,
                                                                               unsigned int width,
                                                                               unsigned int height,
                                                                               unsigned int warmup)
{
    Json::Value meta(Json::objectValue);
    std::vector<std::string> images;
    auto callStart = std::chrono::high_resolution_clock::now();
    double unpackMsTotal = 0.0;

    // Ensure we have a selected material
    mx::MaterialPtr material = getSelectedMaterial();
    if (!material)
    {
        meta["error"] = "No selected material available";
        return { meta, images };
    }

    // Only GLSL pipeline supported here
    try
    {
        auto glslMaterial = std::dynamic_pointer_cast<mx::GlslMaterial>(material);
        if (!glslMaterial)
        {
            meta["error"] = "Selected material is not a GLSL material";
            return { meta, images };
        }

        // Save original shader sources (if any) to restore later
        std::string origVertex;
        std::string origFragment;
        mx::ShaderPtr origShader = material->getShader();
        if (origShader)
        {
            origVertex = origShader->getSourceCode(mx::Stage::VERTEX);
            origFragment = origShader->getSourceCode(mx::Stage::PIXEL);
        }

        ShaderPackage finalPkg;
        auto compileStart = std::chrono::high_resolution_clock::now();
        RemoteViewerCommon::CompileResult cresult = RemoteViewerCommon::compileAndApplyShader(material, candidatePkg, finalPkg);
        auto compileEnd = std::chrono::high_resolution_clock::now();
        if (!cresult.success)
        {
            if (!cresult.error.empty())
            {
                meta["error"] = cresult.error;
            }
            else
            {
                meta["status"] = "compile_error";
                meta["errors"] = cresult.compileErrors;
            }
            return { meta, images };
        }

        mx::ShaderPtr shader = material->getShader();
        const std::string genVertex = shader ? shader->getSourceCode(mx::Stage::VERTEX) : std::string();
        const std::string genFragment = shader ? shader->getSourceCode(mx::Stage::PIXEL) : std::string();

        auto restoreState = [&]() {
            try
            {
                if (!origVertex.empty() || !origFragment.empty())
                {
                    glslMaterial->setProgramStages(origVertex, origFragment);
                }
                else if (!genVertex.empty() || !genFragment.empty())
                {
                    glslMaterial->setProgramStages(genVertex, genFragment);
                }
            }
            catch (...) {}
        };

        auto logFn = [](const std::string& msg) { std::cout << "[RemoteViewer] " << msg << std::endl; };

        auto reapplyResult = RemoteViewerCommon::reapplyStoredUniformValues(material, nullptr);
        if (!reapplyResult.success)
        {
            restoreState();
            meta["error"] = reapplyResult.error;
            return { meta, images };
        }

        auto uniformResult = RemoteViewerCommon::applyUniformPayload(material, uniformsPayload, logFn);
        if (!uniformResult.success)
        {
            restoreState();
            meta["error"] = uniformResult.error;
            return { meta, images };
        }

        // Now perform render frames using the existing protected methods
        set_size(nanogui::Vector2i((int) width, (int) height));

        for (unsigned int i = 0; i < warmup; ++i)
        {
            updateCameras(); clear(); invalidateShadowMap(); draw_contents();
        }

        std::vector<double> timings;
        for (unsigned int f = 0; f < frames; ++f)
        {
            auto start = std::chrono::high_resolution_clock::now();
            updateCameras(); clear(); invalidateShadowMap(); draw_contents();

            mx::ImagePtr img = getRenderPipeline()->getFrameImage();
            if (img)
            {
                Json::Value frameDesc(Json::objectValue);
                std::string packedBytes;
                auto packStart = std::chrono::high_resolution_clock::now();
                RemoteViewerCommon::PackResult packResult = RemoteViewerCommon::packImageToFloatRgb(img, packedBytes, frameDesc);
                auto packEnd = std::chrono::high_resolution_clock::now();
                if (!packResult.success)
                {
                    meta["error"] = packResult.error;
                    return { meta, images };
                }
                frameDesc["index"] = (unsigned int) f;
                if (!meta.isMember("frames_info"))
                {
                    meta["frames_info"] = Json::Value(Json::arrayValue);
                }
                meta["frames_info"].append(frameDesc);
                images.emplace_back(std::move(packedBytes));
                unpackMsTotal += std::chrono::duration<double, std::milli>(packEnd - packStart).count();
            }
            else
            {
                images.emplace_back();
            }

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double, std::milli> elapsed = end - start;
            timings.push_back(elapsed.count());
        }

        meta["status"] = "ok";
        meta["frames"] = (unsigned int) frames;
        if (meta.isMember("frames_info") && meta["frames_info"].isArray() && meta["frames_info"].size() > 0)
        {
            const Json::Value first = meta["frames_info"][0];
            if (first.isObject() && first.isMember("width") && first.isMember("height"))
            {
                meta["width"] = first["width"].asUInt();
                meta["height"] = first["height"].asUInt();
            }
            else
            {
                meta["width"] = (unsigned int) width;
                meta["height"] = (unsigned int) height;
            }
        }
        else
        {
            meta["width"] = (unsigned int) width;
            meta["height"] = (unsigned int) height;
        }
        Json::Value tarr(Json::arrayValue);
        for (double d : timings) tarr.append(d);
        meta["timings_ms"] = tarr;

        double renderMsTotal = 0.0;
        for (double d : timings) renderMsTotal += d;
        auto now = std::chrono::high_resolution_clock::now();
        auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
        const double totalMs = ms(now - callStart);
        const double compileMs = ms(compileEnd - compileStart);
        std::cout << "[RemoteViewer] renderStateless timings compile_ms=" << compileMs
                  << " render_ms=" << renderMsTotal
                  << " unpack_ms=" << unpackMsTotal
                  << " total_ms=" << totalMs
                  << " frames=" << frames
                  << " size=" << width << "x" << height
                  << std::endl;

        restoreState();
    }
    catch (const std::exception& e)
    {
        meta["error"] = e.what();
    }

    return { meta, images };
}

MATERIALX_NAMESPACE_END

#endif // !MATERIALX_REMOTE_EGL_ONLY
