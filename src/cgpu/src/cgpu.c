/*
 * Copyright (C) 2019-2022 Pablo Delgado Krämer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "cgpu.h"
#include "resource_store.h"
#include "shader_reflection.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <volk.h>

#include <vma.h>

#define MIN_VK_API_VERSION VK_API_VERSION_1_1

/* Array and pool allocation limits. */

#define MAX_PHYSICAL_DEVICES 8
#define MAX_DEVICE_EXTENSIONS 1024
#define MAX_QUEUE_FAMILIES 64
#define MAX_DESCRIPTOR_SET_LAYOUT_BINDINGS 128
#define MAX_DESCRIPTOR_BUFFER_INFOS 64
#define MAX_DESCRIPTOR_IMAGE_INFOS 2048
#define MAX_WRITE_DESCRIPTOR_SETS 128
#define MAX_BUFFER_MEMORY_BARRIERS 64
#define MAX_IMAGE_MEMORY_BARRIERS 2048
#define MAX_MEMORY_BARRIERS 128

/* Internal structures. */

typedef struct cgpu_iinstance {
  VkInstance instance;
} cgpu_iinstance;

typedef struct cgpu_idevice {
  VkDevice                      logical_device;
  VkPhysicalDevice              physical_device;
  VkQueue                       compute_queue;
  VkCommandPool                 command_pool;
  VkQueryPool                   timestamp_pool;
  struct VolkDeviceTable        table;
  cgpu_physical_device_features features;
  cgpu_physical_device_limits   limits;
  VmaAllocator                  allocator;
} cgpu_idevice;

typedef struct cgpu_ibuffer {
  VkBuffer       buffer;
  uint64_t       size;
  VmaAllocation  allocation;
} cgpu_ibuffer;

typedef struct cgpu_iimage {
  VkImage       image;
  VkImageView   image_view;
  VmaAllocation allocation;
  uint64_t      size;
  uint32_t      width;
  uint32_t      height;
  uint32_t      depth;
  VkImageLayout layout;
  VkAccessFlags access_mask;
} cgpu_iimage;

typedef struct cgpu_ipipeline {
  VkPipeline                   pipeline;
  VkPipelineLayout             layout;
  VkDescriptorPool             descriptor_pool;
  VkDescriptorSet              descriptor_set;
  VkDescriptorSetLayout        descriptor_set_layout;
  VkDescriptorSetLayoutBinding descriptor_set_layout_bindings[MAX_DESCRIPTOR_SET_LAYOUT_BINDINGS];
  uint32_t                     descriptor_set_layout_binding_count;
  cgpu_shader                  shader;
} cgpu_ipipeline;

typedef struct cgpu_ishader {
  VkShaderModule module;
  cgpu_shader_reflection reflection;
} cgpu_ishader;

typedef struct cgpu_ifence {
  VkFence fence;
} cgpu_ifence;

typedef struct cgpu_icommand_buffer {
  VkCommandBuffer command_buffer;
  cgpu_device     device;
} cgpu_icommand_buffer;

typedef struct cgpu_isampler {
  VkSampler sampler;
} cgpu_isampler;

/* Handle and structure storage. */

static cgpu_iinstance iinstance;
static resource_store idevice_store;
static resource_store ibuffer_store;
static resource_store iimage_store;
static resource_store ishader_store;
static resource_store ipipeline_store;
static resource_store ifence_store;
static resource_store icommand_buffer_store;
static resource_store isampler_store;

/* Helper functions. */

#define CGPU_RETURN_ERROR(msg)                                        \
  do {                                                                \
    fprintf(stderr, "error in %s:%d: %s\n", __FILE__, __LINE__, msg); \
    return false;                                                     \
  } while (false)

#define CGPU_RETURN_ERROR_INVALID_HANDLE                              \
  CGPU_RETURN_ERROR("invalid resource handle");

#define CGPU_RETURN_ERROR_HARDCODED_LIMIT_REACHED                     \
  CGPU_RETURN_ERROR("hardcoded limit reached");

#define CGPU_RESOLVE_HANDLE(RESOURCE_NAME, HANDLE_TYPE, IRESOURCE_TYPE, RESOURCE_STORE)   \
  CGPU_INLINE static bool cgpu_resolve_##RESOURCE_NAME(                                   \
    HANDLE_TYPE handle,                                                                   \
    IRESOURCE_TYPE** idata)                                                               \
  {                                                                                       \
    return resource_store_get(&RESOURCE_STORE, handle.handle, (void**) idata);            \
  }

CGPU_RESOLVE_HANDLE(        device,         cgpu_device,         cgpu_idevice,         idevice_store)
CGPU_RESOLVE_HANDLE(        buffer,         cgpu_buffer,         cgpu_ibuffer,         ibuffer_store)
CGPU_RESOLVE_HANDLE(         image,          cgpu_image,          cgpu_iimage,          iimage_store)
CGPU_RESOLVE_HANDLE(        shader,         cgpu_shader,         cgpu_ishader,         ishader_store)
CGPU_RESOLVE_HANDLE(      pipeline,       cgpu_pipeline,       cgpu_ipipeline,       ipipeline_store)
CGPU_RESOLVE_HANDLE(         fence,          cgpu_fence,          cgpu_ifence,          ifence_store)
CGPU_RESOLVE_HANDLE(command_buffer, cgpu_command_buffer, cgpu_icommand_buffer, icommand_buffer_store)
CGPU_RESOLVE_HANDLE(       sampler,        cgpu_sampler,        cgpu_isampler,        isampler_store)

static VkMemoryPropertyFlags cgpu_translate_memory_properties(CgpuMemoryPropertyFlags memory_properties)
{
  VkMemoryPropertyFlags mem_flags = 0;
  if ((memory_properties & CGPU_MEMORY_PROPERTY_FLAG_DEVICE_LOCAL) == CGPU_MEMORY_PROPERTY_FLAG_DEVICE_LOCAL) {
    mem_flags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  }
  if ((memory_properties & CGPU_MEMORY_PROPERTY_FLAG_HOST_VISIBLE) == CGPU_MEMORY_PROPERTY_FLAG_HOST_VISIBLE) {
    mem_flags |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
  }
  if ((memory_properties & CGPU_MEMORY_PROPERTY_FLAG_HOST_COHERENT) == CGPU_MEMORY_PROPERTY_FLAG_HOST_COHERENT) {
    mem_flags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  }
  if ((memory_properties & CGPU_MEMORY_PROPERTY_FLAG_HOST_CACHED) == CGPU_MEMORY_PROPERTY_FLAG_HOST_CACHED) {
    mem_flags |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  }
  return mem_flags;
}

static VkAccessFlags cgpu_translate_access_flags(CgpuMemoryAccessFlags flags)
{
  VkAccessFlags vk_flags = 0;
  if ((flags & CGPU_MEMORY_ACCESS_FLAG_UNIFORM_READ) == CGPU_MEMORY_ACCESS_FLAG_UNIFORM_READ) {
    vk_flags |= VK_ACCESS_UNIFORM_READ_BIT;
  }
  if ((flags & CGPU_MEMORY_ACCESS_FLAG_SHADER_READ) == CGPU_MEMORY_ACCESS_FLAG_SHADER_READ) {
    vk_flags |= VK_ACCESS_SHADER_READ_BIT;
  }
  if ((flags & CGPU_MEMORY_ACCESS_FLAG_SHADER_WRITE) == CGPU_MEMORY_ACCESS_FLAG_SHADER_WRITE) {
    vk_flags |= VK_ACCESS_SHADER_WRITE_BIT;
  }
  if ((flags & CGPU_MEMORY_ACCESS_FLAG_TRANSFER_READ) == CGPU_MEMORY_ACCESS_FLAG_TRANSFER_READ) {
    vk_flags |= VK_ACCESS_TRANSFER_READ_BIT;
  }
  if ((flags & CGPU_MEMORY_ACCESS_FLAG_TRANSFER_WRITE) == CGPU_MEMORY_ACCESS_FLAG_TRANSFER_WRITE) {
    vk_flags |= VK_ACCESS_TRANSFER_WRITE_BIT;
  }
  if ((flags & CGPU_MEMORY_ACCESS_FLAG_HOST_READ) == CGPU_MEMORY_ACCESS_FLAG_HOST_READ) {
    vk_flags |= VK_ACCESS_HOST_READ_BIT;
  }
  if ((flags & CGPU_MEMORY_ACCESS_FLAG_HOST_WRITE) == CGPU_MEMORY_ACCESS_FLAG_HOST_WRITE) {
    vk_flags |= VK_ACCESS_HOST_WRITE_BIT;
  }
  if ((flags & CGPU_MEMORY_ACCESS_FLAG_MEMORY_READ) == CGPU_MEMORY_ACCESS_FLAG_MEMORY_READ) {
    vk_flags |= VK_ACCESS_MEMORY_READ_BIT;
  }
  if ((flags & CGPU_MEMORY_ACCESS_FLAG_MEMORY_WRITE) == CGPU_MEMORY_ACCESS_FLAG_MEMORY_WRITE) {
    vk_flags |= VK_ACCESS_MEMORY_WRITE_BIT;
  }
  return vk_flags;
}

static cgpu_physical_device_features cgpu_translate_physical_device_features(const VkPhysicalDeviceFeatures* vk_features)
{
  cgpu_physical_device_features features = {0};
  features.textureCompressionBC = vk_features->textureCompressionBC;
  features.pipelineStatisticsQuery = vk_features->pipelineStatisticsQuery;
  features.shaderImageGatherExtended = vk_features->shaderImageGatherExtended;
  features.shaderStorageImageExtendedFormats = vk_features->shaderStorageImageExtendedFormats;
  features.shaderStorageImageReadWithoutFormat = vk_features->shaderStorageImageReadWithoutFormat;
  features.shaderStorageImageWriteWithoutFormat = vk_features->shaderStorageImageWriteWithoutFormat;
  features.shaderUniformBufferArrayDynamicIndexing = vk_features->shaderUniformBufferArrayDynamicIndexing;
  features.shaderSampledImageArrayDynamicIndexing = vk_features->shaderSampledImageArrayDynamicIndexing;
  features.shaderStorageBufferArrayDynamicIndexing = vk_features->shaderStorageBufferArrayDynamicIndexing;
  features.shaderStorageImageArrayDynamicIndexing = vk_features->shaderStorageImageArrayDynamicIndexing;
  features.shaderFloat64 = vk_features->shaderFloat64;
  features.shaderInt64 = vk_features->shaderInt64;
  features.shaderInt16 = vk_features->shaderInt16;
  features.sparseBinding = vk_features->sparseBinding;
  features.sparseResidencyBuffer = vk_features->sparseResidencyBuffer;
  features.sparseResidencyImage2D = vk_features->sparseResidencyImage2D;
  features.sparseResidencyImage3D = vk_features->sparseResidencyImage3D;
  features.sparseResidencyAliased = vk_features->sparseResidencyAliased;
  return features;
}

static cgpu_physical_device_limits cgpu_translate_physical_device_limits(const VkPhysicalDeviceLimits* vk_limits,
                                                                         const VkPhysicalDeviceSubgroupProperties* vk_subgroup_props)
{
  cgpu_physical_device_limits limits = {0};
  limits.maxImageDimension1D = vk_limits->maxImageDimension1D;
  limits.maxImageDimension2D = vk_limits->maxImageDimension2D;
  limits.maxImageDimension3D = vk_limits->maxImageDimension3D;
  limits.maxImageDimensionCube = vk_limits->maxImageDimensionCube;
  limits.maxImageArrayLayers = vk_limits->maxImageArrayLayers;
  limits.maxUniformBufferRange = vk_limits->maxUniformBufferRange;
  limits.maxStorageBufferRange = vk_limits->maxStorageBufferRange;
  limits.maxPushConstantsSize = vk_limits->maxPushConstantsSize;
  limits.maxMemoryAllocationCount = vk_limits->maxMemoryAllocationCount;
  limits.maxSamplerAllocationCount = vk_limits->maxSamplerAllocationCount;
  limits.bufferImageGranularity = vk_limits->bufferImageGranularity;
  limits.sparseAddressSpaceSize = vk_limits->sparseAddressSpaceSize;
  limits.maxBoundDescriptorSets = vk_limits->maxBoundDescriptorSets;
  limits.maxPerStageDescriptorSamplers = vk_limits->maxPerStageDescriptorSamplers;
  limits.maxPerStageDescriptorUniformBuffers = vk_limits->maxPerStageDescriptorUniformBuffers;
  limits.maxPerStageDescriptorStorageBuffers = vk_limits->maxPerStageDescriptorStorageBuffers;
  limits.maxPerStageDescriptorSampledImages = vk_limits->maxPerStageDescriptorSampledImages;
  limits.maxPerStageDescriptorStorageImages = vk_limits->maxPerStageDescriptorStorageImages;
  limits.maxPerStageDescriptorInputAttachments = vk_limits->maxPerStageDescriptorInputAttachments;
  limits.maxPerStageResources = vk_limits->maxPerStageResources;
  limits.maxDescriptorSetSamplers = vk_limits->maxDescriptorSetSamplers;
  limits.maxDescriptorSetUniformBuffers = vk_limits->maxDescriptorSetUniformBuffers;
  limits.maxDescriptorSetUniformBuffersDynamic = vk_limits->maxDescriptorSetUniformBuffersDynamic;
  limits.maxDescriptorSetStorageBuffers = vk_limits->maxDescriptorSetStorageBuffers;
  limits.maxDescriptorSetStorageBuffersDynamic = vk_limits->maxDescriptorSetStorageBuffersDynamic;
  limits.maxDescriptorSetSampledImages = vk_limits->maxDescriptorSetSampledImages;
  limits.maxDescriptorSetStorageImages = vk_limits->maxDescriptorSetStorageImages;
  limits.maxDescriptorSetInputAttachments = vk_limits->maxDescriptorSetInputAttachments;
  limits.maxComputeSharedMemorySize = vk_limits->maxComputeSharedMemorySize;
  limits.maxComputeWorkGroupCount[0] = vk_limits->maxComputeWorkGroupCount[0];
  limits.maxComputeWorkGroupCount[1] = vk_limits->maxComputeWorkGroupCount[1];
  limits.maxComputeWorkGroupCount[2] = vk_limits->maxComputeWorkGroupCount[2];
  limits.maxComputeWorkGroupInvocations = vk_limits->maxComputeWorkGroupInvocations;
  limits.maxComputeWorkGroupSize[0] = vk_limits->maxComputeWorkGroupSize[0];
  limits.maxComputeWorkGroupSize[1] = vk_limits->maxComputeWorkGroupSize[1];
  limits.maxComputeWorkGroupSize[2] = vk_limits->maxComputeWorkGroupSize[2];
  limits.mipmapPrecisionBits = vk_limits->mipmapPrecisionBits;
  limits.maxSamplerLodBias = vk_limits->maxSamplerLodBias;
  limits.maxSamplerAnisotropy = vk_limits->maxSamplerAnisotropy;
  limits.minMemoryMapAlignment = vk_limits->minMemoryMapAlignment;
  limits.minUniformBufferOffsetAlignment = vk_limits->minUniformBufferOffsetAlignment;
  limits.minStorageBufferOffsetAlignment = vk_limits->minStorageBufferOffsetAlignment;
  limits.minTexelOffset = vk_limits->minTexelOffset;
  limits.maxTexelOffset = vk_limits->maxTexelOffset;
  limits.minTexelGatherOffset = vk_limits->minTexelGatherOffset;
  limits.maxTexelGatherOffset = vk_limits->maxTexelGatherOffset;
  limits.minInterpolationOffset = vk_limits->minInterpolationOffset;
  limits.maxInterpolationOffset = vk_limits->maxInterpolationOffset;
  limits.subPixelInterpolationOffsetBits = vk_limits->subPixelInterpolationOffsetBits;
  limits.maxSampleMaskWords = vk_limits->maxSampleMaskWords;
  limits.timestampComputeAndGraphics = vk_limits->timestampComputeAndGraphics;
  limits.timestampPeriod = vk_limits->timestampPeriod;
  limits.discreteQueuePriorities = vk_limits->discreteQueuePriorities;
  limits.optimalBufferCopyOffsetAlignment = vk_limits->optimalBufferCopyOffsetAlignment;
  limits.optimalBufferCopyRowPitchAlignment = vk_limits->optimalBufferCopyRowPitchAlignment;
  limits.nonCoherentAtomSize = vk_limits->nonCoherentAtomSize;
  limits.subgroupSize = vk_subgroup_props->subgroupSize;
  return limits;
}

