//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXRemote/RemoteViewerEgl.h>

#include <MaterialXFormat/XmlIo.h>
#include <MaterialXGenShader/Util.h>
#include <MaterialXCore/Unit.h>
#include <MaterialXGenShader/DefaultColorManagementSystem.h>
#include <MaterialXRender/StbImageLoader.h>
#include <MaterialXRender/TinyObjLoader.h>
#include <MaterialXRender/CgltfLoader.h>
#include <MaterialXRender/Util.h>
#include <MaterialXRender/Harmonics.h>
#include <MaterialXRenderGlsl/External/Glad/glad.h>
#include <MaterialXRenderGlsl/GlslMaterial.h>
#include <MaterialXRemote/RemoteViewerCommon.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <iostream>

MATERIALX_NAMESPACE_BEGIN

namespace
{
const std::string DEFAULT_MATERIAL = "resources/Materials/Examples/StandardSurface/standard_surface_default.mtlx";
const std::string DEFAULT_MESH = "resources/Geometry/shaderball.glb";
const std::string DEFAULT_ENV = "resources/Lights/san_giuseppe_bridge_split.hdr";
const mx::FilePathVec DEFAULT_LIBRARY_FOLDERS = { mx::FilePath("libraries") };
const mx::FilePath IRRADIANCE_MAP_FOLDER("irradiance");
const unsigned int IRRADIANCE_MAP_WIDTH = 256;
const unsigned int IRRADIANCE_MAP_HEIGHT = 128;
const float MAX_ENV_TEXEL_RADIANCE = 100000.0f;
const float IDEAL_ENV_MAP_RADIANCE = 6.0f;
const float ENV_MAP_SPLIT_RADIANCE = 16.0f;
const std::string DIR_LIGHT_NODE_CATEGORY = "directional_light";
const float PI_F = 3.14159265358979323846f;
const int SHADOW_MAP_SIZE = 2048;

mx::DocumentPtr splitDirectLight(mx::ImagePtr envRadianceMap, mx::ImagePtr& indirectMap)
{
    mx::Vector3 lightDir;
    mx::Color3 lightColor;
    mx::ImagePair imagePair = envRadianceMap->splitByLuminance(ENV_MAP_SPLIT_RADIANCE);

    mx::computeDominantLight(imagePair.second, lightDir, lightColor);
    const float lightIntensity = std::max({ lightColor[0], lightColor[1], lightColor[2] });
    mx::Color3 normalizedColor = lightColor;
    if (lightIntensity > 0.0f)
    {
        normalizedColor /= lightIntensity;
    }

    mx::DocumentPtr dirLightDoc = mx::createDocument();
    mx::NodePtr dirLightNode = dirLightDoc->addNode(DIR_LIGHT_NODE_CATEGORY, "dir_light", mx::LIGHT_SHADER_TYPE_STRING);
    dirLightNode->setInputValue("direction", lightDir);
    dirLightNode->setInputValue("color", normalizedColor);
    dirLightNode->setInputValue("intensity", lightIntensity);
    indirectMap = imagePair.first;
    return dirLightDoc;
}
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

    const mx::Matrix44 viewMatrix = mx::Camera::createViewMatrix(_cameraPos, _cameraTarget, mx::Vector3(0, 1, 0));
    _camera->setViewportSize(mx::Vector2((float) safeWidth, (float) safeHeight));
    _camera->setViewMatrix(viewMatrix);

    const float fovY = std::max(1.0f, _cameraViewAngle);
    const float aspect = (float) safeWidth / (float) safeHeight;
    const float n = 0.05f;
    const float f = 100.0f;
    const float fovRad = fovY * PI_F / 180.0f;
    const float t = std::tan(0.5f * fovRad) * n;
    const float b = -t;
    const float r = t * aspect;
    const float l = -r;
    _camera->setProjectionMatrix(mx::Camera::createPerspectiveMatrix(l, r, b, t, n, f));

