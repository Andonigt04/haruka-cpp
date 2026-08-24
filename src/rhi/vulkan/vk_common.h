/**
 * @file vk_common.h
 * @brief Traducción de enums del RHI a constantes de Vulkan. Solo aquí (y en los .cpp del
 *        backend VK) se incluye <vulkan/vulkan.h>. Fuera de src/rhi/vulkan/ no debe haber VK.
 */
#pragma once

#include "core/logger.h"

#include <vulkan/vulkan.h>
#include "rhi/rhi_types.h"

namespace Haruka::RHI::vulkan
{
    /** @brief Formato de un atributo de vértice traducido a Vulkan.
     *  Vulkan no tiene el trío size/type/normalized de GL: cada VkFormat ya implica la
     *  normalización (los *_UNORM/*_SNORM normalizan; los *_UINT/*_SFLOAT no). */
    inline VkFormat vertexFmt(Format f)
    {
        switch (f)
        {
            case Format::R32F:        return VK_FORMAT_R32_SFLOAT;
            case Format::RG32F:       return VK_FORMAT_R32G32_SFLOAT;
            case Format::RGB32F:      return VK_FORMAT_R32G32B32_SFLOAT;
            case Format::RGBA32F:     return VK_FORMAT_R32G32B32A32_SFLOAT;
            case Format::RGBA8:       return VK_FORMAT_R8G8B8A8_UNORM;
            case Format::RG16F:       return VK_FORMAT_R16G16_SFLOAT;
            // Empaquetado (el terreno): normal en 4 B. Convención VK: A2B10G10R10 == GL_INT_2_10_10_10_REV.
            case Format::RGB10A2_SNORM: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
            // El default devuelve un vec4 float en silencio si no logueáramos: un formato no
            // mapeado (p.ej. los empaquetados de arriba) leería BASURA del buffer sin avisar.
            default:
                HARUKA_LOGE("RHI/VK", "vertexFmt: formato de vertice NO soportado (%d)", (int)f);
                return VK_FORMAT_R32G32B32A32_SFLOAT;
        }
    }

    /** @brief Formato de textura/attachment traducido a Vulkan. */
    inline VkFormat texFmt(Format f)
    {
        switch (f)
        {
            case Format::RGBA8:       return VK_FORMAT_R8G8B8A8_UNORM;
            // sRGB (SOLO texturas de color): el HW decodifica a lineal al muestrear → el filtrado
            // se hace en espacio lineal. Ver rhi_types.h.
            case Format::SRGB8_ALPHA8: return VK_FORMAT_R8G8B8A8_SRGB;
            case Format::RGBA16F:     return VK_FORMAT_R16G16B16A16_SFLOAT;
            case Format::RGB16F:      return VK_FORMAT_R16G16B16_SFLOAT;
            case Format::RG16F:       return VK_FORMAT_R16G16_SFLOAT;
            case Format::RGBA32F:     return VK_FORMAT_R32G32B32A32_SFLOAT;
            case Format::RGB32F:      return VK_FORMAT_R32G32B32_SFLOAT;
            case Format::RG32F:       return VK_FORMAT_R32G32_SFLOAT;
            case Format::RGB8:        return VK_FORMAT_R8G8B8_UNORM;
            case Format::R11G11B10F:  return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
            case Format::R8:          return VK_FORMAT_R8_UNORM;
            case Format::RG32UI:      return VK_FORMAT_R32G32_UINT;
            case Format::RGBA16UI:    return VK_FORMAT_R16G16B16A16_UINT;
            case Format::R32F:        return VK_FORMAT_R32_SFLOAT;
            case Format::D24:         return VK_FORMAT_X8_D24_UNORM_PACK32;
            case Format::D24S8:       return VK_FORMAT_D24_UNORM_S8_UINT;
            case Format::D32F:        return VK_FORMAT_D32_SFLOAT;
            default:                  return VK_FORMAT_R8G8B8A8_UNORM;
        }
    }

    inline VkPrimitiveTopology toTopology(PrimitiveTopology t)
    {
        switch (t)
        {
            case PrimitiveTopology::Triangles:     return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            case PrimitiveTopology::Patches:       return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
            case PrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            case PrimitiveTopology::Lines:         return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            case PrimitiveTopology::LineStrip:     return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            case PrimitiveTopology::Points:        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        }
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }

    inline VkCompareOp toCompare(CompareOp c)
    {
        switch (c)
        {
            case CompareOp::Never:        return VK_COMPARE_OP_NEVER;
            case CompareOp::Less:         return VK_COMPARE_OP_LESS;
            case CompareOp::Equal:        return VK_COMPARE_OP_EQUAL;
            case CompareOp::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
            case CompareOp::Greater:      return VK_COMPARE_OP_GREATER;
            case CompareOp::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
            case CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
            case CompareOp::Always:       return VK_COMPARE_OP_ALWAYS;
        }
        return VK_COMPARE_OP_LESS;
    }

    /** @brief Factores de mezcla para un VkPipelineColorBlendAttachmentState.
     *  Mismo par que glBlendFunc (se aplica a color Y a alpha): Alpha = src·a + dst·(1-a);
     *  Additive = src·a + dst (emisivo/radiante: partículas, fogonazos). */
    struct VKBlend { VkBlendFactor srcFactor; VkBlendFactor dstFactor; };