static VkFormat cgpu_translate_image_format(CgpuImageFormat image_format)
{
  switch (image_format)
  {
  case CGPU_IMAGE_FORMAT_UNDEFINED: return VK_FORMAT_UNDEFINED;
  case CGPU_IMAGE_FORMAT_R4G4_UNORM_PACK8: return VK_FORMAT_R4G4_UNORM_PACK8;
  case CGPU_IMAGE_FORMAT_R4G4B4A4_UNORM_PACK16: return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
  case CGPU_IMAGE_FORMAT_B4G4R4A4_UNORM_PACK16: return VK_FORMAT_B4G4R4A4_UNORM_PACK16;
  case CGPU_IMAGE_FORMAT_R5G6B5_UNORM_PACK16: return VK_FORMAT_R5G6B5_UNORM_PACK16;
  case CGPU_IMAGE_FORMAT_B5G6R5_UNORM_PACK16: return VK_FORMAT_B5G6R5_UNORM_PACK16;
  case CGPU_IMAGE_FORMAT_R5G5B5A1_UNORM_PACK16: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
  case CGPU_IMAGE_FORMAT_B5G5R5A1_UNORM_PACK16: return VK_FORMAT_B5G5R5A1_UNORM_PACK16;
  case CGPU_IMAGE_FORMAT_A1R5G5B5_UNORM_PACK16: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
  case CGPU_IMAGE_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
  case CGPU_IMAGE_FORMAT_R8_SNORM: return VK_FORMAT_R8_SNORM;
  case CGPU_IMAGE_FORMAT_R8_USCALED: return VK_FORMAT_R8_USCALED;
  case CGPU_IMAGE_FORMAT_R8_SSCALED: return VK_FORMAT_R8_SSCALED;
  case CGPU_IMAGE_FORMAT_R8_UINT: return VK_FORMAT_R8_UINT;
  case CGPU_IMAGE_FORMAT_R8_SINT: return VK_FORMAT_R8_SINT;
  case CGPU_IMAGE_FORMAT_R8_SRGB: return VK_FORMAT_R8_SRGB;
  case CGPU_IMAGE_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
  case CGPU_IMAGE_FORMAT_R8G8_SNORM: return VK_FORMAT_R8G8_SNORM;
  case CGPU_IMAGE_FORMAT_R8G8_USCALED: return VK_FORMAT_R8G8_USCALED;
  case CGPU_IMAGE_FORMAT_R8G8_SSCALED: return VK_FORMAT_R8G8_SSCALED;
  case CGPU_IMAGE_FORMAT_R8G8_UINT: return VK_FORMAT_R8G8_UINT;
  case CGPU_IMAGE_FORMAT_R8G8_SINT: return VK_FORMAT_R8G8_SINT;
  case CGPU_IMAGE_FORMAT_R8G8_SRGB: return VK_FORMAT_R8G8_SRGB;
  case CGPU_IMAGE_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8_UNORM;
  case CGPU_IMAGE_FORMAT_R8G8B8_SNORM: return VK_FORMAT_R8G8B8_SNORM;
  case CGPU_IMAGE_FORMAT_R8G8B8_USCALED: return VK_FORMAT_R8G8B8_USCALED;
  case CGPU_IMAGE_FORMAT_R8G8B8_SSCALED: return VK_FORMAT_R8G8B8_SSCALED;
  case CGPU_IMAGE_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8_UINT;
  case CGPU_IMAGE_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8_SINT;
  case CGPU_IMAGE_FORMAT_R8G8B8_SRGB: return VK_FORMAT_R8G8B8_SRGB;
  case CGPU_IMAGE_FORMAT_B8G8R8_UNORM: return VK_FORMAT_B8G8R8_UNORM;
  case CGPU_IMAGE_FORMAT_B8G8R8_SNORM: return VK_FORMAT_B8G8R8_SNORM;
  case CGPU_IMAGE_FORMAT_B8G8R8_USCALED: return VK_FORMAT_B8G8R8_USCALED;
  case CGPU_IMAGE_FORMAT_B8G8R8_SSCALED: return VK_FORMAT_B8G8R8_SSCALED;
  case CGPU_IMAGE_FORMAT_B8G8R8_UINT: return VK_FORMAT_B8G8R8_UINT;
  case CGPU_IMAGE_FORMAT_B8G8R8_SINT: return VK_FORMAT_B8G8R8_SINT;
  case CGPU_IMAGE_FORMAT_B8G8R8_SRGB: return VK_FORMAT_B8G8R8_SRGB;
  case CGPU_IMAGE_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
  case CGPU_IMAGE_FORMAT_R8G8B8A8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
  case CGPU_IMAGE_FORMAT_R8G8B8A8_USCALED: return VK_FORMAT_R8G8B8A8_USCALED;
  case CGPU_IMAGE_FORMAT_R8G8B8A8_SSCALED: return VK_FORMAT_R8G8B8A8_SSCALED;
  case CGPU_IMAGE_FORMAT_R8G8B8A8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
  case CGPU_IMAGE_FORMAT_R8G8B8A8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
  case CGPU_IMAGE_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
  case CGPU_IMAGE_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
  case CGPU_IMAGE_FORMAT_B8G8R8A8_SNORM: return VK_FORMAT_B8G8R8A8_SNORM;
  case CGPU_IMAGE_FORMAT_B8G8R8A8_USCALED: return VK_FORMAT_B8G8R8A8_USCALED;
  case CGPU_IMAGE_FORMAT_B8G8R8A8_SSCALED: return VK_FORMAT_B8G8R8A8_SSCALED;
  case CGPU_IMAGE_FORMAT_B8G8R8A8_UINT: return VK_FORMAT_B8G8R8A8_UINT;
  case CGPU_IMAGE_FORMAT_B8G8R8A8_SINT: return VK_FORMAT_B8G8R8A8_SINT;
  case CGPU_IMAGE_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
  case CGPU_IMAGE_FORMAT_A8B8G8R8_UNORM_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
  case CGPU_IMAGE_FORMAT_A8B8G8R8_SNORM_PACK32: return VK_FORMAT_A8B8G8R8_SNORM_PACK32;
  case CGPU_IMAGE_FORMAT_A8B8G8R8_USCALED_PACK32: return VK_FORMAT_A8B8G8R8_USCALED_PACK32;
  case CGPU_IMAGE_FORMAT_A8B8G8R8_SSCALED_PACK32: return VK_FORMAT_A8B8G8R8_SSCALED_PACK32;
  case CGPU_IMAGE_FORMAT_A8B8G8R8_UINT_PACK32: return VK_FORMAT_A8B8G8R8_UINT_PACK32;
  case CGPU_IMAGE_FORMAT_A8B8G8R8_SINT_PACK32: return VK_FORMAT_A8B8G8R8_SINT_PACK32;
  case CGPU_IMAGE_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_SRGB_PACK32;
  case CGPU_IMAGE_FORMAT_A2R10G10B10_UNORM_PACK32: return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
  case CGPU_IMAGE_FORMAT_A2R10G10B10_SNORM_PACK32: return VK_FORMAT_A2R10G10B10_SNORM_PACK32;
  case CGPU_IMAGE_FORMAT_A2R10G10B10_USCALED_PACK32: return VK_FORMAT_A2R10G10B10_USCALED_PACK32;
  case CGPU_IMAGE_FORMAT_A2R10G10B10_SSCALED_PACK32: return VK_FORMAT_A2R10G10B10_SSCALED_PACK32;
  case CGPU_IMAGE_FORMAT_A2R10G10B10_UINT_PACK32: return VK_FORMAT_A2R10G10B10_UINT_PACK32;
  case CGPU_IMAGE_FORMAT_A2R10G10B10_SINT_PACK32: return VK_FORMAT_A2R10G10B10_SINT_PACK32;
  case CGPU_IMAGE_FORMAT_A2B10G10R10_UNORM_PACK32: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  case CGPU_IMAGE_FORMAT_A2B10G10R10_SNORM_PACK32: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
  case CGPU_IMAGE_FORMAT_A2B10G10R10_USCALED_PACK32: return VK_FORMAT_A2B10G10R10_USCALED_PACK32;
  case CGPU_IMAGE_FORMAT_A2B10G10R10_SSCALED_PACK32: return VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
  case CGPU_IMAGE_FORMAT_A2B10G10R10_UINT_PACK32: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
  case CGPU_IMAGE_FORMAT_A2B10G10R10_SINT_PACK32: return VK_FORMAT_A2B10G10R10_SINT_PACK32;
  case CGPU_IMAGE_FORMAT_R16_UNORM: return VK_FORMAT_R16_UNORM;
  case CGPU_IMAGE_FORMAT_R16_SNORM: return VK_FORMAT_R16_SNORM;
  case CGPU_IMAGE_FORMAT_R16_USCALED: return VK_FORMAT_R16_USCALED;
  case CGPU_IMAGE_FORMAT_R16_SSCALED: return VK_FORMAT_R16_SSCALED;
  case CGPU_IMAGE_FORMAT_R16_UINT: return VK_FORMAT_R16_UINT;
  case CGPU_IMAGE_FORMAT_R16_SINT: return VK_FORMAT_R16_SINT;
  case CGPU_IMAGE_FORMAT_R16_SFLOAT: return VK_FORMAT_R16_SFLOAT;
  case CGPU_IMAGE_FORMAT_R16G16_UNORM: return VK_FORMAT_R16G16_UNORM;
  case CGPU_IMAGE_FORMAT_R16G16_SNORM: return VK_FORMAT_R16G16_SNORM;
  case CGPU_IMAGE_FORMAT_R16G16_USCALED: return VK_FORMAT_R16G16_USCALED;
  case CGPU_IMAGE_FORMAT_R16G16_SSCALED: return VK_FORMAT_R16G16_SSCALED;
  case CGPU_IMAGE_FORMAT_R16G16_UINT: return VK_FORMAT_R16G16_UINT;
  case CGPU_IMAGE_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
  case CGPU_IMAGE_FORMAT_R16G16_SFLOAT: return VK_FORMAT_R16G16_SFLOAT;
  case CGPU_IMAGE_FORMAT_R16G16B16_UNORM: return VK_FORMAT_R16G16B16_UNORM;
  case CGPU_IMAGE_FORMAT_R16G16B16_SNORM: return VK_FORMAT_R16G16B16_SNORM;
  case CGPU_IMAGE_FORMAT_R16G16B16_USCALED: return VK_FORMAT_R16G16B16_USCALED;
  case CGPU_IMAGE_FORMAT_R16G16B16_SSCALED: return VK_FORMAT_R16G16B16_SSCALED;
  case CGPU_IMAGE_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16_UINT;
  case CGPU_IMAGE_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16_SINT;
  case CGPU_IMAGE_FORMAT_R16G16B16_SFLOAT: return VK_FORMAT_R16G16B16_SFLOAT;
  case CGPU_IMAGE_FORMAT_R16G16B16A16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
  case CGPU_IMAGE_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
  case CGPU_IMAGE_FORMAT_R16G16B16A16_USCALED: return VK_FORMAT_R16G16B16A16_USCALED;
  case CGPU_IMAGE_FORMAT_R16G16B16A16_SSCALED: return VK_FORMAT_R16G16B16A16_SSCALED;
  case CGPU_IMAGE_FORMAT_R16G16B16A16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
  case CGPU_IMAGE_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
  case CGPU_IMAGE_FORMAT_R16G16B16A16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
  case CGPU_IMAGE_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
  case CGPU_IMAGE_FORMAT_R32_SINT: return VK_FORMAT_R32_SINT;
  case CGPU_IMAGE_FORMAT_R32_SFLOAT: return VK_FORMAT_R32_SFLOAT;
  case CGPU_IMAGE_FORMAT_R32G32_UINT: return VK_FORMAT_R32G32_UINT;
  case CGPU_IMAGE_FORMAT_R32G32_SINT: return VK_FORMAT_R32G32_SINT;
  case CGPU_IMAGE_FORMAT_R32G32_SFLOAT: return VK_FORMAT_R32G32_SFLOAT;
  case CGPU_IMAGE_FORMAT_R32G32B32_UINT: return VK_FORMAT_R32G32B32_UINT;
  case CGPU_IMAGE_FORMAT_R32G32B32_SINT: return VK_FORMAT_R32G32B32_SINT;
  case CGPU_IMAGE_FORMAT_R32G32B32_SFLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
  case CGPU_IMAGE_FORMAT_R32G32B32A32_UINT: return VK_FORMAT_R32G32B32A32_UINT;
  case CGPU_IMAGE_FORMAT_R32G32B32A32_SINT: return VK_FORMAT_R32G32B32A32_SINT;
  case CGPU_IMAGE_FORMAT_R32G32B32A32_SFLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
  case CGPU_IMAGE_FORMAT_R64_UINT: return VK_FORMAT_R64_UINT;
  case CGPU_IMAGE_FORMAT_R64_SINT: return VK_FORMAT_R64_SINT;
  case CGPU_IMAGE_FORMAT_R64_SFLOAT: return VK_FORMAT_R64_SFLOAT;
  case CGPU_IMAGE_FORMAT_R64G64_UINT: return VK_FORMAT_R64G64_UINT;
  case CGPU_IMAGE_FORMAT_R64G64_SINT: return VK_FORMAT_R64G64_SINT;
  case CGPU_IMAGE_FORMAT_R64G64_SFLOAT: return VK_FORMAT_R64G64_SFLOAT;
  case CGPU_IMAGE_FORMAT_R64G64B64_UINT: return VK_FORMAT_R64G64B64_UINT;
  case CGPU_IMAGE_FORMAT_R64G64B64_SINT: return VK_FORMAT_R64G64B64_SINT;
  case CGPU_IMAGE_FORMAT_R64G64B64_SFLOAT: return VK_FORMAT_R64G64B64_SFLOAT;
  case CGPU_IMAGE_FORMAT_R64G64B64A64_UINT: return VK_FORMAT_R64G64B64A64_UINT;
  case CGPU_IMAGE_FORMAT_R64G64B64A64_SINT: return VK_FORMAT_R64G64B64A64_SINT;
  case CGPU_IMAGE_FORMAT_R64G64B64A64_SFLOAT: return VK_FORMAT_R64G64B64A64_SFLOAT;
  case CGPU_IMAGE_FORMAT_B10G11R11_UFLOAT_PACK32: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
  case CGPU_IMAGE_FORMAT_E5B9G9R9_UFLOAT_PACK32: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
  case CGPU_IMAGE_FORMAT_D16_UNORM: return VK_FORMAT_D16_UNORM;
  case CGPU_IMAGE_FORMAT_X8_D24_UNORM_PACK32: return VK_FORMAT_X8_D24_UNORM_PACK32;
  case CGPU_IMAGE_FORMAT_D32_SFLOAT: return VK_FORMAT_D32_SFLOAT;
  case CGPU_IMAGE_FORMAT_S8_UINT: return VK_FORMAT_S8_UINT;
  case CGPU_IMAGE_FORMAT_D16_UNORM_S8_UINT: return VK_FORMAT_D16_UNORM_S8_UINT;
  case CGPU_IMAGE_FORMAT_D24_UNORM_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
  case CGPU_IMAGE_FORMAT_D32_SFLOAT_S8_UINT: return VK_FORMAT_D32_SFLOAT_S8_UINT;
  case CGPU_IMAGE_FORMAT_BC7_UNORM_BLOCK: return VK_FORMAT_BC7_UNORM_BLOCK;
  case CGPU_IMAGE_FORMAT_BC7_SRGB_BLOCK: return VK_FORMAT_BC7_SRGB_BLOCK;
  case CGPU_IMAGE_FORMAT_G8B8G8R8_422_UNORM: return VK_FORMAT_G8B8G8R8_422_UNORM;
  case CGPU_IMAGE_FORMAT_B8G8R8G8_422_UNORM: return VK_FORMAT_B8G8R8G8_422_UNORM;
  case CGPU_IMAGE_FORMAT_G8_B8_R8_3PLANE_420_UNORM: return VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
  case CGPU_IMAGE_FORMAT_G8_B8R8_2PLANE_420_UNORM: return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
  case CGPU_IMAGE_FORMAT_G8_B8_R8_3PLANE_422_UNORM: return VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM;
  case CGPU_IMAGE_FORMAT_G8_B8R8_2PLANE_422_UNORM: return VK_FORMAT_G8_B8R8_2PLANE_422_UNORM;
  case CGPU_IMAGE_FORMAT_G8_B8_R8_3PLANE_444_UNORM: return VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM;
  case CGPU_IMAGE_FORMAT_R10X6_UNORM_PACK16: return VK_FORMAT_R10X6_UNORM_PACK16;
  case CGPU_IMAGE_FORMAT_R10X6G10X6_UNORM_2PACK16: return VK_FORMAT_R10X6G10X6_UNORM_2PACK16;
  case CGPU_IMAGE_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16: return VK_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16;
  case CGPU_IMAGE_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16: return VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16;
  case CGPU_IMAGE_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16: return VK_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16;
  case CGPU_IMAGE_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16: return VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16;
  case CGPU_IMAGE_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16: return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
  case CGPU_IMAGE_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16: return VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16;
  case CGPU_IMAGE_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16: return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16;
  case CGPU_IMAGE_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16: return VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16;
  case CGPU_IMAGE_FORMAT_R12X4_UNORM_PACK16: return VK_FORMAT_R12X4_UNORM_PACK16;
  case CGPU_IMAGE_FORMAT_R12X4G12X4_UNORM_2PACK16: return VK_FORMAT_R12X4G12X4_UNORM_2PACK16;
  case CGPU_IMAGE_FORMAT_R12X4G12X4B12X4A12X4_UNORM_4PACK16: return VK_FORMAT_R12X4G12X4B12X4A12X4_UNORM_4PACK16;
  case CGPU_IMAGE_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16: return VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16;
  case CGPU_IMAGE_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16: return VK_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16;
  case CGPU_IMAGE_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16: return VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16;
  case CGPU_IMAGE_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16: return VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16;
  case CGPU_IMAGE_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16: return VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16;
  case CGPU_IMAGE_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16: return VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16;
  case CGPU_IMAGE_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16: return VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16;
  case CGPU_IMAGE_FORMAT_G16B16G16R16_422_UNORM: return VK_FORMAT_G16B16G16R16_422_UNORM;
  case CGPU_IMAGE_FORMAT_B16G16R16G16_422_UNORM: return VK_FORMAT_B16G16R16G16_422_UNORM;
  case CGPU_IMAGE_FORMAT_G16_B16_R16_3PLANE_420_UNORM: return VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM;
  case CGPU_IMAGE_FORMAT_G16_B16R16_2PLANE_420_UNORM: return VK_FORMAT_G16_B16R16_2PLANE_420_UNORM;
  case CGPU_IMAGE_FORMAT_G16_B16_R16_3PLANE_422_UNORM: return VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM;
  case CGPU_IMAGE_FORMAT_G16_B16R16_2PLANE_422_UNORM: return VK_FORMAT_G16_B16R16_2PLANE_422_UNORM;
  case CGPU_IMAGE_FORMAT_G16_B16_R16_3PLANE_444_UNORM: return VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM;
  default: return VK_FORMAT_UNDEFINED;
  }
}

