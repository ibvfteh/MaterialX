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
#include <MaterialXRemote/RemoteViewerCommon.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

MATERIALX_NAMESPACE_BEGIN

namespace
{
const std::string DEFAULT_MATERIAL = "resources/Materials/Examples/StandardSurface/standard_surface_default.mtlx";
const std::string DEFAULT_MESH = "resources/Geometry/shaderball.glb";
const std::string DEFAULT_ENV = "resources/Lights/san_giuseppe_bridge_split.hdr";
const mx::FilePathVec DEFAULT_LIBRARY_FOLDERS = { mx::FilePath("libraries") };
}

RemoteViewerEgl::RemoteViewerEgl(const Options& options) :
    _options(options),
    _genContext(mx::GlslShaderGenerator::create())
{
    if (_options.materialFilename.empty()) _options.materialFilename = DEFAULT_MATERIAL;
    if (_options.meshFilename.empty()) _options.meshFilename = DEFAULT_MESH;
    if (_options.envRadianceFilename.empty()) _options.envRadianceFilename = DEFAULT_ENV;
    if (_options.searchPath.isEmpty()) _options.searchPath = mx::getDefaultDataSearchPath();
    if (_options.libraryFolders.empty()) _options.libraryFolders = DEFAULT_LIBRARY_FOLDERS;
}

