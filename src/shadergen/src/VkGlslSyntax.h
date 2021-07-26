#pragma once

#include <MaterialXGenGlsl/GlslSyntax.h>

namespace shadergen
{
  class VkGlslSyntax : public MaterialX::GlslSyntax
  {
  public:
    explicit VkGlslSyntax();

    static MaterialX::SyntaxPtr create() { return std::make_shared<VkGlslSyntax>(); }
  };
}
