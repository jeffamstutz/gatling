//
// TM & (c) 2020 Lucasfilm Entertainment Company Ltd. and Lucasfilm Ltd.
// All rights reserved. See LICENSE.txt for license.
//

#include "shadergen.h"

#include "VkGlslShaderGenerator.h"

#include <MaterialXCore/Document.h>
#include <MaterialXCore/Library.h>
#include <MaterialXCore/Material.h>
#include <MaterialXCore/Definition.h>
#include <MaterialXFormat/File.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/Library.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenShader/Util.h>
#include <MaterialXGenShader/DefaultColorManagementSystem.h>

#include <shaderc/shaderc.hpp>

namespace mx = MaterialX;

namespace
{
  void _LoadStandardLibraries(mx::DocumentPtr document)
  {
    const std::unordered_set<std::string> folderNames{ "targets", "stdlib", "pbrlib", "bxdf", "lights" };
    MaterialX::FileSearchPath folderSearchPath;

    // TODO: fix hardcoded path
    folderSearchPath.append(MaterialX::FilePath("E:/gatling/src/shadergen/mtlx"));

    MaterialX::loadLibraries(
      MaterialX::FilePathVec(folderNames.begin(), folderNames.end()),
      folderSearchPath,
      document
    );

    folderSearchPath = MaterialX::FileSearchPath();
    folderSearchPath.append(MaterialX::FilePath("C:/Users/pablode/tmp/BlenderUSDHydraAddon2/bin/MaterialX/install/libraries")); // TODO: fix hardcoded path
    MaterialX::loadLibraries(
      MaterialX::FilePathVec(folderNames.begin(), folderNames.end()),
      folderSearchPath,
      document
    );
  }

  bool _CompileToSpv(const std::string& shaderSource, uint32_t* spvSize, uint16_t** spv)
  {
    shaderc::Compiler compiler;

    shaderc::CompileOptions options;
    // TODO: use CMake for update-to-date version numbers
    options.AddMacroDefinition("GATLING_VERSION_MAJOR", "0");
    options.AddMacroDefinition("GATLING_VERSION_MINOR", "1");
    options.AddMacroDefinition("GATLING_VERSION_PATCH", "0");
#ifdef NDEBUG
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
#else
    options.SetOptimizationLevel(shaderc_optimization_level_zero);
    options.SetGenerateDebugInfo();
#endif
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);

    const shaderc_shader_kind kind = shaderc_glsl_compute_shader;
    const char* inputFileName = "";

    const shaderc::PreprocessedSourceCompilationResult preprocessResult = compiler.PreprocessGlsl(
      shaderSource,
      kind,
      inputFileName,
      options
    );

    if (preprocessResult.GetCompilationStatus() != shaderc_compilation_status_success)
    {
      const std::string errorStr = preprocessResult.GetErrorMessage();
      fprintf(stderr, "Shader preprocess error: %s\n", errorStr.c_str());
      return false;
    }

    const shaderc::AssemblyCompilationResult compilationResult = compiler.CompileGlslToSpvAssembly(
      shaderSource,
      kind,
      inputFileName,
      options
    );

    if (compilationResult.GetCompilationStatus() != shaderc_compilation_status_success)
    {
      const std::string errorStr = compilationResult.GetErrorMessage();
      fprintf(stderr, "Shader compile error: %s\n", errorStr.c_str());
      return false;
    }

    const std::string assemblyStr(compilationResult.cbegin(), compilationResult.cend());
    const uint16_t* assembly = reinterpret_cast<const uint16_t*>(assemblyStr.c_str());
    const uint32_t assemblySize = assemblyStr.size() / (sizeof(uint16_t) / sizeof(char));

    printf("=====\ncompilation successful!\n");
    for (size_t i = 0; i < assemblyStr.size(); i++)
    {
      printf("%02hhx ", assemblyStr[i]);
    }
    printf("\n=====\n");

    return true;
  }
}

