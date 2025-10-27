//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#ifndef MATERIALXREMOTE_TYPES_H
#define MATERIALXREMOTE_TYPES_H

#include <MaterialXCore/Library.h>
#include <MaterialXCore/Util.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace mx = MaterialX;

MATERIALX_NAMESPACE_BEGIN

struct SessionMaterial
{
    std::string name;
    std::string filePath;
};

// Lightweight shader package container: simple storage of stage source strings.
// Renamed from ShaderOverride to ShaderPackage to reflect that this is
// the authoritative set of GLSL stages used for rendering in a session.
struct ShaderPackage
{
    std::string vertex;
    std::string fragment;
};

MATERIALX_NAMESPACE_END

#endif
