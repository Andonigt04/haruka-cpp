#pragma once
/**
 * @file PropGpu.h
 * @brief Lo que comparten en la GPU los dos pases de vegetación/objetos del scatter: `PropSystem`
 *        (instancias: árboles, rocas, casas, cúmulos) y `GrassSystem` (la hierba en masa: brotes de
 *        hierba, flores, líquenes). Mismo vértice de prototipo, mismo UBO, misma malla horneada.
 *
 * Vivían dentro de PropSystem.cpp. La capa masiva NO es un prop —es otro sistema con su propio
 * dibujo y su propio estado— pero dibuja las MISMAS mallas de prototipo con el MISMO fragment
 * (`prop_inst.frag`), así que estos tipos tienen que ser uno solo: si el vértice de uno divergiera
 * del otro, la hierba leería la normal de los bytes del color.
 */
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>

#include "Haruka/RHI/RHITypes.h"
#include "Haruka/RHI/RHIResources.h"

namespace Haruka::World::Props {

/// UBO de los pases de prototipo (binding 6). Gemelo de `assets/shaders/lib/prop_params.glsl`.
/// Los escalares del material son del PROTOTIPO (compartido por sus instancias), por eso viven aquí.
struct PropParams {
    glm::vec3 wind;   float time;       // viento del clima (mundo, m/s) · segundos
    glm::vec4 matPBR;                   // x=metallic y=roughness z=ao w=mascara de texturas (bits)
    glm::vec4 originRel;                // origen del scatter relativo a la camara
    glm::vec4 aerial;                   // perspectiva aerea (lib/aerial.glsl): x = 1/L · y = dia
    glm::vec4 weather;                  // nieve acumulada · suelo mojado · lluvia · nevada
    glm::vec4 flags;                    // x = 1 en Vulkan (prop_burst.vert: quién suma firstInstance)
};
static_assert(sizeof(PropParams) == 96, "PropParams std140 size mismatch (gemelo: lib/prop_params.glsl)");

// Vertice del PROTOTIPO (binding 0): Pos(0)/Normal(1)/Color(2)/Uv(9)/Parte(11). El `Vertex` de
// escena no tiene canal de color, asi que el prototipo usa su propio layout (el Uv a 9 no choca con
// el stream de instancia que ocupa loc 3-8; la 10 es la mascara de roturas, por instancia).
//
// EMPAQUETADO (32 B, antes 48): la normal en 10/10/10/2 con signo y el color en RGBA8 con una ESCALA
// por vértice en el alfa (`rgb × a·4`). La posición y la UV siguen en float: la posición es la del
// prototipo sin más precisión que perder, y la UV no tiene un rango acotado (puede repetir la
// textura muchas veces) — en half se escalonaría.
struct PropVertex {
    glm::vec3 pos;
    uint32_t  normal;   ///< RGB10A2_SNORM (glm::packSnorm3x10_1x2: x en los bits bajos = GL_INT_2_10_10_10_REV)
    uint32_t  color;    ///< RGBA8: rgb / escala, a = escala / 4 (los vertex shaders lo deshacen)
    glm::vec2 uv;
    /// PARTE del esqueleto (tronco = 0, ramas = 1..n). Va como float: el valor es un indice pequeño
    /// y `float` lo representa exacto; como PATRON DE BITS en un R32F caeria en los denormales.
    float partId = 0.0f;
};
static_assert(sizeof(PropVertex) == 32, "PropVertex: el layout empaquetado son 32 B");

/// Atributos del vértice de prototipo (binding 0). `withPart` = también la parte del esqueleto
/// (loc 11): la usan los props (ramas que se rompen); la capa masiva no.
inline void appendPropVertexLayout(RHI::VertexLayout& layout, bool withPart) {
    if (layout.strides.empty()) layout.strides.resize(1);
    if (layout.rates.empty())   layout.rates.resize(1, RHI::InputRate::Vertex);
    layout.strides[0] = (uint32_t)sizeof(PropVertex);
    layout.rates[0]   = RHI::InputRate::Vertex;
    layout.attributes.push_back({ 0, (uint32_t)offsetof(PropVertex, pos),    RHI::Format::RGB32F, 0 });
    layout.attributes.push_back({ 1, (uint32_t)offsetof(PropVertex, normal), RHI::Format::RGB10A2_SNORM, 0 });
    layout.attributes.push_back({ 2, (uint32_t)offsetof(PropVertex, color),  RHI::Format::RGBA8, 0 });
    layout.attributes.push_back({ 9, (uint32_t)offsetof(PropVertex, uv),     RHI::Format::RG32F, 0 });
    if (withPart)
        layout.attributes.push_back({ 11, (uint32_t)offsetof(PropVertex, partId), RHI::Format::R32F, 0 });
}

/// Malla horneada de un prototipo + su material PER-PIXEL. La hornea y la guarda `PropSystem`; la
/// capa masiva la pide prestada (no la duplica: misma hierba, misma malla).
struct PrototypeGpu {
    /// NIVELES DE DETALLE. Medido con RenderDoc, los arboles del scatter eran el 85,8 % de los
    /// triangulos del frame y el instancing no lo arregla: ahorra draw calls, no vertices. El
    /// material es COMPARTIDO por los niveles (mismo prototipo, solo cambia la densidad).
    static constexpr int kLods = 3;
    RHI::BufferHandle vbo[kLods] = {}, ebo[kLods] = {};
    uint32_t vertexCount[kLods] = {0, 0, 0};
    uint32_t indexCount[kLods]  = {0, 0, 0};
    RHI::TextureHandle albedo = {}, normal = {}, metallic = {}, roughness = {}, ao = {};
    float    metallicS  = 0.0f, roughnessS = 0.5f, aoS = 1.0f;
    uint32_t mask       = 0u;       ///< bits de texturas presentes (mismo esquema que prop_inst.frag)
};

} // namespace Haruka::World::Props
