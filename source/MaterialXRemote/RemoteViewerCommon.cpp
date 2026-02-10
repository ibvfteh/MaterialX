//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXRemote/RemoteViewerCommon.h>

#include <MaterialXCore/Value.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXRenderGlsl/GlslMaterial.h>

#include <limits>
#include <sstream>

MATERIALX_NAMESPACE_BEGIN

namespace RemoteViewerCommon
{
namespace
{
bool parseFloatList(const std::string& s, std::vector<float>& out)
{
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
    {
        std::string trimmed = mx::trimSpaces(item);
        if (trimmed.empty())
        {
            continue;
        }
        try
        {
            out.push_back(std::stof(trimmed));
        }
        catch (...)
        {
            return false;
        }
    }
    return !out.empty();
}

bool toFloatVec(const Json::Value& v, std::vector<float>& out)
{
    if (v.isArray())
    {
        for (const auto& e : v)
        {
            if (!e.isNumeric())
            {
                return false;
            }
            out.push_back(e.asFloat());
        }
        return !out.empty();
    }
    if (v.isNumeric())
    {
        out.push_back(v.asFloat());
        return true;
    }
    if (v.isString())
    {
        return parseFloatList(v.asString(), out);
    }
    return false;
}
}

UniformApplyResult applyUniformPayload(mx::MaterialPtr material,
                                       const Json::Value& uniformsPayload,
                                       const std::function<void(const std::string&)>& logFn)
{
    UniformApplyResult result;
    if (!material)
    {
        result.success = false;
        result.error = "material is null";
        return result;
    }

    if (!uniformsPayload.isArray() || uniformsPayload.empty())
    {
        return result;
    }

    for (const Json::Value& entry : uniformsPayload)
    {
        std::string path;
        if (entry.isMember("path"))
        {
            path = entry["path"].asString();
        }
        else if (entry.isMember("name"))
        {
            path = entry["name"].asString();
        }
        if (path.empty())
        {
            continue;
        }

        mx::ShaderPort* uniform = material->findUniform(path);
        if (!uniform)
        {
            continue;
        }
        if (!entry.isMember("value"))
        {
            continue;
        }

        const Json::Value& val = entry["value"];
        const std::string typeName = uniform->getType().getName();
        std::vector<float> floats;

        try
        {
            if (typeName == "float")
            {
                if (!toFloatVec(val, floats) || floats.size() != 1)
                {
                    throw std::runtime_error("expected float");
                }
                material->modifyUniform(path, mx::Value::createValue(floats[0]));
                if (logFn) logFn("uniform set float " + path + " = " + std::to_string(floats[0]));
            }
            else if (typeName == "color3" || typeName == "vector3")
            {
                if (!toFloatVec(val, floats) || floats.size() != 3)
                {
                    throw std::runtime_error("expected float[3]");
                }
                mx::ValuePtr v = (typeName == "color3") ?
                    mx::Value::createValue(mx::Color3(floats[0], floats[1], floats[2])) :
                    mx::Value::createValue(mx::Vector3(floats[0], floats[1], floats[2]));
                material->modifyUniform(path, v);
                if (logFn)
                {
                    std::ostringstream os;
                    os << "uniform set " << typeName << " " << path
                       << " = [" << floats[0] << "," << floats[1] << "," << floats[2] << "]";
                    logFn(os.str());
                }
            }
            else if (typeName == "vector2")
            {
                if (!toFloatVec(val, floats) || floats.size() != 2)
                {
                    throw std::runtime_error("expected float[2]");
                }
                material->modifyUniform(path, mx::Value::createValue(mx::Vector2(floats[0], floats[1])));
                if (logFn)
                {
                    std::ostringstream os;
                    os << "uniform set vector2 " << path
                       << " = [" << floats[0] << "," << floats[1] << "]";
                    logFn(os.str());
                }
            }
            else if (typeName == "vector4")
            {
                if (!toFloatVec(val, floats) || floats.size() != 4)
                {
                    throw std::runtime_error("expected float[4]");
                }
                material->modifyUniform(path, mx::Value::createValue(mx::Vector4(floats[0], floats[1], floats[2], floats[3])));
                if (logFn)
                {
                    std::ostringstream os;
                    os << "uniform set vector4 " << path
                       << " = [" << floats[0] << "," << floats[1] << "," << floats[2] << "," << floats[3] << "]";
                    logFn(os.str());
                }
            }
            else if (typeName == "integer")
            {
                if (val.isNumeric())
                {
                    material->modifyUniform(path, mx::Value::createValue(val.asInt()));
                    if (logFn) logFn("uniform set int " + path + " = " + std::to_string(val.asInt()));
                }
                else if (val.isString())
                {
                    floats.clear();
                    if (!parseFloatList(val.asString(), floats) || floats.size() != 1)
                    {
                        throw std::runtime_error("expected integer");
                    }
                    material->modifyUniform(path, mx::Value::createValue((int) floats[0]));
                    if (logFn) logFn("uniform set int " + path + " = " + std::to_string((int) floats[0]));
                }
                else
                {
                    throw std::runtime_error("expected integer");
                }
            }
            else if (typeName == "boolean")
            {
                if (!val.isBool())
                {
                    throw std::runtime_error("expected boolean");
                }
                material->modifyUniform(path, mx::Value::createValue(val.asBool()));
                if (logFn)
                {
                    std::ostringstream os;
                    os << std::boolalpha;
                    os << "uniform set bool " << path << " = " << val.asBool();
                    logFn(os.str());
                }
            }
            else
            {
                std::string valueStr = val.isString() ? val.asString() : val.toStyledString();
                mx::ValuePtr v = mx::Value::createValueFromStrings(valueStr, typeName);
                material->modifyUniform(path, v, valueStr);
                if (logFn)
                {
                    logFn("uniform set " + typeName + " " + path + " = " + valueStr);
                }
            }
        }
        catch (const std::exception& e)
        {
            result.success = false;
            result.error = "uniform parse failed for " + path + ": " + e.what();
            return result;
        }
        catch (...)
        {
            result.success = false;
            result.error = "uniform parse failed for " + path;
            return result;
        }
    }

    return result;
}

CompileResult compileAndApplyShader(mx::MaterialPtr material,
                                    const ShaderPackage& candidatePkg,
                                    ShaderPackage& mergedPkg)
{
    CompileResult result;

    if (!material)
    {
        result.success = false;
        result.error = "No selected material available";
        return result;
    }

    auto shader = material->getShader();
    if (!shader)
    {
        result.success = false;
        result.error = "Shader not generated for selected material";
        return result;
    }

    mergedPkg = candidatePkg;
    const std::string genVertex = shader->getSourceCode(mx::Stage::VERTEX);
    const std::string genFragment = shader->getSourceCode(mx::Stage::PIXEL);
    if (mergedPkg.vertex.empty()) mergedPkg.vertex = genVertex;
    if (mergedPkg.fragment.empty()) mergedPkg.fragment = genFragment;

    auto glslMaterial = std::dynamic_pointer_cast<mx::GlslMaterial>(material);
    if (!glslMaterial)
    {
        result.success = false;
        result.error = "Selected material is not a GLSL material";
        return result;
    }

    auto compileStart = std::chrono::high_resolution_clock::now();
    try
    {
        glslMaterial->setProgramStages(mergedPkg.vertex, mergedPkg.fragment);
    }
    catch (const std::exception& e)
    {
        result.success = false;
        result.error = e.what();
        return result;
    }

    auto compileEnd = std::chrono::high_resolution_clock::now();
    result.compileMs = std::chrono::duration<double, std::milli>(compileEnd - compileStart).count();
    return result;
}

PackResult packImageToFloatRgb(mx::ImagePtr img, std::string& outBytes, Json::Value& frameDesc)
{
    PackResult result;
    if (!img)
    {
        result.success = false;
        result.error = "Image is null";
        return result;
    }

    const unsigned int iw = img->getWidth();
    const unsigned int ih = img->getHeight();
    const unsigned int ich = img->getChannelCount();
    const auto baseType = img->getBaseType();

    std::vector<float> packed;
    packed.resize((size_t) iw * ih * 3);
    const unsigned int rowStride = img->getRowStride();
    void* resBuf = img->getResourceBuffer();

    auto writePixel = [&](unsigned int x, unsigned int y, float r, float g, float b) {
        size_t idx = (size_t)(y * iw + x) * 3;
        packed[idx + 0] = r;
        packed[idx + 1] = g;
        packed[idx + 2] = b;
    };

    if (baseType == mx::Image::BaseType::UINT8)
    {
        for (unsigned int y = 0; y < ih; ++y)
        {
            uint8_t* row = reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(resBuf) + (size_t) y * rowStride);
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
        const float scale = 1.0f / (float) std::numeric_limits<uint16_t>::max();
        for (unsigned int y = 0; y < ih; ++y)
        {
            uint16_t* row = reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(resBuf) + (size_t) y * rowStride);
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
        for (unsigned int y = 0; y < ih; ++y)
        {
            float* row = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(resBuf) + (size_t) y * rowStride);
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
        for (unsigned int y = 0; y < ih; ++y)
        {
            mx::Half* row = reinterpret_cast<mx::Half*>(reinterpret_cast<uint8_t*>(resBuf) + (size_t) y * rowStride);
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
        for (unsigned int y = 0; y < ih; ++y)
        {
            uint8_t* row = reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(resBuf) + (size_t) y * rowStride);
            for (unsigned int x = 0; x < iw; ++x)
            {
                uint8_t c0 = ich > 0 ? row[x * ich + 0] : 0;
                uint8_t c1 = ich > 1 ? row[x * ich + 1] : c0;
                uint8_t c2 = ich > 2 ? row[x * ich + 2] : c0;
                writePixel(x, y, c0 / 255.0f, c1 / 255.0f, c2 / 255.0f);
            }
        }
    }

    const char* p = reinterpret_cast<const char*>(packed.data());
    outBytes.assign(p, packed.size() * sizeof(float));

    frameDesc["width"] = iw;
    frameDesc["height"] = ih;
    frameDesc["channels"] = 3u;
    frameDesc["dtype"] = "float32";
    frameDesc["byteLength"] = (Json::UInt64)(packed.size() * sizeof(float));
    frameDesc["origin"] = "bottom-left";
    return result;
}

} // namespace RemoteViewerCommon

MATERIALX_NAMESPACE_END
