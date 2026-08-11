/**
 * @file backend.glsl
 * @brief Macros de portabilidad GL ↔ Vulkan: UN mismo fuente .vert/.frag/.comp para los DOS
 *        backends, sin reescritura en runtime.
 *
 * ── POR QUÉ ───────────────────────────────────────────────────────────────────────────────────
 * GL tiene TRES espacios de binding SEPARADOS (UBO, SSBO, textura): un sampler puede ir a
 * binding 0 a la vez que un UBO a binding 0. Vulkan tiene UN solo espacio por descriptor set, y
 * el set COMPARTIDO del motor lo divide en rangos fijos (vk_common.h §4.3):
 *     UBO  : binding 0..31    → uboSlotToBinding(slot)   = slot
 *     SSBO : binding 32..63   → ssboSlotToBinding(slot)  = slot + 32
 *     textura: binding 64..95 → textureSlotToBinding(slot) = slot + 64
 * Estos macros convierten el número de slot que el motor usa (el MISMO en GL y VK) al binding
 * absoluto de cada backend. En GL se expanden a la identidad; en Vulkan desplazan al rango.
 * El preprocesador corre ANTES de glslang → `layout(binding = BIND_TEX(0))` se vuelve un
 * literal constante válido en los dos targets.
 *
 * ── BUILTINS ─────────────────────────────────────────────────────────────────────────────────
 * gl_VertexID/gl_InstanceID son los nombres de GL; SPIR-V (Vulkan) exige gl_VertexIndex /
 * gl_InstanceIndex. VERTEX_INDEX / INSTANCE_INDEX resuelven la discrepancia con el MISMO truco.
 *
 * ── CÓMO SE ACTIVA ────────────────────────────────────────────────────────────────────────────
 * La compilación de VULKAN pasa -DVULKAN (CMake: glslc --target-env=vulkan -DVULKAN). GL no la
 * define → rama #else. La rama #ifdef del glslangValidator runtime de emergencia también define
 * -DVULKAN (ver vk_pipeline.cpp). El backend GL nunca ve este símbolo.
 */
#ifndef HARUKA_BACKEND_GLSL
#define HARUKA_BACKEND_GLSL

#ifdef VULKAN
#  define BIND_UBO(slot)     ((slot))
#  define BIND_SSBO(slot)    ((slot) + 32)
#  define BIND_TEX(slot)     ((slot) + 64)
#  define VERTEX_INDEX       gl_VertexIndex
#  define INSTANCE_INDEX     gl_InstanceIndex
#else
#  define BIND_UBO(slot)     ((slot))
#  define BIND_SSBO(slot)    ((slot))
#  define BIND_TEX(slot)     ((slot))
#  define VERTEX_INDEX       gl_VertexID
#  define INSTANCE_INDEX     gl_InstanceID
#endif

#endif // HARUKA_BACKEND_GLSL
