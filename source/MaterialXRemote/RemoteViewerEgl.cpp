//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXRemote/RemoteViewerEgl.h>

#include <MaterialXFormat/XmlIo.h>
#include <MaterialXGenShader/Util.h>
#include <MaterialXRender/StbImageLoader.h>
#include <MaterialXRender/TinyObjLoader.h>
#include <MaterialXRender/Util.h>
#include <MaterialXRenderGlsl/External/Glad/glad.h>
#include <MaterialXRenderGlsl/GlslMaterial.h>

#include <chrono>
#include <stdexcept>

MATERIALX_NAMESPACE_BEGIN

namespace
{
const std::string DEFAULT_MATERIAL = "resources/Materials/Examples/StandardSurface/standard_surface_default.mtlx";
const std::string DEFAULT_MESH = "resources/Geometry/shaderball.glb";
const std::string DEFAULT_ENV = "resources/Lights/san_giuseppe_bridge_split.hdr";
const mx::FilePathVec DEFAULT_LIBRARY_FOLDERS = { mx::FilePath("libraries") };
}

RemoteViewerEgl::RemoteViewerEgl(const Options& options) : _options(options)
{
    if (_options.materialFilename.empty()) _options.materialFilename = DEFAULT_MATERIAL;
    if (_options.meshFilename.empty()) _options.meshFilename = DEFAULT_MESH;
    if (_options.envRadianceFilename.empty()) _options.envRadianceFilename = DEFAULT_ENV;
    if (_options.searchPath.isEmpty()) _options.searchPath = mx::getDefaultDataSearchPath();
    if (_options.libraryFolders.empty()) _options.libraryFolders = DEFAULT_LIBRARY_FOLDERS;
}

RemoteViewerEgl::~RemoteViewerEgl()
{
    destroyEglHeadlessContext(_eglCtx);
}

void RemoteViewerEgl::initializeRemote()
{
    if (_initialized)
    {
        return;
    }

    if (!createEglHeadlessContext(_eglCtx))
    {
        throw std::runtime_error("Failed to initialize EGL headless context");
    }

    if (!gladLoadGL())
    {
        throw std::runtime_error("Failed to load GL functions");
    }

    // Load stdlib
    mx::FileSearchPath search = _options.searchPath;
    for (const mx::FilePath& p : _options.libraryFolders)
    {
        search.append(p);
    }
    _options.searchPath = search;
    _stdlib = mx::createDocument();
    mx::StringVec libraries;
    for (const mx::FilePath& p : _options.libraryFolders)
    {
        libraries.push_back(p.asString());
    }
    mx::loadLibraries(libraries, search, _stdlib);

    // Generator context
    mx::ShaderGeneratorPtr gen = mx::GlslShaderGenerator::create();
    _genContext = mx::GenContext(gen);
    _genContext.registerSourceCodeSearchPath(search);
    _genContext.getOptions().hwMaxActiveLightSources = 3;
    _genContext.getOptions().hwSpecularEnvironmentMethod = mx::SPECULAR_ENVIRONMENT_FIS;
    _genContext.getOptions().hwLightShaderName = "shadow_lighting";

    // Handlers
    _imageHandler = mx::GLTextureHandler::create(mx::StbImageLoader::create());
    _imageHandler->setSearchPath(search);
    _geometryHandler = mx::GeometryHandler::create();
    _geometryHandler->addLoader(mx::TinyObjLoader::create());
    _lightHandler = mx::LightHandler::create();
    _camera = mx::Camera::create();

    // Set defaults
    _width = static_cast<unsigned int>(_options.screenWidth);
    _height = static_cast<unsigned int>(_options.screenHeight);

    // Load initial document
    loadDocumentFromFile(mx::FilePath(_options.materialFilename));

    _initialized = true;
}