static VkSamplerAddressMode cgpu_translate_address_mode(CgpuSamplerAddressMode mode)
{
  switch (mode)
  {
  case CGPU_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  case CGPU_SAMPLER_ADDRESS_MODE_REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
  case CGPU_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
  case CGPU_SAMPLER_ADDRESS_MODE_CLAMP_TO_BLACK: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  default: return VK_SAMPLER_ADDRESS_MODE_MAX_ENUM;
  }
}

/* API method implementation. */

bool cgpu_initialize(const char* p_app_name,
                     uint32_t version_major,
                     uint32_t version_minor,
                     uint32_t version_patch)
{
  VkResult result = volkInitialize();

  if (result != VK_SUCCESS || volkGetInstanceVersion() < MIN_VK_API_VERSION) {
    CGPU_RETURN_ERROR("failed to initialize volk");
  }

#ifndef NDEBUG
  const char* validation_layers[] = {
      "VK_LAYER_KHRONOS_validation"
  };
  const char* instance_extensions[] = {
      VK_EXT_DEBUG_UTILS_EXTENSION_NAME
  };
  uint32_t validation_layer_count = 1;
  uint32_t instance_extension_count = 1;
#else
  const char** validation_layers = NULL;
  uint32_t validation_layer_count = 0;
  const char** instance_extensions = NULL;
  uint32_t instance_extension_count = 0;
#endif

  VkApplicationInfo app_info;
  app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app_info.pNext = NULL;
  app_info.pApplicationName = p_app_name;
  app_info.applicationVersion = VK_MAKE_VERSION(version_major, version_minor, version_patch);
  app_info.pEngineName = p_app_name;
  app_info.engineVersion = VK_MAKE_VERSION(version_major, version_minor, version_patch);
  app_info.apiVersion = MIN_VK_API_VERSION;

  VkInstanceCreateInfo create_info;
  create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create_info.pNext = NULL;
  create_info.flags = 0;
  create_info.pApplicationInfo = &app_info;
  create_info.enabledLayerCount = validation_layer_count;
  create_info.ppEnabledLayerNames = validation_layers;
  create_info.enabledExtensionCount = instance_extension_count;
  create_info.ppEnabledExtensionNames = instance_extensions;

  result = vkCreateInstance(&create_info, NULL, &iinstance.instance);
  if (result != VK_SUCCESS) {
    CGPU_RETURN_ERROR("failed to create vulkan instance");
  }

  volkLoadInstanceOnly(iinstance.instance);

  resource_store_create(&idevice_store, sizeof(cgpu_idevice), 1);
  resource_store_create(&ibuffer_store, sizeof(cgpu_ibuffer), 16);
  resource_store_create(&iimage_store, sizeof(cgpu_iimage), 64);
  resource_store_create(&ishader_store, sizeof(cgpu_ishader), 16);
  resource_store_create(&ipipeline_store, sizeof(cgpu_ipipeline), 8);
  resource_store_create(&ifence_store, sizeof(cgpu_ifence), 8);
  resource_store_create(&icommand_buffer_store, sizeof(cgpu_icommand_buffer), 16);
  resource_store_create(&isampler_store, sizeof(cgpu_isampler), 64);

  return true;
}

void cgpu_terminate(void)
{
  resource_store_destroy(&idevice_store);
  resource_store_destroy(&ibuffer_store);
  resource_store_destroy(&iimage_store);
  resource_store_destroy(&ishader_store);
  resource_store_destroy(&ipipeline_store);
  resource_store_destroy(&ifence_store);
  resource_store_destroy(&icommand_buffer_store);
  resource_store_destroy(&isampler_store);

  vkDestroyInstance(iinstance.instance, NULL);
}

static bool cgpu_find_device_extension(const char* extension_name,
                                       uint32_t extension_count,
                                       VkExtensionProperties* extensions)
{
  for (uint32_t i = 0; i < extension_count; ++i)
  {
    const VkExtensionProperties* extension = &extensions[i];

    if (strcmp(extension->extensionName, extension_name) == 0)
    {
      return true;
    }
  }
  return false;
}

