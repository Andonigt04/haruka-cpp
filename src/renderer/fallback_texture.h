#pragma once
/**
 * @file renderer/fallback_texture.h
 * @brief Texturas de relleno 1x1 para los slots de material ausentes. Compartidas por los pases
 *        que atan los cinco slots (props, piezas de construccion): en Vulkan un descriptor sin
 *        atar es basura, asi que siempre se ata ALGO.
 *
 * ⚠️ EL VALOR NEUTRO NO ES EL MISMO EN TODOS LOS SLOTS, y ponerlo mal se ve. Al principio se relleno
 * todo con BLANCO, y en albedo o AO es neutro pero en METALLIC blanco significa **metal puro**:
 * cualquier prop con ese slot ausente se convertia en un espejo. Los neutros correctos son:
 *     albedo    -> blanco  (multiplica por 1)
 *     normal    -> (0.5, 0.5, 1) = normal plana en espacio tangente
 *     metallic  -> NEGRO   (0 = dielectrico; blanco = metal)
 *     roughness -> blanco  (1 = totalmente rugoso, sin especular de espejo)
 *     ao        -> blanco  (1 = sin oclusion)
 */
#include "rhi/rhi_types.h"
#include "rhi/rhi_resources.h"

namespace Haruka::Renderer {

enum class FallbackTex { White = 0, Normal, Metallic, Count };

/// Perezosa y cacheada por proceso (viven mientras viva el device).
Haruka::RHI::TextureHandle fallbackTexture(FallbackTex kind);

} // namespace Haruka::Renderer