ShaderGenResult shadergen_gen_main(const char** materials,
                                   uint32_t material_count,
                                   uint32_t* spv_size,
                                   uint16_t** spv)
{
  // Create the MaterialX document.
  mx::DocumentPtr mainDoc = mx::createDocument();

  mx::DocumentPtr dependLib = mx::createDocument();
  _LoadStandardLibraries(dependLib);

  for (uint32_t i = 0; i < material_count; i++)
  {
    auto materialStr = mx::string(materials[i]);

    if (materialStr == "") // TODO: how to handle default material?
    {
      continue;
    }

    mx::DocumentPtr materialDoc = mx::createDocument();
    mx::readFromXmlString(materialDoc, materialStr);

    // <DEBUG>
    mainDoc = mx::createDocument();
    materialStr =
      "<?xml version=\"1.0\"?>"
      "<materialx version=\"1.38\" colorspace=\"lin_rec709\">"
      "  <nodedef name=\"NG_mymaterial\" node=\"mymaterial\">"
      "    <output name=\"out\" type=\"surfaceshader\" />"
      "  </nodedef>"
      "  <nodegraph name=\"blah\" nodedef=\"NG_mymaterial\" >"
      "    <UsdPreviewSurface name=\"SR_default\" type=\"surfaceshader\">"
      "        <input name=\"diffuseColor\" type=\"color3\" nodename=\"add1\"/>"
      "    </UsdPreviewSurface>"
      "    <output name=\"out\" type=\"surfaceshader\" nodename=\"SR_default\" />"
      "  </nodegraph>"
      "  <mymaterial name=\"Material1\" type=\"surfaceshader\" />"
      "  <output name=\"out1\" nodename=\"Material1\" type=\"surfaceshader\" />"
        "</materialx>";
    mx::readFromXmlString(mainDoc, materialStr);
    // </DEBUG>
  }
  // TODO: insert MaterialSwitch node which can map material id -> material function call
  // TODO: insert SourceCodeNode with rest of ray tracing kernel

  mainDoc->importLibrary(dependLib);

  // Remap references to unimplemented shader nodedefs.
  for (mx::NodePtr materialNode : mainDoc->getMaterialNodes())
  {
    for (mx::NodePtr shader : getShaderNodes(materialNode))
    {
      mx::NodeDefPtr nodeDef = shader->getNodeDef();
      if (nodeDef && !nodeDef->getImplementation())
      {
        std::vector<mx::NodeDefPtr> altNodeDefs = mainDoc->getMatchingNodeDefs(nodeDef->getNodeString());
        for (mx::NodeDefPtr altNodeDef : altNodeDefs)
        {
          if (altNodeDef->getImplementation())
          {
            shader->setNodeDefString(altNodeDef->getName());
          }
        }
      }
    }
  }

  // Find element to generate shader for.
  mx::StringVec renderablePaths;
  std::vector<mx::TypedElementPtr> elems;
  mx::findRenderableElements(mainDoc, elems);
  if (elems.empty())
  {
    return SHADERGEN_RESULT_ERROR_INVALID_DOCUMENT;
  }
  for (mx::TypedElementPtr elem : elems)
  {
    mx::TypedElementPtr renderableElem = elem;
    mx::NodePtr node = elem->asA<mx::Node>();
    if (node && node->getType() == mx::MATERIAL_TYPE_STRING)
    {
      std::unordered_set<mx::NodePtr> shaderNodes = mx::getShaderNodes(node);
      if (!shaderNodes.empty())
      {
        renderableElem = *shaderNodes.begin();
      }
    }
    renderablePaths.push_back(renderableElem->getNamePath());
  }

  mx::TypedElementPtr& element = elems.front();

  for (size_t i = 0; i < renderablePaths.size(); i++)
  {
    const auto& renderablePath = renderablePaths[i];
    mx::ElementPtr elem = mainDoc->getDescendant(renderablePath);
    mx::TypedElementPtr typedElem = elem ? elem->asA<mx::TypedElement>() : nullptr;
    if (!typedElem)
    {
      continue;
    }
    element = typedElem;
  }

  // Generate shader source from graph.
  mx::ShaderPtr shader;
  {
    const std::string name = "test"; // TODO: pass material name

    // TODO: cache shadergen, stdlib doc and color/unit management doc
    auto shaderGen = shadergen::VkGlslShaderGenerator::create();
    mx::GenContext context(shaderGen);

    shaderGen->registerShaderMetadata(mainDoc, context);

    // TODO: get paths at runtime
    mx::FileSearchPath codeSearchPath = "C:/Users/pablode/tmp/BlenderUSDHydraAddon2/bin/MaterialX/install/libraries";
    codeSearchPath.append(MaterialX::FilePath("E:/gatling/src/shadergen/mtlx"));
    context.registerSourceCodeSearchPath(codeSearchPath);

    // Initialize color management.
    mx::DefaultColorManagementSystemPtr cms = mx::DefaultColorManagementSystem::create(context.getShaderGenerator().getTarget());
    cms->loadLibrary(mainDoc);
    context.getShaderGenerator().setColorManagementSystem(cms);

    // Initialize unit management.
    auto unitRegistry = mx::UnitConverterRegistry::create();
    mx::UnitSystemPtr unitSystem = mx::UnitSystem::create(context.getShaderGenerator().getTarget());
    unitSystem->loadLibrary(mainDoc);
    unitSystem->setUnitConverterRegistry(unitRegistry);
    context.getShaderGenerator().setUnitSystem(unitSystem);
    context.getOptions().targetDistanceUnit = "meter";

    try
    {
      shader = shaderGen->generate(name, element, context);
    }
    catch (const std::exception& ex)
    {
      fprintf(stderr, "Exception when generating GLSL source code\n==============\n%s\n==============\n", ex.what());
      return SHADERGEN_RESULT_ERROR_CODEGEN;
    }
  }

  if (!shader)
  {
    return SHADERGEN_RESULT_ERROR_CODEGEN;
  }

  // Compile to SPIR-V using shaderc.
  {
    const mx::ShaderStage pixelStage = shader->getStage(mx::Stage::PIXEL);
    const mx::string shaderSource = pixelStage.getSourceCode();

    printf("=====\nGenerated code:\n%s\n=====\n", shaderSource.c_str());

    if (!_CompileToSpv(shaderSource, spv_size, spv))
    {
      return SHADERGEN_RESULT_ERROR_SHADER_COMPILE;
    }
  }

  return SHADERGEN_RESULT_OK;
}