    const float scale = std::max(0.0001f, _geometryScale * _cameraZoom);
    _camera->setWorldMatrix(mx::Matrix44::createTranslation(-_centeringOffset) * mx::Matrix44::createScale(mx::Vector3(scale)));
}

void RemoteViewerEgl::centerCameraToGeometry()
{
    if (!_geometryHandler || _geometryHandler->getMeshes().empty())
    {
        _centeringOffset = mx::Vector3(0.0f);
        applyCameraState();
        return;
    }

    const mx::Vector3& boxMax = _geometryHandler->getMaximumBounds();
    const mx::Vector3& boxMin = _geometryHandler->getMinimumBounds();
    const mx::Vector3 center = (boxMax + boxMin) * 0.5f;
    const float radius = (center - boxMin).getMagnitude();

    _centeringOffset = center;
    _geometryScale = (radius > 0.0f) ? (2.0f / radius) : 1.0f; // Align with windowed viewer IDEAL_MESH_SPHERE_RADIUS=2
    applyCameraState();
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

    if (!createEglHeadlessContext(_eglCtx, 1, 1, _options.gpuIndex))
    {
        throw std::runtime_error("Failed to initialize EGL headless context");
    }

    if (!gladLoadGL())
    {
        throw std::runtime_error("Failed to load GL functions");
    }

    mx::FileSearchPath search = _options.searchPath;
    search.append(mx::FilePath::getCurrentPath());
    for (const mx::FilePath& p : _options.libraryFolders)
    {
        search.append(p);
    }
    _options.searchPath = search;
    _stdlib = mx::createDocument();
    mx::loadLibraries(_options.libraryFolders, search, _stdlib);

    mx::ShaderGeneratorPtr gen = mx::GlslShaderGenerator::create();
    _genContext = mx::GenContext(gen);
    _genContext.registerSourceCodeSearchPath(search);
    _genContext.getOptions().targetColorSpaceOverride = "lin_rec709";
    _genContext.getOptions().fileTextureVerticalFlip = true;
    _genContext.getOptions().hwShadowMap = true;
    _genContext.getOptions().hwImplicitBitangents = false;
    _genContext.getOptions().hwMaxActiveLightSources = 3;
    _genContext.getOptions().hwSpecularEnvironmentMethod = mx::SPECULAR_ENVIRONMENT_FIS;

    // Color management and unit system mirror Viewer::initContext
    mx::ColorManagementSystemPtr cms = mx::DefaultColorManagementSystem::create(_genContext.getShaderGenerator().getTarget());
    cms->loadLibrary(_stdlib);
    _genContext.getShaderGenerator().setColorManagementSystem(cms);

    mx::UnitSystemPtr unitSystem = mx::UnitSystem::create(_genContext.getShaderGenerator().getTarget());
    unitSystem->loadLibrary(_stdlib);
    _unitRegistry = mx::UnitConverterRegistry::create();
    mx::UnitTypeDefPtr distanceTypeDef = _stdlib->getUnitTypeDef("distance");
    if (distanceTypeDef)
    {
        mx::LinearUnitConverterPtr distanceConverter = mx::LinearUnitConverter::create(distanceTypeDef);
        _unitRegistry->addUnitConverter(distanceTypeDef, distanceConverter);
    }
    mx::UnitTypeDefPtr angleTypeDef = _stdlib->getUnitTypeDef("angle");
    if (angleTypeDef)
    {
        mx::LinearUnitConverterPtr angleConverter = mx::LinearUnitConverter::create(angleTypeDef);
        _unitRegistry->addUnitConverter(angleTypeDef, angleConverter);
    }
    unitSystem->setUnitConverterRegistry(_unitRegistry);
    _genContext.getShaderGenerator().setUnitSystem(unitSystem);
    _genContext.getOptions().targetDistanceUnit = "meter";

    _imageHandler = mx::GLTextureHandler::create(mx::StbImageLoader::create());
    _imageHandler->setSearchPath(search);
    _geometryHandler = mx::GeometryHandler::create();
    _geometryHandler->addLoader(mx::TinyObjLoader::create());
    _geometryHandler->addLoader(mx::CgltfLoader::create());
    _lightHandler = mx::LightHandler::create();
    _lightHandler->setRefractionTwoSided(true);
    _lightHandler->setUsePrefilteredMap(_genContext.getOptions().hwSpecularEnvironmentMethod != mx::SPECULAR_ENVIRONMENT_FIS);
    _shadowCamera = mx::Camera::create();
    _camera = mx::Camera::create();

    _width = static_cast<unsigned int>(_options.screenWidth);
    _height = static_cast<unsigned int>(_options.screenHeight);
    _envRadianceFilename = _options.envRadianceFilename;
    _cameraPos = mx::Vector3(0.0f, 0.0f, 5.0f);
    _cameraTarget = mx::Vector3(0.0f, 0.0f, 0.0f);
    _cameraViewAngle = 45.0f;
    _cameraZoom = 1.0f;

    loadDocumentFromFile(mx::FilePath(_options.materialFilename));

    _initialized = true;
}

