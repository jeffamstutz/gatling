//
// TM & (c) 2020 Lucasfilm Entertainment Company Ltd. and Lucasfilm Ltd.
// All rights reserved. See LICENSE.txt for license.
//

#include "SurfaceNodeVkGlsl.h"

#include "VkGlslShaderGenerator.h"

#include <MaterialXGenShader/GenContext.h>

namespace shadergen
{
  mx::ShaderNodeImplPtr SurfaceNodeVkGlsl::create()
  {
    return std::make_shared<SurfaceNodeVkGlsl>();
  }

  void SurfaceNodeVkGlsl::createVariables(const mx::ShaderNode & node, mx::GenContext & context, mx::Shader & shader) const
  {
  }

  void SurfaceNodeVkGlsl::emitFunctionCall(const mx::ShaderNode& node, mx::GenContext& context, mx::ShaderStage& stage) const
  {
    BEGIN_SHADER_STAGE(stage, mx::Stage::PIXEL)

    const VkGlslShaderGenerator& shadergen = static_cast<const VkGlslShaderGenerator&>(context.getShaderGenerator());
    const mx::ShaderGraph& graph = *node.getParent();

    const mx::ShaderOutput* output = node.getOutput();
    const mx::string outColor = output->getVariable() + ".color";
    const mx::string outEmission = output->getVariable() + ".emission";
    const mx::string outTransparency = output->getVariable() + ".transparency";

    shadergen.emitLineBegin(stage);
    shadergen.emitOutput(node.getOutput(), true, true, context, stage);
    shadergen.emitLineEnd(stage);

    shadergen.emitComment("Add surface emission", stage);
    shadergen.emitScopeBegin(stage);
    mx::string emission;
    shadergen.emitEdfNodes(graph, node, context, stage, emission);
    shadergen.emitLine(outEmission + " += " + emission, stage);
    shadergen.emitScopeEnd(stage);

    shadergen.emitLineBreak(stage);

    shadergen.emitComment("Add indirect contribution", stage);
    shadergen.emitScopeBegin(stage);
    mx::string bsdf;
    shadergen.emitBsdfNodes(graph, node, context, stage, bsdf);
    shadergen.emitLineBreak(stage);
    shadergen.emitLine(outColor + " += " + bsdf, stage);
    shadergen.emitScopeEnd(stage);

    shadergen.emitLineBreak(stage);

    shadergen.emitLine(outTransparency + " = vec3(1.0)", stage);

    END_SHADER_STAGE(stage, Stage::PIXEL)
  }
}