void RemoteViewerEgl::applyCameraState()
{
    if (!_camera)
    {
        return;
    }

    const unsigned int safeWidth = std::max(1u, _width);
    const unsigned int safeHeight = std::max(1u, _height);

    _camera->setViewportSize(mx::Vector2((float) safeWidth, (float) safeHeight));
    _camera->setViewMatrix(mx::Camera::createViewMatrix(_cameraPos, _cameraTarget, mx::Vector3(0, 1, 0)));

    const float fovY = std::max(1.0f, _cameraViewAngle / std::max(0.001f, _cameraZoom));
    const float aspect = (float) safeWidth / (float) safeHeight;
    const float n = 0.05f;
    const float f = 100.0f;
    const float fovRad = fovY * 3.14159265358979323846f / 180.0f;
    const float t = std::tan(0.5f * fovRad) * n;
    const float b = -t;
    const float r = t * aspect;
    const float l = -r;
    _camera->setProjectionMatrix(mx::Camera::createPerspectiveMatrix(l, r, b, t, n, f));
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
    mx::loadLibraries(_options.libraryFolders, search, _stdlib);

    // Generator context
    mx::ShaderGeneratorPtr gen = mx::GlslShaderGenerator::create();
    _genContext = mx::GenContext(gen);
    _genContext.registerSourceCodeSearchPath(search);
    _genContext.getOptions().hwMaxActiveLightSources = 3;
    _genContext.getOptions().hwSpecularEnvironmentMethod = mx::SPECULAR_ENVIRONMENT_FIS;
    // hwLightShaderName removed in current GenOptions

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
    _envRadianceFilename = _options.envRadianceFilename;
    _cameraPos = mx::Vector3(0.0f, 0.0f, 3.0f);
    _cameraTarget = mx::Vector3(0.0f, 0.0f, 0.0f);
    _cameraViewAngle = 45.0f;
    _cameraZoom = 1.0f;

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
    if (!_geometryHandler->loadGeometry(_options.meshFilename))
    {
        throw std::runtime_error("Failed to load geometry: " + _options.meshFilename);
    }

    const auto& meshes = _geometryHandler->getMeshes();
    if (!_activeGeometryId.empty())
    {
        bool found = false;
        for (const auto& mesh : meshes)
        {
            if (mesh && mesh->getName() == _activeGeometryId)
            {
                found = true;
                break;
            }
        }
        if (!found && !meshes.empty() && meshes[0])
        {
            _activeGeometryId = meshes[0]->getName();
        }
    }
    else if (!meshes.empty() && meshes[0])
    {
        _activeGeometryId = meshes[0]->getName();
    }

    // Lights
    std::vector<mx::NodePtr> lights;
    _lightHandler->findLights(_doc, lights);
    _lightHandler->setLightSources(lights);
    _lightHandler->registerLights(_doc, lights, _genContext);

    mx::ImagePtr envMap = _imageHandler->acquireImage(_envRadianceFilename);
    if (envMap)
    {
        _lightHandler->setEnvRadianceMap(envMap);
    }
    _lightHandler->setEnvLightIntensity(_envLightIntensity);
    _lightHandler->setLightTransform(mx::Matrix44::createRotationY(_lightRotation / 180.0f * 3.14159265358979323846f));

    // Camera defaults
    applyCameraState();
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
        applyCameraState();
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

    for (const auto& mesh : _geometryHandler->getMeshes())
    {
        if (!mesh)
        {
            continue;
        }
        if (!_activeGeometryId.empty() && mesh->getName() != _activeGeometryId)
        {
            continue;
        }
        glslMaterial->bindMesh(mesh);
        const size_t partCount = mesh->getPartitionCount();
        for (size_t i = 0; i < partCount; ++i)
        {
            mx::MeshPartitionPtr part = mesh->getPartition(i);
            if (!part)
            {
                continue;
            }
            glslMaterial->bindPartition(part);
            glslMaterial->drawPartition(part);
        }
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

    RemoteViewerCommon::PackResult packResult = RemoteViewerCommon::packImageToFloatRgb(img, outBytes, frameDesc);
    return packResult.success;
}

Json::Value RemoteViewerEgl::applyShaderPackage(const ShaderPackage& pkg)
{
    Json::Value out(Json::objectValue);
    mx::MaterialPtr material = _material;
    if (!material)
    {
        out["error"] = "No material or shader available";
        return out;
    }

    auto glslMaterial = std::dynamic_pointer_cast<mx::GlslMaterial>(material);
    if (!glslMaterial)
    {
        out["error"] = "Selected material is not a GLSL material";
        return out;
    }

    try
    {
        glslMaterial->setProgramStages(pkg.vertex, pkg.fragment);
        _program = glslMaterial->getProgram();
        out["status"] = "ok";
    }
    catch (const std::exception& e)
    {
        out["error"] = e.what();
    }
    return out;
}

std::pair<Json::Value, std::vector<std::string>> RemoteViewerEgl::renderStateless(const ShaderPackage& candidatePkg,
                                                                               const Json::Value& uniformsPayload,
                                                                               unsigned int frames,
                                                                               unsigned int width,
                                                                               unsigned int height,
                                                                               unsigned int warmup)
{
    Json::Value meta(Json::objectValue);
    std::vector<std::string> images;

    const auto callStart = std::chrono::high_resolution_clock::now();
    double renderMsTotal = 0.0;

    try
    {
        mx::MaterialPtr material = _material;
        if (!material)
        {
            meta["error"] = "No selected material available";
            return { meta, images };
        }

        auto glslMaterial = std::dynamic_pointer_cast<mx::GlslMaterial>(material);
        if (!glslMaterial)
        {
            meta["error"] = "Selected material is not a GLSL material";
            return { meta, images };
        }

        mx::ShaderPtr shader = material->getShader();
        if (!shader)
        {
            meta["error"] = "Shader not generated for selected material";
            return { meta, images };
        }

        std::string origVertex;
        std::string origFragment;
        mx::ShaderPtr origShader = material->getShader();
        if (origShader)
        {
            origVertex = origShader->getSourceCode(mx::Stage::VERTEX);
            origFragment = origShader->getSourceCode(mx::Stage::PIXEL);
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
                _program = glslMaterial->getProgram();
            }
            catch (...) {}
        };

        ShaderPackage finalPkg = candidatePkg;
        if (finalPkg.vertex.empty()) finalPkg.vertex = genVertex;
        if (finalPkg.fragment.empty()) finalPkg.fragment = genFragment;

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
        _program = glslMaterial->getProgram();

        RemoteViewerCommon::UniformApplyResult uniformResult = RemoteViewerCommon::applyUniformPayload(material, uniformsPayload);
        if (!uniformResult.success)
        {
            restoreState();
            meta["error"] = uniformResult.error;
            return { meta, images };
        }

        resizeFramebuffer(width, height);

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
                restoreState();
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
            renderMsTotal += elapsed.count();
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

        auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
        const double compileMs = ms(compileEnd - compileStart);
        const double totalMs = ms(std::chrono::high_resolution_clock::now() - callStart);
        std::cout << "[RemoteViewerEgl] renderStateless timings compile_ms=" << compileMs
                  << " render_ms=" << renderMsTotal
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

mx::MaterialPtr RemoteViewerEgl::getSelectedMaterial()
{
    return _material;
}

void RemoteViewerEgl::setCameraPosition(const mx::Vector3& pos)
{
    _cameraPos = pos;
    applyCameraState();
}

void RemoteViewerEgl::setCameraTarget(const mx::Vector3& tgt)
{
    _cameraTarget = tgt;
    applyCameraState();
}

void RemoteViewerEgl::setCameraViewAngle(float degrees)
{
    _cameraViewAngle = std::max(1.0f, degrees);
    applyCameraState();
}

void RemoteViewerEgl::setCameraZoom(float zoom)
{
    _cameraZoom = std::max(0.001f, zoom);
    applyCameraState();
}

void RemoteViewerEgl::setEnvRadianceFilename(const mx::FilePath& path)
{
    _envRadianceFilename = path;
    if (_imageHandler && _lightHandler)
    {
        mx::ImagePtr envMap = _imageHandler->acquireImage(_envRadianceFilename);
        if (envMap)
        {
            _lightHandler->setEnvRadianceMap(envMap);
        }
    }
}

void RemoteViewerEgl::setEnvLightIntensity(float intensity)
{
    _envLightIntensity = intensity;
    if (_lightHandler)
    {
        _lightHandler->setEnvLightIntensity(intensity);
    }
}

void RemoteViewerEgl::setLightRotation(float yRotationDegrees)
{
    _lightRotation = yRotationDegrees;
    if (_lightHandler)
    {
        _lightHandler->setLightTransform(mx::Matrix44::createRotationY(_lightRotation / 180.0f * 3.14159265358979323846f));
    }
}

std::vector<std::string> RemoteViewerEgl::listGeometry() const
{
    std::vector<std::string> out;
    if (!_geometryHandler)
    {
        return out;
    }
    for (const auto& mesh : _geometryHandler->getMeshes())
    {
        if (mesh)
        {
            out.push_back(mesh->getName());
        }
    }
    return out;
}

void RemoteViewerEgl::setActiveGeometryById(const std::string& id)
{
    if (id.empty() || !_geometryHandler)
    {
        return;
    }
    for (const auto& mesh : _geometryHandler->getMeshes())
    {
        if (mesh && mesh->getName() == id)
        {
            _activeGeometryId = id;
            break;
        }
    }
}

MATERIALX_NAMESPACE_END
