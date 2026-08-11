/**
 * @file vk_pipeline.h
 * @brief Fase 4 del port Vulkan: compilación GLSL→SPIR-V + construcción de pipelines gráficos y
 *        compute con el descriptor set layout COMPARTIDO (bindings de PLAN_VULKAN.md §4.3).
 *
 * El VKDevice delega aquí TODO el trabajo de "crear un pipeline": carga/compila las etapas a
 * SPIR-V, monta el layout compartido y rellena el VkGraphicsPipelineCreateInfo con el estado
 * inmutable (lo que en GL hacía bindPipeline con glEnable/glDepthFunc/... — ver gl_context.cpp).
 *
 * Regla del plan: <vulkan/vulkan.h> solo se incluye aquí y en el resto del backend VK. Nada de
 * Vulkan fuera de src/rhi/vulkan/.
 */
#pragma once

#include <vector>
#include <string>
#include <cstdint>

#include "rhi/rhi_types.h"
#include "rhi/rhi_resources.h"
#include "rhi/vulkan/vk_common.h"

namespace Haruka::RHI::vulkan
{
    /** @brief Índice de una etapa en los arrays de SPIR-V (uno por VkShaderModule potencial). */
    enum VkStageIdx : uint8_t
    {
        kStageVert = 0,
        kStageFrag,
        kStageGeom,
        kStageTesc,   // teselación control
        kStageTese,   // teselación evaluación
        kStageComp,
        kStageCount
    };

    /**
     * @brief Compila un shader GLSL (cadena) a SPIR-V para Vulkan.
     *
     * Invoca `glslangValidator -V -S <stage>` en el PATH con los shifts de binding de §4.3
     * (--shift-ssbo-binding / --shift-combined-sampler-binding) para separar UBO/SSBO/textura en
     * el espacio de binding único de Vulkan. El source debe venir YA resuelto de #include (o no
     * tenerlos). La caché es por hash del (source, stage): la misma cadena compila una sola vez.
     * Devuelve vacío si glslang no está o la compilación falla (la causa se loguea).
     */
    std::vector<uint32_t> compileGlslToSpv(const std::string& source, VkShaderStageFlagBits stage);

    /**
     * @brief Carga UNA etapa a bytes SPIR-V (sin crear VkShaderModule todavía).
     * Fuentes, en orden de prioridad: GLSL source inline (se compila) > ruta (GLSL-first, cae a
     * "<ruta>.spv") > bytes SPIR-V directos. Devuelve vacío si no hay nada o falla la compilación.
     */
    std::vector<uint32_t> loadStageSpv(const char* path, const char* source,
                                       const void* bytes, size_t size,
                                       VkShaderStageFlagBits stage);

    /**
     * @brief Crea el descriptor set layout COMPARTIDO (set 0) con TODOS los slots:
     * bindings 0..31 UBO, 32..63 SSBO, 64..95 combined image sampler, cada uno con
     * DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT (un slot sin binding no invalida el set).
     * Requiere el feature descriptorIndexing habilitado en el device (lo hace VKDevice).
     */
    VkDescriptorSetLayout createSharedSetLayout(VkDevice device);

    /** @brief Crea el pipeline layout compartido: un único descriptor set (el compartido). */
    VkPipelineLayout createSharedPipelineLayout(VkDevice device, VkDescriptorSetLayout setLayout);

    /**
     * @brief Construye un pipeline GRÁFICO completo.
     *
     * `spv[0..5]` son los bytes SPIR-V por etapa (vacíos = etapa ausente; vert+frag obligatorias
     * salvo pipelines de solo-compute). TODO el estado de bindPipeline GL (gl_context.cpp:105-167)
     * es aquí INMUTABLE dentro del VkGraphicsPipelineCreateInfo. El pipeline se crea contra
     * `renderPass`; para usarlo con otro target el VKDevice crea una variante (ver
     * VKDevice::pipelineForRenderPass). Devuelve VK_NULL_HANDLE si algo falla (logueado).
     */
    VkPipeline buildGraphicsPipeline(VkDevice device, const PipelineDesc& desc,
                                     const std::vector<std::vector<uint32_t>>& spv,
                                     VkPipelineLayout layout, VkRenderPass renderPass,
                                     uint32_t colorAttachmentCount = 1);

    /** @brief Construye un pipeline COMPUTE (mismo layout compartido, sin estado de raster). */
    VkPipeline buildComputePipeline(VkDevice device, const std::vector<uint32_t>& spv,
                                    VkPipelineLayout layout);
}