    inline VKBlend toBlend(BlendMode m)
    {
        switch (m)
        {
            case BlendMode::Alpha:    return { VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA };
            case BlendMode::Additive: return { VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE };
        }
        return { VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA };
    }

    inline VkCullModeFlags toCull(CullMode m)
    {
        switch (m)
        {
            case CullMode::None:  return VK_CULL_MODE_NONE;
            case CullMode::Back:  return VK_CULL_MODE_BACK_BIT;
            case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
        }
        return VK_CULL_MODE_NONE;
    }

    /** @brief Uso principal de un buffer. El caller (createBuffer) OR-ea los bits auxiliares que
     *  falten (TRANSFER_DST/SRC para staging y copias), igual que GL añade targets en el draw. */
    inline VkBufferUsageFlags toUsage(BufferUsage u)
    {
        switch (u)
        {
            case BufferUsage::Vertex:   return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            case BufferUsage::Index:    return VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
            case BufferUsage::Uniform:  return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            case BufferUsage::Indirect: return VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
            case BufferUsage::Storage:  return VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        }
        return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }

    inline VkSamplerAddressMode toWrap(Wrap w)
    {
        switch (w)
        {
            case Wrap::Repeat:         return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case Wrap::ClampToEdge:    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case Wrap::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        }
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }

    inline VkFilter toFilter(Filter f)
    {
        return (f == Filter::Nearest) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    }

    inline VkShaderStageFlagBits toStage(ShaderStage s)
    {
        switch (s)
        {
            case ShaderStage::Vertex:   return VK_SHADER_STAGE_VERTEX_BIT;
            case ShaderStage::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
            case ShaderStage::Geometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
            case ShaderStage::Compute:  return VK_SHADER_STAGE_COMPUTE_BIT;
        }
        return VK_SHADER_STAGE_VERTEX_BIT;
    }

    // ------------------------------------------------------------------------------------------
    // BINDINGS del descriptor set compartido — VER PLAN_VULKAN.md §4.3.
    //
    // GL tiene TRES espacios de binding SEPARADOS (UBO, SSBO, textura), así que los shaders
    // reutilizan el mismo número de slot entre tipos. Vulkan tiene UN solo espacio por descriptor
    // set → los bindings se separan POR TIPO en rangos fijos:
    //   UBO  : binding 0..31    (slot RHI = binding)
    //   SSBO : binding 32..63   (slot RHI → binding = slot + kSsboBindingBase)
    //   textura: binding 64..95 (slot RHI → binding = slot + kTextureBindingBase)
    // Esta CONSTANTE es la fuente de verdad: es la MISMA que se pasa a glslangValidator como
    // --shift-ssbo-binding / --shift-combined-sampler-binding al re-encodear los shaders para VK.
    // Si cambia aquí, cambiar también los shifts de la compilación (vk_pipeline.cpp).
    // ------------------------------------------------------------------------------------------
    constexpr uint32_t kUboBindingCount    = 32;   // bindings 0..31
    constexpr uint32_t kSsboBindingBase    = 32;   // bindings 32..63
    constexpr uint32_t kSsboBindingCount   = 32;
    constexpr uint32_t kTextureBindingBase = 64;   // bindings 64..95
    constexpr uint32_t kTextureBindingCount = 32;
    constexpr uint32_t kTotalBindings = kUboBindingCount + kSsboBindingCount + kTextureBindingCount;

    /** @brief Traduce un slot RHI (bindUniformBuffer/bindStorageBuffer/bindTexture) al binding VK
     *  dentro del descriptor set compartido. Mismo mapeo que los shifts de glslang. */
    inline uint32_t uboSlotToBinding(uint32_t slot)     { return slot; }
    inline uint32_t ssboSlotToBinding(uint32_t slot)    { return kSsboBindingBase + slot; }

    /// ⚠️ EL ASPECTO DE UNA IMAGEN DE PROFUNDIDAD INCLUYE EL STENCIL SI EL FORMATO LO TIENE.
    ///
    /// Con `separateDepthStencilLayouts` desactivado, la spec EXIGE que el `aspectMask` de una
    /// barrera sobre un formato depth+stencil lleve LOS DOS bits. El motor usaba
    /// `VK_IMAGE_ASPECT_DEPTH_BIT` a secas, y con D24S8 —el formato por defecto de los render
    /// targets— la validacion lo canta en cada transicion. Lo destapo el primer test que dibujo a
    /// un target offscreen; hasta entonces ese camino no se habia ejercido.
    inline VkImageAspectFlags depthAspectOf(VkFormat f) {
        const bool hasStencil = (f == VK_FORMAT_D24_UNORM_S8_UINT) ||
                                (f == VK_FORMAT_D32_SFLOAT_S8_UINT) ||
                                (f == VK_FORMAT_D16_UNORM_S8_UINT);
        return VK_IMAGE_ASPECT_DEPTH_BIT | (hasStencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    }
    inline uint32_t textureSlotToBinding(uint32_t slot) { return kTextureBindingBase + slot; }
}
