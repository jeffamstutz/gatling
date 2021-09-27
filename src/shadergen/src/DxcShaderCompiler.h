#pragma once

#include "IShaderCompiler.h"

namespace sg
{
  class DxcShaderCompiler : public IShaderCompiler
  {
  public:
    DxcShaderCompiler(const std::string& shaderPath);

  public:
    bool init() override;
    void release() override;

    bool compileHlslToSpv(const std::string& source,
                          const std::string& filePath,
                          const char* entryPoint,
                          uint32_t* spvSize,
                          uint32_t** spv) override;

  private:
    void* m_dxcCompiler;
  };
}
