//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#ifndef MATERIALXREMOTE_REMOTEVIEWERCOMMON_H
#define MATERIALXREMOTE_REMOTEVIEWERCOMMON_H

#include <MaterialXRemote/Types.h>
#include <MaterialXRender/ShaderMaterial.h>

#include <json/json.h>

#include <functional>
#include <string>

namespace mx = MaterialX;

MATERIALX_NAMESPACE_BEGIN

namespace RemoteViewerCommon
{
struct UniformApplyResult
{
    bool success = true;
    std::string error;
};

struct CompileResult
{
    bool success = true;
    double compileMs = 0.0;
    Json::Value compileErrors { Json::arrayValue };
    std::string error;
};

struct PackResult
{
    bool success = true;
    std::string error;
};

// Apply a JSON uniform payload to a material. On failure returns success=false and an error message.
// Optional logFn is invoked with human-readable status strings when uniforms are applied.
UniformApplyResult applyUniformPayload(mx::MaterialPtr material,
                                       const Json::Value& uniformsPayload,
                                       const std::function<void(const std::string&)>& logFn = {});

// Merge candidate shader stages with generated stages and compile/apply to the given material.
// Returns mergedPkg containing the actual stages used. On compile failures, success=false and
// compileErrors is populated. On other failures, error contains a message.
CompileResult compileAndApplyShader(mx::MaterialPtr material,
                                    const ShaderPackage& candidatePkg,
                                    ShaderPackage& mergedPkg);

// Pack an Image into tightly packed float32 RGB bytes with frame descriptor metadata.
PackResult packImageToFloatRgb(mx::ImagePtr img, std::string& outBytes, Json::Value& frameDesc);
}

MATERIALX_NAMESPACE_END

#endif
