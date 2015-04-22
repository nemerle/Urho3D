//
// Copyright (c) 2008-2014 the Urho3D project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "Precompiled.h"
#include "Graphics.h"
#include "GraphicsImpl.h"
#include "ShaderProgram.h"
#include "ShaderVariation.h"

#include "DebugNew.h"

namespace Urho3D
{

ShaderProgram::ShaderProgram(Graphics* graphics, ShaderVariation* vertexShader, ShaderVariation* pixelShader) :
    GPUObject(graphics),
    vertexShader_(vertexShader),
    pixelShader_(pixelShader)
{
    for (auto & elem : useTextureUnit_)
        elem = false;
}

ShaderProgram::~ShaderProgram()
{
    Release();
}

void ShaderProgram::OnDeviceLost()
{
    GPUObject::OnDeviceLost();

    if (graphics_ && graphics_->GetShaderProgram() == this)
        graphics_->SetShaders(nullptr, nullptr);


    linkerOutput_.clear();
}

void ShaderProgram::Release()
{
    if (object_)
    {
        if (!graphics_)
            return;

        if (!graphics_->IsDeviceLost())
        {
            if (graphics_->GetShaderProgram() == this)
                graphics_->SetShaders(nullptr, nullptr);

            glDeleteProgram(object_);
        }

        object_ = 0;
        linkerOutput_.clear();
        shaderParameters_.clear();

        for (auto & elem : useTextureUnit_)
            elem = false;
    }
}

bool ShaderProgram::Link()
{
    Release();

    if (!vertexShader_ || !pixelShader_ || !vertexShader_->GetGPUObject() || !pixelShader_->GetGPUObject())
        return false;

    object_ = glCreateProgram();
    if (!object_)
    {
        linkerOutput_ = "Could not create shader program";
        return false;
    }

    // Bind vertex attribute locations to ensure they are the same in all shaders
    // Note: this is not the same order as in VertexBuffer, instead a remapping is used to ensure everything except cube texture
    // coordinates fit to the first 8 for better GLES2 device compatibility
    glBindAttribLocation(object_, 0, "iPos");
    glBindAttribLocation(object_, 1, "iNormal");
    glBindAttribLocation(object_, 2, "iColor");
    glBindAttribLocation(object_, 3, "iTexCoord");
    glBindAttribLocation(object_, 4, "iTexCoord2");
    glBindAttribLocation(object_, 5, "iTangent");
    glBindAttribLocation(object_, 6, "iBlendWeights");
    glBindAttribLocation(object_, 7, "iBlendIndices");
    glBindAttribLocation(object_, 8, "iCubeTexCoord");
    glBindAttribLocation(object_, 9, "iCubeTexCoord2");
    #ifndef GL_ES_VERSION_2_0
    glBindAttribLocation(object_, 10, "iInstanceMatrix1");
    glBindAttribLocation(object_, 11, "iInstanceMatrix2");
    glBindAttribLocation(object_, 12, "iInstanceMatrix3");
    #endif

    glAttachShader(object_, vertexShader_->GetGPUObject());
    glAttachShader(object_, pixelShader_->GetGPUObject());
    glLinkProgram(object_);

    int linked, length;
    glGetProgramiv(object_, GL_LINK_STATUS, &linked);
    if (!linked)
    {
        glGetProgramiv(object_, GL_INFO_LOG_LENGTH, &length);
        linkerOutput_.resize(length);
        int outLength;
        glGetProgramInfoLog(object_, length, &outLength, &linkerOutput_[0]);
        glDeleteProgram(object_);
        object_ = 0;
    }
    else
        linkerOutput_.clear();

    if (!object_)
        return false;

    const int MAX_PARAMETER_NAME_LENGTH = 256;
    char uniformName[MAX_PARAMETER_NAME_LENGTH];
    int uniformCount;

    glUseProgram(object_);
    glGetProgramiv(object_, GL_ACTIVE_UNIFORMS, &uniformCount);

    // Check for shader parameters and texture units
    for (int i = 0; i < uniformCount; ++i)
    {
        unsigned type;
        int count;

        glGetActiveUniform(object_, i, MAX_PARAMETER_NAME_LENGTH, nullptr, &count, &type, uniformName);
        int location = glGetUniformLocation(object_, uniformName);

        // Skip inbuilt or disabled uniforms
        if (location < 0)
            continue;

        // Check for array index included in the name and strip it
        String name(uniformName);
        unsigned index = name.indexOf('[');
        if (index != String::NPOS)
        {
            // If not the first index, skip
            if (name.indexOf("[0]", index) == String::NPOS)
                continue;

            name = name.Substring(0, index);
        }

        if (name[0] == 'c')
        {
            // Store the constant uniform mapping
            String paramName = name.Substring(1);
            ShaderParameter newParam;
            newParam.location_ = location;
            newParam.type_ = type;
            shaderParameters_[StringHash(paramName)] = newParam;
        }
        else if (name[0] == 's')
        {
            // Set the samplers here so that they do not have to be set later
            int unit = graphics_->GetTextureUnit(name.Substring(1));
            if (unit >= MAX_TEXTURE_UNITS)
            {
                // If texture unit name is not recognized, search for a digit in the name and use that as the unit index
                for (unsigned j = 1; j < name.length(); ++j)
                {
                    if (name[j] >= '0' && name[j] <= '9')
                    {
                        unit = name[j] - '0';
                        break;
                    }
                }
            }

            if (unit < MAX_TEXTURE_UNITS)
            {
                useTextureUnit_[unit] = true;
                glUniform1iv(location, 1, &unit);
            }
        }
    }

    // Rehash the parameter map to ensure minimal load factor
    shaderParameters_.reserve(NextPowerOfTwo(shaderParameters_.size()));

    return true;
}

ShaderVariation* ShaderProgram::GetVertexShader() const
{
    return vertexShader_;
}

ShaderVariation* ShaderProgram::GetPixelShader() const
{
    return pixelShader_;
}

bool ShaderProgram::HasParameter(StringHash param) const
{
    return shaderParameters_.contains(param);
}

const ShaderParameter* ShaderProgram::GetParameter(StringHash param) const
{
    auto i = shaderParameters_.find(param);
    if (i != shaderParameters_.end())
        return &(MAP_VALUE(i));
    else
        return nullptr;
}

}
