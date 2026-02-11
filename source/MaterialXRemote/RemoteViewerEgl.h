//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#ifndef MATERIALXREMOTE_REMOTEVIEWEREGL_H
#define MATERIALXREMOTE_REMOTEVIEWEREGL_H

#include <MaterialXCore/Library.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXFormat/File.h>
#include <MaterialXGenGlsl/GlslShaderGenerator.h>
#include <MaterialXRenderGlsl/GlslProgram.h>
#include <MaterialXRenderGlsl/GLFramebuffer.h>
#include <MaterialXRenderGlsl/GLTextureHandler.h>
#include <MaterialXRenderGlsl/GlslMaterial.h>
#include <MaterialXRender/ShaderMaterial.h>
#include <MaterialXRender/Camera.h>
#include <MaterialXRender/GeometryHandler.h>
#include <MaterialXRender/LightHandler.h>
#include <MaterialXRemote/Types.h>
#include <MaterialXRenderGlsl/GLUtil.h>

#include <json/json.h>

#include <memory>
#include <string>
#include <vector>

namespace mx = MaterialX;

MATERIALX_NAMESPACE_BEGIN

/// Minimal headless EGL-only viewer for remote rendering.
class RemoteViewerEgl
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
        int gpuIndex = 0;
        // Headless flag: false -> windowed, true -> windowless. EGL builds ignore and run headless.
        bool headless = true;
    };

    explicit RemoteViewerEgl(const Options& options);
    ~RemoteViewerEgl();

    void initializeRemote();
    void loadDocumentFromFile(const mx::FilePath& filename);

    Json::Value applyShaderPackage(const ShaderPackage& pkg);
    std::pair<Json::Value, std::vector<std::string>> renderStateless(const ShaderPackage& candidatePkg,
                                                                     const Json::Value& uniformsPayload,
                                                                     unsigned int frames,
                                                                     unsigned int width,
                                                                     unsigned int height,
                                                                     unsigned int warmup);

    // Compatibility surface with the windowed viewer API used by RemoteServer routes.
    void setCameraPosition(const mx::Vector3& pos);
    void setCameraTarget(const mx::Vector3& tgt);
    mx::Vector3 getCameraPosition() const { return _cameraPos; }
    mx::Vector3 getCameraTarget() const { return _cameraTarget; }

    void setCameraViewAngle(float degrees);
    float getCameraViewAngle() const { return _cameraViewAngle; }

    void setCameraZoom(float zoom);
    float getCameraZoom() const { return _cameraZoom; }

    void setEnvRadianceFilename(const mx::FilePath& path);
    const mx::FilePath& getEnvRadianceFilename() const { return _envRadianceFilename; }

    void setEnvLightIntensity(float intensity);
    float getEnvLightIntensity() const { return _envLightIntensity; }

    void setLightRotation(float yRotationDegrees);
    float getLightRotation() const { return _lightRotation; }

    std::vector<std::string> listGeometry() const;
    std::string getActiveGeometryId() const { return _activeGeometryId; }
    void setActiveGeometryById(const std::string& id);

    mx::MaterialPtr getSelectedMaterial();
    mx::ShaderPtr getShader() const { return _shader; }

    bool isHeadless() const { return true; }

  private:
    void applyCameraState();
    void centerCameraToGeometry();
    bool ensureShader(const ShaderPackage& pkg);
    void resizeFramebuffer(unsigned int width, unsigned int height);
    mx::ImagePtr renderShadowMap(mx::NodePtr dirLight, int shadowMapSize);
    void renderFrame();
    bool captureFrame(std::string& outBytes, Json::Value& frameDesc);

  private:
    Options _options;
    bool _initialized = false;
    EglHeadlessContext _eglCtx;

    // MaterialX state
    mx::GenContext _genContext;
    mx::DocumentPtr _stdlib;
    mx::DocumentPtr _doc;
    mx::MaterialPtr _material;
    mx::ShaderPtr _shader;
    mx::UnitConverterRegistryPtr _unitRegistry;

    mx::GlslProgramPtr _program;
    mx::GLFramebufferPtr _framebuffer;
    mx::ImageHandlerPtr _imageHandler;
    mx::GeometryHandlerPtr _geometryHandler;
    mx::LightHandlerPtr _lightHandler;
    mx::CameraPtr _camera;
    mx::CameraPtr _shadowCamera;
    mx::ImagePtr _shadowMap;
    mx::GlslMaterialPtr _shadowMaterial;
    mx::GlslMaterialPtr _shadowBlurMaterial;
    unsigned int _shadowSoftness { 1 };
    mx::MeshPtr _quadMesh;
    float _geometryScale { 1.0f };
    mx::DocumentPtr _lightRigDoc;
    mx::FilePath _lightRigFilename;

    mx::Vector3 _cameraPos { 0.0f, 0.0f, 5.0f };
    mx::Vector3 _cameraTarget { 0.0f, 0.0f, 0.0f };
    float _cameraViewAngle { 45.0f };
    float _cameraZoom { 1.0f };
    mx::Vector3 _centeringOffset { 0.0f, 0.0f, 0.0f };

    mx::FilePath _envRadianceFilename;
    float _envLightIntensity { 1.0f };
    float _lightRotation { 0.0f };
    float _ambientOcclusionGain { 0.6f };
    bool _normalizeEnvironment { false };
    bool _splitDirectLight { false };
    bool _generateReferenceIrradiance { false };
    bool _saveGeneratedLights { false };

    std::string _activeGeometryId;

    unsigned int _width = 0;
    unsigned int _height = 0;
};

MATERIALX_NAMESPACE_END

#endif
