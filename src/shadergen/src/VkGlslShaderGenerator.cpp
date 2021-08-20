//
// TM & (c) 2020 Lucasfilm Entertainment Company Ltd. and Lucasfilm Ltd.
// All rights reserved. See LICENSE.txt for license.
//

#include "VkGlslShaderGenerator.h"

#include "VkGlslSyntax.h"
#include "Nodes/SurfaceNodeVkGlsl.h"

#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenShader/Nodes/SwizzleNode.h>
#include <MaterialXGenShader/Nodes/ConvertNode.h>
#include <MaterialXGenShader/Nodes/CombineNode.h>
#include <MaterialXGenShader/Nodes/SwitchNode.h>
#include <MaterialXGenShader/Nodes/IfNode.h>
#include <MaterialXGenShader/Nodes/LayerNode.h>
#include <MaterialXGenShader/Nodes/ThinFilmNode.h>
#include <MaterialXGenShader/Nodes/BsdfNodes.h>

#include <deque>

namespace mx = MaterialX;

namespace shadergen
{
  // <DEBUG>
  class NoOpNodeVkGlsl : public mx::ShaderNodeImpl
  {
  public:
    static mx::ShaderNodeImplPtr create()
    {
      return std::make_shared<NoOpNodeVkGlsl>();
    }
    void createVariables(const mx::ShaderNode& node, mx::GenContext&, mx::Shader& shader) const override
    {
    }
    void emitFunctionCall(const mx::ShaderNode& node, mx::GenContext& context, mx::ShaderStage& stage) const override
    {
    }
  };
  // </DEBUG>

  const mx::string VkGlslShaderGenerator::TARGET = "genvkglsl";
  const mx::string VkGlslShaderGenerator::GLSL_VERSION = "450";

  namespace VKGLSL
  {
    const mx::string SURFACE_INPUTS = "SurfaceInputs";
    const mx::string SURFACE_OUTPUTS = "SurfaceOutputs";
    const mx::string T_IN_POSITION = "$inPosition";
    const mx::string T_IN_NORMAL = "$inNormal";
    const mx::string DIR_N = "N";
    const mx::string DIR_L = "L";
    const mx::string DIR_V = "V";
    const mx::string WORLD_POSITION = "P";
    // TODO: geomprop for normal & tangent?
  }