void RemoteViewerEgl::loadDocumentFromFile(const mx::FilePath& filename)
{
    _doc = mx::createDocument();
    mx::XmlReadOptions readOptions;
    readOptions.readComments = false;
    mx::readFromXmlFile(_doc, filename, _options.searchPath, &readOptions);
    _doc->importLibrary(_stdlib);

    std::vector<mx::TypedElementPtr> renderables;
    mx::findRenderableElements(_doc, renderables);
    if (renderables.empty())
    {
        throw std::runtime_error("No materials found in document");
    }

    mx::TypedElementPtr renderable = renderables.front();
    mx::NodePtr matNode = renderable->asA<mx::Node>();

    _material = mx::GlslMaterial::create();
    _material->setDocument(_doc);
    _material->setElement(renderable);
    if (matNode)
    {
        _material->setMaterialNode(matNode);
    }
    if (!_material->generateShader(_genContext))
    {
        throw std::runtime_error("Failed to generate shader for material");
    }
    _shader = _material->getShader();

    // Load geometry
    _geometryHandler->clearGeometry();
    if (!_geometryHandler->loadGeometry(_options.meshFilename, _options.searchPath))
    {
        throw std::runtime_error("Failed to load geometry: " + _options.meshFilename);
    }

    // Lights
    std::vector<mx::NodePtr> lights;
    _lightHandler->findLights(_doc, lights);
    _lightHandler->setLightSources(lights);
    _lightHandler->registerLights(_doc, lights, _genContext);

    mx::ImagePtr envMap = _imageHandler->acquireImage(_options.envRadianceFilename);
    if (envMap)
    {
        _lightHandler->setEnvRadianceMap(envMap);
    }

    // Camera defaults
    _camera->setViewport(static_cast<float>(_width), static_cast<float>(_height));
    _camera->setViewMatrix(mx::Matrix44::createLookAt(mx::Vector3(0.0f, 0.0f, 3.0f), mx::Vector3(0.0f), mx::Vector3(0, 1, 0)));
    _camera->setPerspective(45.0f, (float) _width / (float) _height, 0.05f, 100.0f);
}

bool RemoteViewerEgl::ensureShader(const ShaderPackage& pkg)
{
    if (!_material || !_shader)
    {
        return false;
    }

    auto glslMaterial = std::dynamic_pointer_cast<mx::GlslMaterial>(_material);
    if (!glslMaterial)
    {
        return false;
    }

    ShaderPackage merged = pkg;
    if (merged.vertex.empty())
    {
        merged.vertex = _shader->getSourceCode(mx::Stage::VERTEX);
    }
    if (merged.fragment.empty())
    {
        merged.fragment = _shader->getSourceCode(mx::Stage::PIXEL);
    }

    glslMaterial->setProgramStages(merged.vertex, merged.fragment);
    _program = glslMaterial->getProgram();

    return true;
}

void RemoteViewerEgl::resizeFramebuffer(unsigned int width, unsigned int height)
{
    if (width == 0) width = 1;
    if (height == 0) height = 1;

    if (!_framebuffer || _width != width || _height != height)
    {
        _framebuffer = mx::GLFramebuffer::create(width, height, 4, mx::Image::BaseType::FLOAT);
        _width = width;
        _height = height;

        _camera->setViewport(static_cast<float>(_width), static_cast<float>(_height));
        _camera->setPerspective(45.0f, (float) _width / (float) _height, 0.05f, 100.0f);
    }
}

void RemoteViewerEgl::renderFrame()
{
    auto glslMaterial = std::dynamic_pointer_cast<mx::GlslMaterial>(_material);
    if (!glslMaterial || !_program || !_framebuffer)
    {
        throw std::runtime_error("renderFrame: program or framebuffer not ready");
    }

    _framebuffer->bind();
    glViewport(0, 0, static_cast<GLsizei>(_width), static_cast<GLsizei>(_height));
    glClearColor(_options.screenColor[0], _options.screenColor[1], _options.screenColor[2], 1.0f);
    glEnable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    mx::ShadowState shadowState;
    if (!glslMaterial->bindShader())
    {
        throw std::runtime_error("Failed to bind GLSL material");
    }

    if (_program)
    {
        _program->bindTimeAndFrame();
    }

    glslMaterial->bindViewInformation(_camera);
    glslMaterial->bindLighting(_lightHandler, _imageHandler, shadowState);
    glslMaterial->bindImages(_imageHandler, _options.searchPath);

    const auto& partList = _geometryHandler->getGeometryPartitions();
    for (const auto& part : partList)
    {
        mx::MeshPtr mesh = _geometryHandler->findParentMesh(part);
        if (!mesh)
        {
            continue;
        }
        glslMaterial->bindMesh(mesh);
        glslMaterial->bindPartition(part);
        glslMaterial->drawPartition(part);
    }

    glslMaterial->unbindImages(_imageHandler);
    glslMaterial->unbindGeometry();
    _framebuffer->unbind();
}

