//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXRemote/RemoteViewer.h>

#if defined(MATERIALX_REMOTE_EGL_ONLY)

// EGL-only implementation lives in RemoteViewerEgl.{h,cpp}.

#else

#include <MaterialXFormat/Util.h>
#include <MaterialXRenderGlsl/GlslMaterial.h>
#include <MaterialXRender/ShaderRenderer.h>

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
    return _options.backend != Options::Backend::GLFWWindowed;
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

#endif

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

        // Determine generated stages to merge missing candidate stages
        mx::ShaderPtr shader = material->getShader();
        if (!shader)
        {
            meta["error"] = "Shader not generated for selected material";
            return { meta, images };
        }
        const std::string genVertex = shader->getSourceCode(mx::Stage::VERTEX);
        const std::string genFragment = shader->getSourceCode(mx::Stage::PIXEL);

        auto restoreState = [&]() {
            try
            {
                if (!origVertex.empty() || !origFragment.empty())
                {
                    glslMaterial->setProgramStages(origVertex, origFragment);
                }
                else
                {
                    glslMaterial->setProgramStages(genVertex, genFragment);
                }
            }
            catch (...) {}
        };

        ShaderPackage finalPkg = candidatePkg;
        if (finalPkg.vertex.empty()) finalPkg.vertex = genVertex;
        if (finalPkg.fragment.empty()) finalPkg.fragment = genFragment;

        // Compile/apply the candidate program (may throw on compile error)
        auto compileStart = std::chrono::high_resolution_clock::now();
        try
        {
            glslMaterial->setProgramStages(finalPkg.vertex, finalPkg.fragment);
        }
        catch (const mx::ExceptionRenderError& e)
        {
            Json::Value errors(Json::arrayValue);
            for (const std::string& err : e.errorLog()) errors.append(err);
            meta["status"] = "compile_error";
            meta["errors"] = errors;
            return { meta, images };
        }
        auto compileEnd = std::chrono::high_resolution_clock::now();

        auto parseFloatList = [](const std::string& s, std::vector<float>& out) -> bool {
            std::stringstream ss(s);
            std::string item;
            while (std::getline(ss, item, ','))
            {
                std::string trimmed = mx::trimSpaces(item);
                if (trimmed.empty()) continue;
                try { out.push_back(std::stof(trimmed)); }
                catch (...) { return false; }
            }
            return !out.empty();
        };

        auto toFloatVec = [&](const Json::Value& v, std::vector<float>& out) -> bool {
            if (v.isArray())
            {
                for (const auto& e : v)
                {
                    if (!e.isNumeric()) return false;
                    out.push_back(e.asFloat());
                }
                return !out.empty();
            }
            if (v.isNumeric()) { out.push_back(v.asFloat()); return true; }
            if (v.isString()) return parseFloatList(v.asString(), out);
            return false;
        };

        auto jsonToString = [](const Json::Value& v) {
            Json::StreamWriterBuilder b; b["indentation"] = "";
            return Json::writeString(b, v);
        };

        // Apply inline uniforms (if any)
        if (uniformsPayload.isArray() && uniformsPayload.size() > 0)
        {
            for (const Json::Value& entry : uniformsPayload)
            {
                std::string path;
                if (entry.isMember("path")) path = entry["path"].asString();
                else if (entry.isMember("name")) path = entry["name"].asString();
                if (path.empty()) continue;

                mx::ShaderPort* uniform = material->findUniform(path);
                if (!uniform) continue;

                if (!entry.isMember("value")) continue;
                const Json::Value& val = entry["value"];

                const std::string typeName = uniform->getType().getName();
                std::vector<float> floats;

                try
                {
                    if (typeName == "float")
                    {
                        if (!toFloatVec(val, floats) || floats.size() != 1)
                            throw std::runtime_error("expected float");
                        mx::ValuePtr v = mx::Value::createValue(floats[0]);
                        material->modifyUniform(path, v);
                        std::cout << "[RemoteViewer] uniform set float " << path << " = " << floats[0] << std::endl;
                    }
                    else if (typeName == "color3" || typeName == "vector3")
                    {
                        if (!toFloatVec(val, floats) || floats.size() != 3)
                            throw std::runtime_error("expected float[3]");
                        mx::ValuePtr v = (typeName == "color3") ?
                            mx::Value::createValue(mx::Color3(floats[0], floats[1], floats[2])) :
                            mx::Value::createValue(mx::Vector3(floats[0], floats[1], floats[2]));
                        material->modifyUniform(path, v);
                        std::cout << "[RemoteViewer] uniform set " << typeName << " " << path
                                  << " = [" << floats[0] << "," << floats[1] << "," << floats[2] << "]" << std::endl;
                    }
                    else if (typeName == "vector2")
                    {
                        if (!toFloatVec(val, floats) || floats.size() != 2)
                            throw std::runtime_error("expected float[2]");
                        mx::ValuePtr v = mx::Value::createValue(mx::Vector2(floats[0], floats[1]));
                        material->modifyUniform(path, v);
                        std::cout << "[RemoteViewer] uniform set vector2 " << path
                                  << " = [" << floats[0] << "," << floats[1] << "]" << std::endl;
                    }
                    else if (typeName == "vector4")
                    {
                        if (!toFloatVec(val, floats) || floats.size() != 4)
                            throw std::runtime_error("expected float[4]");
                        mx::ValuePtr v = mx::Value::createValue(mx::Vector4(floats[0], floats[1], floats[2], floats[3]));
                        material->modifyUniform(path, v);
                        std::cout << "[RemoteViewer] uniform set vector4 " << path
                                  << " = [" << floats[0] << "," << floats[1] << "," << floats[2] << "," << floats[3] << "]" << std::endl;
                    }
                    else if (typeName == "integer")
                    {
                        if (val.isNumeric())
                        {
                            mx::ValuePtr v = mx::Value::createValue(val.asInt());
                            material->modifyUniform(path, v);
                            std::cout << "[RemoteViewer] uniform set int " << path << " = " << val.asInt() << std::endl;
                        }
                        else if (val.isString())
                        {
                            floats.clear();
                            if (!parseFloatList(val.asString(), floats) || floats.size() != 1)
                                throw std::runtime_error("expected integer");
                            mx::ValuePtr v = mx::Value::createValue((int)floats[0]);
                            material->modifyUniform(path, v);
                            std::cout << "[RemoteViewer] uniform set int " << path << " = " << (int)floats[0] << std::endl;
                        }
                        else
                        {
                            throw std::runtime_error("expected integer");
                        }
                    }
                    else if (typeName == "boolean")
                    {
                        if (!val.isBool()) throw std::runtime_error("expected boolean");
                        mx::ValuePtr v = mx::Value::createValue(val.asBool());
                        material->modifyUniform(path, v);
                        std::cout << "[RemoteViewer] uniform set bool " << path << " = " << std::boolalpha << val.asBool() << std::endl;
                    }
                    else
                    {
                        // Fallback: try the original string-based conversion
                        std::string valueStr = val.isString() ? val.asString() : val.toStyledString();
                        mx::ValuePtr v = mx::Value::createValueFromStrings(valueStr, typeName);
                        material->modifyUniform(path, v, valueStr);
                        std::cout << "[RemoteViewer] uniform set " << typeName << " " << path
                                  << " = " << jsonToString(val) << std::endl;
                    }
                }
                catch (const std::exception& e)
                {
                        restoreState();
                        meta["error"] = std::string("uniform parse failed for ") + path + ": " + e.what();
                    return { meta, images };
                }
                catch (...) {
                    meta["error"] = std::string("uniform parse failed for ") + path;
                        restoreState();
                    return { meta, images };
                }
            }
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
                const unsigned int iw = img->getWidth();
                const unsigned int ih = img->getHeight();
                const unsigned int ich = img->getChannelCount();
                const auto baseType = img->getBaseType();
                std::vector<float> packed; packed.resize((size_t)iw * ih * 3);
                    auto packStart = std::chrono::high_resolution_clock::now();
                const unsigned int rowStride = img->getRowStride();
                void* resBuf = img->getResourceBuffer();
                auto writePixel = [&](unsigned int x, unsigned int y, float r, float g, float b) {
                    size_t idx = (size_t)(y * iw + x) * 3; packed[idx+0]=r; packed[idx+1]=g; packed[idx+2]=b; };

                if (baseType == mx::Image::BaseType::UINT8)
                {
                    for (unsigned int y=0;y<ih;++y)
                    {
                        uint8_t* row = reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(resBuf) + (size_t)y * rowStride);
                        for (unsigned int x=0;x<iw;++x)
                        {
                            uint8_t c0 = ich>0?row[x*ich+0]:0; uint8_t c1 = ich>1?row[x*ich+1]:c0; uint8_t c2 = ich>2?row[x*ich+2]:c0;
                            writePixel(x,y,c0/255.0f,c1/255.0f,c2/255.0f);
                        }
                    }
                }
                else if (baseType == mx::Image::BaseType::UINT16)
                {
                    const float scale = 1.0f/(float)std::numeric_limits<uint16_t>::max();
                    for (unsigned int y=0;y<ih;++y)
                    {
                        uint16_t* row = reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(resBuf) + (size_t)y * rowStride);
                        for (unsigned int x=0;x<iw;++x)
                        {
                            uint16_t c0 = ich>0?row[x*ich+0]:0; uint16_t c1 = ich>1?row[x*ich+1]:c0; uint16_t c2 = ich>2?row[x*ich+2]:c0;
                            writePixel(x,y,c0*scale,c1*scale,c2*scale);
                        }
                    }
                }
                else if (baseType == mx::Image::BaseType::FLOAT)
                {
                    for (unsigned int y=0;y<ih;++y)
                    {
                        float* row = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(resBuf) + (size_t)y * rowStride);
                        for (unsigned int x=0;x<iw;++x)
                        {
                            float c0 = ich>0?row[x*ich+0]:0.0f; float c1 = ich>1?row[x*ich+1]:c0; float c2 = ich>2?row[x*ich+2]:c0;
                            writePixel(x,y,c0,c1,c2);
                        }
                    }
                }
                else if (baseType == mx::Image::BaseType::HALF)
                {
                    for (unsigned int y=0;y<ih;++y)
                    {
                        mx::Half* row = reinterpret_cast<mx::Half*>(reinterpret_cast<uint8_t*>(resBuf) + (size_t)y * rowStride);
                        for (unsigned int x=0;x<iw;++x)
                        {
                            float c0 = ich>0?float(row[x*ich+0]):0.0f; float c1 = ich>1?float(row[x*ich+1]):c0; float c2 = ich>2?float(row[x*ich+2]):c0;
                            writePixel(x,y,c0,c1,c2);
                        }
                    }
                }
                else
                {
                    for (unsigned int y=0;y<ih;++y)
                    {
                        uint8_t* row = reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(resBuf) + (size_t)y * rowStride);
                        for (unsigned int x=0;x<iw;++x)
                        {
                            uint8_t c0 = ich>0?row[x*ich+0]:0; uint8_t c1 = ich>1?row[x*ich+1]:c0; uint8_t c2 = ich>2?row[x*ich+2]:c0;
                            writePixel(x,y,c0/255.0f,c1/255.0f,c2/255.0f);
                        }
                    }
                }

                const char* p = reinterpret_cast<const char*>(packed.data());
                images.emplace_back(p, packed.size() * sizeof(float));

                Json::Value frameDesc(Json::objectValue);
                frameDesc["index"] = (unsigned int) f;
                frameDesc["width"] = iw;
                frameDesc["height"] = ih;
                frameDesc["channels"] = 3u;
                frameDesc["dtype"] = "float32";
                frameDesc["byteLength"] = (Json::UInt64)(packed.size() * sizeof(float));
                frameDesc["origin"] = "bottom-left";
                if (!meta.isMember("frames_info")) meta["frames_info"] = Json::Value(Json::arrayValue);
                meta["frames_info"].append(frameDesc);
                    auto packEnd = std::chrono::high_resolution_clock::now();
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