void RemoteViewerEgl::loadDocumentFromFile(const mx::FilePath& filename)
{
    mx::FileSearchPath docSearch = _options.searchPath;
    docSearch.append(filename.getParentPath());
    if (_imageHandler)
    {
        _imageHandler->setSearchPath(docSearch);
    }

    std::cout << "[RemoteViewerEgl] load doc=" << filename.asString()
              << " searchPath=" << docSearch.asString() << std::endl;

    _doc = mx::createDocument();
    mx::XmlReadOptions readOptions;
    readOptions.readComments = false;
    mx::readFromXmlFile(_doc, filename, docSearch, &readOptions);
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

    if (_shader)
    {
        auto glslMaterial = std::dynamic_pointer_cast<mx::GlslMaterial>(_material);
        if (glslMaterial)
        {
            glslMaterial->setProgramStages(_shader->getSourceCode(mx::Stage::VERTEX), _shader->getSourceCode(mx::Stage::PIXEL));
            _program = glslMaterial->getProgram();
        }
    }

    std::cout << "[RemoteViewerEgl] material element=" << renderable->getNamePath() << std::endl;

    _geometryHandler->clearGeometry();
    mx::FilePath meshPath = _options.searchPath.find(_options.meshFilename);
    if (meshPath.isEmpty())
    {
        meshPath = _options.meshFilename;
    }
    std::cout << "[RemoteViewerEgl] mesh path=" << meshPath.asString() << std::endl;
    if (!_geometryHandler->loadGeometry(meshPath.asString()))
    {
        throw std::runtime_error("Failed to load geometry: " + meshPath.asString());
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
        if (!found)
        {
            _activeGeometryId.clear(); // Unknown selection: render all meshes
        }
    }

    if (!meshes.empty())
    {
        std::cout << "[RemoteViewerEgl] geometry count=" << meshes.size() << " active="
                  << (_activeGeometryId.empty() ? std::string("<all>") : _activeGeometryId) << std::endl;
        for (const auto& mesh : meshes)
        {
            if (!mesh) continue;
            std::cout << "[RemoteViewerEgl] mesh name=" << mesh->getName() << " partitions=" << mesh->getPartitionCount()
                      << " faces=" << mesh->getPartition(0)->getFaceCount() << std::endl;
        }
        const mx::Vector3& boxMax = _geometryHandler->getMaximumBounds();
        const mx::Vector3& boxMin = _geometryHandler->getMinimumBounds();
        std::cout << "[RemoteViewerEgl] bounds min=(" << boxMin[0] << "," << boxMin[1] << "," << boxMin[2]
              << ") max=(" << boxMax[0] << "," << boxMax[1] << "," << boxMax[2] << ")" << std::endl;
    }

    mx::FilePath envRadiancePath = docSearch.find(_envRadianceFilename);
    if (envRadiancePath.isEmpty())
    {
        std::string s = _envRadianceFilename.asString();
        for (char& c : s) if (c == '\\') c = '/';
        envRadiancePath = mx::FilePath(s);
    }
    mx::ImagePtr envRadianceMap = _imageHandler->acquireImage(envRadiancePath);
    mx::ImagePtr envIrradianceMap;
    _lightRigDoc = nullptr;
    _lightRigFilename = mx::FilePath();

    if (envRadianceMap)
    {
        std::cout << "[RemoteViewerEgl] env radiance path=" << envRadiancePath.asString() << std::endl;

        if (_normalizeEnvironment)
        {
            envRadianceMap = mx::normalizeEnvironment(envRadianceMap, IDEAL_ENV_MAP_RADIANCE, MAX_ENV_TEXEL_RADIANCE);
            if (_saveGeneratedLights)
            {
                _imageHandler->saveImage("NormalizedRadiance.hdr", envRadianceMap);
            }
        }

        if (_splitDirectLight)
        {
            _lightRigDoc = splitDirectLight(envRadianceMap, envRadianceMap);
            _lightRigFilename = envRadiancePath;
            _lightRigFilename.removeExtension();
            _lightRigFilename.addExtension(mx::MTLX_EXTENSION);
            if (_saveGeneratedLights)
            {
                _imageHandler->saveImage("IndirectRadiance.hdr", envRadianceMap);
                if (_lightRigDoc)
                {
                    mx::writeToXmlFile(_lightRigDoc, "DirectLightRig.mtlx");
                }
            }
        }
        else
        {
            _lightRigFilename = envRadiancePath;
            _lightRigFilename.removeExtension();
            _lightRigFilename.addExtension(mx::MTLX_EXTENSION);
            _lightRigFilename = docSearch.find(_lightRigFilename);
            if (_lightRigFilename.exists())
            {
                _lightRigDoc = mx::createDocument();
                mx::readFromXmlFile(_lightRigDoc, _lightRigFilename, docSearch);
            }
        }

        if (!_normalizeEnvironment && !_splitDirectLight)
        {
            mx::FilePath envIrradiancePath = envRadiancePath.getParentPath() / IRRADIANCE_MAP_FOLDER / envRadiancePath.getBaseName();
            envIrradianceMap = _imageHandler->acquireImage(envIrradiancePath);
        }

        if (!envIrradianceMap || envIrradianceMap->getWidth() == 1)
        {
            if (_generateReferenceIrradiance)
            {
                envIrradianceMap = mx::renderReferenceIrradiance(envRadianceMap, IRRADIANCE_MAP_WIDTH, IRRADIANCE_MAP_HEIGHT);
                if (_saveGeneratedLights)
                {
                    _imageHandler->saveImage("ReferenceIrradiance.hdr", envIrradianceMap);
                }
            }
            else
            {
                mx::Sh3ColorCoeffs shIrradiance = mx::projectEnvironment(envRadianceMap, true);
                envIrradianceMap = mx::renderEnvironment(shIrradiance, IRRADIANCE_MAP_WIDTH, IRRADIANCE_MAP_HEIGHT);
                if (_saveGeneratedLights)
                {
                    _imageHandler->saveImage("SphericalHarmonicIrradiance.hdr", envIrradianceMap);
                }
                std::cout << "[RemoteViewerEgl] generated SH irradiance map" << std::endl;
            }
        }

        _lightHandler->setEnvRadianceMap(envRadianceMap);
        _lightHandler->setEnvIrradianceMap(envIrradianceMap);
        _lightHandler->setEnvPrefilteredMap(nullptr);
    }
    else
    {
        std::cout << "[RemoteViewerEgl] failed to load env radiance map: " << envRadiancePath.asString() << std::endl;
    }

    if (_lightRigDoc)
    {
        _doc->importLibrary(_lightRigDoc);
    }

    std::vector<mx::NodePtr> lights;
    _lightHandler->findLights(_doc, lights);
    _lightHandler->registerLights(_doc, lights, _genContext);
    _lightHandler->setLightSources(lights);
    _lightHandler->setEnvLightIntensity(_envLightIntensity);
    _lightHandler->setLightTransform(mx::Matrix44::createRotationY(_lightRotation / 180.0f * PI_F));

    centerCameraToGeometry();
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
        // Use 8-bit sRGB framebuffer to match windowed viewer backbuffer behavior
        _framebuffer = mx::GLFramebuffer::create(width, height, 4, mx::Image::BaseType::UINT8);
        _framebuffer->setEncodeSrgb(true);
        _width = width;
        _height = height;
        applyCameraState();
    }
}