bool RemoteViewerEgl::captureFrame(std::string& outBytes, Json::Value& frameDesc)
{
    if (!_framebuffer)
    {
        return false;
    }

    mx::ImagePtr img = _framebuffer->getColorImage();
    if (!img)
    {
        return false;
    }

    const unsigned int iw = img->getWidth();
    const unsigned int ih = img->getHeight();
    const unsigned int ich = img->getChannelCount();
    const auto baseType = img->getBaseType();

    std::vector<float> packed;
    packed.resize(static_cast<size_t>(iw) * ih * 3);
    const unsigned int rowStride = img->getRowStride();
    void* resBuf = img->getResourceBuffer();
    auto writePixel = [&](unsigned int x, unsigned int y, float r, float g, float b) {
        size_t idx = (size_t)(y * iw + x) * 3;
        packed[idx + 0] = r;
        packed[idx + 1] = g;
        packed[idx + 2] = b;
    };

    if (baseType == mx::Image::BaseType::FLOAT)
    {
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
    else if (baseType == mx::Image::BaseType::UINT8)
    {
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
    else
    {
        return false;
    }

    const char* p = reinterpret_cast<const char*>(packed.data());
    outBytes.assign(p, packed.size() * sizeof(float));

    frameDesc["width"] = iw;
    frameDesc["height"] = ih;
    frameDesc["channels"] = 3u;
    frameDesc["dtype"] = "float32";
    frameDesc["byteLength"] = (Json::UInt64)(packed.size() * sizeof(float));
    frameDesc["origin"] = "bottom-left";
    return true;
}

Json::Value RemoteViewerEgl::applyShaderPackage(const ShaderPackage& pkg)
{
    Json::Value out(Json::objectValue);
    try
    {
        if (!ensureShader(pkg))
        {
            out["error"] = "No material or shader available";
            return out;
        }
        out["status"] = "ok";
    }
    catch (const std::exception& e)
    {
        out["error"] = e.what();
    }
    return out;
}

std::pair<Json::Value, std::vector<std::string>> RemoteViewerEgl::renderAndCapture(const ShaderPackage& pkg,
                                                                                unsigned int frames,
                                                                                unsigned int width,
                                                                                unsigned int height,
                                                                                unsigned int warmup)
{
    Json::Value meta(Json::objectValue);
    std::vector<std::string> images;

    try
    {
        if (!ensureShader(pkg))
        {
            meta["error"] = "No material or shader available";
            return { meta, images };
        }

        resizeFramebuffer(width, height);

        // Warmup frames
        for (unsigned int i = 0; i < warmup; ++i)
        {
            renderFrame();
        }

        std::vector<double> timings;
        for (unsigned int f = 0; f < frames; ++f)
        {
            auto start = std::chrono::high_resolution_clock::now();
            renderFrame();
            Json::Value desc;
            std::string bytes;
            if (!captureFrame(bytes, desc))
            {
                meta["error"] = "Failed to capture frame";
                return { meta, images };
            }
            images.emplace_back(std::move(bytes));
            desc["index"] = (unsigned int) f;
            if (!meta.isMember("frames_info"))
            {
                meta["frames_info"] = Json::Value(Json::arrayValue);
            }
            meta["frames_info"].append(desc);
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

std::pair<Json::Value, std::vector<std::string>> RemoteViewerEgl::renderStateless(const ShaderPackage& candidatePkg,
                                                                               const Json::Value& uniformsPayload,
                                                                               unsigned int frames,
                                                                               unsigned int width,
                                                                               unsigned int height,
                                                                               unsigned int warmup)
{
    (void) uniformsPayload;
    return renderAndCapture(candidatePkg, frames, width, height, warmup);
}

mx::MaterialPtr RemoteViewerEgl::getSelectedMaterial()
{
    return _material;
}

MATERIALX_NAMESPACE_END
