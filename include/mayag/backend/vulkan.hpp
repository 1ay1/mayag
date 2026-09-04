#pragma once
// mayag::backend::VulkanDevice — the cross-platform GPU backend
//
// Metal covers Apple. This covers everything else: Linux, Windows and Android
// all speak Vulkan, so one backend makes the GPU the default on every platform
// mayag targets rather than only on the Mac.
//
// It is built the same way as mayag's other system integrations, and for the
// same reason: **it links nothing**. libvulkan.so.1 / vulkan-1.dll is resolved
// with dlopen at startup, and the slice of the Vulkan ABI mayag uses — a few
// dozen structs and ~40 entry points — is transcribed by hand below rather
// than pulled from vulkan.h. The consequences are the ones that matter:
//
//   * no find_package(Vulkan), no SDK on the build machine, no header/loader
//     version skew;
//   * a binary built with the Vulkan backend still RUNS on a box with no
//     Vulkan — the loader fails, `init` returns false, and the runtime falls
//     back to the software rasteriser instead of dying at startup;
//   * the shader is SPIR-V embedded at build time (render/spirv_generated.hpp,
//     produced by tools/embed_spirv.py from the same GLSL the docs show), so
//     there is no runtime GLSL and no shaderc dependency either.
//
// The pipeline is exactly mayag's model: ONE instanced draw per batch, no
// vertex buffer (the vertex shader builds each quad from gl_VertexIndex), the
// instance buffer is the `Instance` array uploaded verbatim, and the fragment
// shader is the shared SDF kernel. A frame is 1-3 draws, text included.

#include "../render/draw_list.hpp"
#include "../render/spirv_generated.hpp"
#include "software.hpp"   // Framebuffer, for the offscreen readback path

#include <cstdint>
#include <cstring>
#include <vector>