mx::ImagePtr RemoteViewerEgl::renderShadowMap(mx::NodePtr dirLight, int shadowMapSize)
{
    if (!_genContext.getOptions().hwShadowMap || !dirLight || !_geometryHandler || !_shadowCamera || !_imageHandler)
    {
        return nullptr;
    }

    const mx::Vector3& boxMax = _geometryHandler->getMaximumBounds();
    const mx::Vector3& boxMin = _geometryHandler->getMinimumBounds();
    const mx::Vector3 sphereCenter = (boxMax + boxMin) * 0.5f;
    const float r = (sphereCenter - boxMin).getMagnitude();
    if (r <= 0.0f)
    {
        return nullptr;
    }

    _shadowCamera->setWorldMatrix(mx::Matrix44::createTranslation(-sphereCenter));
    _shadowCamera->setProjectionMatrix(mx::Camera::createOrthographicMatrixZP(-r, r, -r, r, 0.0f, r * 2.0f));
    mx::ValuePtr value = dirLight->getInputValue("direction");
    if (value && value->isA<mx::Vector3>())
    {
        mx::Vector3 dir = mx::Matrix44::createRotationY(_lightRotation / 180.0f * PI_F).transformVector(value->asA<mx::Vector3>());
        _shadowCamera->setViewMatrix(mx::Camera::createViewMatrix(dir * -r, mx::Vector3(0.0f), mx::Vector3(0, 1, 0)));
    }

    if (!_shadowMaterial)
    {
        try
        {
            mx::ShaderPtr hwShader = mx::createDepthShader(_genContext, _stdlib, "__SHADOW_SHADER__");
            _shadowMaterial = mx::GlslMaterial::create();
            _shadowMaterial->generateShader(hwShader);
        }
        catch (const std::exception& e)
        {
            std::cerr << "Failed to generate shadow shader: " << e.what() << std::endl;
            _shadowMaterial = nullptr;
            return nullptr;
        }
    }

    if (!_shadowBlurMaterial)
    {
        try
        {
            mx::ShaderPtr hwShader = mx::createBlurShader(_genContext, _stdlib, "__SHADOW_BLUR_SHADER__", "gaussian", 1.0f);
            _shadowBlurMaterial = mx::GlslMaterial::create();
            _shadowBlurMaterial->generateShader(hwShader);
        }
        catch (const std::exception& e)
        {
            std::cerr << "Failed to generate shadow blur shader: " << e.what() << std::endl;
            _shadowBlurMaterial = nullptr;
        }
    }

    mx::GLFramebufferPtr framebuffer = mx::GLFramebuffer::create(static_cast<unsigned int>(shadowMapSize), static_cast<unsigned int>(shadowMapSize), 2, mx::Image::BaseType::FLOAT);
    framebuffer->bind();
    glViewport(0, 0, shadowMapSize, shadowMapSize);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    auto shadowMaterial = std::dynamic_pointer_cast<mx::GlslMaterial>(_shadowMaterial);
    if (!shadowMaterial)
    {
        framebuffer->unbind();
        return nullptr;
    }

    shadowMaterial->bindShader();
    for (const auto& mesh : _geometryHandler->getMeshes())
    {
        if (!mesh)
        {
            continue;
        }
        shadowMaterial->bindMesh(mesh);
        shadowMaterial->bindViewInformation(_shadowCamera);
        for (size_t i = 0; i < mesh->getPartitionCount(); ++i)
        {
            mx::MeshPartitionPtr geom = mesh->getPartition(i);
            if (geom)
            {
                shadowMaterial->drawPartition(geom);
            }
        }
    }

    if (_shadowMap && _imageHandler)
    {
        _imageHandler->releaseRenderResources(_shadowMap);
    }
    _shadowMap = framebuffer->getColorImage();

    if (_shadowBlurMaterial && _shadowSoftness > 0)
    {
        if (!_quadMesh)
        {
            _quadMesh = mx::GeometryHandler::createQuadMesh();
        }

        mx::ImageSamplingProperties blurSamplingProperties;
        blurSamplingProperties.uaddressMode = mx::ImageSamplingProperties::AddressMode::CLAMP;
        blurSamplingProperties.vaddressMode = mx::ImageSamplingProperties::AddressMode::CLAMP;
        blurSamplingProperties.filterType = mx::ImageSamplingProperties::FilterType::CLOSEST;
        for (unsigned int i = 0; i < _shadowSoftness; ++i)
        {
            framebuffer->bind();
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

            auto blurMaterial = std::dynamic_pointer_cast<mx::GlslMaterial>(_shadowBlurMaterial);
            if (!blurMaterial)
            {
                break;
            }

            blurMaterial->bindShader();
            if (_imageHandler->bindImage(_shadowMap, blurSamplingProperties))
            {
                mx::GLTextureHandlerPtr textureHandler = std::static_pointer_cast<mx::GLTextureHandler>(_imageHandler);
                int textureLocation = textureHandler->getBoundTextureLocation(_shadowMap->getResourceId());
                if (textureLocation >= 0)
                {
                    blurMaterial->getProgram()->bindUniform("image_file", mx::Value::createValue(textureLocation));
                }
            }

            blurMaterial->bindMesh(_quadMesh);
            blurMaterial->drawPartition(_quadMesh->getPartition(0));
            _imageHandler->releaseRenderResources(_shadowMap);
            _shadowMap = framebuffer->getColorImage();
        }
    }

    framebuffer->unbind();
    glDisable(GL_FRAMEBUFFER_SRGB);

    return _shadowMap;
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
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE); // EGL sometimes preserves a cull state from previous clients; force disabled for parity
    glFrontFace(GL_CCW);
    glCullFace(GL_BACK);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_FRAMEBUFFER_SRGB); // clear in linear so background is unaffected
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_FRAMEBUFFER_SRGB); // enable for encoded draw writes

    mx::ShadowState shadowState;
    shadowState.ambientOcclusionGain = _ambientOcclusionGain;
    if (_lightHandler && _camera && _shadowCamera)
    {
        mx::NodePtr dirLight = _lightHandler->getFirstLightOfCategory(DIR_LIGHT_NODE_CATEGORY);
        if (dirLight && _genContext.getOptions().hwShadowMap)
        {
            mx::ImagePtr shadowMap = renderShadowMap(dirLight, SHADOW_MAP_SIZE);
            if (shadowMap)
            {
                shadowState.shadowMap = shadowMap;
                shadowState.shadowMatrix = _camera->getWorldMatrix().getInverse() * _shadowCamera->getWorldViewProjMatrix();
            }
            // Restore primary framebuffer and state after offscreen shadow pass.
            _framebuffer->bind();
            glViewport(0, 0, static_cast<GLsizei>(_width), static_cast<GLsizei>(_height));
            glEnable(GL_FRAMEBUFFER_SRGB);
        }
    }

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
    glDisable(GL_FRAMEBUFFER_SRGB);
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

        auto logFn = [](const std::string& msg) { std::cout << "[RemoteViewerEgl] " << msg << std::endl; };

        RemoteViewerCommon::UniformApplyResult reapplyResult = RemoteViewerCommon::reapplyStoredUniformValues(material, nullptr);
        if (!reapplyResult.success)
        {
            restoreState();
            meta["error"] = reapplyResult.error;
            return { meta, images };
        }

        RemoteViewerCommon::UniformApplyResult uniformResult = RemoteViewerCommon::applyUniformPayload(material, uniformsPayload, logFn);
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
        _lightHandler->setLightTransform(mx::Matrix44::createRotationY(_lightRotation / 180.0f * PI_F));
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