  VkGlslShaderGenerator::VkGlslShaderGenerator() :
    ShaderGenerator(shadergen::VkGlslSyntax::create())
  {
    //
    // Register all custom node implementation classes
    //

    // <!-- <if*> -->
    static const mx::string SEPARATOR = "_";
    static const mx::string INT_SEPARATOR = "I_";
    static const mx::string BOOL_SEPARATOR = "B_";
    static const mx::StringVec IMPL_PREFIXES = { "IM_ifgreater_", "IM_ifgreatereq_", "IM_ifequal_" };
    static const mx::vector<mx::CreatorFunction<mx::ShaderNodeImpl>> IMPL_CREATE_FUNCTIONS =
    { mx::IfGreaterNode::create,  mx::IfGreaterEqNode::create, mx::IfEqualNode::create };
    static const mx::vector<bool> IMPL_HAS_INTVERSION = { true, true, true };
    static const mx::vector<bool> IMPL_HAS_BOOLVERSION = { false, false, true };
    static const mx::StringVec IMPL_TYPES = { "float", "color3", "color4", "vector2", "vector3", "vector4" };
    for (size_t i = 0; i < IMPL_PREFIXES.size(); i++)
    {
      const mx::string& implPrefix = IMPL_PREFIXES[i];
      for (const mx::string& implType : IMPL_TYPES)
      {
        const mx::string implRoot = implPrefix + implType;
        registerImplementation(implRoot + SEPARATOR + VkGlslShaderGenerator::TARGET, IMPL_CREATE_FUNCTIONS[i]);
        if (IMPL_HAS_INTVERSION[i])
        {
          registerImplementation(implRoot + INT_SEPARATOR + VkGlslShaderGenerator::TARGET, IMPL_CREATE_FUNCTIONS[i]);
        }
        if (IMPL_HAS_BOOLVERSION[i])
        {
          registerImplementation(implRoot + BOOL_SEPARATOR + VkGlslShaderGenerator::TARGET, IMPL_CREATE_FUNCTIONS[i]);
        }
      }
    }

    // <!-- <switch> -->
    // <!-- 'which' type : float -->
    registerImplementation("IM_switch_float_" + VkGlslShaderGenerator::TARGET, mx::SwitchNode::create);
    registerImplementation("IM_switch_color3_" + VkGlslShaderGenerator::TARGET, mx::SwitchNode::create);
    registerImplementation("IM_switch_color4_" + VkGlslShaderGenerator::TARGET, mx::SwitchNode::create);
    registerImplementation("IM_switch_vector2_" + VkGlslShaderGenerator::TARGET, mx::SwitchNode::create);
    registerImplementation("IM_switch_vector3_" + VkGlslShaderGenerator::TARGET, mx::SwitchNode::create);
    registerImplementation("IM_switch_vector4_" + VkGlslShaderGenerator::TARGET, mx::SwitchNode::create);
    // <!-- 'which' type : integer -->
    registerImplementation("IM_switch_floatI_" + VkGlslShaderGenerator::TARGET, mx::SwitchNode::create);
    registerImplementation("IM_switch_color3I_" + VkGlslShaderGenerator::TARGET, mx::SwitchNode::create);
    registerImplementation("IM_switch_color4I_" + VkGlslShaderGenerator::TARGET, mx::SwitchNode::create);
    registerImplementation("IM_switch_vector2I_" + VkGlslShaderGenerator::TARGET, mx::SwitchNode::create);
    registerImplementation("IM_switch_vector3I_" + VkGlslShaderGenerator::TARGET, mx::SwitchNode::create);
    registerImplementation("IM_switch_vector4I_" + VkGlslShaderGenerator::TARGET, mx::SwitchNode::create);

    // <!-- <swizzle> -->
    // <!-- from type : float -->
    registerImplementation("IM_swizzle_float_color3_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_float_color4_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_float_vector2_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_float_vector3_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_float_vector4_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    // <!-- from type : color3 -->
    registerImplementation("IM_swizzle_color3_float_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_color3_color3_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_color3_color4_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_color3_vector2_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_color3_vector3_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_color3_vector4_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    // <!-- from type : color4 -->
    registerImplementation("IM_swizzle_color4_float_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_color4_color3_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_color4_color4_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_color4_vector2_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_color4_vector3_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_color4_vector4_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    // <!-- from type : vector2 -->
    registerImplementation("IM_swizzle_vector2_float_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_vector2_color3_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_vector2_color4_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_vector2_vector2_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_vector2_vector3_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_vector2_vector4_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    // <!-- from type : vector3 -->
    registerImplementation("IM_swizzle_vector3_float_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_vector3_color3_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_vector3_color4_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_vector3_vector2_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_vector3_vector3_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_vector3_vector4_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    // <!-- from type : vector4 -->
    registerImplementation("IM_swizzle_vector4_float_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_vector4_color3_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_vector4_color4_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_vector4_vector2_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_vector4_vector3_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);
    registerImplementation("IM_swizzle_vector4_vector4_" + VkGlslShaderGenerator::TARGET, mx::SwizzleNode::create);

    // <!-- <convert> -->
    registerImplementation("IM_convert_float_color3_" + VkGlslShaderGenerator::TARGET, mx::ConvertNode::create);
    registerImplementation("IM_convert_float_color4_" + VkGlslShaderGenerator::TARGET, mx::ConvertNode::create);
    registerImplementation("IM_convert_float_vector2_" + VkGlslShaderGenerator::TARGET, mx::ConvertNode::create);
    registerImplementation("IM_convert_float_vector3_" + VkGlslShaderGenerator::TARGET, mx::ConvertNode::create);
    registerImplementation("IM_convert_float_vector4_" + VkGlslShaderGenerator::TARGET, mx::ConvertNode::create);
    registerImplementation("IM_convert_vector2_vector3_" + VkGlslShaderGenerator::TARGET, mx::ConvertNode::create);
    registerImplementation("IM_convert_vector3_vector2_" + VkGlslShaderGenerator::TARGET, mx::ConvertNode::create);
    registerImplementation("IM_convert_vector3_color3_" + VkGlslShaderGenerator::TARGET, mx::ConvertNode::create);
    registerImplementation("IM_convert_vector3_vector4_" + VkGlslShaderGenerator::TARGET, mx::ConvertNode::create);
    registerImplementation("IM_convert_vector4_vector3_" + VkGlslShaderGenerator::TARGET, mx::ConvertNode::create);
    registerImplementation("IM_convert_vector4_color4_" + VkGlslShaderGenerator::TARGET, mx::ConvertNode::create);
    registerImplementation("IM_convert_color3_vector3_" + VkGlslShaderGenerator::TARGET, mx::ConvertNode::create);
    registerImplementation("IM_convert_color4_vector4_" + VkGlslShaderGenerator::TARGET, mx::ConvertNode::create);
    registerImplementation("IM_convert_color3_color4_" + VkGlslShaderGenerator::TARGET, mx::ConvertNode::create);
    registerImplementation("IM_convert_color4_color3_" + VkGlslShaderGenerator::TARGET, mx::ConvertNode::create);
    registerImplementation("IM_convert_boolean_float_" + VkGlslShaderGenerator::TARGET, mx::ConvertNode::create);
    registerImplementation("IM_convert_integer_float_" + VkGlslShaderGenerator::TARGET, mx::ConvertNode::create);

    // <!-- <combine> -->
    registerImplementation("IM_combine2_vector2_" + VkGlslShaderGenerator::TARGET, mx::CombineNode::create);
    registerImplementation("IM_combine2_color4CF_" + VkGlslShaderGenerator::TARGET, mx::CombineNode::create);
    registerImplementation("IM_combine2_vector4VF_" + VkGlslShaderGenerator::TARGET, mx::CombineNode::create);
    registerImplementation("IM_combine2_vector4VV_" + VkGlslShaderGenerator::TARGET, mx::CombineNode::create);
    registerImplementation("IM_combine3_color3_" + VkGlslShaderGenerator::TARGET, mx::CombineNode::create);
    registerImplementation("IM_combine3_vector3_" + VkGlslShaderGenerator::TARGET, mx::CombineNode::create);
    registerImplementation("IM_combine4_color4_" + VkGlslShaderGenerator::TARGET, mx::CombineNode::create);
    registerImplementation("IM_combine4_vector4_" + VkGlslShaderGenerator::TARGET, mx::CombineNode::create);

    // <!-- <layer> -->
    registerImplementation("IM_layer_bsdf_" + VkGlslShaderGenerator::TARGET, mx::LayerNode::create);
    // <!-- <thin_film_bsdf> -->
    registerImplementation("IM_thin_film_bsdf_" + VkGlslShaderGenerator::TARGET, mx::ThinFilmNode::create);
    // <!-- <dielectric_bsdf> -->
    registerImplementation("IM_dielectric_bsdf_" + VkGlslShaderGenerator::TARGET, mx::DielectricBsdfNode::create);
    // <!-- <generalized_schlick_bsdf> -->
    registerImplementation("IM_generalized_schlick_bsdf_" + VkGlslShaderGenerator::TARGET, mx::DielectricBsdfNode::create);
    // <!-- <conductor_bsdf> -->
    registerImplementation("IM_conductor_bsdf_" + VkGlslShaderGenerator::TARGET, mx::ConductorBsdfNode::create);
    // <!-- <sheen_bsdf> -->
    registerImplementation("IM_sheen_bsdf_" + VkGlslShaderGenerator::TARGET, mx::SheenBsdfNode::create);

    // TODO: add proper implementation
    registerImplementation("IM_normal_vector3_" + VkGlslShaderGenerator::TARGET, NoOpNodeVkGlsl::create);
    registerImplementation("IM_tangent_vector3_" + VkGlslShaderGenerator::TARGET, NoOpNodeVkGlsl::create);

    registerImplementation("IM_surface_" + VkGlslShaderGenerator::TARGET, SurfaceNodeVkGlsl::create);
  }

  mx::ShaderPtr VkGlslShaderGenerator::generate(const mx::string& name, mx::ElementPtr element, mx::GenContext& context) const
  {
    mx::ShaderPtr shader = createShader(name, element, context);

    // Make sure floats are not expressed as integers or unsupported scientific notations.
    mx::ScopedFloatFormatting fmt(mx::Value::FloatFormatFixed);

    // Emit code for the compute shader stage.
    mx::ShaderStage& stage = shader->getStage(mx::Stage::PIXEL);
    emitComputeStage(name, shader->getGraph(), context, stage);
    replaceTokens(_tokenSubstitutions, stage);

    return shader;
  }

  mx::ShaderPtr VkGlslShaderGenerator::createShader(const mx::string& name, mx::ElementPtr element, mx::GenContext& context) const
  {
    // Create the root shader graph
    mx::ShaderGraphPtr graph = mx::ShaderGraph::create(nullptr, name, element, context);
    mx::ShaderPtr shader = std::make_shared<mx::Shader>(name, graph);

    // Create compute stage.
    mx::ShaderStagePtr cs = createStage(mx::Stage::PIXEL, *shader);
    mx::VariableBlockPtr csInputs = cs->createInputBlock(VKGLSL::SURFACE_INPUTS, "i_cs");
    mx::VariableBlockPtr csOutputs = cs->createOutputBlock(VKGLSL::SURFACE_OUTPUTS, "o_ps");

    // Create shader variables for all nodes that need this.
    for (mx::ShaderNode* node : graph->getNodes())
    {
      node->getImplementation().createVariables(*node, context, *shader);
    }

    // Create input variables for the graph interface
    for (mx::ShaderGraphInputSocket* inputSocket : graph->getInputSockets())
    {
      // Only for inputs that are connected/used internally, and are editable.
      if (!inputSocket->getConnections().empty() && graph->isEditable(*inputSocket))
      {
        csInputs->add(inputSocket->getSelf());
      }
    }

    // Create outputs from the graph interface.
    for (mx::ShaderGraphOutputSocket* outputSocket : graph->getOutputSockets())
    {
      csOutputs->add(outputSocket->getSelf());
    }

    return shader;
  }

  void VkGlslShaderGenerator::emitComputeStage(const mx::string& name,
    const mx::ShaderGraph& graph,
    mx::GenContext& context,
    mx::ShaderStage& stage) const
  {
    // Add directives
    emitLine("#version " + getVkGlslVersion(), stage, false);
    emitLineBreak(stage);

    // Add global constants
    emitInclude("pbrlib/" + VkGlslShaderGenerator::TARGET + "/lib/mx_defines.glsl", context, stage);
    emitLineBreak(stage);

    // Add type definitions
    emitTypeDefinitions(context, stage);

    // Add all constants
    const mx::VariableBlock& constants = stage.getConstantBlock();
    if (!constants.empty())
    {
      emitVariableDeclarations(constants, _syntax->getConstantQualifier(), mx::Syntax::SEMICOLON, context, stage);
      emitLineBreak(stage);
    }

    // Add all uniforms
    for (const auto& it : stage.getUniformBlocks())
    {
      const mx::VariableBlock& uniforms = *it.second;

      if (uniforms.empty())
      {
        continue;
      }

      emitComment("Uniform block: " + uniforms.getName(), stage);

      emitVariableDeclarations(uniforms, _syntax->getUniformQualifier(), mx::Syntax::SEMICOLON, context, stage);
      emitLineBreak(stage);
    }

    // Emit common math functions
    emitInclude("pbrlib/" + VkGlslShaderGenerator::TARGET + "/lib/mx_math.glsl", context, stage);

    emitLineBreak(stage);

    // Set the include file to use for uv transformations, depending on the vertical flip flag.
    if (context.getOptions().fileTextureVerticalFlip)
    {
      _tokenSubstitutions[ShaderGenerator::T_FILE_TRANSFORM_UV] = "stdlib/" + VkGlslShaderGenerator::TARGET + "/lib/mx_transform_uv_vflip.glsl";
    }
    else
    {
      _tokenSubstitutions[ShaderGenerator::T_FILE_TRANSFORM_UV] = "stdlib/" + VkGlslShaderGenerator::TARGET + "/lib/mx_transform_uv.glsl";
    }

    // TODO: var prefix?
    // TODO: unify geomprops with non-geomprops?
    // TODO: are N & T in correct coord sys?
    // <DEBUG>
    emitLine("vec3 geomprop_Nworld_out = vec3(0.0)", stage);
    emitLine("vec3 geomprop_Tworld_out = vec3(0.0)", stage);
    emitLine("vec3 V = vec3(0.0)", stage);
    emitLine("vec3 L = vec3(0.0)", stage);
    emitLine("vec3 N = vec3(0.0)", stage);
    emitLine("vec3 P = vec3(0.0)", stage);
    emitLine("vec3 T = vec3(0.0)", stage);
    emitLine("vec3 B = vec3(0.0)", stage);
    emitLine("vec4 out1 = vec4(0.0)", stage);
    // </DEBUG>

    // Add all functions for node implementations
    emitFunctionDefinitions(graph, context, stage);

    // Begin main function
    setFunctionName(name, stage);
    emitLine("void " + name + "()", stage, false);
    emitScopeBegin(stage);

    const mx::ShaderGraphOutputSocket* outputSocket = graph.getOutputSocket();

    if (graph.hasClassification(mx::ShaderNode::Classification::CLOSURE))
    {
      // Handle the case where the graph is a direct closure.
      // We don't support rendering closures without attaching
      // to a surface shader, so just output black.
      emitLine(outputSocket->getVariable() + " = vec4(0.0, 0.0, 0.0, 1.0)", stage);
    }
    else
    {
      // Add all function calls
      emitFunctionCalls(graph, context, stage);

      // Emit final output
      const mx::ShaderOutput* outputConnection = outputSocket->getConnection();
      if (outputConnection)
      {
        mx::string finalOutput = outputConnection->getVariable();
        const mx::string& channels = outputSocket->getChannels();
        if (!channels.empty())
        {
          finalOutput = _syntax->getSwizzledVariable(finalOutput, outputConnection->getType(), channels, outputSocket->getType());
        }

        if (graph.hasClassification(mx::ShaderNode::Classification::SURFACE))
        {
          emitLine(outputSocket->getVariable() + " = vec4(" + finalOutput + ".color, 1.0)", stage);
        }
        else
        {
          if (!outputSocket->getType()->isFloat4())
          {
            toVec4(outputSocket->getType(), finalOutput);
          }
          emitLine(outputSocket->getVariable() + " = " + finalOutput, stage);
        }
      }
      else
      {
        mx::string outputValue = outputSocket->getValue() ? _syntax->getValue(outputSocket->getType(), *outputSocket->getValue()) : _syntax->getDefaultValue(outputSocket->getType());
        if (!outputSocket->getType()->isFloat4())
        {
          mx::string finalOutput = outputSocket->getVariable() + "_tmp";
          emitLine(_syntax->getTypeName(outputSocket->getType()) + " " + finalOutput + " = " + outputValue, stage);
          toVec4(outputSocket->getType(), finalOutput);
          emitLine(outputSocket->getVariable() + " = " + finalOutput, stage);
        }
        else
        {
          emitLine(outputSocket->getVariable() + " = " + outputValue, stage);
        }
      }
    }

    // End main function
    emitScopeEnd(stage);
    emitLineBreak(stage);

    // TODO: just for testing purposes
    emitLine("void main() { " + name + "(); }", stage, false);
  }

  void VkGlslShaderGenerator::emitTextureNodes(const mx::ShaderGraph& graph, mx::GenContext& context, mx::ShaderStage& stage) const
  {
    // Emit function calls for all texturing nodes
    bool found = false;
    for (const mx::ShaderNode* node : graph.getNodes())
    {
      if (node->hasClassification(mx::ShaderNode::Classification::TEXTURE) && !node->referencedConditionally())
      {
        emitFunctionCall(*node, context, stage, false);
        found = true;
      }
    }

    if (found)
    {
      emitLineBreak(stage);
    }
  }

  void VkGlslShaderGenerator::emitFunctionCalls(const mx::ShaderGraph& graph,
    mx::GenContext& context,
    mx::ShaderStage& stage) const
  {
    BEGIN_SHADER_STAGE(stage, mx::Stage::PIXEL)
      // For pixel stage surface shaders need special handling
      if (graph.hasClassification(mx::ShaderNode::Classification::SHADER | mx::ShaderNode::Classification::SURFACE))
      {
        // Handle all texturing nodes. These are inputs to any closure/shader nodes and need to be emitted first.
        emitTextureNodes(graph, context, stage);

        // Emit function calls for all surface shader nodes
        for (const mx::ShaderNode* node : graph.getNodes())
        {
          if (node->hasClassification(mx::ShaderNode::Classification::SHADER | mx::ShaderNode::Classification::SURFACE))
          {
            emitFunctionCall(*node, context, stage, false);
          }
        }
      }
      else
      {
        // No surface shader or closure graph, so generate a normal function call.
        for (const mx::ShaderNode* node : graph.getNodes())
        {
          emitFunctionCall(*node, context, stage, false);
        }
      }
    END_SHADER_STAGE(stage, mx::Stage::PIXEL)
  }

  void VkGlslShaderGenerator::emitFunctionCall(const mx::ShaderNode& node, mx::GenContext& context, mx::ShaderStage& stage, bool checkScope) const
  {
    // Omit node if it's tagged to be excluded.
    if (node.getFlag(mx::ShaderNodeFlag::EXCLUDE_FUNCTION_CALL))
    {
      return;
    }

    // Omit node if it's only used inside a conditional branch
    if (checkScope && node.referencedConditionally())
    {
      emitComment("Omitted node '" + node.getName() + "'. Only used in conditional node '" +
        node.getScopeInfo().conditionalNode->getName() + "'", stage);
      return;
    }

    bool match = true;

    if (node.hasClassification(mx::ShaderNode::Classification::CLOSURE))
    {
      // If a layer operator is used the node to check classification on
      // is the node connected to the top layer input.
      const mx::ShaderNode* classifyNode = &node;
      if (node.hasClassification(mx::ShaderNode::Classification::LAYER))
      {
        const mx::ShaderInput* top = node.getInput(mx::LayerNode::TOP);
        if (top && top->getConnection())
        {
          classifyNode = top->getConnection()->getNode();
        }
      }

      match =
        // For reflection and indirect we don't support pure transmissive closures.
        (classifyNode->hasClassification(mx::ShaderNode::Classification::BSDF) && !classifyNode->hasClassification(mx::ShaderNode::Classification::BSDF_T)) ||
        // For transmissive we don't support pure reflective closures.
        (classifyNode->hasClassification(mx::ShaderNode::Classification::BSDF) && !classifyNode->hasClassification(mx::ShaderNode::Classification::BSDF_R)) ||
        // For emission we only support emission closures.
        classifyNode->hasClassification(mx::ShaderNode::Classification::EDF);
    }

    if (match)
    {
      // A match between closure context and node classification was found.
      // So emit the function call in this context.
      node.getImplementation().emitFunctionCall(node, context, stage);
    }
    else
    {
      // Context and node classification doesn't match so just
      // emit the output variable set to default value, in case
      // it is referenced by another nodes in this context.
      emitLineBegin(stage);
      emitOutput(node.getOutput(), true, true, context, stage);
      emitLineEnd(stage);
    }
  }

  void VkGlslShaderGenerator::emitVariableDeclaration(const mx::ShaderPort* variable,
    const mx::string& qualifier,
    mx::GenContext& context,
    mx::ShaderStage& stage,
    bool assignValue) const
  {
    // A file texture input needs special handling on GLSL
    if (variable->getType() == mx::Type::FILENAME)
    {
      // Samplers must always be uniforms
      mx::string str = qualifier.empty() ? mx::EMPTY_STRING : qualifier + " ";
      emitString(str + "sampler2D " + variable->getVariable(), stage);
    }
    else
    {
      mx::string str = qualifier.empty() ? mx::EMPTY_STRING : qualifier + " ";
      str += _syntax->getTypeName(variable->getType()) + " " + variable->getVariable();

      // If an array we need an array qualifier (suffix) for the variable name
      if (variable->getType()->isArray() && variable->getValue())
      {
        str += _syntax->getArrayVariableSuffix(variable->getType(), *variable->getValue());
      }

      if (!variable->getSemantic().empty())
      {
        str += " : " + variable->getSemantic();
      }

      if (assignValue)
      {
        const mx::string valueStr = (variable->getValue() ?
          _syntax->getValue(variable->getType(), *variable->getValue(), true) :
          _syntax->getDefaultValue(variable->getType(), true));
        str += valueStr.empty() ? mx::EMPTY_STRING : " = " + valueStr;
      }

      emitString(str, stage);
    }
  }

  void VkGlslShaderGenerator::emitBsdfNodes(const mx::ShaderGraph& graph, const mx::ShaderNode& shaderNode, mx::GenContext& context, mx::ShaderStage& stage, mx::string& bsdf) const
  {
    bsdf = _syntax->getTypeSyntax(mx::Type::BSDF).getDefaultValue(false);

    // Emit function calls for all BSDF nodes used by this surface shader.
    // The last node will hold the final result.
    const mx::ShaderNode* last = nullptr;
    for (const mx::ShaderNode* node : graph.getNodes())
    {
      if (node->hasClassification(mx::ShaderNode::Classification::BSDF) && shaderNode.isUsedClosure(node))
      {
        emitFunctionCall(*node, context, stage, false);
        last = node;
      }
    }
    if (last)
    {
      bsdf = last->getOutput()->getVariable();
    }
  }

  void VkGlslShaderGenerator::emitEdfNodes(const mx::ShaderGraph& graph, const mx::ShaderNode& shaderNode, mx::GenContext& context, mx::ShaderStage& stage, mx::string& edf) const
  {
    edf = _syntax->getTypeSyntax(mx::Type::EDF).getDefaultValue(false);

    // Emit function calls for all EDF nodes used by this shader
    // The last node will hold the final result
    const mx::ShaderNode* last = nullptr;
    for (const mx::ShaderNode* node : graph.getNodes())
    {
      if (node->hasClassification(mx::ShaderNode::Classification::EDF) && shaderNode.isUsedClosure(node))
      {
        emitFunctionCall(*node, context, stage, false);
        last = node;
      }
    }
    if (last)
    {
      edf = last->getOutput()->getVariable();
    }
  }

  void VkGlslShaderGenerator::toVec4(const mx::TypeDesc* type, mx::string& variable)
  {
    if (type->isFloat3())
    {
      variable = "vec4(" + variable + ", 1.0)";
    }
    else if (type->isFloat2())
    {
      variable = "vec4(" + variable + ", 0.0, 1.0)";
    }
    else if (type == mx::Type::FLOAT || type == mx::Type::INTEGER)
    {
      variable = "vec4(" + variable + ", " + variable + ", " + variable + ", 1.0)";
    }
    else if (type == mx::Type::BSDF || type == mx::Type::EDF)
    {
      variable = "vec4(" + variable + ", 1.0)";
    }
    else
    {
      // Can't understand other types. Just return black.
      variable = "vec4(0.0, 0.0, 0.0, 1.0)";
    }
  }
}
