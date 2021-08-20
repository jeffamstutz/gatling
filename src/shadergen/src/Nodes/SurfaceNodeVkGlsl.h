#pragma once

#include <MaterialXGenShader/ShaderNodeImpl.h>

namespace mx = MaterialX;

namespace shadergen
{
  class SurfaceNodeVkGlsl : public mx::ShaderNodeImpl
  {
  public:
    static mx::ShaderNodeImplPtr create();

    void createVariables(const mx::ShaderNode& node, mx::GenContext& context, mx::Shader& shader) const override;

    void emitFunctionCall(const mx::ShaderNode& node, mx::GenContext& context, mx::ShaderStage& stage) const override;
  };
}