#if defined(__linux__) || defined(__FreeBSD__) || defined(_WIN32)

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace mayag::backend::vk {

// ════════════════════════════════════════════════════════════════════════
// The slice of the Vulkan ABI mayag uses, redeclared
//
// Vulkan's ABI is stable and these are the exact definitions from vulkan_core.h
// for the handful of types and calls the backend touches. Redeclaring them is
// what lets this compile with no Vulkan SDK installed.
// ════════════════════════════════════════════════════════════════════════

using Flags     = std::uint32_t;
using Bool32    = std::uint32_t;
using DeviceSize = std::uint64_t;

#if defined(_WIN32) || defined(__LP64__) || defined(_LP64)
#  define MG_VK_HANDLE(name) using name = struct name##_T*;
#else
// On 32-bit, non-dispatchable handles are uint64_t. mayag targets 64-bit.
#  define MG_VK_HANDLE(name) using name = struct name##_T*;
#endif

MG_VK_HANDLE(VkInstance)
MG_VK_HANDLE(VkPhysicalDevice)
MG_VK_HANDLE(VkDevice)
MG_VK_HANDLE(VkQueue)
MG_VK_HANDLE(VkCommandPool)
MG_VK_HANDLE(VkCommandBuffer)
MG_VK_HANDLE(VkBuffer)
MG_VK_HANDLE(VkDeviceMemory)
MG_VK_HANDLE(VkImage)
MG_VK_HANDLE(VkImageView)
MG_VK_HANDLE(VkSampler)
MG_VK_HANDLE(VkShaderModule)
MG_VK_HANDLE(VkPipeline)
MG_VK_HANDLE(VkPipelineLayout)
MG_VK_HANDLE(VkPipelineCache)
MG_VK_HANDLE(VkRenderPass)
MG_VK_HANDLE(VkFramebuffer)
MG_VK_HANDLE(VkDescriptorSetLayout)
MG_VK_HANDLE(VkDescriptorPool)
MG_VK_HANDLE(VkDescriptorSet)
MG_VK_HANDLE(VkFence)
MG_VK_HANDLE(VkSemaphore)
MG_VK_HANDLE(VkSurfaceKHR)
MG_VK_HANDLE(VkSwapchainKHR)

using VkResult = std::int32_t;
inline constexpr VkResult VK_SUCCESS = 0;
inline constexpr VkResult VK_NOT_READY = 1;
inline constexpr VkResult VK_SUBOPTIMAL_KHR = 1000001003;
inline constexpr VkResult VK_ERROR_OUT_OF_DATE_KHR = -1000001004;

// enums (only the values used)
enum { VK_STRUCTURE_TYPE_APPLICATION_INFO = 0,
       VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1,
       VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2,
       VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3,
       VK_STRUCTURE_TYPE_SUBMIT_INFO = 4,
       VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5,
       VK_STRUCTURE_TYPE_FENCE_CREATE_INFO = 8,
       VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO = 9,
       VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO = 12,
       VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO = 14,
       VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO = 15,
       VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO = 16,
       VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO = 18,
       VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO = 19,
       VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO = 20,
       VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO = 22,
       VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO = 23,
       VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO = 24,
       VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO = 26,
       VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO = 27,
       VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO = 28,
       VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO = 30,
       VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO = 31,
       VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO = 32,
       VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO = 33,
       VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO = 34,
       VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET = 35,
       VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET = 36,
       VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO = 38,
       VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO = 39,
       VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 40,
       VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO = 42,
       VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO = 43,
       VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER = 44,
       VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER = 45,
       VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO = 37,
       VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR = 1000001000,
       VK_STRUCTURE_TYPE_PRESENT_INFO_KHR = 1000001001,
       VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR = 1000006000,
       VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR = 1000009000 };

enum { VK_FORMAT_R8G8B8A8_UNORM = 37,
       VK_FORMAT_B8G8R8A8_UNORM = 44,
       VK_FORMAT_B8G8R8A8_SRGB = 50,
       VK_FORMAT_R8G8B8A8_SRGB = 43,
       VK_FORMAT_R8_UNORM = 9,
       VK_FORMAT_R32G32B32A32_SFLOAT = 109,
       VK_FORMAT_R32G32B32A32_UINT = 107 };

enum { VK_IMAGE_LAYOUT_UNDEFINED = 0,
       VK_IMAGE_LAYOUT_GENERAL = 1,
       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL = 2,
       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL = 5,
       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL = 6,
       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL = 7,
       VK_IMAGE_LAYOUT_PRESENT_SRC_KHR = 1000001002 };

enum { VK_QUEUE_GRAPHICS_BIT = 1 };
enum { VK_SHADER_STAGE_VERTEX_BIT = 1, VK_SHADER_STAGE_FRAGMENT_BIT = 16 };
enum { VK_BUFFER_USAGE_TRANSFER_SRC_BIT = 1, VK_BUFFER_USAGE_TRANSFER_DST_BIT = 2,
       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT = 0x80 };
enum { VK_IMAGE_USAGE_TRANSFER_SRC_BIT = 1, VK_IMAGE_USAGE_TRANSFER_DST_BIT = 2,
       VK_IMAGE_USAGE_SAMPLED_BIT = 4, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT = 16 };
enum { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT = 1, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 2,
       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 4 };
enum { VK_SHARING_MODE_EXCLUSIVE = 0 };
enum { VK_IMAGE_TYPE_2D = 1, VK_IMAGE_VIEW_TYPE_2D = 1 };
enum { VK_IMAGE_TILING_OPTIMAL = 0, VK_IMAGE_TILING_LINEAR = 1 };
enum { VK_SAMPLE_COUNT_1_BIT = 1 };
enum { VK_IMAGE_ASPECT_COLOR_BIT = 1 };
enum { VK_COMMAND_BUFFER_LEVEL_PRIMARY = 0 };
enum { VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT = 2 };
enum { VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT = 1 };
enum { VK_SUBPASS_CONTENTS_INLINE = 0 };
enum { VK_ATTACHMENT_LOAD_OP_CLEAR = 1, VK_ATTACHMENT_LOAD_OP_DONT_CARE = 2,
       VK_ATTACHMENT_STORE_OP_STORE = 0, VK_ATTACHMENT_STORE_OP_DONT_CARE = 1 };
enum { VK_PIPELINE_BIND_POINT_GRAPHICS = 0 };
enum { VK_VERTEX_INPUT_RATE_VERTEX = 0, VK_VERTEX_INPUT_RATE_INSTANCE = 1 };
enum { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP = 4 };
enum { VK_POLYGON_MODE_FILL = 0, VK_CULL_MODE_NONE = 0, VK_FRONT_FACE_COUNTER_CLOCKWISE = 0 };
enum { VK_BLEND_FACTOR_ONE = 1, VK_BLEND_FACTOR_ZERO = 0,
       VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA = 7, VK_BLEND_OP_ADD = 0 };
enum { VK_COLOR_COMPONENT_R_BIT = 1, VK_COLOR_COMPONENT_G_BIT = 2,
       VK_COLOR_COMPONENT_B_BIT = 4, VK_COLOR_COMPONENT_A_BIT = 8 };
enum { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER = 1 };
enum { VK_FILTER_LINEAR = 1, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE = 2,
       VK_SAMPLER_MIPMAP_MODE_LINEAR = 1 };
enum { VK_FENCE_CREATE_SIGNALED_BIT = 1 };
enum { VK_DYNAMIC_STATE_VIEWPORT = 0, VK_DYNAMIC_STATE_SCISSOR = 1 };
enum { VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT = 1,
       VK_PIPELINE_STAGE_TRANSFER_BIT = 0x1000,
       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT = 0x400,
       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT = 0x80,
       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT = 0x2000 };
enum { VK_ACCESS_TRANSFER_WRITE_BIT = 0x1000, VK_ACCESS_TRANSFER_READ_BIT = 0x800,
       VK_ACCESS_SHADER_READ_BIT = 0x20, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT = 0x100,
       VK_ACCESS_MEMORY_READ_BIT = 0x8000 };
enum { VK_PRESENT_MODE_FIFO_KHR = 2, VK_PRESENT_MODE_MAILBOX_KHR = 1,
       VK_PRESENT_MODE_IMMEDIATE_KHR = 0 };
enum { VK_COLOR_SPACE_SRGB_NONLINEAR_KHR = 0 };
enum { VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR = 1 };
enum { VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR = 1 };
enum { VK_SUBPASS_EXTERNAL = ~0u };
enum { VK_DEPENDENCY_BY_REGION_BIT = 1 };
enum { VK_SHADER_STAGE_ALL = 0x7FFFFFFF };
enum { VK_WHOLE_SIZE = ~0ull };
enum { VK_QUEUE_FAMILY_IGNORED = ~0u };

struct VkExtent2D { std::uint32_t width, height; };
struct VkExtent3D { std::uint32_t width, height, depth; };
struct VkOffset2D { std::int32_t x, y; };
struct VkRect2D { VkOffset2D offset; VkExtent2D extent; };
struct VkViewport { float x, y, width, height, minDepth, maxDepth; };

struct VkApplicationInfo {
    int sType; const void* pNext; const char* pApplicationName; std::uint32_t applicationVersion;
    const char* pEngineName; std::uint32_t engineVersion; std::uint32_t apiVersion;
};
struct VkInstanceCreateInfo {
    int sType; const void* pNext; Flags flags; const VkApplicationInfo* pApplicationInfo;
    std::uint32_t enabledLayerCount; const char* const* ppEnabledLayerNames;
    std::uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames;
};
struct VkQueueFamilyProperties {
    Flags queueFlags; std::uint32_t queueCount; std::uint32_t timestampValidBits;
    VkExtent3D minImageTransferGranularity;
};
struct VkDeviceQueueCreateInfo {
    int sType; const void* pNext; Flags flags; std::uint32_t queueFamilyIndex;
    std::uint32_t queueCount; const float* pQueuePriorities;
};
struct VkDeviceCreateInfo {
    int sType; const void* pNext; Flags flags; std::uint32_t queueCreateInfoCount;
    const VkDeviceQueueCreateInfo* pQueueCreateInfos; std::uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames; std::uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames; const void* pEnabledFeatures;
};
struct VkMemoryType { Flags propertyFlags; std::uint32_t heapIndex; };
struct VkMemoryHeap { DeviceSize size; Flags flags; };
struct VkPhysicalDeviceMemoryProperties {
    std::uint32_t memoryTypeCount; VkMemoryType memoryTypes[32];
    std::uint32_t memoryHeapCount; VkMemoryHeap memoryHeaps[16];
};
struct VkMemoryRequirements { DeviceSize size, alignment; std::uint32_t memoryTypeBits; };
struct VkMemoryAllocateInfo {
    int sType; const void* pNext; DeviceSize allocationSize; std::uint32_t memoryTypeIndex;
};
struct VkBufferCreateInfo {
    int sType; const void* pNext; Flags flags; DeviceSize size; Flags usage;
    int sharingMode; std::uint32_t queueFamilyIndexCount; const std::uint32_t* pQueueFamilyIndices;
};
struct VkComponentMapping { int r, g, b, a; };
struct VkImageSubresourceRange {
    Flags aspectMask; std::uint32_t baseMipLevel, levelCount, baseArrayLayer, layerCount;
};
struct VkImageCreateInfo {
    int sType; const void* pNext; Flags flags; int imageType; int format; VkExtent3D extent;
    std::uint32_t mipLevels, arrayLayers; int samples; int tiling; Flags usage; int sharingMode;
    std::uint32_t queueFamilyIndexCount; const std::uint32_t* pQueueFamilyIndices; int initialLayout;
};
struct VkImageViewCreateInfo {
    int sType; const void* pNext; Flags flags; VkImage image; int viewType; int format;
    VkComponentMapping components; VkImageSubresourceRange subresourceRange;
};
struct VkSamplerCreateInfo {
    int sType; const void* pNext; Flags flags; int magFilter, minFilter, mipmapMode;
    int addressModeU, addressModeV, addressModeW; float mipLodBias; Bool32 anisotropyEnable;
    float maxAnisotropy; Bool32 compareEnable; int compareOp; float minLod, maxLod;
    int borderColor; Bool32 unnormalizedCoordinates;
};
struct VkShaderModuleCreateInfo {
    int sType; const void* pNext; Flags flags; std::size_t codeSize; const std::uint32_t* pCode;
};
struct VkSpecializationInfo;
struct VkPipelineShaderStageCreateInfo {
    int sType; const void* pNext; Flags flags; Flags stage; VkShaderModule module;
    const char* pName; const VkSpecializationInfo* pSpecializationInfo;
};
struct VkVertexInputBindingDescription { std::uint32_t binding, stride; int inputRate; };
struct VkVertexInputAttributeDescription {
    std::uint32_t location, binding; int format; std::uint32_t offset;
};
struct VkPipelineVertexInputStateCreateInfo {
    int sType; const void* pNext; Flags flags; std::uint32_t vertexBindingDescriptionCount;
    const VkVertexInputBindingDescription* pVertexBindingDescriptions;
    std::uint32_t vertexAttributeDescriptionCount;
    const VkVertexInputAttributeDescription* pVertexAttributeDescriptions;
};
struct VkPipelineInputAssemblyStateCreateInfo {
    int sType; const void* pNext; Flags flags; int topology; Bool32 primitiveRestartEnable;
};
struct VkPipelineViewportStateCreateInfo {
    int sType; const void* pNext; Flags flags; std::uint32_t viewportCount;
    const VkViewport* pViewports; std::uint32_t scissorCount; const VkRect2D* pScissors;
};
struct VkPipelineRasterizationStateCreateInfo {
    int sType; const void* pNext; Flags flags; Bool32 depthClampEnable, rasterizerDiscardEnable;
    int polygonMode; Flags cullMode; int frontFace; Bool32 depthBiasEnable;
    float depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor, lineWidth;
};
struct VkPipelineMultisampleStateCreateInfo {
    int sType; const void* pNext; Flags flags; int rasterizationSamples; Bool32 sampleShadingEnable;
    float minSampleShading; const std::uint32_t* pSampleMask; Bool32 alphaToCoverageEnable, alphaToOneEnable;
};
struct VkPipelineColorBlendAttachmentState {
    Bool32 blendEnable; int srcColorBlendFactor, dstColorBlendFactor, colorBlendOp;
    int srcAlphaBlendFactor, dstAlphaBlendFactor, alphaBlendOp; Flags colorWriteMask;
};
struct VkPipelineColorBlendStateCreateInfo {
    int sType; const void* pNext; Flags flags; Bool32 logicOpEnable; int logicOp;
    std::uint32_t attachmentCount; const VkPipelineColorBlendAttachmentState* pAttachments;
    float blendConstants[4];
};
struct VkPipelineDynamicStateCreateInfo {
    int sType; const void* pNext; Flags flags; std::uint32_t dynamicStateCount; const int* pDynamicStates;
};
struct VkPushConstantRange { Flags stageFlags; std::uint32_t offset, size; };
struct VkPipelineLayoutCreateInfo {
    int sType; const void* pNext; Flags flags; std::uint32_t setLayoutCount;
    const VkDescriptorSetLayout* pSetLayouts; std::uint32_t pushConstantRangeCount;
    const VkPushConstantRange* pPushConstantRanges;
};
struct VkAttachmentDescription {
    Flags flags; int format, samples, loadOp, storeOp, stencilLoadOp, stencilStoreOp,
    initialLayout, finalLayout;
};
struct VkAttachmentReference { std::uint32_t attachment; int layout; };
struct VkSubpassDescription {
    Flags flags; int pipelineBindPoint; std::uint32_t inputAttachmentCount;
    const VkAttachmentReference* pInputAttachments; std::uint32_t colorAttachmentCount;
    const VkAttachmentReference* pColorAttachments; const VkAttachmentReference* pResolveAttachments;
    const VkAttachmentReference* pDepthStencilAttachment; std::uint32_t preserveAttachmentCount;
    const std::uint32_t* pPreserveAttachments;
};
struct VkSubpassDependency {
    std::uint32_t srcSubpass, dstSubpass; Flags srcStageMask, dstStageMask,
    srcAccessMask, dstAccessMask, dependencyFlags;
};
struct VkRenderPassCreateInfo {
    int sType; const void* pNext; Flags flags; std::uint32_t attachmentCount;
    const VkAttachmentDescription* pAttachments; std::uint32_t subpassCount;
    const VkSubpassDescription* pSubpasses; std::uint32_t dependencyCount;
    const VkSubpassDependency* pDependencies;
};
struct VkGraphicsPipelineCreateInfo {
    int sType; const void* pNext; Flags flags; std::uint32_t stageCount;
    const VkPipelineShaderStageCreateInfo* pStages;
    const VkPipelineVertexInputStateCreateInfo* pVertexInputState;
    const VkPipelineInputAssemblyStateCreateInfo* pInputAssemblyState;
    const void* pTessellationState; const VkPipelineViewportStateCreateInfo* pViewportState;
    const VkPipelineRasterizationStateCreateInfo* pRasterizationState;
    const VkPipelineMultisampleStateCreateInfo* pMultisampleState;
    const void* pDepthStencilState; const VkPipelineColorBlendStateCreateInfo* pColorBlendState;
    const VkPipelineDynamicStateCreateInfo* pDynamicState; VkPipelineLayout layout;
    VkRenderPass renderPass; std::uint32_t subpass; VkPipeline basePipelineHandle;
    std::int32_t basePipelineIndex;
};
struct VkFramebufferCreateInfo {
    int sType; const void* pNext; Flags flags; VkRenderPass renderPass;
    std::uint32_t attachmentCount; const VkImageView* pAttachments;
    std::uint32_t width, height, layers;
};
struct VkDescriptorSetLayoutBinding {
    std::uint32_t binding; int descriptorType; std::uint32_t descriptorCount;
    Flags stageFlags; const VkSampler* pImmutableSamplers;
};
struct VkDescriptorSetLayoutCreateInfo {
    int sType; const void* pNext; Flags flags; std::uint32_t bindingCount;
    const VkDescriptorSetLayoutBinding* pBindings;
};
struct VkDescriptorPoolSize { int type; std::uint32_t descriptorCount; };
struct VkDescriptorPoolCreateInfo {
    int sType; const void* pNext; Flags flags; std::uint32_t maxSets;
    std::uint32_t poolSizeCount; const VkDescriptorPoolSize* pPoolSizes;
};
struct VkDescriptorSetAllocateInfo {
    int sType; const void* pNext; VkDescriptorPool descriptorPool;
    std::uint32_t descriptorSetCount; const VkDescriptorSetLayout* pSetLayouts;
};
struct VkDescriptorImageInfo { VkSampler sampler; VkImageView imageView; int imageLayout; };
struct VkWriteDescriptorSet {
    int sType; const void* pNext; VkDescriptorSet dstSet; std::uint32_t dstBinding, dstArrayElement,
    descriptorCount; int descriptorType; const VkDescriptorImageInfo* pImageInfo;
    const void* pBufferInfo; const void* pTexelBufferView;
};
struct VkCommandPoolCreateInfo {
    int sType; const void* pNext; Flags flags; std::uint32_t queueFamilyIndex;
};
struct VkCommandBufferAllocateInfo {
    int sType; const void* pNext; VkCommandPool commandPool; int level; std::uint32_t commandBufferCount;
};
struct VkCommandBufferBeginInfo {
    int sType; const void* pNext; Flags flags; const void* pInheritanceInfo;
};
union VkClearColorValue { float float32[4]; std::uint32_t uint32[4]; std::int32_t int32[4]; };
struct VkClearValue { VkClearColorValue color; };
struct VkRenderPassBeginInfo {
    int sType; const void* pNext; VkRenderPass renderPass; VkFramebuffer framebuffer;
    VkRect2D renderArea; std::uint32_t clearValueCount; const VkClearValue* pClearValues;
};
struct VkImageSubresourceLayers {
    Flags aspectMask; std::uint32_t mipLevel, baseArrayLayer, layerCount;
};
struct VkBufferImageCopy {
    DeviceSize bufferOffset; std::uint32_t bufferRowLength, bufferImageHeight;
    VkImageSubresourceLayers imageSubresource; VkOffset2D imageOffset; std::int32_t z;
    VkExtent3D imageExtent;
};
struct VkImageMemoryBarrier {
    int sType; const void* pNext; Flags srcAccessMask, dstAccessMask; int oldLayout, newLayout;
    std::uint32_t srcQueueFamilyIndex, dstQueueFamilyIndex; VkImage image;
    VkImageSubresourceRange subresourceRange;
};
struct VkSubmitInfo {
    int sType; const void* pNext; std::uint32_t waitSemaphoreCount; const VkSemaphore* pWaitSemaphores;
    const Flags* pWaitDstStageMask; std::uint32_t commandBufferCount; const VkCommandBuffer* pCommandBuffers;
    std::uint32_t signalSemaphoreCount; const VkSemaphore* pSignalSemaphores;
};
struct VkFenceCreateInfo { int sType; const void* pNext; Flags flags; };
struct VkSemaphoreCreateInfo { int sType; const void* pNext; Flags flags; };

// surface / swapchain
struct VkSurfaceCapabilitiesKHR {
    std::uint32_t minImageCount, maxImageCount; VkExtent2D currentExtent, minImageExtent, maxImageExtent;
    std::uint32_t maxImageArrayLayers; Flags supportedTransforms; int currentTransform;
    Flags supportedCompositeAlpha, supportedUsageFlags;
};
struct VkSurfaceFormatKHR { int format; int colorSpace; };
struct VkSwapchainCreateInfoKHR {
    int sType; const void* pNext; Flags flags; VkSurfaceKHR surface; std::uint32_t minImageCount;
    int imageFormat; int imageColorSpace; VkExtent2D imageExtent; std::uint32_t imageArrayLayers;
    Flags imageUsage; int imageSharingMode; std::uint32_t queueFamilyIndexCount;
    const std::uint32_t* pQueueFamilyIndices; int preTransform; int compositeAlpha; int presentMode;
    Bool32 clipped; VkSwapchainKHR oldSwapchain;
};
struct VkPresentInfoKHR {
    int sType; const void* pNext; std::uint32_t waitSemaphoreCount; const VkSemaphore* pWaitSemaphores;
    std::uint32_t swapchainCount; const VkSwapchainKHR* pSwapchains; const std::uint32_t* pImageIndices;
    VkResult* pResults;
};
struct VkWaylandSurfaceCreateInfoKHR {
    int sType; const void* pNext; Flags flags; void* display; void* surface;
};
#if defined(_WIN32)
struct VkWin32SurfaceCreateInfoKHR {
    int sType; const void* pNext; Flags flags; void* hinstance; void* hwnd;
};
#endif

// ════════════════════════════════════════════════════════════════════════
// The loader
// ════════════════════════════════════════════════════════════════════════

struct Api {
    void* handle = nullptr;
    bool  ok = false;

    // loader / instance bootstrap
    void* (*GetInstanceProcAddr)(VkInstance, const char*) = nullptr;
    VkResult (*CreateInstance)(const VkInstanceCreateInfo*, const void*, VkInstance*) = nullptr;
    void (*DestroyInstance)(VkInstance, const void*) = nullptr;
    VkResult (*EnumeratePhysicalDevices)(VkInstance, std::uint32_t*, VkPhysicalDevice*) = nullptr;
    void (*GetPhysicalDeviceQueueFamilyProperties)(VkPhysicalDevice, std::uint32_t*, VkQueueFamilyProperties*) = nullptr;
    void (*GetPhysicalDeviceMemoryProperties)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*) = nullptr;
    VkResult (*CreateDevice)(VkPhysicalDevice, const VkDeviceCreateInfo*, const void*, VkDevice*) = nullptr;
    void (*DestroyDevice)(VkDevice, const void*) = nullptr;
    void (*GetDeviceQueue)(VkDevice, std::uint32_t, std::uint32_t, VkQueue*) = nullptr;
    void* (*GetDeviceProcAddr)(VkDevice, const char*) = nullptr;

    // device
    VkResult (*CreateCommandPool)(VkDevice, const VkCommandPoolCreateInfo*, const void*, VkCommandPool*) = nullptr;
    void (*DestroyCommandPool)(VkDevice, VkCommandPool, const void*) = nullptr;
    VkResult (*AllocateCommandBuffers)(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*) = nullptr;
    VkResult (*BeginCommandBuffer)(VkCommandBuffer, const VkCommandBufferBeginInfo*) = nullptr;
    VkResult (*EndCommandBuffer)(VkCommandBuffer) = nullptr;
    VkResult (*ResetCommandBuffer)(VkCommandBuffer, Flags) = nullptr;
    VkResult (*QueueSubmit)(VkQueue, std::uint32_t, const VkSubmitInfo*, VkFence) = nullptr;
    VkResult (*QueueWaitIdle)(VkQueue) = nullptr;
    VkResult (*DeviceWaitIdle)(VkDevice) = nullptr;
    VkResult (*CreateBuffer)(VkDevice, const VkBufferCreateInfo*, const void*, VkBuffer*) = nullptr;
    void (*DestroyBuffer)(VkDevice, VkBuffer, const void*) = nullptr;
    void (*GetBufferMemoryRequirements)(VkDevice, VkBuffer, VkMemoryRequirements*) = nullptr;
    VkResult (*AllocateMemory)(VkDevice, const VkMemoryAllocateInfo*, const void*, VkDeviceMemory*) = nullptr;
    void (*FreeMemory)(VkDevice, VkDeviceMemory, const void*) = nullptr;
    VkResult (*BindBufferMemory)(VkDevice, VkBuffer, VkDeviceMemory, DeviceSize) = nullptr;
    VkResult (*MapMemory)(VkDevice, VkDeviceMemory, DeviceSize, DeviceSize, Flags, void**) = nullptr;
    void (*UnmapMemory)(VkDevice, VkDeviceMemory) = nullptr;
    VkResult (*CreateImage)(VkDevice, const VkImageCreateInfo*, const void*, VkImage*) = nullptr;
    void (*DestroyImage)(VkDevice, VkImage, const void*) = nullptr;
    void (*GetImageMemoryRequirements)(VkDevice, VkImage, VkMemoryRequirements*) = nullptr;
    VkResult (*BindImageMemory)(VkDevice, VkImage, VkDeviceMemory, DeviceSize) = nullptr;
    VkResult (*CreateImageView)(VkDevice, const VkImageViewCreateInfo*, const void*, VkImageView*) = nullptr;
    void (*DestroyImageView)(VkDevice, VkImageView, const void*) = nullptr;
    VkResult (*CreateSampler)(VkDevice, const VkSamplerCreateInfo*, const void*, VkSampler*) = nullptr;
    void (*DestroySampler)(VkDevice, VkSampler, const void*) = nullptr;
    VkResult (*CreateShaderModule)(VkDevice, const VkShaderModuleCreateInfo*, const void*, VkShaderModule*) = nullptr;
    void (*DestroyShaderModule)(VkDevice, VkShaderModule, const void*) = nullptr;
    VkResult (*CreatePipelineLayout)(VkDevice, const VkPipelineLayoutCreateInfo*, const void*, VkPipelineLayout*) = nullptr;
    void (*DestroyPipelineLayout)(VkDevice, VkPipelineLayout, const void*) = nullptr;
    VkResult (*CreateRenderPass)(VkDevice, const VkRenderPassCreateInfo*, const void*, VkRenderPass*) = nullptr;
    void (*DestroyRenderPass)(VkDevice, VkRenderPass, const void*) = nullptr;
    VkResult (*CreateGraphicsPipelines)(VkDevice, VkPipelineCache, std::uint32_t, const VkGraphicsPipelineCreateInfo*, const void*, VkPipeline*) = nullptr;
    void (*DestroyPipeline)(VkDevice, VkPipeline, const void*) = nullptr;
    VkResult (*CreateFramebuffer)(VkDevice, const VkFramebufferCreateInfo*, const void*, VkFramebuffer*) = nullptr;
    void (*DestroyFramebuffer)(VkDevice, VkFramebuffer, const void*) = nullptr;
    VkResult (*CreateDescriptorSetLayout)(VkDevice, const VkDescriptorSetLayoutCreateInfo*, const void*, VkDescriptorSetLayout*) = nullptr;
    void (*DestroyDescriptorSetLayout)(VkDevice, VkDescriptorSetLayout, const void*) = nullptr;
    VkResult (*CreateDescriptorPool)(VkDevice, const VkDescriptorPoolCreateInfo*, const void*, VkDescriptorPool*) = nullptr;
    void (*DestroyDescriptorPool)(VkDevice, VkDescriptorPool, const void*) = nullptr;
    VkResult (*AllocateDescriptorSets)(VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet*) = nullptr;
    void (*UpdateDescriptorSets)(VkDevice, std::uint32_t, const VkWriteDescriptorSet*, std::uint32_t, const void*) = nullptr;
    VkResult (*CreateFence)(VkDevice, const VkFenceCreateInfo*, const void*, VkFence*) = nullptr;
    void (*DestroyFence)(VkDevice, VkFence, const void*) = nullptr;
    VkResult (*WaitForFences)(VkDevice, std::uint32_t, const VkFence*, Bool32, std::uint64_t) = nullptr;
    VkResult (*ResetFences)(VkDevice, std::uint32_t, const VkFence*) = nullptr;
    VkResult (*CreateSemaphore)(VkDevice, const VkSemaphoreCreateInfo*, const void*, VkSemaphore*) = nullptr;
    void (*DestroySemaphore)(VkDevice, VkSemaphore, const void*) = nullptr;

    // command recording
    void (*CmdBeginRenderPass)(VkCommandBuffer, const VkRenderPassBeginInfo*, int) = nullptr;
    void (*CmdEndRenderPass)(VkCommandBuffer) = nullptr;
    void (*CmdBindPipeline)(VkCommandBuffer, int, VkPipeline) = nullptr;
    void (*CmdBindVertexBuffers)(VkCommandBuffer, std::uint32_t, std::uint32_t, const VkBuffer*, const DeviceSize*) = nullptr;
    void (*CmdBindDescriptorSets)(VkCommandBuffer, int, VkPipelineLayout, std::uint32_t, std::uint32_t, const VkDescriptorSet*, std::uint32_t, const std::uint32_t*) = nullptr;
    void (*CmdPushConstants)(VkCommandBuffer, VkPipelineLayout, Flags, std::uint32_t, std::uint32_t, const void*) = nullptr;
    void (*CmdSetViewport)(VkCommandBuffer, std::uint32_t, std::uint32_t, const VkViewport*) = nullptr;
    void (*CmdSetScissor)(VkCommandBuffer, std::uint32_t, std::uint32_t, const VkRect2D*) = nullptr;
    void (*CmdDraw)(VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) = nullptr;
    void (*CmdCopyBufferToImage)(VkCommandBuffer, VkBuffer, VkImage, int, std::uint32_t, const VkBufferImageCopy*) = nullptr;
    void (*CmdCopyImageToBuffer)(VkCommandBuffer, VkImage, int, VkBuffer, std::uint32_t, const VkBufferImageCopy*) = nullptr;
    void (*CmdPipelineBarrier)(VkCommandBuffer, Flags, Flags, Flags, std::uint32_t, const void*, std::uint32_t, const void*, std::uint32_t, const VkImageMemoryBarrier*) = nullptr;

    // surface / swapchain (KHR, resolved from the instance)
    void (*DestroySurfaceKHR)(VkInstance, VkSurfaceKHR, const void*) = nullptr;
    VkResult (*GetPhysicalDeviceSurfaceSupportKHR)(VkPhysicalDevice, std::uint32_t, VkSurfaceKHR, Bool32*) = nullptr;
    VkResult (*GetPhysicalDeviceSurfaceCapabilitiesKHR)(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR*) = nullptr;
    VkResult (*GetPhysicalDeviceSurfaceFormatsKHR)(VkPhysicalDevice, VkSurfaceKHR, std::uint32_t*, VkSurfaceFormatKHR*) = nullptr;
    VkResult (*GetPhysicalDeviceSurfacePresentModesKHR)(VkPhysicalDevice, VkSurfaceKHR, std::uint32_t*, int*) = nullptr;
    VkResult (*CreateSwapchainKHR)(VkDevice, const VkSwapchainCreateInfoKHR*, const void*, VkSwapchainKHR*) = nullptr;
    void (*DestroySwapchainKHR)(VkDevice, VkSwapchainKHR, const void*) = nullptr;
    VkResult (*GetSwapchainImagesKHR)(VkDevice, VkSwapchainKHR, std::uint32_t*, VkImage*) = nullptr;
    VkResult (*AcquireNextImageKHR)(VkDevice, VkSwapchainKHR, std::uint64_t, VkSemaphore, VkFence, std::uint32_t*) = nullptr;
    VkResult (*QueuePresentKHR)(VkQueue, const VkPresentInfoKHR*) = nullptr;
    VkResult (*CreateWaylandSurfaceKHR)(VkInstance, const VkWaylandSurfaceCreateInfoKHR*, const void*, VkSurfaceKHR*) = nullptr;
#if defined(_WIN32)
    VkResult (*CreateWin32SurfaceKHR)(VkInstance, const void*, const void*, VkSurfaceKHR*) = nullptr;
#endif
};

