/**
 * @file rhi_resources.h
 * @brief "Recetas" (descriptores) para crear recursos. Son solo datos agrupados (POD-ish).
 *
 * Rellenas la struct y se la pasas al Device::create*(). Sin métodos, sin lógica.
 */
#pragma once

#include "rhi_types.h"
#include <vector>
#include <cstddef>
#include <cstdint>

namespace Haruka::RHI
{
    /** @brief Receta para crear una textura 2D. */
    struct TextureDesc
    {
        uint32_t    width = 0;
        uint32_t    height = 0;
        Format      format = Format::RGBA8;
        bool        renderTarget = false;      // ¿se puede dibujar encima? (attachment de FBO)
        Filter      filter = Filter::Linear;
        Wrap        wrap = Wrap::Repeat;
        bool        mipmaps = false;
        bool        cube = false;              // textura cubemap (6 caras) para IBL/reflejos
        float       maxAnisotropy = 1.0f;      // 1 = isotrópico (clampeado al máx del driver)
        float       lodBias = 0.0f;            // >0 mips más borrosos (barato), <0 más nítidos
        const void* initialData = nullptr;     // píxeles iniciales, o null si vacía
    };

    /** @brief Receta para crear un sampler (cómo se muestrea una textura). */
    struct SamplerDesc
    {
        Filter filter = Filter::Linear;
        Wrap   wrap = Wrap::Repeat;
        float  maxAnisotropy = 1.0f;
    };

    /** @brief Un atributo de vértice: qué es y dónde está. Reemplaza glVertexAttribPointer. */
    struct VertexAttribute
    {
        uint32_t location;      // el "layout(location = N)" del shader
        uint32_t offset;        // bytes desde el inicio del vértice DENTRO de su binding
        Format   format;        // RGB32F = vec3, RG32F = vec2, RGBA8 = color normalizado...
        uint32_t binding = 0;   // de QUÉ vertex buffer se alimenta (ver VertexLayout::strides)
    };

    /** @brief Cada cuánto AVANZA el binding: por vértice (lo normal) o por INSTANCIA.
     *  Es lo que hace posible el instancing sin tocar GL: los atributos que describen la INSTANCIA
     *  (matriz de modelo, color, escala) salen de un buffer que avanza una vez por instancia, no por
     *  vértice. Mapea 1:1 a `VkVertexInputBindingDescription::inputRate` (en GL: divisor 0/1). */
    enum class InputRate { Vertex, Instance };

    /** @brief Layout completo de un vértice. Un stride por BINDING (= por vertex buffer).
     *  El caso común —todo interleaved en un solo buffer— usa un único stride. El TERRENO usa
     *  un buffer POR STREAM de atributo (posición / normal empaquetada / uv), cada uno con su
     *  stride → varios bindings. Mapea 1:1 a VkVertexInputBindingDescription. */
    struct VertexLayout
    {
        std::vector<uint32_t>        strides;      // strides[i] = bytes/vértice del binding i
        std::vector<VertexAttribute> attributes;
        /// rates[i] = cada cuánto avanza el binding i. Vacío o corto ⇒ Vertex (comportamiento previo).
        std::vector<InputRate>       rates;
    };

    /**
     * @brief Estado de profundidad.
     *
     * ⚠️ El motor usa **REVERSED-Z** (near→1, infinito→0; ver Camera::getProjectionMatrix):
     * lo CERCANO tiene z MAYOR → el test por defecto es **Greater**, no Less. Un pase que
     * ponga `Less` con la proyección de la cámara NO dibujará nada (o solo lo más lejano).
     * Los pases con proyección PROPIA no invertida (shadow maps, IBL) deben pedir Less/LessEqual
     * explícitamente Y limpiar su depth a 1.0.
     */
    struct DepthState
    {
        bool      test = true;
        bool      write = true;
        CompareOp compare = CompareOp::Greater;   // reversed-Z: lo más cercano gana con >
    };

