#ifndef SHADERGEN_H
#define SHADERGEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ShaderGenResult {
  SHADERGEN_RESULT_OK,
  SHADERGEN_RESULT_ERROR_DOCUMENT_PARSE,
  SHADERGEN_RESULT_ERROR_INVALID_DOCUMENT,
  SHADERGEN_RESULT_ERROR_SHADER_COMPILE,
  SHADERGEN_RESULT_ERROR_CODEGEN,
} ShaderGenResult;

ShaderGenResult shadergen_gen_main(
  const char** materials,
  uint32_t material_count,
  uint32_t* spv_size,
  uint16_t** spv
);

#ifdef __cplusplus
}
#endif

#endif
