/**
 * @file rhi_types.h
 * @brief Tipos base del RHI: enums e identificadores. Sin lógica, sin GL, sin Vulkan.
 *
 * Este header es la "lengua franca" del RHI: solo define nombres. No incluye ninguna
 * API gráfica, así que puede incluirse desde cualquier parte sin acoplar el motor a GL/Vulkan.
 *
 * @par API Status — FROZEN
 * Breaking changes will not be made without a major version bump.
 */
#pragma once

#include <cstdint>

namespace Haruka::RHI
{
    /** @brief Backend gráfico activo. Lo elige la fábrica Device::create en el arranque. */
    enum class Backend { OpenGL, Vulkan };

    /**
     * @brief Formato de un dato en GPU. Doble uso:
     *  - formato de PÍXEL de textura/attachment (RGBA8, D24S8…).
     *  - formato de un ATRIBUTO de vértice (RGB32F = vec3, RG32F = vec2…).
     */
    enum class Format
    {
        Unknown,
        RGBA8,     // color normal, 8 bits por canal (0..1)
        RGBA16F,   // color HDR
        RGB16F,    // color HDR 3 canales (cubemaps IBL)
        RG16F,     // 2 canales half-float (BRDF LUT)
        RGBA32F,   // vec4 float
        RGB32F,    // vec3 float (posiciones/normales de vértice)
        RG32F,     // vec2 float (uv de vértice)
        RGB8,      // color LDR de 3 canales (texturas sin alpha)
        R11G11B10F,// color HDR compacto (4 B/px, sin alpha) — usado por HDR/bloom
        R8,        // un canal 8-bit (SSAO occlusion)
        RG32UI,    // 2 canales enteros 32-bit (page table de virtual texturing)
        RGBA16UI,  // 4 canales enteros 16-bit (feedback de virtual texturing)
        R32F,      // un solo canal float (heightfields, sombras)
        D24,       // depth 24 (sin stencil)
        D24S8,     // depth 24 + stencil 8
        D32F,      // depth 32 float
        // Formato de VÉRTICE empaquetado: normal en 4 B (10/10/10/2 con signo, normalizado).
        // GL: GL_INT_2_10_10_10_REV · Vulkan: VK_FORMAT_A2B10G10R10_SNORM_PACK32.
        // AÑADIR SIEMPRE AL FINAL: insertar en medio desplaza los valores numéricos de los que
        // vienen detrás — inocuo si TODO se recompila, pero es una bomba de relojería gratuita.
        RGB10A2_SNORM,

        // Color en sRGB: el HW linealiza (decode) ANTES de filtrar y al escribir. Para texturas de
        // COLOR (albedo, emisión). Los mapas de DATOS (normal, roughness, metallic, ao, height) van
        // LINEALES (RGBA8/R8). Añadido aquí para poder corregir el filtrado del albedo: con RGBA8,
        // como el shader linealiza en el último momento (pow 2.2), se filtra en sRGB y se linealiza
        // DESPUÉS — media con gamma ≠ gamma de la media ("gamma-incorrect filtering").
        SRGB8_ALPHA8,
    };

    // Patches = entrada de la TESELACIÓN. No es un modo de dibujo más: con teselación activa el
    // rasterizador NO recibe triángulos, recibe parches que las etapas de control y evaluación
    // subdividen. En GL obliga a GL_PATCHES + glPatchParameteri(GL_PATCH_VERTICES, n).
    enum class PrimitiveTopology { Triangles, TriangleStrip, Lines, LineStrip, Points, Patches };
    // Modo de mezcla. Alpha = transparencia normal (src·a + dst·(1-a)). Additive = emisivo/radiante
    // (src·a + dst): partículas, fogonazos. Mapea a VkPipelineColorBlendAttachmentState.
    enum class BlendMode { Alpha, Additive };
    // Culling de caras. None = a dos caras (agua, vegetación). Mapea a
    // VkPipelineRasterizationStateCreateInfo.cullMode.
    enum class CullMode { None, Back, Front };
    // Indirect = buffer de comandos de dibujo leídos por la GPU (multi-draw indirect). En Vulkan
    // necesita VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, así que debe declararse explícitamente.
    enum class BufferUsage { Vertex, Index, Uniform, Storage, Indirect };
    // Static  = inmutable, subida en creación (geometría fija).
    // Dynamic = inmutable pero actualizable por subdata (tamaño fijo, p.ej. UBO/instancia).
    // Stream  = MUTABLE y reasignable (glBufferData): mallas dinámicas que cambian de tamaño cada frame.
    // Readback: la GPU escribe, la CPU LEE. Se mapea PERSISTENTE al crearlo, así leerlo es un
    // memcpy y no una llamada al driver (glGetBufferSubData sincroniza y cuesta ~100 µs cada una).
    // En Vulkan es literalmente lo mismo: memoria HOST_VISIBLE|HOST_CACHED mapeada una vez.
    enum class BufferMemory { Static, Dynamic, Stream, Readback };
    enum class ShaderStage { Vertex, Fragment, Geometry, Compute };
    enum class CompareOp { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
    enum class Filter { Nearest, Linear };
    enum class Wrap { Repeat, ClampToEdge, MirroredRepeat };

    // --- Memory barrier flags (GPU → CPU readback, SSBO after compute). ---
    // Bitmask, combinable con |. Mapea 1:1 a los bits de glMemoryBarrier / VkMemoryBarrier.
    enum BarrierBits : uint32_t {
        Barrier_Storage       = 0x00002000,  // GL_SHADER_STORAGE_BARRIER_BIT
        Barrier_Update        = 0x00000200,  // GL_BUFFER_UPDATE_BARRIER_BIT
        Barrier_Mapped        = 0x00004000,  // GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT
        Barrier_All           = 0xFFFFFFFF,  // GL_ALL_BARRIER_BITS
    };

    // -----------------------------------------------------------------------------------
    // Handles: un uint con TIPO. Reemplazan al `unsigned int ID` suelto de los wrappers GL.
    // id == 0 = vacío/inválido (como un puntero null). El backend traduce id -> objeto real.
    // -----------------------------------------------------------------------------------
    struct BufferHandle     { uint32_t id = 0; };
    struct TextureHandle    { uint32_t id = 0; };
    struct SamplerHandle    { uint32_t id = 0; };
    struct PipelineHandle   { uint32_t id = 0; };
    struct RenderPassHandle { uint32_t id = 0; }; // id 0 = backbuffer (framebuffer por defecto)
    struct FenceHandle     { uint32_t id = 0; };

    inline bool valid(BufferHandle h)     { return h.id != 0; }
    inline bool valid(TextureHandle h)    { return h.id != 0; }
    inline bool valid(SamplerHandle h)    { return h.id != 0; }
    inline bool valid(PipelineHandle h)   { return h.id != 0; }
    inline bool valid(RenderPassHandle h) { return h.id != 0; }
    inline bool valid(FenceHandle h)      { return h.id != 0; }
}
