#include "VkGlslSyntax.h"

#include <MaterialXGenShader/TypeDesc.h>

namespace mx = MaterialX;

namespace shadergen
{
  VkGlslSyntax::VkGlslSyntax() : mx::GlslSyntax()
  {
    registerTypeSyntax
    (
      mx::Type::SURFACESHADER,
      std::make_shared<mx::AggregateTypeSyntax>(
        "surfaceshader",
        "surfaceshader(vec3(0.0),vec3(0.0),vec3(0.0))",
        mx::EMPTY_STRING,
        mx::EMPTY_STRING,
        "struct surfaceshader { vec3 color; vec3 emission; vec3 transparency; };")
    );

    // TODO: add new keywords and types of GL_KHR_vulkan_glsl extension
  }
}