    /** @brief Estado de mezcla (hoy son tus glBlendFunc sueltos). alpha estándar src/1-src. */
    struct BlendState
    {
        bool      enable = false;
        BlendMode mode   = BlendMode::Alpha;
    };

    /**
     * @brief Receta de un PIPELINE: junta shaders + layout + estado en UN objeto inmutable.
     *
     * Grafico: rellena spirvVertex + spirvFragment (+ opcional geometry).
     * Compute: rellena SOLO spirvCompute (el resto se ignora).
     * Los bytes SPIR-V los carga el llamador (el RHI es agnóstico del sistema de ficheros).
     */
    struct PipelineDesc
    {
        // Opción A (motor actual): rutas COMPLETAS a los shaders. El backend GL carga
        // GLSL-source-first y cae a ".spv" (fiabilidad en Mesa/AMD). El de Vulkan usará ".spv".
        const char* vertexPath = nullptr;
        const char* fragmentPath = nullptr;
        const char* geometryPath = nullptr;
        const char* computePath = nullptr;

        // Opción B: bytes SPIR-V directos (si no se dan rutas). Útil para pipelines generados.
        const void* spirvVertex = nullptr;   size_t spirvVertexSize = 0;
        const void* spirvFragment = nullptr; size_t spirvFragmentSize = 0;
        const void* spirvGeometry = nullptr; size_t spirvGeometrySize = 0;
        const void* spirvCompute = nullptr;  size_t spirvComputeSize = 0;

        // Opción C: GLSL source EN LÍNEA (para shaders generados/embebidos, p.ej. compute de postfx).
        // Solo GL; para Vulkan habría que extraerlos a .spv. Tiene prioridad sobre rutas/bytes.
        const char* vertexSource = nullptr;
        const char* fragmentSource = nullptr;
        const char* computeSource = nullptr;

        VertexLayout      vertexLayout;    // puede ir vacío (Shader solo-programa durante leaf-swap)
        PrimitiveTopology topology = PrimitiveTopology::Triangles;
        DepthState        depth;
        BlendState        blend;
        // OJO al default: None = SIN culling. Los pases que quieran descartar caras traseras
        // (la escena sólida) deben pedir Back explícitamente.
        CullMode          cull = CullMode::None;
    };

    /**
     * @brief Receta de un render target (FBO).
     *
     * - `colorFormats`: 0..N attachments de color (MRT). Vacío = sin color (shadow maps depth-only).
     * - `hasDepth` + `depthFormat`: profundidad. Por defecto como renderbuffer (rápido, no muestreable).
     * - `depthAsTexture`: la profundidad se adjunta como TEXTURA muestreable (shadow maps).
     * - `depthBorderClamp`: CLAMP_TO_BORDER + borde blanco (fuera del shadow map = sin sombra).
     * - `depthCube`: la profundidad es un CUBEMAP (point/omni shadows).
     */
    struct RenderTargetDesc
    {
        uint32_t            width = 0;
        uint32_t            height = 0;
        std::vector<Format> colorFormats;                 // MRT; vacío = depth-only
        Filter              colorFilter = Filter::Linear;
        bool                hasDepth = true;
        Format              depthFormat = Format::D24S8;
        bool                depthAsTexture = false;
        bool                depthBorderClamp = false;
        bool                depthCube = false;
        Filter              depthFilter = Filter::Nearest;   // LINEAR = PCF hardware suave
        bool                depthCompare = false;            // sampler de sombra (COMPARE_REF_TO_TEXTURE, LEQUAL)
    };

    /** @brief Valores de limpieza al comenzar un render pass.
     *  ⚠️ REVERSED-Z: el "infinito" es 0 → el clear de profundidad es **0.0**, no 1.0.
     *  (Los pases con proyección no invertida —shadow maps, IBL— deben pasar depth = 1.0.) */
    struct ClearValues
    {
        bool  clearColor = true;
        float color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        bool  clearDepth = true;
        float depth = 0.0f;   // reversed-Z: lejano = 0
    };
}
