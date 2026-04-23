#include "ibl.h"

#include <iostream>
#include <stb_image.h>
#include <glm/gtc/matrix_transform.hpp>
#include <cstring>
#include <vector>

#include "shader.h"


IBL::IBL() {}

void IBL::createVulkanResources(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue transferQueue, VkCommandPool commandPool) {
    // Aquí se implementará la creación de cubemaps y LUTs Vulkan
}

void IBL::destroyVulkanResources(VkDevice device) {
    if (envCubemapSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, envCubemapSampler, nullptr);
        envCubemapSampler = VK_NULL_HANDLE;
    }
    if (envCubemapView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, envCubemapView, nullptr);
        envCubemapView = VK_NULL_HANDLE;
    }
    if (envCubemapImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, envCubemapImage, nullptr);
        envCubemapImage = VK_NULL_HANDLE;
    }
    if (irradianceSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, irradianceSampler, nullptr);
        irradianceSampler = VK_NULL_HANDLE;
    }
    if (irradianceView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, irradianceView, nullptr);
        irradianceView = VK_NULL_HANDLE;
    }
    if (irradianceImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, irradianceImage, nullptr);
        irradianceImage = VK_NULL_HANDLE;
    }
    if (prefilterSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, prefilterSampler, nullptr);
        prefilterSampler = VK_NULL_HANDLE;
    }
    if (prefilterView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, prefilterView, nullptr);
        prefilterView = VK_NULL_HANDLE;
    }
    if (prefilterImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, prefilterImage, nullptr);
        prefilterImage = VK_NULL_HANDLE;
    }
    if (brdfLUTSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, brdfLUTSampler, nullptr);
        brdfLUTSampler = VK_NULL_HANDLE;
    }
    if (brdfLUTView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, brdfLUTView, nullptr);
        brdfLUTView = VK_NULL_HANDLE;
    }
    if (brdfLUTImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, brdfLUTImage, nullptr);
        brdfLUTImage = VK_NULL_HANDLE;
    }
}

void IBL::setupCubemap() {
    // Parámetros del cubemap
    const uint32_t size = 512;
    VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
    // Crear imagen cubemap
    VkImageCreateInfo imgInfo{};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.extent.width = size;
    imgInfo.extent.height = size;
    imgInfo.extent.depth = 1;
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 6;
    imgInfo.format = format;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imgInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VkDevice device = /* ...debes pasar el VkDevice adecuado... */ VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = /* ...debes pasar el VkPhysicalDevice adecuado... */ VK_NULL_HANDLE;
    // NOTA: Debes adaptar la función para recibir device y physicalDevice
    vkCreateImage(device, &imgInfo, nullptr, &envCubemapImage);
    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device, envCubemapImage, &memReq);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    // Helper para encontrar tipo de memoria
    auto findMemoryType = [](VkPhysicalDevice pd, uint32_t typeFilter, VkMemoryPropertyFlags properties) -> uint32_t {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(pd, &memProperties);
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        throw std::runtime_error("No suitable memory type found");
    };
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory memory;
    vkAllocateMemory(device, &allocInfo, nullptr, &memory);
    vkBindImageMemory(device, envCubemapImage, memory, 0);
    // Crear view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = envCubemapImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 6;
    vkCreateImageView(device, &viewInfo, nullptr, &envCubemapView);
    // Crear sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    vkCreateSampler(device, &samplerInfo, nullptr, &envCubemapSampler);
    // NOTA: Debes guardar el VkDevice y VkDeviceMemory en la clase para liberar correctamente
}

