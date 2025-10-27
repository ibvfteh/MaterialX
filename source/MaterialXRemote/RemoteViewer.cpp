//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXRemote/RemoteViewer.h>

#include <MaterialXFormat/Util.h>
#include <MaterialXRenderGlsl/GlslMaterial.h>
#include <MaterialXRender/ShaderRenderer.h>

#include <nanogui/window.h>
// For timing
#include <chrono>

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

std::pair<Json::Value, std::vector<std::string>> RemoteViewer::renderAndCapture(const ShaderPackage& pkg,
                                                                                unsigned int frames,
                                                                                unsigned int width,
                                                                                unsigned int height,
                                                                                unsigned int warmup)
{
    Json::Value meta(Json::objectValue);
    std::vector<std::string> images;

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
        mx::ShaderPtr shader = material->getShader();
        if (!shader)
        {
            meta["error"] = "Shader not generated for selected material";
            return { meta, images };
        }

        ShaderPackage finalPkg = pkg;
        const std::string genVertex = shader->getSourceCode(mx::Stage::VERTEX);
        const std::string genFragment = shader->getSourceCode(mx::Stage::PIXEL);
        if (finalPkg.vertex.empty()) finalPkg.vertex = genVertex;
        if (finalPkg.fragment.empty()) finalPkg.fragment = genFragment;

        auto glslMaterial = std::dynamic_pointer_cast<mx::GlslMaterial>(material);
        if (!glslMaterial)
        {
            meta["error"] = "Selected material is not a GLSL material";
            return { meta, images };
        }

        // Compile and apply program stages (may throw with diagnostics)
        try
        {
            glslMaterial->setProgramStages(finalPkg.vertex, finalPkg.fragment);
        }
        catch (const mx::ExceptionRenderError& e)
        {
            Json::Value errors(Json::arrayValue);
            for (const std::string& err : e.errorLog())
            {
                errors.append(err);
            }
            meta["status"] = "compile_error";
            meta["errors"] = errors;
            return { meta, images };
        }

        // Resize the viewer window which will trigger framebuffer resize logic
        set_size(nanogui::Vector2i((int) width, (int) height));

        // Warmup frames
        for (unsigned int i = 0; i < warmup; ++i)
        {
            updateCameras();
            clear();
            invalidateShadowMap();
            draw_contents();
        }

        // Timed frames
        std::vector<double> timings;
        for (unsigned int f = 0; f < frames; ++f)
        {
            auto start = std::chrono::high_resolution_clock::now();
            updateCameras();
            clear();
            invalidateShadowMap();
            draw_contents();

            mx::ImagePtr img = getRenderPipeline()->getFrameImage();
            if (img)
            {
                // Repack into tightly-packed float32 RGB for highest fidelity comparisons.
                const unsigned int iw = img->getWidth();
                const unsigned int ih = img->getHeight();
                const unsigned int ich = img->getChannelCount();
                const auto baseType = img->getBaseType();

                // Destination packed float RGB (3 channels)
                std::vector<float> packed;
                packed.resize((size_t)iw * ih * 3);

                // Source row stride (in bytes) as reported by Image API.
                const unsigned int rowStride = img->getRowStride();

                // Pointers to resource buffer
                void* resBuf = img->getResourceBuffer();

                // Helper lambda to write into packed array
                auto writePixel = [&](unsigned int x, unsigned int y, float r, float g, float b) {
                    size_t idx = (size_t)(y * iw + x) * 3;
                    packed[idx + 0] = r;
                    packed[idx + 1] = g;
                    packed[idx + 2] = b;
                };

                // Convert per base type
                if (baseType == mx::Image::BaseType::UINT8)
                {
                    uint8_t* src = static_cast<uint8_t*>(resBuf);
                    for (unsigned int y = 0; y < ih; ++y)
                    {
                        uint8_t* row = reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(resBuf) + (size_t)y * rowStride);
                        for (unsigned int x = 0; x < iw; ++x)
                        {
                            uint8_t c0 = ich > 0 ? row[x * ich + 0] : 0;
                            uint8_t c1 = ich > 1 ? row[x * ich + 1] : c0;
                            uint8_t c2 = ich > 2 ? row[x * ich + 2] : c0;
                            writePixel(x, y, c0 / 255.0f, c1 / 255.0f, c2 / 255.0f);
                        }
                    }
                }
                else if (baseType == mx::Image::BaseType::UINT16)
                {
                    uint16_t* src = static_cast<uint16_t*>(resBuf);
                    const float scale = 1.0f / (float)std::numeric_limits<uint16_t>::max();
                    for (unsigned int y = 0; y < ih; ++y)
                    {
                        uint16_t* row = reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(resBuf) + (size_t)y * rowStride);
                        for (unsigned int x = 0; x < iw; ++x)
                        {
                            uint16_t c0 = ich > 0 ? row[x * ich + 0] : 0;
                            uint16_t c1 = ich > 1 ? row[x * ich + 1] : c0;
                            uint16_t c2 = ich > 2 ? row[x * ich + 2] : c0;
                            writePixel(x, y, c0 * scale, c1 * scale, c2 * scale);
                        }
                    }
                }
                else if (baseType == mx::Image::BaseType::FLOAT)
                {
                    float* src = static_cast<float*>(resBuf);
                    for (unsigned int y = 0; y < ih; ++y)
                    {
                        float* row = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(resBuf) + (size_t)y * rowStride);
                        for (unsigned int x = 0; x < iw; ++x)
                        {
                            float c0 = ich > 0 ? row[x * ich + 0] : 0.0f;
                            float c1 = ich > 1 ? row[x * ich + 1] : c0;
                            float c2 = ich > 2 ? row[x * ich + 2] : c0;
                            writePixel(x, y, c0, c1, c2);
                        }
                    }
                }
                else if (baseType == mx::Image::BaseType::HALF)
                {
                    // Half class supports conversion to float
                    mx::Half* src = static_cast<mx::Half*>(resBuf);
                    for (unsigned int y = 0; y < ih; ++y)
                    {
                        mx::Half* row = reinterpret_cast<mx::Half*>(reinterpret_cast<uint8_t*>(resBuf) + (size_t)y * rowStride);
                        for (unsigned int x = 0; x < iw; ++x)
                        {
                            float c0 = ich > 0 ? float(row[x * ich + 0]) : 0.0f;
                            float c1 = ich > 1 ? float(row[x * ich + 1]) : c0;
                            float c2 = ich > 2 ? float(row[x * ich + 2]) : c0;
                            writePixel(x, y, c0, c1, c2);
                        }
                    }
                }
                else
                {
                    // Fallback: treat as uint8
                    for (unsigned int y = 0; y < ih; ++y)
                    {
                        uint8_t* row = reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(resBuf) + (size_t)y * rowStride);
                        for (unsigned int x = 0; x < iw; ++x)
                        {
                            uint8_t c0 = ich > 0 ? row[x * ich + 0] : 0;
                            uint8_t c1 = ich > 1 ? row[x * ich + 1] : c0;
                            uint8_t c2 = ich > 2 ? row[x * ich + 2] : c0;
                            writePixel(x, y, c0 / 255.0f, c1 / 255.0f, c2 / 255.0f);
                        }
                    }
                }

                // Move packed float bytes into string for multipart transport
                const char* p = reinterpret_cast<const char*>(packed.data());
                images.emplace_back(p, packed.size() * sizeof(float));

                // Record per-frame descriptor metadata
                Json::Value frameDesc(Json::objectValue);
                frameDesc["index"] = (unsigned int) f;
                frameDesc["width"] = iw;
                frameDesc["height"] = ih;
                frameDesc["channels"] = 3u;
                frameDesc["dtype"] = "float32";
                frameDesc["byteLength"] = (Json::UInt64)(packed.size() * sizeof(float));
                frameDesc["origin"] = "bottom-left";
                // Append to meta array (create if missing)
                if (!meta.isMember("frames_info"))
                {
                    meta["frames_info"] = Json::Value(Json::arrayValue);
                }
                meta["frames_info"].append(frameDesc);
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
        // If we recorded per-frame descriptors, use the first frame's width/height as authoritative
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
    }
    catch (const std::exception& e)
    {
        meta["error"] = e.what();
    }

    return { meta, images };
}

MATERIALX_NAMESPACE_END