namespace detail {

inline void* load_library() noexcept {
#if defined(_WIN32)
    return reinterpret_cast<void*>(::LoadLibraryA("vulkan-1.dll"));
#elif defined(__APPLE__)
    return ::dlopen("libvulkan.1.dylib", RTLD_LAZY | RTLD_LOCAL);
#else
    void* h = ::dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (h == nullptr) h = ::dlopen("libvulkan.so", RTLD_LAZY | RTLD_LOCAL);
    return h;
#endif
}

inline void* sym(void* h, const char* name) noexcept {
#if defined(_WIN32)
    return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(h), name));
#else
    return ::dlsym(h, name);
#endif
}

template <typename T>
inline bool load_global(Api& a, T& fn, const char* name) noexcept {
    fn = reinterpret_cast<T>(a.GetInstanceProcAddr(nullptr, name));
    return fn != nullptr;
}

}  // namespace detail

/// The loader, resolved once. `vkGetInstanceProcAddr` is the only symbol taken
/// from the .so directly; everything else is resolved through it, which is the
/// portable path and works with a layer/loader in between.
[[nodiscard]] inline const Api& api() noexcept {
    static const Api a = [] {
        Api api{};
        api.handle = detail::load_library();
        if (api.handle == nullptr) return api;
        api.GetInstanceProcAddr = reinterpret_cast<decltype(api.GetInstanceProcAddr)>(
            detail::sym(api.handle, "vkGetInstanceProcAddr"));
        if (api.GetInstanceProcAddr == nullptr) return api;

        bool ok = true;
        ok &= detail::load_global(api, api.CreateInstance, "vkCreateInstance");
        api.ok = ok;
        return api;
    }();
    return a;
}

}  // namespace mayag::backend::vk

#include "vulkan_device.hpp"

#endif  // platform guard