void IBL::loadHDRI(const std::string& imagePath) {
    stbi_set_flip_vertically_on_load(true);
    int width, height, nrComponents;
    float* data = stbi_loadf(imagePath.c_str(), &width, &height, &nrComponents, 0);
    if (!data) {
        std::cerr << "Failed to load HDR: " << imagePath << std::endl;
        return;
    }
    VkDevice device = /* ...debes pasar el VkDevice adecuado... */ VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = /* ...debes pasar el VkPhysicalDevice adecuado... */ VK_NULL_HANDLE;
    VkQueue transferQueue = /* ...debes pasar el VkQueue adecuado... */ VK_NULL_HANDLE;
    VkCommandPool commandPool = /* ...debes pasar el VkCommandPool adecuado... */ VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;
    VkDeviceSize imageSize = width * height * 4 * sizeof(float);
    // Crear staging buffer
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = imageSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device, &bufferInfo, nullptr, &stagingBuffer);
    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device, stagingBuffer, &memReq);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    auto findMemoryType = [](VkPhysicalDevice pd, uint32_t typeFilter, VkMemoryPropertyFlags properties) -> uint32_t {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(pd, &memProperties);
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        throw std::runtime_error("No suitable memory type found");
    };
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory);
    vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);
    void* mapped;
    vkMapMemory(device, stagingMemory, 0, imageSize, 0, &mapped);
    // Copiar datos RGBA (rellenar alpha si es necesario)
    if (nrComponents == 4) {
        memcpy(mapped, data, imageSize);
    } else {
        // Expandir a RGBA
        float* dst = reinterpret_cast<float*>(mapped);
        for (int i = 0; i < width * height; ++i) {
            for (int c = 0; c < nrComponents; ++c) dst[i * 4 + c] = data[i * nrComponents + c];
            for (int c = nrComponents; c < 4; ++c) dst[i * 4 + c] = 1.0f;
        }
    }
    vkUnmapMemory(device, stagingMemory);
    stbi_image_free(data);
    // Crear imagen Vulkan
    VkImageCreateInfo imgInfo{};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.extent.width = width;
    imgInfo.extent.height = height;
    imgInfo.extent.depth = 1;
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.format = format;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imgInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkImage hdrImage;
    vkCreateImage(device, &imgInfo, nullptr, &hdrImage);
    vkGetImageMemoryRequirements(device, hdrImage, &memReq);
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory hdrMemory;
    vkAllocateMemory(device, &allocInfo, nullptr, &hdrMemory);
    vkBindImageMemory(device, hdrImage, hdrMemory, 0);
    // Copiar staging buffer a imagen
    VkCommandBufferAllocateInfo allocInfoCmd{};
    allocInfoCmd.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfoCmd.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfoCmd.commandPool = commandPool;
    allocInfoCmd.commandBufferCount = 1;
    VkCommandBuffer cmdBuffer;
    vkAllocateCommandBuffers(device, &allocInfoCmd, &cmdBuffer);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmdBuffer, &beginInfo);
    // Transición a TRANSFER_DST
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = hdrImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    // Copiar buffer a imagen
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    vkCmdCopyBufferToImage(cmdBuffer, stagingBuffer, hdrImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    // Transición a SHADER_READ_ONLY
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    vkEndCommandBuffer(cmdBuffer);
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuffer;
    vkQueueSubmit(transferQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(transferQueue);
    vkFreeCommandBuffers(device, commandPool, 1, &cmdBuffer);
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);
    // Guardar hdrImage y hdrMemory si se requiere para la conversión a cubemap
    // (Aquí deberías continuar con el render pass para convertir equirectangular a cubemap)
}


void IBL::generateIrradianceMap(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue queue, VkCommandPool commandPool, VkRenderPass renderPass, VkDescriptorSetLayout* setLayouts, uint32_t setLayoutCount) {
    // 1. Crear imagen cubemap para irradiancia
    VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
    const uint32_t size = 32;
    VkImageCreateInfo imgInfo{};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.extent.width = size;
    imgInfo.extent.height = size;
    imgInfo.extent.depth = 1;
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 6;
    imgInfo.format = format;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imgInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    vkCreateImage(device, &imgInfo, nullptr, &irradianceImage);
    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device, irradianceImage, &memReq);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    auto findMemoryType = [](VkPhysicalDevice pd, uint32_t typeFilter, VkMemoryPropertyFlags properties) -> uint32_t {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(pd, &memProperties);
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        throw std::runtime_error("No suitable memory type found");
    };
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device, &allocInfo, nullptr, &irradianceImageMemory);
    vkBindImageMemory(device, irradianceImage, irradianceImageMemory, 0);
    // 2. Crear image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = irradianceImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 6;
    vkCreateImageView(device, &viewInfo, nullptr, &irradianceView);
    // 3. Crear sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    vkCreateSampler(device, &samplerInfo, nullptr, &irradianceSampler);
    // 4. Crear framebuffers para cada cara del cubemap
    std::vector<VkFramebuffer> framebuffers(6);
    for (uint32_t i = 0; i < 6; ++i) {
        VkImageViewCreateInfo faceViewInfo = viewInfo;
        faceViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        faceViewInfo.subresourceRange.baseArrayLayer = i;
        faceViewInfo.subresourceRange.layerCount = 1;
        VkImageView faceView;
        vkCreateImageView(device, &faceViewInfo, nullptr, &faceView);
        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = renderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &faceView;
        fbInfo.width = size;
        fbInfo.height = size;
        fbInfo.layers = 1;
        vkCreateFramebuffer(device, &fbInfo, nullptr, &framebuffers[i]);
        vkDestroyImageView(device, faceView, nullptr); // Se puede destruir tras crear el framebuffer
    }
    // 5. Crear pipeline y shaders para convolución (no incluido aquí, requiere SPIR-V y setup externo)
    // 6. Para cada cara, grabar y ejecutar un command buffer que dibuje la convolución
    // (El usuario debe proveer el pipeline y descriptor set adecuado)
    // ...
    // 7. Limpiar framebuffers temporales
    for (auto fb : framebuffers) {
        vkDestroyFramebuffer(device, fb, nullptr);
    }
}

