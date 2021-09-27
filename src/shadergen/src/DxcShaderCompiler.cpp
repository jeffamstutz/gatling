#include "DxcShaderCompiler.h"

#include <dxcapi.h>

namespace sg
{
  DxcShaderCompiler::DxcShaderCompiler(const std::string& shaderPath)
    : IShaderCompiler(shaderPath)
  {
  }

  bool DxcShaderCompiler::init()
  {
    REFCLSID rclsid;
    REFIID riid;
    IUnknown **ppInterface;

    HRESULT r = DxcCreateInstance(rclsid, riid, ppv);

    return false;
  }

  void DxcShaderCompiler::release()
  {
    return false;
  }

  bool compileHlslToSpv(const std::string& source,
                        const std::string& filePath,
                        const char* entryPoint,
                        uint32_t* spvSize,
                        uint32_t** spv)
  {
    return false;
  }
}
