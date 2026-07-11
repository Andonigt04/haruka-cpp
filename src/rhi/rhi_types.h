/**
 * @file rhi_types.h
 * @brief Tipos base del RHI: enums e identificadores. Sin lógica, sin GL, sin Vulkan.
 *
 * Este header es la "lengua franca" del RHI: solo define nombres. No incluye ninguna
 * API gráfica, así que puede incluirse desde cualquier parte sin acoplar el motor a GL/Vulkan.
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
    };

    enum class PrimitiveTopology { Triangles, TriangleStrip, Lines, LineStrip, Points };
    enum class BufferUsage { Vertex, Index, Uniform, Storage };
    // Static  = inmutable, subida en creación (geometría fija).
    // Dynamic = inmutable pero actualizable por subdata (tamaño fijo, p.ej. UBO/instancia).
    // Stream  = MUTABLE y reasignable (glBufferData): mallas dinámicas que cambian de tamaño cada frame.
    enum class BufferMemory { Static, Dynamic, Stream };
    enum class ShaderStage { Vertex, Fragment, Geometry, Compute };
    enum class CompareOp { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
    enum class Filter { Nearest, Linear };
    enum class Wrap { Repeat, ClampToEdge, MirroredRepeat };

    // -----------------------------------------------------------------------------------
    // Handles: un uint con TIPO. Reemplazan al `unsigned int ID` suelto de los wrappers GL.
    // id == 0 = vacío/inválido (como un puntero null). El backend traduce id -> objeto real.
    // -----------------------------------------------------------------------------------
    struct BufferHandle     { uint32_t id = 0; };
    struct TextureHandle    { uint32_t id = 0; };
    struct SamplerHandle    { uint32_t id = 0; };
    struct PipelineHandle   { uint32_t id = 0; };
    struct RenderPassHandle { uint32_t id = 0; }; // id 0 = backbuffer (framebuffer por defecto)

    inline bool valid(BufferHandle h)     { return h.id != 0; }
    inline bool valid(TextureHandle h)    { return h.id != 0; }
    inline bool valid(SamplerHandle h)    { return h.id != 0; }
    inline bool valid(PipelineHandle h)   { return h.id != 0; }
    inline bool valid(RenderPassHandle h) { return h.id != 0; }
}