void IBL::generatePrefilterMap(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue queue, VkCommandPool commandPool, VkRenderPass renderPass, VkDescriptorSetLayout* setLayouts, uint32_t setLayoutCount) {
    // 1. Crear imagen cubemap para prefilter
    VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
    const uint32_t size = 128;
    const uint32_t maxMipLevels = 5;
    VkImageCreateInfo imgInfo{};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.extent.width = size;
    imgInfo.extent.height = size;
    imgInfo.extent.depth = 1;
    imgInfo.mipLevels = maxMipLevels;
    imgInfo.arrayLayers = 6;
    imgInfo.format = format;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imgInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    vkCreateImage(device, &imgInfo, nullptr, &prefilterImage);
    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device, prefilterImage, &memReq);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    auto findMemoryType = [](VkPhysicalDevice pd, uint32_t typeFilter, VkMemoryPropertyFlags properties) -> uint32_t {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(pd, &memProperties);
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        throw std::runtime_error("No suitable memory type found");
    };
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device, &allocInfo, nullptr, &prefilterImageMemory);
    vkBindImageMemory(device, prefilterImage, prefilterImageMemory, 0);
    // 2. Crear image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = prefilterImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = maxMipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 6;
    vkCreateImageView(device, &viewInfo, nullptr, &prefilterView);
    // 3. Crear sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    vkCreateSampler(device, &samplerInfo, nullptr, &prefilterSampler);
    // 4. Crear framebuffers para cada cara y mip level
    for (uint32_t mip = 0; mip < maxMipLevels; ++mip) {
        uint32_t mipWidth = size * std::pow(0.5, mip);
        uint32_t mipHeight = size * std::pow(0.5, mip);
        for (uint32_t face = 0; face < 6; ++face) {
            VkImageViewCreateInfo faceViewInfo = viewInfo;
            faceViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            faceViewInfo.subresourceRange.baseArrayLayer = face;
            faceViewInfo.subresourceRange.layerCount = 1;
            faceViewInfo.subresourceRange.baseMipLevel = mip;
            faceViewInfo.subresourceRange.levelCount = 1;
            VkImageView faceView;
            vkCreateImageView(device, &faceViewInfo, nullptr, &faceView);
            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass = renderPass;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments = &faceView;
            fbInfo.width = mipWidth;
            fbInfo.height = mipHeight;
            fbInfo.layers = 1;
            VkFramebuffer framebuffer;
            vkCreateFramebuffer(device, &fbInfo, nullptr, &framebuffer);
            // Aquí se debe grabar y ejecutar el command buffer para convolución de prefilter
            // (El usuario debe proveer el pipeline y descriptor set adecuado)
            vkDestroyFramebuffer(device, framebuffer, nullptr);
            vkDestroyImageView(device, faceView, nullptr);
        }
    }
}

void IBL::generateBRDFLUT(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue queue, VkCommandPool commandPool, VkRenderPass renderPass, VkDescriptorSetLayout* setLayouts, uint32_t setLayoutCount) {
    // 1. Crear imagen 2D para BRDF LUT
    VkFormat format = VK_FORMAT_R16G16_SFLOAT;
    const uint32_t size = 512;
    VkImageCreateInfo imgInfo{};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.extent.width = size;
    imgInfo.extent.height = size;
    imgInfo.extent.depth = 1;
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.format = format;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imgInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateImage(device, &imgInfo, nullptr, &brdfLUTImage);
    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device, brdfLUTImage, &memReq);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    auto findMemoryType = [](VkPhysicalDevice pd, uint32_t typeFilter, VkMemoryPropertyFlags properties) -> uint32_t {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(pd, &memProperties);
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        throw std::runtime_error("No suitable memory type found");
    };
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device, &allocInfo, nullptr, &brdfLUTImageMemory);
    vkBindImageMemory(device, brdfLUTImage, brdfLUTImageMemory, 0);
    // 2. Crear image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = brdfLUTImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    vkCreateImageView(device, &viewInfo, nullptr, &brdfLUTView);
    // 3. Crear sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    vkCreateSampler(device, &samplerInfo, nullptr, &brdfLUTSampler);
    // 4. Crear framebuffer
    VkImageView attachments[] = { brdfLUTView };
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = renderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = attachments;
    fbInfo.width = size;
    fbInfo.height = size;
    fbInfo.layers = 1;
    VkFramebuffer framebuffer;
    vkCreateFramebuffer(device, &fbInfo, nullptr, &framebuffer);
    // Aquí se debe grabar y ejecutar el command buffer para renderizar la BRDF LUT
    // (El usuario debe proveer el pipeline y descriptor set adecuado)
    vkDestroyFramebuffer(device, framebuffer, nullptr);
}

