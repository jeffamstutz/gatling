#ifndef MATERIALX_VKGLSLSHADERGENERATOR_H
#define MATERIALX_VKGLSLSHADERGENERATOR_H

#include <MaterialXGenShader/ShaderGenerator.h>

namespace mx = MaterialX;

namespace shadergen
{
  namespace VKGLSL
  {
    extern const mx::string SURFACE_INPUTS;
    extern const mx::string SURFACE_OUTPUTS;
    extern const mx::string T_IN_POSITION;
    extern const mx::string T_IN_NORMAL;
    extern const mx::string DIR_N;
    extern const mx::string DIR_L;
    extern const mx::string DIR_V;
    extern const mx::string WORLD_POSITION;
  }

  using VkGlslShaderGeneratorPtr = mx::shared_ptr<class VkGlslShaderGenerator>;

  class VkGlslShaderGenerator : public mx::ShaderGenerator
  {
  private:
    static const mx::string TARGET;
    static const mx::string GLSL_VERSION;

  public:
    explicit VkGlslShaderGenerator();

    static VkGlslShaderGeneratorPtr create() { return std::make_shared<VkGlslShaderGenerator>(); }

    mx::ShaderPtr generate(const mx::string& name, mx::ElementPtr element, mx::GenContext& context) const override;

    const mx::string& getTarget() const override { return TARGET; }

    void emitFunctionCalls(const mx::ShaderGraph& graph,
                           mx::GenContext& context,
                           mx::ShaderStage& stage) const override;

    void emitVariableDeclaration(const mx::ShaderPort* variable,
                                 const mx::string& qualifier,
                                 mx::GenContext& context,
                                 mx::ShaderStage& stage,
                                 bool assignValue) const override;

    void emitBsdfNodes(const mx::ShaderGraph& graph,
                       const mx::ShaderNode& shaderNode,
                       mx::GenContext& context,
                       mx::ShaderStage& stage,
                       mx::string& bsdf) const;

    void emitEdfNodes(const mx::ShaderGraph& graph,
                      const mx::ShaderNode& shaderNode,
                      mx::GenContext& context,
                      mx::ShaderStage& stage,
                      mx::string& edf) const;

  private:
    const mx::string& getVkGlslVersion() const { return GLSL_VERSION; }

    mx::ShaderPtr createShader(const mx::string& name, mx::ElementPtr element, mx::GenContext& context) const;

    void emitTextureNodes(const mx::ShaderGraph& graph, mx::GenContext& context, mx::ShaderStage& stage) const;

    void emitFunctionCall(const mx::ShaderNode& node, mx::GenContext& context, mx::ShaderStage& stage, bool checkScope = true) const override;

    void emitComputeStage(const mx::string& name,
                          const mx::ShaderGraph& graph,
                          mx::GenContext& context,
                          mx::ShaderStage& stage) const;

    static void toVec4(const mx::TypeDesc* type, mx::string& variable);
  };
}

#endif