bool cgpu_create_device(cgpu_device* p_device)
{
  p_device->handle = resource_store_create_handle(&idevice_store);

  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(*p_device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  uint32_t phys_device_count;
  vkEnumeratePhysicalDevices(
    iinstance.instance,
    &phys_device_count,
    NULL
  );

  if (phys_device_count > MAX_PHYSICAL_DEVICES)
  {
    resource_store_free_handle(&idevice_store, p_device->handle);
    CGPU_RETURN_ERROR_HARDCODED_LIMIT_REACHED;
  }

  if (phys_device_count == 0)
  {
    resource_store_free_handle(&idevice_store, p_device->handle);
    CGPU_RETURN_ERROR("no physical device found");
  }

  VkPhysicalDevice phys_devices[MAX_PHYSICAL_DEVICES];

  vkEnumeratePhysicalDevices(
    iinstance.instance,
    &phys_device_count,
    phys_devices
  );

  idevice->physical_device = phys_devices[0];

  VkPhysicalDeviceFeatures features;
  vkGetPhysicalDeviceFeatures(idevice->physical_device, &features);
  idevice->features = cgpu_translate_physical_device_features(&features);

  VkPhysicalDeviceSubgroupProperties subgroup_properties;
  subgroup_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
  subgroup_properties.pNext = NULL;

  VkPhysicalDeviceProperties2 device_properties;
  device_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  device_properties.pNext = &subgroup_properties;

  vkGetPhysicalDeviceProperties2(idevice->physical_device, &device_properties);

  if (device_properties.properties.apiVersion < MIN_VK_API_VERSION)
  {
    resource_store_free_handle(&idevice_store, p_device->handle);
    CGPU_RETURN_ERROR("unsupported vulkan version");
  }

  if ((subgroup_properties.supportedStages & VK_QUEUE_COMPUTE_BIT) != VK_QUEUE_COMPUTE_BIT ||
      (subgroup_properties.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT) != VK_SUBGROUP_FEATURE_BASIC_BIT ||
      (subgroup_properties.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT) != VK_SUBGROUP_FEATURE_BALLOT_BIT)
  {
    resource_store_free_handle(&idevice_store, p_device->handle);
    CGPU_RETURN_ERROR("subgroup features not supported");
  }

  const VkPhysicalDeviceLimits* limits = &device_properties.properties.limits;
  idevice->limits = cgpu_translate_physical_device_limits(limits, &subgroup_properties);

  uint32_t device_ext_count;
  vkEnumerateDeviceExtensionProperties(
    idevice->physical_device,
    NULL,
    &device_ext_count,
    NULL
  );

  if (device_ext_count > MAX_DEVICE_EXTENSIONS)
  {
    resource_store_free_handle(&idevice_store, p_device->handle);
    CGPU_RETURN_ERROR_HARDCODED_LIMIT_REACHED;
  }

  VkExtensionProperties device_extensions[MAX_DEVICE_EXTENSIONS];

  vkEnumerateDeviceExtensionProperties(
    idevice->physical_device,
    NULL,
    &device_ext_count,
    device_extensions
  );

  const char* required_extensions[] = {
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME, // required by VK_KHR_acceleration_structure
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, // required by VK_KHR_acceleration_structure
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, // required by VK_KHR_acceleration_structure
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_KHR_SPIRV_1_4_EXTENSION_NAME, // required by VK_KHR_ray_tracing_pipeline
    VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME // required by VK_KHR_spirv_1_4
  };
  uint32_t required_extension_count = sizeof(required_extensions) / sizeof(required_extensions[0]);

  uint32_t enabled_device_extension_count = 0;
  const char* enabled_device_extensions[32];

  for (uint32_t i = 0; i < required_extension_count; i++)
  {
    const char* extension = required_extensions[i];

    if (!cgpu_find_device_extension(extension, device_ext_count, device_extensions))
    {
      resource_store_free_handle(&idevice_store, p_device->handle);

      fprintf(stderr, "error in %s:%d: extension %s not supported\n", __FILE__, __LINE__, extension);
      return false;
    }

    enabled_device_extensions[enabled_device_extension_count] = extension;
    enabled_device_extension_count++;
  }

  const char* VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME = "VK_KHR_portability_subset";
  if (cgpu_find_device_extension(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME, device_ext_count, device_extensions))
  {
    enabled_device_extensions[enabled_device_extension_count] = VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME;
    enabled_device_extension_count++;
  }

#ifndef NDEBUG
  if (cgpu_find_device_extension(VK_KHR_SHADER_CLOCK_EXTENSION_NAME, device_ext_count, device_extensions) && features.shaderInt64)
  {
    idevice->features.shaderClock = true;
    enabled_device_extensions[enabled_device_extension_count] = VK_KHR_SHADER_CLOCK_EXTENSION_NAME;
    enabled_device_extension_count++;
  }

#ifndef __APPLE__
  if (cgpu_find_device_extension(VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME, device_ext_count, device_extensions))
  {
    idevice->features.debugPrintf = true;
    enabled_device_extensions[enabled_device_extension_count] = VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME;
    enabled_device_extension_count++;
  }
#endif
#endif

  uint32_t queue_family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(
    idevice->physical_device,
    &queue_family_count,
    NULL
  );

  if (queue_family_count > MAX_QUEUE_FAMILIES)
  {
    resource_store_free_handle(&idevice_store, p_device->handle);
    CGPU_RETURN_ERROR_HARDCODED_LIMIT_REACHED;
  }

  VkQueueFamilyProperties queue_families[MAX_QUEUE_FAMILIES];

  vkGetPhysicalDeviceQueueFamilyProperties(
    idevice->physical_device,
    &queue_family_count,
    queue_families
  );

  int32_t queue_family_index = -1;
  for (uint32_t i = 0; i < queue_family_count; ++i)
  {
    const VkQueueFamilyProperties* queue_family = &queue_families[i];

    if ((queue_family->queueFlags & VK_QUEUE_COMPUTE_BIT) && (queue_family->queueFlags & VK_QUEUE_TRANSFER_BIT))
    {
      queue_family_index = i;
    }
  }
  if (queue_family_index == -1) {
    resource_store_free_handle(&idevice_store, p_device->handle);
    CGPU_RETURN_ERROR("no suitable queue family");
  }

  VkDeviceQueueCreateInfo queue_create_info;
  queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue_create_info.pNext = NULL;
  queue_create_info.flags = 0;
  queue_create_info.queueFamilyIndex = queue_family_index;
  queue_create_info.queueCount = 1;
  const float queue_priority = 1.0f;
  queue_create_info.pQueuePriorities = &queue_priority;

  VkPhysicalDeviceShaderClockFeaturesKHR shader_clock_features = {0};
  shader_clock_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR;
  shader_clock_features.pNext = NULL;
  shader_clock_features.shaderSubgroupClock = VK_TRUE;
  shader_clock_features.shaderDeviceClock = VK_FALSE;

  VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration_structure_features = {0};
  acceleration_structure_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
  acceleration_structure_features.pNext = idevice->features.shaderClock ? &shader_clock_features : NULL;
  acceleration_structure_features.accelerationStructure = VK_TRUE;
  acceleration_structure_features.accelerationStructureCaptureReplay = VK_FALSE;
  acceleration_structure_features.accelerationStructureIndirectBuild = VK_FALSE;
  acceleration_structure_features.accelerationStructureHostCommands = VK_FALSE;
  acceleration_structure_features.descriptorBindingAccelerationStructureUpdateAfterBind = VK_FALSE;

  VkPhysicalDeviceRayTracingPipelineFeaturesKHR ray_tracing_pipeline_features = {0};
  ray_tracing_pipeline_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
  ray_tracing_pipeline_features.pNext = &acceleration_structure_features;
  ray_tracing_pipeline_features.rayTracingPipeline = VK_TRUE;
  ray_tracing_pipeline_features.rayTracingPipelineShaderGroupHandleCaptureReplay = VK_FALSE;
  ray_tracing_pipeline_features.rayTracingPipelineShaderGroupHandleCaptureReplayMixed = VK_FALSE;
  ray_tracing_pipeline_features.rayTracingPipelineTraceRaysIndirect = VK_FALSE;
  ray_tracing_pipeline_features.rayTraversalPrimitiveCulling = VK_FALSE;

  VkPhysicalDeviceBufferDeviceAddressFeaturesKHR buffer_device_address_features = {0};
  buffer_device_address_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
  buffer_device_address_features.pNext = &ray_tracing_pipeline_features;
  buffer_device_address_features.bufferDeviceAddress = VK_TRUE;
  buffer_device_address_features.bufferDeviceAddressCaptureReplay = VK_FALSE;
  buffer_device_address_features.bufferDeviceAddressMultiDevice = VK_FALSE;

  VkPhysicalDeviceDescriptorIndexingFeaturesEXT descriptor_indexing_features = {0};
  descriptor_indexing_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
  descriptor_indexing_features.pNext = &buffer_device_address_features;
  descriptor_indexing_features.shaderInputAttachmentArrayDynamicIndexing = VK_FALSE;
  descriptor_indexing_features.shaderUniformTexelBufferArrayDynamicIndexing = VK_FALSE;
  descriptor_indexing_features.shaderStorageTexelBufferArrayDynamicIndexing = VK_FALSE;
  descriptor_indexing_features.shaderUniformBufferArrayNonUniformIndexing = VK_FALSE;
  descriptor_indexing_features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
  descriptor_indexing_features.shaderStorageBufferArrayNonUniformIndexing = VK_FALSE;
  descriptor_indexing_features.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
  descriptor_indexing_features.shaderInputAttachmentArrayNonUniformIndexing = VK_FALSE;
  descriptor_indexing_features.shaderUniformTexelBufferArrayNonUniformIndexing = VK_FALSE;
  descriptor_indexing_features.shaderStorageTexelBufferArrayNonUniformIndexing = VK_FALSE;
  descriptor_indexing_features.descriptorBindingUniformBufferUpdateAfterBind = VK_FALSE;
  descriptor_indexing_features.descriptorBindingSampledImageUpdateAfterBind = VK_FALSE;
  descriptor_indexing_features.descriptorBindingStorageImageUpdateAfterBind = VK_FALSE;
  descriptor_indexing_features.descriptorBindingStorageBufferUpdateAfterBind = VK_FALSE;
  descriptor_indexing_features.descriptorBindingUniformTexelBufferUpdateAfterBind = VK_FALSE;
  descriptor_indexing_features.descriptorBindingStorageTexelBufferUpdateAfterBind = VK_FALSE;
  descriptor_indexing_features.descriptorBindingUpdateUnusedWhilePending = VK_FALSE;
  descriptor_indexing_features.descriptorBindingPartiallyBound = VK_FALSE;
  descriptor_indexing_features.descriptorBindingVariableDescriptorCount = VK_FALSE;
  descriptor_indexing_features.runtimeDescriptorArray = VK_FALSE;

  VkPhysicalDevice16BitStorageFeatures device_16bit_storage_featurs = {0};
  device_16bit_storage_featurs.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
  device_16bit_storage_featurs.pNext = &descriptor_indexing_features;
  device_16bit_storage_featurs.storageBuffer16BitAccess = VK_TRUE;
  device_16bit_storage_featurs.uniformAndStorageBuffer16BitAccess = VK_FALSE;
  device_16bit_storage_featurs.storagePushConstant16 = VK_FALSE;
  device_16bit_storage_featurs.storageInputOutput16 = VK_FALSE;

  VkPhysicalDeviceFeatures2 device_features2;
  device_features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  device_features2.pNext = &device_16bit_storage_featurs;
  device_features2.features.robustBufferAccess = VK_FALSE;
  device_features2.features.fullDrawIndexUint32 = VK_FALSE;
  device_features2.features.imageCubeArray = VK_FALSE;
  device_features2.features.independentBlend = VK_FALSE;
  device_features2.features.geometryShader = VK_FALSE;
  device_features2.features.tessellationShader = VK_FALSE;
  device_features2.features.sampleRateShading = VK_FALSE;
  device_features2.features.dualSrcBlend = VK_FALSE;
  device_features2.features.logicOp = VK_FALSE;
  device_features2.features.multiDrawIndirect = VK_FALSE;
  device_features2.features.drawIndirectFirstInstance = VK_FALSE;
  device_features2.features.depthClamp = VK_FALSE;
  device_features2.features.depthBiasClamp = VK_FALSE;
  device_features2.features.fillModeNonSolid = VK_FALSE;
  device_features2.features.depthBounds = VK_FALSE;
  device_features2.features.wideLines = VK_FALSE;
  device_features2.features.largePoints = VK_FALSE;
  device_features2.features.alphaToOne = VK_FALSE;
  device_features2.features.multiViewport = VK_FALSE;
  device_features2.features.samplerAnisotropy = VK_TRUE;
  device_features2.features.textureCompressionETC2 = VK_FALSE;
  device_features2.features.textureCompressionASTC_LDR = VK_FALSE;
  device_features2.features.textureCompressionBC = VK_FALSE;
  device_features2.features.occlusionQueryPrecise = VK_FALSE;
  device_features2.features.pipelineStatisticsQuery = VK_FALSE;
  device_features2.features.vertexPipelineStoresAndAtomics = VK_FALSE;
  device_features2.features.fragmentStoresAndAtomics = VK_FALSE;
  device_features2.features.shaderTessellationAndGeometryPointSize = VK_FALSE;
  device_features2.features.shaderImageGatherExtended = VK_TRUE;
  device_features2.features.shaderStorageImageExtendedFormats = VK_FALSE;
  device_features2.features.shaderStorageImageMultisample = VK_FALSE;
  device_features2.features.shaderStorageImageReadWithoutFormat = VK_FALSE;
  device_features2.features.shaderStorageImageWriteWithoutFormat = VK_FALSE;
  device_features2.features.shaderUniformBufferArrayDynamicIndexing = VK_FALSE;
  device_features2.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
  device_features2.features.shaderStorageBufferArrayDynamicIndexing = VK_FALSE;
  device_features2.features.shaderStorageImageArrayDynamicIndexing = VK_FALSE;
  device_features2.features.shaderClipDistance = VK_FALSE;
  device_features2.features.shaderCullDistance = VK_FALSE;
  device_features2.features.shaderFloat64 = VK_FALSE;
  device_features2.features.shaderInt64 = idevice->features.shaderClock;
  device_features2.features.shaderInt16 = VK_TRUE;
  device_features2.features.shaderResourceResidency = VK_FALSE;
  device_features2.features.shaderResourceMinLod = VK_FALSE;
  device_features2.features.sparseBinding = VK_FALSE;
  device_features2.features.sparseResidencyBuffer = VK_FALSE;
  device_features2.features.sparseResidencyImage2D = VK_FALSE;
  device_features2.features.sparseResidencyImage3D = VK_FALSE;
  device_features2.features.sparseResidency2Samples = VK_FALSE;
  device_features2.features.sparseResidency4Samples = VK_FALSE;
  device_features2.features.sparseResidency8Samples = VK_FALSE;
  device_features2.features.sparseResidency16Samples = VK_FALSE;
  device_features2.features.sparseResidencyAliased = VK_FALSE;
  device_features2.features.variableMultisampleRate = VK_FALSE;
  device_features2.features.inheritedQueries = VK_FALSE;

  VkDeviceCreateInfo device_create_info;
  device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_create_info.pNext = &device_features2;
  device_create_info.flags = 0;
  device_create_info.queueCreateInfoCount = 1;
  device_create_info.pQueueCreateInfos = &queue_create_info;
  /* These two fields are ignored by up-to-date implementations since
   * nowadays, there is no difference to instance validation layers. */
  device_create_info.enabledLayerCount = 0;
  device_create_info.ppEnabledLayerNames = NULL;
  device_create_info.enabledExtensionCount = enabled_device_extension_count;
  device_create_info.ppEnabledExtensionNames = enabled_device_extensions;
  device_create_info.pEnabledFeatures = NULL;

  VkResult result = vkCreateDevice(
    idevice->physical_device,
    &device_create_info,
    NULL,
    &idevice->logical_device
  );
  if (result != VK_SUCCESS) {
    resource_store_free_handle(&idevice_store, p_device->handle);
    CGPU_RETURN_ERROR("failed to create device");
  }

  volkLoadDeviceTable(
    &idevice->table,
    idevice->logical_device
  );

  idevice->table.vkGetDeviceQueue(
    idevice->logical_device,
    queue_family_index,
    0,
    &idevice->compute_queue
  );

  VkCommandPoolCreateInfo pool_info;
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.pNext = NULL;
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = queue_family_index;

  result = idevice->table.vkCreateCommandPool(
    idevice->logical_device,
    &pool_info,
    NULL,
    &idevice->command_pool
  );

  if (result != VK_SUCCESS)
  {
    resource_store_free_handle(&idevice_store, p_device->handle);

    idevice->table.vkDestroyDevice(
      idevice->logical_device,
      NULL
    );

    CGPU_RETURN_ERROR("failed to create command pool");
  }

  VkQueryPoolCreateInfo timestamp_pool_info;
  timestamp_pool_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  timestamp_pool_info.pNext = NULL;
  timestamp_pool_info.flags = 0;
  timestamp_pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
  timestamp_pool_info.queryCount = CGPU_MAX_TIMESTAMP_QUERIES;
  timestamp_pool_info.pipelineStatistics = 0;

  result = idevice->table.vkCreateQueryPool(
    idevice->logical_device,
    &timestamp_pool_info,
    NULL,
    &idevice->timestamp_pool
  );

  if (result != VK_SUCCESS)
  {
    resource_store_free_handle(&idevice_store, p_device->handle);

    idevice->table.vkDestroyCommandPool(
      idevice->logical_device,
      idevice->command_pool,
      NULL
    );
    idevice->table.vkDestroyDevice(
      idevice->logical_device,
      NULL
    );

    CGPU_RETURN_ERROR("failed to create query pool");
  }

  VmaVulkanFunctions vulkan_functions = {0};
  vulkan_functions.vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties;
  vulkan_functions.vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties;
  vulkan_functions.vkGetPhysicalDeviceMemoryProperties2KHR = vkGetPhysicalDeviceMemoryProperties2;
  vulkan_functions.vkAllocateMemory = idevice->table.vkAllocateMemory;
  vulkan_functions.vkFreeMemory = idevice->table.vkFreeMemory;
  vulkan_functions.vkMapMemory = idevice->table.vkMapMemory;
  vulkan_functions.vkUnmapMemory = idevice->table.vkUnmapMemory;
  vulkan_functions.vkFlushMappedMemoryRanges = idevice->table.vkFlushMappedMemoryRanges;
  vulkan_functions.vkInvalidateMappedMemoryRanges = idevice->table.vkInvalidateMappedMemoryRanges;
  vulkan_functions.vkBindBufferMemory = idevice->table.vkBindBufferMemory;
  vulkan_functions.vkBindImageMemory = idevice->table.vkBindImageMemory;
  vulkan_functions.vkGetBufferMemoryRequirements = idevice->table.vkGetBufferMemoryRequirements;
  vulkan_functions.vkGetImageMemoryRequirements = idevice->table.vkGetImageMemoryRequirements;
  vulkan_functions.vkCreateBuffer = idevice->table.vkCreateBuffer;
  vulkan_functions.vkDestroyBuffer = idevice->table.vkDestroyBuffer;
  vulkan_functions.vkCreateImage = idevice->table.vkCreateImage;
  vulkan_functions.vkDestroyImage = idevice->table.vkDestroyImage;
  vulkan_functions.vkCmdCopyBuffer = idevice->table.vkCmdCopyBuffer;
  vulkan_functions.vkGetBufferMemoryRequirements2KHR = idevice->table.vkGetBufferMemoryRequirements2;
  vulkan_functions.vkGetImageMemoryRequirements2KHR = idevice->table.vkGetImageMemoryRequirements2;
  vulkan_functions.vkBindBufferMemory2KHR = idevice->table.vkBindBufferMemory2;
  vulkan_functions.vkBindImageMemory2KHR = idevice->table.vkBindImageMemory2;

  VmaAllocatorCreateInfo alloc_create_info = {0};
  alloc_create_info.vulkanApiVersion = MIN_VK_API_VERSION;
  alloc_create_info.physicalDevice = idevice->physical_device;
  alloc_create_info.device = idevice->logical_device;
  alloc_create_info.instance = iinstance.instance;
  alloc_create_info.pVulkanFunctions = &vulkan_functions;

  result = vmaCreateAllocator(&alloc_create_info, &idevice->allocator);

  if (result != VK_SUCCESS)
  {
    resource_store_free_handle(&idevice_store, p_device->handle);

    idevice->table.vkDestroyQueryPool(
      idevice->logical_device,
      idevice->timestamp_pool,
      NULL
    );
    idevice->table.vkDestroyCommandPool(
      idevice->logical_device,
      idevice->command_pool,
      NULL
    );
    idevice->table.vkDestroyDevice(
      idevice->logical_device,
      NULL
    );
    CGPU_RETURN_ERROR("failed to create vma allocator");
  }

  return true;
}

bool cgpu_destroy_device(cgpu_device device)
{
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  vmaDestroyAllocator(idevice->allocator);

  idevice->table.vkDestroyQueryPool(
    idevice->logical_device,
    idevice->timestamp_pool,
    NULL
  );
  idevice->table.vkDestroyCommandPool(
    idevice->logical_device,
    idevice->command_pool,
    NULL
  );
  idevice->table.vkDestroyDevice(
    idevice->logical_device,
    NULL
  );

  resource_store_free_handle(&idevice_store, device.handle);
  return true;
}

bool cgpu_create_shader(cgpu_device device,
                        uint64_t size,
                        const uint8_t* p_source,
                        cgpu_shader* p_shader)
{
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  p_shader->handle = resource_store_create_handle(&ishader_store);

  cgpu_ishader* ishader;
  if (!cgpu_resolve_shader(*p_shader, &ishader)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  VkShaderModuleCreateInfo shader_module_create_info;
  shader_module_create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  shader_module_create_info.pNext = NULL;
  shader_module_create_info.flags = 0;
  shader_module_create_info.codeSize = size;
  shader_module_create_info.pCode = (uint32_t*) p_source;

  VkResult result = idevice->table.vkCreateShaderModule(
    idevice->logical_device,
    &shader_module_create_info,
    NULL,
    &ishader->module
  );
  if (result != VK_SUCCESS) {
    resource_store_free_handle(&ishader_store, p_shader->handle);
    CGPU_RETURN_ERROR("failed to create shader module");
  }

  if (!cgpu_perform_shader_reflection(size, (uint32_t*) p_source, &ishader->reflection))
  {
    idevice->table.vkDestroyShaderModule(
      idevice->logical_device,
      ishader->module,
      NULL
    );
    resource_store_free_handle(&ishader_store, p_shader->handle);
    CGPU_RETURN_ERROR("failed to reflect shader");
  }

  return true;
}

bool cgpu_destroy_shader(cgpu_device device,
                         cgpu_shader shader)
{
  cgpu_idevice* idevice;
  cgpu_ishader* ishader;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  if (!cgpu_resolve_shader(shader, &ishader)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  cgpu_destroy_shader_reflection(&ishader->reflection);

  idevice->table.vkDestroyShaderModule(
    idevice->logical_device,
    ishader->module,
    NULL
  );

  resource_store_free_handle(&ishader_store, shader.handle);

  return true;
}

bool cgpu_create_buffer(cgpu_device device,
                        CgpuBufferUsageFlags usage,
                        CgpuMemoryPropertyFlags memory_properties,
                        uint64_t size,
                        cgpu_buffer* p_buffer)
{
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  p_buffer->handle = resource_store_create_handle(&ibuffer_store);

  cgpu_ibuffer* ibuffer;
  if (!cgpu_resolve_buffer(*p_buffer, &ibuffer)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  VkBufferUsageFlags vk_buffer_usage = 0;
  if ((usage & CGPU_BUFFER_USAGE_FLAG_TRANSFER_SRC) == CGPU_BUFFER_USAGE_FLAG_TRANSFER_SRC) {
    vk_buffer_usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  }
  if ((usage & CGPU_BUFFER_USAGE_FLAG_TRANSFER_DST) == CGPU_BUFFER_USAGE_FLAG_TRANSFER_DST) {
    vk_buffer_usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  }
  if ((usage & CGPU_BUFFER_USAGE_FLAG_UNIFORM_BUFFER) == CGPU_BUFFER_USAGE_FLAG_UNIFORM_BUFFER) {
    vk_buffer_usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  }
  if ((usage & CGPU_BUFFER_USAGE_FLAG_STORAGE_BUFFER) == CGPU_BUFFER_USAGE_FLAG_STORAGE_BUFFER) {
    vk_buffer_usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  }

  VkBufferCreateInfo buffer_info;
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.pNext = NULL;
  buffer_info.flags = 0;
  buffer_info.size = size;
  buffer_info.usage = vk_buffer_usage;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  buffer_info.queueFamilyIndexCount = 0;
  buffer_info.pQueueFamilyIndices = NULL;

  VmaAllocationCreateInfo alloc_info = {0};
  alloc_info.requiredFlags = cgpu_translate_memory_properties(memory_properties);

  VkResult result = vmaCreateBuffer(
    idevice->allocator,
    &buffer_info,
    &alloc_info,
    &ibuffer->buffer,
    &ibuffer->allocation,
    NULL
  );

  if (result != VK_SUCCESS)
  {
    resource_store_free_handle(&ibuffer_store, p_buffer->handle);
    CGPU_RETURN_ERROR("failed to create buffer");
  }

  ibuffer->size = size;

  return true;
}

bool cgpu_destroy_buffer(cgpu_device device,
                         cgpu_buffer buffer)
{
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_ibuffer* ibuffer;
  if (!cgpu_resolve_buffer(buffer, &ibuffer)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  vmaDestroyBuffer(idevice->allocator, ibuffer->buffer, ibuffer->allocation);

  resource_store_free_handle(&ibuffer_store, buffer.handle);

  return true;
}

bool cgpu_map_buffer(cgpu_device device,
                     cgpu_buffer buffer,
                     void** pp_mapped_mem)
{
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_ibuffer* ibuffer;
  if (!cgpu_resolve_buffer(buffer, &ibuffer)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  if (vmaMapMemory(idevice->allocator, ibuffer->allocation, pp_mapped_mem) != VK_SUCCESS) {
    CGPU_RETURN_ERROR("failed to map buffer memory");
  }
  return true;
}

bool cgpu_unmap_buffer(cgpu_device device,
                       cgpu_buffer buffer)
{
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_ibuffer* ibuffer;
  if (!cgpu_resolve_buffer(buffer, &ibuffer)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  vmaUnmapMemory(idevice->allocator, ibuffer->allocation);
  return true;
}

bool cgpu_create_image(cgpu_device device,
                       const cgpu_image_description* image_desc,
                       cgpu_image* p_image)
{
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  p_image->handle = resource_store_create_handle(&iimage_store);

  cgpu_iimage* iimage;
  if (!cgpu_resolve_image(*p_image, &iimage)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  VkImageTiling vk_image_tiling = VK_IMAGE_TILING_OPTIMAL;
  if (image_desc->usage == CGPU_IMAGE_USAGE_FLAG_TRANSFER_SRC ||
      image_desc->usage == CGPU_IMAGE_USAGE_FLAG_TRANSFER_DST)
  {
    vk_image_tiling = VK_IMAGE_TILING_LINEAR;
  }

  VkImageUsageFlags vk_image_usage = 0;
  if ((image_desc->usage & CGPU_IMAGE_USAGE_FLAG_TRANSFER_SRC) == CGPU_IMAGE_USAGE_FLAG_TRANSFER_SRC) {
    vk_image_usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  }
  if ((image_desc->usage & CGPU_IMAGE_USAGE_FLAG_TRANSFER_DST) == CGPU_IMAGE_USAGE_FLAG_TRANSFER_DST) {
    vk_image_usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  }
  if ((image_desc->usage & CGPU_IMAGE_USAGE_FLAG_SAMPLED) == CGPU_IMAGE_USAGE_FLAG_SAMPLED) {
    vk_image_usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
  }
  if ((image_desc->usage & CGPU_IMAGE_USAGE_FLAG_STORAGE) == CGPU_IMAGE_USAGE_FLAG_STORAGE) {
    vk_image_usage |= VK_IMAGE_USAGE_STORAGE_BIT;
  }

  VkFormat vk_format = cgpu_translate_image_format(image_desc->format);

  VkImageCreateInfo image_info;
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.pNext = NULL;
  image_info.flags = 0;
  image_info.imageType = image_desc->is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  image_info.format = vk_format;
  image_info.extent.width = image_desc->width;
  image_info.extent.height = image_desc->height;
  image_info.extent.depth = image_desc->is3d ? image_desc->depth : 1;
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = vk_image_tiling;
  image_info.usage = vk_image_usage;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.queueFamilyIndexCount = 0;
  image_info.pQueueFamilyIndices = NULL;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VmaAllocationCreateInfo alloc_info = {0};
  alloc_info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

  VkResult result = vmaCreateImage(
    idevice->allocator,
    &image_info,
    &alloc_info,
    &iimage->image,
    &iimage->allocation,
    NULL
  );

  if (result != VK_SUCCESS) {
    resource_store_free_handle(&iimage_store, p_image->handle);
    CGPU_RETURN_ERROR("failed to create image");
  }

  VmaAllocationInfo allocation_info;
  vmaGetAllocationInfo(
    idevice->allocator,
    iimage->allocation,
    &allocation_info
  );

  iimage->size = allocation_info.size;

  VkImageViewCreateInfo image_view_info;
  image_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  image_view_info.pNext = NULL;
  image_view_info.flags = 0;
  image_view_info.image = iimage->image;
  image_view_info.viewType = image_desc->is3d ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
  image_view_info.format = vk_format;
  image_view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  image_view_info.subresourceRange.baseMipLevel = 0;
  image_view_info.subresourceRange.levelCount = 1;
  image_view_info.subresourceRange.baseArrayLayer = 0;
  image_view_info.subresourceRange.layerCount = 1;

  result = idevice->table.vkCreateImageView(
    idevice->logical_device,
    &image_view_info,
    NULL,
    &iimage->image_view
  );
  if (result != VK_SUCCESS)
  {
    resource_store_free_handle(&iimage_store, p_image->handle);
    vmaDestroyImage(idevice->allocator, iimage->image, iimage->allocation);
    CGPU_RETURN_ERROR("failed to create image view");
  }

  iimage->width = image_desc->width;
  iimage->height = image_desc->height;
  iimage->depth = image_desc->is3d ? image_desc->depth : 1;
  iimage->layout = VK_IMAGE_LAYOUT_UNDEFINED;
  iimage->access_mask = 0;

  return true;
}

bool cgpu_destroy_image(cgpu_device device,
                        cgpu_image image)
{
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_iimage* iimage;
  if (!cgpu_resolve_image(image, &iimage)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  idevice->table.vkDestroyImageView(
    idevice->logical_device,
    iimage->image_view,
    NULL
  );

  vmaDestroyImage(idevice->allocator, iimage->image, iimage->allocation);

  resource_store_free_handle(&iimage_store, image.handle);

  return true;
}

bool cgpu_map_image(cgpu_device device,
                    cgpu_image image,
                    void** pp_mapped_mem)
{
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_iimage* iimage;
  if (!cgpu_resolve_image(image, &iimage)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  if (vmaMapMemory(idevice->allocator, iimage->allocation, pp_mapped_mem) != VK_SUCCESS) {
    CGPU_RETURN_ERROR("failed to map image memory");
  }
  return true;
}

bool cgpu_unmap_image(cgpu_device device,
                      cgpu_image image)
{
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_iimage* iimage;
  if (!cgpu_resolve_image(image, &iimage)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  vmaUnmapMemory(idevice->allocator, iimage->allocation);
  return true;
}

bool cgpu_create_sampler(cgpu_device device,
                         CgpuSamplerAddressMode address_mode_u,
                         CgpuSamplerAddressMode address_mode_v,
                         CgpuSamplerAddressMode address_mode_w,
                         cgpu_sampler* p_sampler)
{
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  p_sampler->handle = resource_store_create_handle(&isampler_store);

  cgpu_isampler* isampler;
  if (!cgpu_resolve_sampler(*p_sampler, &isampler)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  // Emulate MDL's clip wrap mode if necessary; use optimal mode (according to ARM) if not.
  bool clampToBlack = (address_mode_u == CGPU_SAMPLER_ADDRESS_MODE_CLAMP_TO_BLACK) ||
                      (address_mode_v == CGPU_SAMPLER_ADDRESS_MODE_CLAMP_TO_BLACK) ||
                      (address_mode_w == CGPU_SAMPLER_ADDRESS_MODE_CLAMP_TO_BLACK);

  VkSamplerCreateInfo create_info;
  create_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  create_info.pNext = NULL;
  create_info.flags = 0;
  create_info.magFilter = VK_FILTER_LINEAR;
  create_info.minFilter = VK_FILTER_LINEAR;
  create_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  create_info.addressModeU = cgpu_translate_address_mode(address_mode_u);
  create_info.addressModeV = cgpu_translate_address_mode(address_mode_v);
  create_info.addressModeW = cgpu_translate_address_mode(address_mode_w);
  create_info.mipLodBias = 0.0f;
  create_info.anisotropyEnable = VK_FALSE;
  create_info.maxAnisotropy = 1.0f;
  create_info.compareEnable = VK_FALSE;
  create_info.compareOp = VK_COMPARE_OP_NEVER;
  create_info.minLod = 0.0f;
  create_info.maxLod = VK_LOD_CLAMP_NONE;
  create_info.borderColor = clampToBlack ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  create_info.unnormalizedCoordinates = VK_FALSE;

  VkResult result = idevice->table.vkCreateSampler(
    idevice->logical_device,
    &create_info,
    NULL,
    &isampler->sampler
  );

  if (result != VK_SUCCESS) {
    resource_store_free_handle(&isampler_store, p_sampler->handle);
    CGPU_RETURN_ERROR("failed to create sampler");
  }

  return true;
}

bool cgpu_destroy_sampler(cgpu_device device,
                          cgpu_sampler sampler)
{
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_isampler* isampler;
  if (!cgpu_resolve_sampler(sampler, &isampler)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  idevice->table.vkDestroySampler(idevice->logical_device, isampler->sampler, NULL);

  resource_store_free_handle(&isampler_store, sampler.handle);

  return true;
}

static bool cgpu_create_pipeline_layout(cgpu_idevice* idevice, cgpu_ipipeline* ipipeline, cgpu_ishader* ishader)
{
  VkPushConstantRange push_const_range;
  push_const_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  push_const_range.offset = 0;
  push_const_range.size = ishader->reflection.push_constants_size;

  VkPipelineLayoutCreateInfo pipeline_layout_create_info;
  pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_create_info.pNext = NULL;
  pipeline_layout_create_info.flags = 0;
  pipeline_layout_create_info.setLayoutCount = 1;
  pipeline_layout_create_info.pSetLayouts = &ipipeline->descriptor_set_layout;
  pipeline_layout_create_info.pushConstantRangeCount = push_const_range.size ? 1 : 0;
  pipeline_layout_create_info.pPushConstantRanges = &push_const_range;

  return idevice->table.vkCreatePipelineLayout(idevice->logical_device,
                                               &pipeline_layout_create_info,
                                               NULL,
                                               &ipipeline->layout) == VK_SUCCESS;
}

static bool cgpu_create_pipeline_descriptors(cgpu_idevice* idevice, cgpu_ipipeline* ipipeline, cgpu_ishader* ishader)
{
  const cgpu_shader_reflection* shader_reflection = &ishader->reflection;

  if (shader_reflection->binding_count >= MAX_DESCRIPTOR_SET_LAYOUT_BINDINGS)
  {
    CGPU_RETURN_ERROR_HARDCODED_LIMIT_REACHED;
  }

  ipipeline->descriptor_set_layout_binding_count = shader_reflection->binding_count;

  for (uint32_t i = 0; i < shader_reflection->binding_count; i++)
  {
    const cgpu_shader_reflection_binding* binding = &shader_reflection->bindings[i];

    VkDescriptorSetLayoutBinding* descriptor_set_layout_binding = &ipipeline->descriptor_set_layout_bindings[i];
    descriptor_set_layout_binding->binding = binding->binding;
    descriptor_set_layout_binding->descriptorType = binding->descriptor_type;
    descriptor_set_layout_binding->descriptorCount = binding->count;
    descriptor_set_layout_binding->stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    descriptor_set_layout_binding->pImmutableSamplers = NULL;
  }

  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info;
  descriptor_set_layout_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  descriptor_set_layout_create_info.pNext = NULL;
  descriptor_set_layout_create_info.flags = 0;
  descriptor_set_layout_create_info.bindingCount = shader_reflection->binding_count;
  descriptor_set_layout_create_info.pBindings = ipipeline->descriptor_set_layout_bindings;

  VkResult result = idevice->table.vkCreateDescriptorSetLayout(
    idevice->logical_device,
    &descriptor_set_layout_create_info,
    NULL,
    &ipipeline->descriptor_set_layout
  );

  if (result != VK_SUCCESS) {
    CGPU_RETURN_ERROR("failed to create descriptor set layout");
  }

  uint32_t buffer_count = 0;
  uint32_t storage_image_count = 0;
  uint32_t sampled_image_count = 0;
  uint32_t sampler_count = 0;

  for (uint32_t i = 0; i < shader_reflection->binding_count; i++)
  {
    const cgpu_shader_reflection_binding* binding = &shader_reflection->bindings[i];

    switch (binding->descriptor_type)
    {
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: buffer_count += binding->count; break;
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: storage_image_count += binding->count; break;
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: sampled_image_count += binding->count; break;
    case VK_DESCRIPTOR_TYPE_SAMPLER: sampler_count += binding->count; break;
    default: {
      idevice->table.vkDestroyDescriptorSetLayout(
        idevice->logical_device,
        ipipeline->descriptor_set_layout,
        NULL
      );
      CGPU_RETURN_ERROR("invalid descriptor type");
    }
    }
  }

  uint32_t pool_size_count = 0;
  VkDescriptorPoolSize pool_sizes[16];

  if (buffer_count > 0)
  {
    pool_sizes[pool_size_count].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[pool_size_count].descriptorCount = buffer_count;
    pool_size_count++;
  }
  if (storage_image_count > 0)
  {
    pool_sizes[pool_size_count].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_sizes[pool_size_count].descriptorCount = storage_image_count;
    pool_size_count++;
  }
  if (sampled_image_count > 0)
  {
    pool_sizes[pool_size_count].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    pool_sizes[pool_size_count].descriptorCount = sampled_image_count;
    pool_size_count++;
  }
  if (sampler_count > 0)
  {
    pool_sizes[pool_size_count].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    pool_sizes[pool_size_count].descriptorCount = sampler_count;
    pool_size_count++;
  }

  VkDescriptorPoolCreateInfo descriptor_pool_create_info;
  descriptor_pool_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  descriptor_pool_create_info.pNext = NULL;
  descriptor_pool_create_info.flags = 0;
  descriptor_pool_create_info.maxSets = 1;
  descriptor_pool_create_info.poolSizeCount = pool_size_count;
  descriptor_pool_create_info.pPoolSizes = pool_sizes;

  result = idevice->table.vkCreateDescriptorPool(
    idevice->logical_device,
    &descriptor_pool_create_info,
    NULL,
    &ipipeline->descriptor_pool
  );
  if (result != VK_SUCCESS) {
    idevice->table.vkDestroyDescriptorSetLayout(
      idevice->logical_device,
      ipipeline->descriptor_set_layout,
      NULL
    );
    CGPU_RETURN_ERROR("failed to create descriptor pool");
  }

  VkDescriptorSetAllocateInfo descriptor_set_allocate_info;
  descriptor_set_allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  descriptor_set_allocate_info.pNext = NULL;
  descriptor_set_allocate_info.descriptorPool = ipipeline->descriptor_pool;
  descriptor_set_allocate_info.descriptorSetCount = 1;
  descriptor_set_allocate_info.pSetLayouts = &ipipeline->descriptor_set_layout;

  result = idevice->table.vkAllocateDescriptorSets(
    idevice->logical_device,
    &descriptor_set_allocate_info,
    &ipipeline->descriptor_set
  );
  if (result != VK_SUCCESS) {
    idevice->table.vkDestroyDescriptorPool(
      idevice->logical_device,
      ipipeline->descriptor_pool,
      NULL
    );
    idevice->table.vkDestroyDescriptorSetLayout(
      idevice->logical_device,
      ipipeline->descriptor_set_layout,
      NULL
    );
    CGPU_RETURN_ERROR("failed to allocate descriptor set");
  }

  return true;
}

bool cgpu_create_pipeline(cgpu_device device,
                          cgpu_shader shader,
                          cgpu_pipeline* p_pipeline)
{
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_ishader* ishader;
  if (!cgpu_resolve_shader(shader, &ishader)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  p_pipeline->handle = resource_store_create_handle(&ipipeline_store);

  cgpu_ipipeline* ipipeline;
  if (!cgpu_resolve_pipeline(*p_pipeline, &ipipeline)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  if (!cgpu_create_pipeline_descriptors(idevice, ipipeline, ishader))
  {
    resource_store_free_handle(&ipipeline_store, p_pipeline->handle);
    CGPU_RETURN_ERROR("failed to create descriptor set layout");
  }

  if (!cgpu_create_pipeline_layout(idevice, ipipeline, ishader))
  {
    resource_store_free_handle(&ipipeline_store, p_pipeline->handle);
    idevice->table.vkDestroyDescriptorSetLayout(
      idevice->logical_device,
      ipipeline->descriptor_set_layout,
      NULL
    );
    idevice->table.vkDestroyDescriptorPool(
      idevice->logical_device,
      ipipeline->descriptor_pool,
      NULL
    );
    CGPU_RETURN_ERROR("failed to create pipeline layout");
  }

  VkPipelineShaderStageCreateInfo pipeline_shader_stage_create_info;
  pipeline_shader_stage_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pipeline_shader_stage_create_info.pNext = NULL;
  pipeline_shader_stage_create_info.flags = 0;
  pipeline_shader_stage_create_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipeline_shader_stage_create_info.module = ishader->module;
  pipeline_shader_stage_create_info.pName = "main";
  pipeline_shader_stage_create_info.pSpecializationInfo = NULL;

  VkComputePipelineCreateInfo pipeline_create_info;
  pipeline_create_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipeline_create_info.pNext = NULL;
  pipeline_create_info.flags = VK_PIPELINE_CREATE_DISPATCH_BASE;
  pipeline_create_info.stage = pipeline_shader_stage_create_info;
  pipeline_create_info.layout = ipipeline->layout;
  pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  pipeline_create_info.basePipelineIndex = 0;

  VkResult result = idevice->table.vkCreateComputePipelines(
    idevice->logical_device,
    VK_NULL_HANDLE,
    1,
    &pipeline_create_info,
    NULL,
    &ipipeline->pipeline
  );

  if (result != VK_SUCCESS) {
    resource_store_free_handle(&ipipeline_store, p_pipeline->handle);
    idevice->table.vkDestroyPipelineLayout(
      idevice->logical_device,
      ipipeline->layout,
      NULL
    );
    idevice->table.vkDestroyDescriptorSetLayout(
      idevice->logical_device,
      ipipeline->descriptor_set_layout,
      NULL
    );
    idevice->table.vkDestroyDescriptorPool(
      idevice->logical_device,
      ipipeline->descriptor_pool,
      NULL
    );
    CGPU_RETURN_ERROR("failed to create compute pipeline");
  }

  ipipeline->shader = shader;

  return true;
}

bool cgpu_destroy_pipeline(cgpu_device device,
                           cgpu_pipeline pipeline)
{
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_ipipeline* ipipeline;
  if (!cgpu_resolve_pipeline(pipeline, &ipipeline)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  idevice->table.vkDestroyDescriptorPool(
    idevice->logical_device,
    ipipeline->descriptor_pool,
    NULL
  );
  idevice->table.vkDestroyPipeline(
    idevice->logical_device,
    ipipeline->pipeline,
    NULL
  );
  idevice->table.vkDestroyPipelineLayout(
    idevice->logical_device,
    ipipeline->layout,
    NULL
  );
  idevice->table.vkDestroyDescriptorSetLayout(
    idevice->logical_device,
    ipipeline->descriptor_set_layout,
    NULL
  );

  resource_store_free_handle(&ipipeline_store, pipeline.handle);

  return true;
}

bool cgpu_create_command_buffer(cgpu_device device,
                                cgpu_command_buffer* p_command_buffer)
{
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  p_command_buffer->handle = resource_store_create_handle(&icommand_buffer_store);

  cgpu_icommand_buffer* icommand_buffer;
  if (!cgpu_resolve_command_buffer(*p_command_buffer, &icommand_buffer)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  icommand_buffer->device.handle = device.handle;

  VkCommandBufferAllocateInfo cmdbuf_alloc_info;
  cmdbuf_alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cmdbuf_alloc_info.pNext = NULL;
  cmdbuf_alloc_info.commandPool = idevice->command_pool;
  cmdbuf_alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmdbuf_alloc_info.commandBufferCount = 1;

  VkResult result = idevice->table.vkAllocateCommandBuffers(
    idevice->logical_device,
    &cmdbuf_alloc_info,
    &icommand_buffer->command_buffer
  );
  if (result != VK_SUCCESS) {
    CGPU_RETURN_ERROR("failed to allocate command buffer");
  }

  return true;
}

bool cgpu_destroy_command_buffer(cgpu_device device,
                                 cgpu_command_buffer command_buffer)
{
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_icommand_buffer* icommand_buffer;
  if (!cgpu_resolve_command_buffer(command_buffer, &icommand_buffer)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  idevice->table.vkFreeCommandBuffers(
    idevice->logical_device,
    idevice->command_pool,
    1,
    &icommand_buffer->command_buffer
  );

  resource_store_free_handle(&icommand_buffer_store, command_buffer.handle);
  return true;
}

bool cgpu_begin_command_buffer(cgpu_command_buffer command_buffer)
{
  cgpu_icommand_buffer* icommand_buffer;
  if (!cgpu_resolve_command_buffer(command_buffer, &icommand_buffer)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(icommand_buffer->device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  VkCommandBufferBeginInfo command_buffer_begin_info;
  command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  command_buffer_begin_info.pNext = NULL;
  command_buffer_begin_info.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
  command_buffer_begin_info.pInheritanceInfo = NULL;

  VkResult result = idevice->table.vkBeginCommandBuffer(
    icommand_buffer->command_buffer,
    &command_buffer_begin_info
  );

  if (result != VK_SUCCESS) {
    CGPU_RETURN_ERROR("failed to begin command buffer");
  }
  return true;
}

bool cgpu_cmd_bind_pipeline(cgpu_command_buffer command_buffer,
                            cgpu_pipeline pipeline)
{
  cgpu_icommand_buffer* icommand_buffer;
  if (!cgpu_resolve_command_buffer(command_buffer, &icommand_buffer)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(icommand_buffer->device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_ipipeline* ipipeline;
  if (!cgpu_resolve_pipeline(pipeline, &ipipeline)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  idevice->table.vkCmdBindPipeline(
    icommand_buffer->command_buffer,
    VK_PIPELINE_BIND_POINT_COMPUTE,
    ipipeline->pipeline
  );
  idevice->table.vkCmdBindDescriptorSets(
    icommand_buffer->command_buffer,
    VK_PIPELINE_BIND_POINT_COMPUTE,
    ipipeline->layout,
    0,
    1,
    &ipipeline->descriptor_set,
    0,
    0
  );

  return true;
}

static bool cgpu_transition_image_layouts_for_shader(cgpu_ipipeline* ipipeline,
                                                     cgpu_icommand_buffer* icommand_buffer,
                                                     uint32_t image_count,
                                                     const cgpu_image_binding* p_images)
{
  cgpu_ishader* ishader;
  if (!cgpu_resolve_shader(ipipeline->shader, &ishader)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(icommand_buffer->device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  VkImageMemoryBarrier barriers[MAX_IMAGE_MEMORY_BARRIERS];
  uint32_t barrier_count = 0;

  /* FIXME: this has quadratic complexity */
  const cgpu_shader_reflection* reflection = &ishader->reflection;
  for (uint32_t i = 0; i < reflection->binding_count; i++)
  {
    const cgpu_shader_reflection_binding* binding = &reflection->bindings[i];

    VkImageLayout new_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (binding->descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
    {
      new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    else if (binding->descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
    {
      new_layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    else
    {
      /* Not an image. */
      continue;
    }

    for (uint32_t j = 0; j < binding->count; j++)
    {
      /* Image layout needs transitioning. */
      const cgpu_image_binding* image_binding = NULL;
      for (uint32_t k = 0; k < image_count; k++)
      {
        if (p_images[k].binding == binding->binding && p_images[k].index == j)
        {
          image_binding = &p_images[k];
          break;
        }
      }
      if (!image_binding)
      {
        CGPU_RETURN_ERROR("descriptor set binding mismatch");
      }

      cgpu_iimage* iimage;
      if (!cgpu_resolve_image(image_binding->image, &iimage)) {
        CGPU_RETURN_ERROR_INVALID_HANDLE;
      }

      VkImageLayout old_layout = iimage->layout;
      if (new_layout == old_layout)
      {
        continue;
      }

      VkAccessFlags access_mask = 0;
      if (binding->read_access) {
        access_mask = VK_ACCESS_SHADER_READ_BIT;
      }
      if (binding->write_access) {
        access_mask = VK_ACCESS_SHADER_WRITE_BIT;
      }

      if (barrier_count >= MAX_IMAGE_MEMORY_BARRIERS) {
        CGPU_RETURN_ERROR_HARDCODED_LIMIT_REACHED;
      }

      VkImageMemoryBarrier* barrier = &barriers[barrier_count++];
      barrier->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barrier->pNext = NULL;
      barrier->srcAccessMask = iimage->access_mask;
      barrier->dstAccessMask = access_mask;
      barrier->oldLayout = old_layout;
      barrier->newLayout = new_layout;
      barrier->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier->image = iimage->image;
      barrier->subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      barrier->subresourceRange.baseMipLevel = 0;
      barrier->subresourceRange.levelCount = 1;
      barrier->subresourceRange.baseArrayLayer = 0;
      barrier->subresourceRange.layerCount = 1;

      iimage->access_mask = access_mask;
      iimage->layout = new_layout;
    }
  }

  if (barrier_count > 0)
  {
    idevice->table.vkCmdPipelineBarrier(
      icommand_buffer->command_buffer,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      0,
      0,
      NULL,
      0,
      NULL,
      barrier_count,
      barriers
    );
  }

  return true;
}

bool cgpu_cmd_update_bindings(cgpu_command_buffer command_buffer,
                              cgpu_pipeline pipeline,
                              const cgpu_bindings* bindings
)
{
  cgpu_icommand_buffer* icommand_buffer;
  if (!cgpu_resolve_command_buffer(command_buffer, &icommand_buffer)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(icommand_buffer->device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_ipipeline* ipipeline;
  if (!cgpu_resolve_pipeline(pipeline, &ipipeline)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  bool c_result = cgpu_transition_image_layouts_for_shader(
    ipipeline,
    icommand_buffer,
    bindings->image_count,
    bindings->p_images
  );
  if (c_result != true) {
    return c_result;
  }

  VkDescriptorBufferInfo buffer_infos[MAX_DESCRIPTOR_BUFFER_INFOS];
  uint32_t buffer_info_count = 0;
  VkDescriptorImageInfo image_infos[MAX_DESCRIPTOR_IMAGE_INFOS];
  uint32_t image_info_count = 0;

  VkWriteDescriptorSet write_descriptor_sets[MAX_WRITE_DESCRIPTOR_SETS];
  uint32_t write_descriptor_set_count = 0;

  /* FIXME: this has a rather high complexity */
  for (uint32_t i = 0; i < ipipeline->descriptor_set_layout_binding_count; i++)
  {
    const VkDescriptorSetLayoutBinding* layout_binding = &ipipeline->descriptor_set_layout_bindings[i];

    if (write_descriptor_set_count >= MAX_WRITE_DESCRIPTOR_SETS) {
      CGPU_RETURN_ERROR_HARDCODED_LIMIT_REACHED;
    }

    VkWriteDescriptorSet* write_descriptor_set = &write_descriptor_sets[write_descriptor_set_count++];
    write_descriptor_set->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write_descriptor_set->pNext = NULL;
    write_descriptor_set->dstSet = ipipeline->descriptor_set;
    write_descriptor_set->dstBinding = layout_binding->binding;
    write_descriptor_set->dstArrayElement = 0;
    write_descriptor_set->descriptorCount = layout_binding->descriptorCount;
    write_descriptor_set->descriptorType = layout_binding->descriptorType;
    write_descriptor_set->pTexelBufferView = NULL;
    write_descriptor_set->pBufferInfo = NULL;
    write_descriptor_set->pImageInfo = NULL;

    for (uint32_t j = 0; j < layout_binding->descriptorCount; j++)
    {
      bool slotHandled = false;

      if (layout_binding->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
      {
        for (uint32_t k = 0; k < bindings->buffer_count; ++k)
        {
          const cgpu_buffer_binding* buffer_binding = &bindings->p_buffers[k];

          if (buffer_binding->binding != layout_binding->binding || buffer_binding->index != j)
          {
            continue;
          }

          cgpu_ibuffer* ibuffer;
          cgpu_buffer buffer = buffer_binding->buffer;
          if (!cgpu_resolve_buffer(buffer, &ibuffer)) {
            CGPU_RETURN_ERROR_INVALID_HANDLE;
          }

          if ((buffer_binding->offset % idevice->limits.minStorageBufferOffsetAlignment) != 0) {
            CGPU_RETURN_ERROR("buffer binding offset not aligned");
          }

          if (image_info_count >= MAX_DESCRIPTOR_BUFFER_INFOS) {
            CGPU_RETURN_ERROR_HARDCODED_LIMIT_REACHED;
          }

          VkDescriptorBufferInfo* buffer_info = &buffer_infos[buffer_info_count++];
          buffer_info->buffer = ibuffer->buffer;
          buffer_info->offset = buffer_binding->offset;
          buffer_info->range = (buffer_binding->size == CGPU_WHOLE_SIZE) ? (ibuffer->size - buffer_binding->offset) : buffer_binding->size;

          if (j == 0)
          {
            write_descriptor_set->pBufferInfo = buffer_info;
          }

          slotHandled = true;
          break;
        }
      }
      else if (layout_binding->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
               layout_binding->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
      {
        for (uint32_t k = 0; k < bindings->image_count; k++)
        {
          const cgpu_image_binding* image_binding = &bindings->p_images[k];

          if (image_binding->binding != layout_binding->binding || image_binding->index != j)
          {
            continue;
          }

          cgpu_iimage* iimage;
          cgpu_image image = image_binding->image;
          if (!cgpu_resolve_image(image, &iimage)) {
            CGPU_RETURN_ERROR_INVALID_HANDLE;
          }

          if (image_info_count >= MAX_DESCRIPTOR_IMAGE_INFOS) {
            CGPU_RETURN_ERROR_HARDCODED_LIMIT_REACHED;
          }

          VkDescriptorImageInfo* image_info = &image_infos[image_info_count++];
          image_info->sampler = VK_NULL_HANDLE;
          image_info->imageView = iimage->image_view;
          image_info->imageLayout = iimage->layout;

          if (j == 0)
          {
            write_descriptor_set->pImageInfo = image_info;
          }

          slotHandled = true;
          break;
        }
      }
      else if (layout_binding->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER)
      {
        for (uint32_t k = 0; k < bindings->sampler_count; k++)
        {
          const cgpu_sampler_binding* sampler_binding = &bindings->p_samplers[k];

          if (sampler_binding->binding != layout_binding->binding || sampler_binding->index != j)
          {
            continue;
          }

          cgpu_isampler* isampler;
          cgpu_sampler sampler = sampler_binding->sampler;
          if (!cgpu_resolve_sampler(sampler, &isampler)) {
            CGPU_RETURN_ERROR_INVALID_HANDLE;
          }

          if (image_info_count >= MAX_DESCRIPTOR_IMAGE_INFOS) {
            CGPU_RETURN_ERROR_HARDCODED_LIMIT_REACHED;
          }

          VkDescriptorImageInfo* image_info = &image_infos[image_info_count++];
          image_info->sampler = isampler->sampler;
          image_info->imageView = VK_NULL_HANDLE;
          image_info->imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

          if (j == 0)
          {
            write_descriptor_set->pImageInfo = image_info;
          }

          slotHandled = true;
          break;
        }
      }

      if (!slotHandled)
      {
        CGPU_RETURN_ERROR("resource binding mismatch");
      }
    }
  }

  idevice->table.vkUpdateDescriptorSets(
    idevice->logical_device,
    write_descriptor_set_count,
    write_descriptor_sets,
    0,
    NULL
  );

  return true;
}

bool cgpu_cmd_copy_buffer(cgpu_command_buffer command_buffer,
                          cgpu_buffer source_buffer,
                          uint64_t source_offset,
                          cgpu_buffer destination_buffer,
                          uint64_t destination_offset,
                          uint64_t size)
{
  cgpu_icommand_buffer* icommand_buffer;
  if (!cgpu_resolve_command_buffer(command_buffer, &icommand_buffer)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(icommand_buffer->device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_ibuffer* isource_buffer;
  if (!cgpu_resolve_buffer(source_buffer, &isource_buffer)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_ibuffer* idestination_buffer;
  if (!cgpu_resolve_buffer(destination_buffer, &idestination_buffer)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  VkBufferCopy region;
  region.srcOffset = source_offset;
  region.dstOffset = destination_offset;
  region.size = (size == CGPU_WHOLE_SIZE) ? isource_buffer->size : size;

  idevice->table.vkCmdCopyBuffer(
    icommand_buffer->command_buffer,
    isource_buffer->buffer,
    idestination_buffer->buffer,
    1,
    &region
  );

  return true;
}

bool cgpu_cmd_copy_buffer_to_image(cgpu_command_buffer command_buffer,
                                   cgpu_buffer buffer,
                                   uint64_t buffer_offset,
                                   cgpu_image image)
{
  cgpu_icommand_buffer* icommand_buffer;
  if (!cgpu_resolve_command_buffer(command_buffer, &icommand_buffer)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(icommand_buffer->device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_ibuffer* ibuffer;
  if (!cgpu_resolve_buffer(buffer, &ibuffer)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_iimage* iimage;
  if (!cgpu_resolve_image(image, &iimage)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  if (iimage->layout != VK_IMAGE_LAYOUT_GENERAL)
  {
    VkAccessFlags access_mask = iimage->access_mask | VK_ACCESS_MEMORY_WRITE_BIT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;

    VkImageMemoryBarrier barrier;
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.pNext = NULL;
    barrier.srcAccessMask = iimage->access_mask;
    barrier.dstAccessMask = access_mask;
    barrier.oldLayout = iimage->layout;
    barrier.newLayout = layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = iimage->image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    idevice->table.vkCmdPipelineBarrier(
      icommand_buffer->command_buffer,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      0,
      0,
      NULL,
      0,
      NULL,
      1,
      &barrier
    );

    iimage->layout = layout;
    iimage->access_mask = access_mask;
  }

  VkImageSubresourceLayers layers;
  layers.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  layers.mipLevel = 0;
  layers.baseArrayLayer = 0;
  layers.layerCount = 1;

  VkOffset3D offset;
  offset.x = 0;
  offset.y = 0;
  offset.z = 0;

  VkExtent3D extent;
  extent.width = iimage->width;
  extent.height = iimage->height;
  extent.depth = iimage->depth;

  VkBufferImageCopy region;
  region.bufferOffset = buffer_offset;
  region.bufferRowLength = 0;
  region.bufferImageHeight = 0;
  region.imageSubresource = layers;
  region.imageOffset = offset;
  region.imageExtent = extent;

  idevice->table.vkCmdCopyBufferToImage(
    icommand_buffer->command_buffer,
    ibuffer->buffer,
    iimage->image,
    iimage->layout,
    1,
    &region
  );

  return true;
}

bool cgpu_cmd_push_constants(cgpu_command_buffer command_buffer,
                             cgpu_pipeline pipeline,
                             const void* p_data)
{
  cgpu_icommand_buffer* icommand_buffer;
  if (!cgpu_resolve_command_buffer(command_buffer, &icommand_buffer)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(icommand_buffer->device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_ipipeline* ipipeline;
  if (!cgpu_resolve_pipeline(pipeline, &ipipeline)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_ishader* ishader;
  if (!cgpu_resolve_shader(ipipeline->shader, &ishader)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  idevice->table.vkCmdPushConstants(
    icommand_buffer->command_buffer,
    ipipeline->layout,
    VK_SHADER_STAGE_COMPUTE_BIT,
    0,
    ishader->reflection.push_constants_size,
    p_data
  );
  return true;
}

bool cgpu_cmd_dispatch(cgpu_command_buffer command_buffer,
                       uint32_t dim_x,
                       uint32_t dim_y,
                       uint32_t dim_z)
{
  cgpu_icommand_buffer* icommand_buffer;
  if (!cgpu_resolve_command_buffer(command_buffer, &icommand_buffer)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(icommand_buffer->device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  idevice->table.vkCmdDispatch(
    icommand_buffer->command_buffer,
    dim_x,
    dim_y,
    dim_z
  );
  return true;
}

bool cgpu_cmd_pipeline_barrier(cgpu_command_buffer command_buffer,
                               uint32_t barrier_count,
                               const cgpu_memory_barrier* p_barriers,
                               uint32_t buffer_barrier_count,
                               const cgpu_buffer_memory_barrier* p_buffer_barriers,
                               uint32_t image_barrier_count,
                               const cgpu_image_memory_barrier* p_image_barriers)
{
  cgpu_icommand_buffer* icommand_buffer;
  if (!cgpu_resolve_command_buffer(command_buffer, &icommand_buffer)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(icommand_buffer->device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  if (barrier_count >= MAX_MEMORY_BARRIERS ||
      buffer_barrier_count >= MAX_BUFFER_MEMORY_BARRIERS ||
      image_barrier_count >= MAX_IMAGE_MEMORY_BARRIERS)
  {
    CGPU_RETURN_ERROR_HARDCODED_LIMIT_REACHED;
  }

  VkMemoryBarrier vk_memory_barriers[MAX_MEMORY_BARRIERS];

  for (uint32_t i = 0; i < barrier_count; ++i)
  {
    const cgpu_memory_barrier* b_cgpu = &p_barriers[i];
    VkMemoryBarrier* b_vk = &vk_memory_barriers[i];
    b_vk->sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b_vk->pNext = NULL;
    b_vk->srcAccessMask = cgpu_translate_access_flags(b_cgpu->src_access_flags);
    b_vk->dstAccessMask = cgpu_translate_access_flags(b_cgpu->dst_access_flags);
  }

  VkBufferMemoryBarrier vk_buffer_memory_barriers[MAX_BUFFER_MEMORY_BARRIERS];
  VkImageMemoryBarrier vk_image_memory_barriers[MAX_IMAGE_MEMORY_BARRIERS];

  for (uint32_t i = 0; i < buffer_barrier_count; ++i)
  {
    const cgpu_buffer_memory_barrier* b_cgpu = &p_buffer_barriers[i];

    cgpu_ibuffer* ibuffer;
    if (!cgpu_resolve_buffer(b_cgpu->buffer, &ibuffer)) {
      CGPU_RETURN_ERROR_INVALID_HANDLE;
    }

    VkBufferMemoryBarrier* b_vk = &vk_buffer_memory_barriers[i];
    b_vk->sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b_vk->pNext = NULL;
    b_vk->srcAccessMask = cgpu_translate_access_flags(b_cgpu->src_access_flags);
    b_vk->dstAccessMask = cgpu_translate_access_flags(b_cgpu->dst_access_flags);
    b_vk->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b_vk->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b_vk->buffer = ibuffer->buffer;
    b_vk->offset = b_cgpu->offset;
    b_vk->size = (b_cgpu->size == CGPU_WHOLE_SIZE) ? VK_WHOLE_SIZE : b_cgpu->size;
  }

  for (uint32_t i = 0; i < image_barrier_count; ++i)
  {
    const cgpu_image_memory_barrier* b_cgpu = &p_image_barriers[i];

    cgpu_iimage* iimage;
    if (!cgpu_resolve_image(b_cgpu->image, &iimage)) {
      CGPU_RETURN_ERROR_INVALID_HANDLE;
    }

    VkAccessFlags access_mask = cgpu_translate_access_flags(b_cgpu->access_mask);

    VkImageMemoryBarrier* b_vk = &vk_image_memory_barriers[i];
    b_vk->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b_vk->pNext = NULL;
    b_vk->srcAccessMask = iimage->access_mask;
    b_vk->dstAccessMask = access_mask;
    b_vk->oldLayout = iimage->layout;
    b_vk->newLayout = iimage->layout;
    b_vk->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b_vk->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b_vk->image = iimage->image;
    b_vk->subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b_vk->subresourceRange.baseMipLevel = 0;
    b_vk->subresourceRange.levelCount = 1;
    b_vk->subresourceRange.baseArrayLayer = 0;
    b_vk->subresourceRange.layerCount = 1;

    iimage->access_mask = access_mask;
  }

  idevice->table.vkCmdPipelineBarrier(
    icommand_buffer->command_buffer,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
    0,
    barrier_count,
    vk_memory_barriers,
    buffer_barrier_count,
    vk_buffer_memory_barriers,
    image_barrier_count,
    vk_image_memory_barriers
  );

  return true;
}

bool cgpu_cmd_reset_timestamps(cgpu_command_buffer command_buffer,
                               uint32_t offset,
                               uint32_t count)
{
  cgpu_icommand_buffer* icommand_buffer;
  if (!cgpu_resolve_command_buffer(command_buffer, &icommand_buffer)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(icommand_buffer->device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  idevice->table.vkCmdResetQueryPool(
    icommand_buffer->command_buffer,
    idevice->timestamp_pool,
    offset,
    count
  );

  return true;
}

bool cgpu_cmd_write_timestamp(cgpu_command_buffer command_buffer,
                              uint32_t timestamp_index)
{
  cgpu_icommand_buffer* icommand_buffer;
  if (!cgpu_resolve_command_buffer(command_buffer, &icommand_buffer)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(icommand_buffer->device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  idevice->table.vkCmdWriteTimestamp(
    icommand_buffer->command_buffer,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    idevice->timestamp_pool,
    timestamp_index
  );

  return true;
}

bool cgpu_cmd_copy_timestamps(cgpu_command_buffer command_buffer,
                              cgpu_buffer buffer,
                              uint32_t offset,
                              uint32_t count,
                              bool wait_until_available)
{
  uint32_t last_index = offset + count;
  if (last_index >= CGPU_MAX_TIMESTAMP_QUERIES) {
    CGPU_RETURN_ERROR_HARDCODED_LIMIT_REACHED;
  }

  cgpu_icommand_buffer* icommand_buffer;
  if (!cgpu_resolve_command_buffer(command_buffer, &icommand_buffer)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(icommand_buffer->device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_ibuffer* ibuffer;
  if (!cgpu_resolve_buffer(buffer, &ibuffer)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  VkQueryResultFlags wait_flag = wait_until_available ? VK_QUERY_RESULT_WAIT_BIT : VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;

  idevice->table.vkCmdCopyQueryPoolResults(
    icommand_buffer->command_buffer,
    idevice->timestamp_pool,
    offset,
    count,
    ibuffer->buffer,
    0,
    sizeof(uint64_t),
    VK_QUERY_RESULT_64_BIT | wait_flag
  );

  return true;
}

bool cgpu_end_command_buffer(cgpu_command_buffer command_buffer)
{
  cgpu_icommand_buffer* icommand_buffer;
  if (!cgpu_resolve_command_buffer(command_buffer, &icommand_buffer)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(icommand_buffer->device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  idevice->table.vkEndCommandBuffer(icommand_buffer->command_buffer);
  return true;
}

bool cgpu_create_fence(cgpu_device device,
                       cgpu_fence* p_fence)
{
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  p_fence->handle = resource_store_create_handle(&ifence_store);

  cgpu_ifence* ifence;
  if (!cgpu_resolve_fence(*p_fence, &ifence)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  VkFenceCreateInfo fence_create_info;
  fence_create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fence_create_info.pNext = NULL;
  fence_create_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

  VkResult result = idevice->table.vkCreateFence(
    idevice->logical_device,
    &fence_create_info,
    NULL,
    &ifence->fence
  );

  if (result != VK_SUCCESS) {
    resource_store_free_handle(&ifence_store, p_fence->handle);
    CGPU_RETURN_ERROR("failed to create fence");
  }
  return true;
}

bool cgpu_destroy_fence(cgpu_device device,
                        cgpu_fence fence)
{
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_ifence* ifence;
  if (!cgpu_resolve_fence(fence, &ifence)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  idevice->table.vkDestroyFence(
    idevice->logical_device,
    ifence->fence,
    NULL
  );
  resource_store_free_handle(&ifence_store, fence.handle);
  return true;
}

bool cgpu_reset_fence(cgpu_device device,
                      cgpu_fence fence)
{
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_ifence* ifence;
  if (!cgpu_resolve_fence(fence, &ifence)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  VkResult result = idevice->table.vkResetFences(
    idevice->logical_device,
    1,
    &ifence->fence
  );
  if (result != VK_SUCCESS) {
    CGPU_RETURN_ERROR("failed to reset fence");
  }
  return true;
}

bool cgpu_wait_for_fence(cgpu_device device, cgpu_fence fence)
{
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_ifence* ifence;
  if (!cgpu_resolve_fence(fence, &ifence)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  VkResult result = idevice->table.vkWaitForFences(
    idevice->logical_device,
    1,
    &ifence->fence,
    VK_TRUE,
    UINT64_MAX
  );
  if (result != VK_SUCCESS) {
    CGPU_RETURN_ERROR("failed to wait for fence");
  }
  return true;
}

bool cgpu_submit_command_buffer(cgpu_device device,
                                cgpu_command_buffer command_buffer,
                                cgpu_fence fence)
{
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_icommand_buffer* icommand_buffer;
  if (!cgpu_resolve_command_buffer(command_buffer, &icommand_buffer)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_ifence* ifence;
  if (!cgpu_resolve_fence(fence, &ifence)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  VkSubmitInfo submit_info;
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.pNext = NULL;
  submit_info.waitSemaphoreCount = 0;
  submit_info.pWaitSemaphores = NULL;
  submit_info.pWaitDstStageMask = NULL;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &icommand_buffer->command_buffer;
  submit_info.signalSemaphoreCount = 0;
  submit_info.pSignalSemaphores = NULL;

  VkResult result = idevice->table.vkQueueSubmit(
    idevice->compute_queue,
    1,
    &submit_info,
    ifence->fence
  );

  if (result != VK_SUCCESS) {
    CGPU_RETURN_ERROR("failed to submit command buffer");
  }
  return true;
}

bool cgpu_flush_mapped_memory(cgpu_device device,
                              cgpu_buffer buffer,
                              uint64_t offset,
                              uint64_t size)
{
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_ibuffer* ibuffer;
  if (!cgpu_resolve_buffer(buffer, &ibuffer)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  VkResult result = vmaFlushAllocation(
    idevice->allocator,
    ibuffer->allocation,
    offset,
    (size == CGPU_WHOLE_SIZE) ? ibuffer->size : size
  );

  if (result != VK_SUCCESS) {
    CGPU_RETURN_ERROR("failed to flush mapped memory");
  }
  return true;
}

bool cgpu_invalidate_mapped_memory(cgpu_device device,
                                   cgpu_buffer buffer,
                                   uint64_t offset,
                                   uint64_t size)
{
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  cgpu_ibuffer* ibuffer;
  if (!cgpu_resolve_buffer(buffer, &ibuffer)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }

  VkResult result = vmaInvalidateAllocation(
    idevice->allocator,
    ibuffer->allocation,
    offset,
    (size == CGPU_WHOLE_SIZE) ? ibuffer->size : size
  );

  if (result != VK_SUCCESS) {
    CGPU_RETURN_ERROR("failed to invalidate mapped memory");
  }
  return true;
}

bool cgpu_get_physical_device_features(cgpu_device device,
                                       cgpu_physical_device_features* p_features)
{
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  memcpy(p_features, &idevice->features, sizeof(cgpu_physical_device_features));
  return true;
}

bool cgpu_get_physical_device_limits(cgpu_device device,
                                     cgpu_physical_device_limits* p_limits)
{
  cgpu_idevice* idevice;
  if (!cgpu_resolve_device(device, &idevice)) {
    CGPU_RETURN_ERROR_INVALID_HANDLE;
  }
  memcpy(p_limits, &idevice->limits, sizeof(cgpu_physical_device_limits));
  return true;
}