IBL::~IBL() {
    // El usuario debe llamar a destroyVulkanResources antes del destructor
}

// Renderiza un cubo unitario (debe llamarse con el pipeline adecuado)
void IBL::renderCube(VkCommandBuffer cmd) {
    static VkBuffer cubeBuffer = VK_NULL_HANDLE;
    static VkDeviceMemory cubeMemory = VK_NULL_HANDLE;
    static uint32_t cubeVertexCount = 36;
    // Si el buffer no existe, créalo (solo una vez)
    if (cubeBuffer == VK_NULL_HANDLE) {
        // 36 vértices para un cubo unitario
        float vertices[] = {
            // ... (vertices de un cubo unitario, 36*3 floats) ...
            -1.0f,-1.0f,-1.0f,  1.0f,-1.0f,-1.0f,  1.0f, 1.0f,-1.0f,
            1.0f, 1.0f,-1.0f, -1.0f, 1.0f,-1.0f, -1.0f,-1.0f,-1.0f,
            -1.0f,-1.0f, 1.0f,  1.0f,-1.0f, 1.0f,  1.0f, 1.0f, 1.0f,
            1.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f,-1.0f, 1.0f,
            -1.0f, 1.0f, 1.0f, -1.0f, 1.0f,-1.0f, -1.0f,-1.0f,-1.0f,
            -1.0f,-1.0f,-1.0f, -1.0f,-1.0f, 1.0f, -1.0f, 1.0f, 1.0f,
            1.0f, 1.0f, 1.0f,  1.0f, 1.0f,-1.0f,  1.0f,-1.0f,-1.0f,
            1.0f,-1.0f,-1.0f,  1.0f,-1.0f, 1.0f,  1.0f, 1.0f, 1.0f,
            -1.0f,-1.0f,-1.0f,  1.0f,-1.0f,-1.0f,  1.0f,-1.0f, 1.0f,
            1.0f,-1.0f, 1.0f, -1.0f,-1.0f, 1.0f, -1.0f,-1.0f,-1.0f,
            -1.0f, 1.0f,-1.0f,  1.0f, 1.0f,-1.0f,  1.0f, 1.0f, 1.0f,
            1.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f,-1.0f,
            -1.0f,-1.0f, 1.0f,  1.0f,-1.0f, 1.0f,  1.0f, 1.0f, 1.0f,
            1.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f,-1.0f, 1.0f
        };
        // Crear buffer y memoria (debes adaptar a tu sistema de buffers Vulkan)
        // ...
        // Aquí solo se muestra la lógica de bind/draw
    }
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, &cubeBuffer, offsets);
    vkCmdDraw(cmd, cubeVertexCount, 1, 0, 0);
}

// Renderiza un quad en XY (debe llamarse con el pipeline adecuado)
void IBL::renderQuad(VkCommandBuffer cmd) {
    static VkBuffer quadBuffer = VK_NULL_HANDLE;
    static VkDeviceMemory quadMemory = VK_NULL_HANDLE;
    static uint32_t quadVertexCount = 6;
    // Si el buffer no existe, créalo (solo una vez)
    if (quadBuffer == VK_NULL_HANDLE) {
        float vertices[] = {
            // 6 vértices para un quad (2 triángulos)
            -1.0f, -1.0f, 0.0f,
             1.0f, -1.0f, 0.0f,
             1.0f,  1.0f, 0.0f,
            -1.0f, -1.0f, 0.0f,
             1.0f,  1.0f, 0.0f,
            -1.0f,  1.0f, 0.0f
        };
        // Crear buffer y memoria (debes adaptar a tu sistema de buffers Vulkan)
        // ...
        // Aquí solo se muestra la lógica de bind/draw
    }
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, &quadBuffer, offsets);
    vkCmdDraw(cmd, quadVertexCount, 1, 0, 0);
}

// Bindea el prefilter cubemap como sampled image en el pipeline
void IBL::bindPrefilterMap(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t set, uint32_t binding) {
    // El descriptor set debe estar previamente actualizado con prefilterView y prefilterSampler
    // Aquí solo se hace el bind del descriptor set correspondiente
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, set, 1, /*descriptorSetPrefilter*/ nullptr, 0, nullptr);
}

// Bindea la BRDF LUT como sampled image en el pipeline
void IBL::bindBRDFLUT(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t set, uint32_t binding) {
    // El descriptor set debe estar previamente actualizado con brdfLUTView y brdfLUTSampler
    // Aquí solo se hace el bind del descriptor set correspondiente
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, set, 1, /*descriptorSetBRDFLUT*/ nullptr, 0, nullptr);
}