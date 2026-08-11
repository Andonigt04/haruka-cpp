# Plan del backend Vulkan (port desde OpenGL, por fases)

> Estado: **FASE 4 IMPLEMENTADA** (2026-08-07). El backend VK vive en `src/rhi/vulkan/`: fases 1-3
> hechas (`vk_device.{h,cpp}` con instance/device/swapchain/pipelines y pools con free-list),
> fase 4 hecha (`vk_pipeline.{h,cpp}` con compilación GLSL→SPIR-V vía glslang con shifts, descriptor
> set layout COMPARTIDO y builders gráfico/compute; `VKDevice::createPipeline` delega en ellos y
> usa el layout compartido). Pendiente: fase 5 (`VKContext`) y fases 6-7. Sin toolchain ni SDK Vulkan
> en esta máquina: el código se escribe por lectura contra la spec y el backend GL, **sin compilar**
> (los ficheros nuevos entran solos en el build vía `file(GLOB_RECURSE ... "src/*.cpp")`,
> `CMakeLists.txt:182`).

---

## 0. La regla de oro: un solo dueño del estado

El backend GL mantiene estado global GL (FBO actual, depth func, blend, polygon offset…) y se
sincroniza con `glDisable`/`glEnable` en cada `bindPipeline`. **Vulkan NO tiene ese problema**: el
estado vive INMUTABLE dentro del pipeline object, y el único estado en vivo es el command buffer que
se graba. Regla para todo el port:

- Todo lo que hoy es un `glEnable`/`glColorMask`/`glBlendFunc` en `bindPipeline` (gl_context.cpp:105-167)
  pasa a ser **campo de `VkGraphicsPipelineCreateInfo`** en la creación del pipeline. Cero estado vivo.
- El `VKContext` solo graba comandos y mantiene "qué hay bindeado" (pipeline, vertex/index buffers,
  descriptors, render pass activo) para poder hacer los **barriers implícitos** y la validación barata.
- Los pools de recursos del device (`m_buffers`, `m_textures`, …, con free-list y `handle.id = índice+1`)
  se REPLICAN tal cual: ya están esbozados en `vk_device.h:16-38` y son el patrón probado de `gl_device.h`.

Consecuencia de diseño: **un `VKDevice` + un `VKContext`**, no el par `GLDevice`+`GLContext` con el
estado copiado por valor (gl_context.h:50-60). El pool del device puede realocar libremente: el
context graba POR VALOR lo que necesita (punteros a `VkBuffer`/`VkPipeline` capturados en el momento
del bind), igual que `GLContext` copia `m_vao`/`m_strides`.

---

## 1. Fase 1 — `vk_common.h`: traducción de enums RHI → Vulkan

**Ficheros**: `src/rhi/vulkan/vk_common.h` (hoy 15 líneas, namespace vacío) + `vk_common.cpp` (0 B).
**Referencia**: `gl_common.h` (traducción GL ya verificada, 1:1 en casos).

Contenido (funciones `inline`, sin estado, como `gl_common.h`):

| Función | RHI (rhi_types.h) | Vulkan |
|---|---|---|
| `vertexFmt(Format)` | R32F, RG32F, RGB32F, RGBA32F, RGBA8, RGB10A2_SNORM, RG16F | `VkFormat` + si va normalizado (`VK_FORMAT_*_SNORM` ya lo lleva) |
| `texFmt(Format)` | RGBA8, SRGB8_ALPHA8, RGBA16F, RGB16F, RG16F, RGBA32F, RGB32F, RG32F, RGB8, R11G11B10F, R8, RG32UI, RGBA16UI, R32F, D24, D24S8, D32F | `VkFormat` (ej. `VK_FORMAT_R8G8B8A8_UNORM`, `VK_FORMAT_B10G11R11_UFLOAT_PACK32`, `VK_FORMAT_D32_SFLOAT`…) + `VkImageType` para 1D/2D/array/cube |
| `toTopology(PrimitiveTopology)` | Triangles, TriangleStrip, Lines, LineStrip, Points, Patches | `VkPrimitiveTopology::VK_PRIMITIVE_TOPOLOGY_*` (Patches → `PATCH_LIST`) |
| `toCompare(CompareOp)` | Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always | `VkCompareOp::VK_COMPARE_OP_*` |
| `toBlend(BlendMode)` | Alpha, Additive | `VkBlendFactor` srcAlpha/dst (Alpha→`ONE_MINUS_SRC_ALPHA`; Additive→`ONE`) |
| `toCull(CullMode)` | None, Back, Front | `VkCullModeFlagBits` (None→`VK_CULL_MODE_NONE`) |
| `toUsage(BufferUsage)` | Vertex, Index, Uniform, Storage, Indirect | bits `VkBufferUsageFlagBits` |
| `toWrap(Wrap)` | Repeat, ClampToEdge, MirroredRepeat | `VkSamplerAddressMode::VK_SAMPLER_ADDRESS_MODE_*` |
| `toStage(ShaderStage)` | Vertex, Fragment, Geometry, Compute | `VkShaderStageFlagBits` |

Decisión ya tomada en `rhi_types.h:45`: RGB10A2_SNORM ↔ `VK_FORMAT_A2B10G10R10_SNORM_PACK32`.

`vertexFmt` y `texFmt` devuelven `VkFormat` directo (Vulkan no tiene el trío internal/format/type de GL).
El default NO debe ser silencioso: replicar la política de `gl_common.h:35-38` (loguear `HARUKA_LOGE`
y devolver un formato de relleno).

**Hecho (ya en el header)**: include de `<vulkan/vulkan.h>` solo aquí y en los `.cpp` del backend —
nada de Vulkan fuera de `src/rhi/vulkan/` (rhi_device.h:1-5).

---

## 2. Fase 2 — `vk_swapchain.{h,cpp}`: surface + swapchain + imágenes + framebuffers

**Ficheros**: `src/rhi/vulkan/vk_swapchain.{h,cpp}` (hoy 0 B).
**Referencia**: `src/rhi/vulkan/vk_swapchain.cpp` (implementación definitiva; el spike `tests/vk_resize` se eliminó al quedar integrado).

Clase interna `VKSwapchain` que posee:
- `VkSurfaceKHR` (creada vía `SDL_Vulkan_CreateSurface`)
- `VkSwapchainKHR` + `std::vector<VkImage>` + `std::vector<VkImageView>` (formato BGRA8, image count
  = present mode mínimo)
- `std::vector<VkFramebuffer>` del backbuffer (uno por imagen) — el RenderPass se crea en la fase 3/4,
  pero el swapchain guarda las images/views y la clase de framebuffers se monta aquí.
- El extent: replicar `chooseExtent` de `vk_swapchain.cpp` (Wayland `currentExtent` = 0xFFFFFFFF
  → clamp del tamaño en píxeles de SDL al rango válido).
- Present mode: **`VK_PRESENT_MODE_FIFO_KHR`** (v-sync, el comportamiento actual de GL con
  `SDL_GL_SwapWindow`). FIFO_RELAXED/MAILBOX solo si se decide v-sync desacoplado.

**Decisión de N-buffering** (pendiente en el MEMO para `GPUInstancing`): la fase 2 entrega el swapchain
CON un solo set de framebuffers; el N-buffering de frames en vuelo se añade en la fase 5 (ver §6.1),
cuando exista `VKContext`. No adelantar aquí la sincronización.

---

## 3. Fase 3 — `vk_device.cpp`: instance / physical / logical device + recursos

**Ficheros**: `src/rhi/vulkan/vk_device.{h,cpp}` (hoy a medias; base = `vk_device.cpp:23-67`).
**Referencia**: `gl_device.cpp` (639 líneas) + `src/rhi/vulkan/vk_device.cpp` (setup del device).

### 3.1 Arranque (constructor)
Reescribir `VKDevice::VKDevice` y `createInstance` (vk_device.cpp:23-67) con el patrón del spike,
corrigiendo los fallos del WIP actual:
- `sdlExtensionCount` y `sdlExtensions` están SIN inicializar en `vk_device.cpp:36-37` → usar
  `SDL_Vulkan_GetInstanceExtensions(&nSdlExt)` como en `vk_device.cpp:101-103`.
- `VK_KHR_PORTABILITY_ENUMERATION`/`VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` son para
  macOS/MoltenVK; en Linux X11/Wayland sobran (pero no rompen). Dejarlos condicionados a plataforma
  Apple para no pagar ruido en la validación.
- `apiVersion`: usar `VK_API_VERSION_1_3` (spike) en vez de 1_0.
- Añadir la capa de validación `VK_LAYER_KHRONOS_validation` + `VK_EXT_DEBUG_UTILS_MESSENGER` cuando
  esté disponible (`vk_device.cpp:107-133`), gated por `#ifndef NDEBUG`.
- `pickPhysicalDevice`: seleccionar la GPU con `VK_QUEUE_GRAPHICS_BIT` + soporte de present para la
  surface (preferir la discreta si hay varias; loguear el `deviceName`).
- `createLogicalDevice`: `VK_KHR_SWAPCHAIN_EXTENSION_NAME`, una queue graphics/present (el backend usa
  una sola familia con ambas capacidades, vk_device.cpp:251-320).
- Guardar `m_graphicsQueue`/`m_graphicsFamily` (ya declarados en vk_device.h:121-122). El device
  Vulkan SIEMPRE es owned (no hay "adoptado" como en GL: SDL solo crea surface, no contexto VK).

### 3.2 Recursos — tabla equivalente a `gl_device.cpp:224-342`

| RHI | GL (`gl_device.cpp`) | Vulkan |
|---|---|---|
| `createBuffer` | named buffer + `glNamedBufferData` | `vkCreateBuffer` + memoria. **Static/Dynamic/Stream**: `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT`; **Readback**: `HOST_VISIBLE | HOST_CACHED` mapeada persistente (mismo contrato que `GL_MAP_PERSISTENT|COHERENT`, gl_device.cpp:234-244). Subida inicial: staging buffer + `vkCmdCopyBuffer` (o `HOST_VISIBLE` si no hay device-local) |
| `updateBuffer` | `glNamedBufferSubData` | `vkCmdCopyBuffer` staging (Dynamic/Stream), o write a memoria mapeada (Readback no aplica: la escribe la GPU) |
| `uploadBuffer` | `glNamedBufferData` (reasigna) | En VK el tamaño es fijo en creación → **recrear el buffer** (nuevo `VkBuffer`+memoria), copiar datos nuevos, actualizar el slot del pool. El handle NO cambia (la tabla apunta al nuevo objeto) |
| `copyBuffer` | `glCopyNamedBufferSubData` | `vkCmdCopyBuffer` (replicar `copyBuffer` de la fase 5, ejecutado fuera del frame con una submit de un solo uso, o grabar en el command buffer del frame siguiente) |
| `createTexture` | `glTextureStorage2D` + parámetros | `vkCreateImage` + memoria + `VkImageView`. Formato vía `texFmt`. `layers>1` → `VK_IMAGE_TYPE_2D` + `arrayLayers`. `cube` → `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT`. `mipmaps` → calcular niveles = `mipLevels()` (gl_device.cpp:177-180). `initialData` → staging + `vkCmdCopyImage` (o `vkCmdBlitImage` para generar mips) |
| `createSampler` | `glCreateSamplers` | `VkSamplerCreateInfo` (min/mag filter, addressMode, `maxAnisotropy`, `mipLodBias`, `mipmapMode = LINEAR` si hay mips) |
| `updateCubemapFace` | `glTextureSubImage3D` con cara como layer | staging + `vkCmdCopyImage` a `VkImageSubresourceLayers` con `layerCount=1`, `baseArrayLayer=face` (array cubemap: 6 caras = 6 layers) |
| `readPixels` | `glReadPixels` | copia de `VkImage` del frame actual a un buffer `HOST_VISIBLE` + `vkQueueWaitIdle` + memcpy. Solo RGBA8/RGB8/R32F, depuración |
| `mappedData` | `glMapNamedBufferRange` persistente | devolver el puntero del mapeo persistente (memoria `HOST_VISIBLE|HOST_CACHED`) |
| `nativeTexture/Framebuffer/Program/Buffer` | GLuint | En VK no hay "uint" nativo: devolver 0 (o el `uint64_t` de `VkImage`/`VkPipeline` truncado a 32 bits solo si el llamador lo necesita para algo). **Documentar en el código que son escotillas de la migración y que en VK no aplican**. Revisar llamadores: si alguno depende de estos para ImGui, hay que tratarlos aparte |

### 3.3 Render targets — `gl_device.cpp:452-532`

| GL | Vulkan |
|---|---|
| FBO + N color textures + depth RBO/textura/cubemap | `VkRenderPass` (descripción de attachments + subpass) + `VkFramebuffer`. Los attachments de color = `VkImageView` de imágenes propias (replicar `VKRenderTarget` en vk_device.h:35-38 con `std::vector<VkImageView> colorViews`, depth view, width/height) |
| `drawBufs` (MRT / NONE) | `colorAttachmentCount` + `pColorAttachments` en el subpass (0 en depth-only) |
| `depthCompare` (sampler de sombra, `GL_COMPARE_REF_TO_TEXTURE`) | `VkSamplerCreateInfo::compareEnable = VK_TRUE` + `compareOp = VK_COMPARE_OP_LESS_OR_EQUAL` (VK: el sampling en sombra usa `compareOp` del sampler) |
| `depthBorderClamp` (`CLAMP_TO_BORDER` blanco) | `VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER` + `borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE` |
| **Reversed-Z** (rhi_resources.h:79-104, 186-194) | El clear depth default = **0.0** (lejano). El depth compare default = `GREATER` (reversed-Z). Los pases con proyección no invertida (shadow/IBL) piden `Less`/`LessEqual` y clear a 1.0 — igual que GL, pero aquí se congela EN el pipeline, sin `glClearDepth` dinámico |

**RenderPass dedicado por combinación**: en VK un render pass se crea una vez por firma de attachments
+ loadOp/clearOp. Se cachea por (colorFormats, hasDepth, depthFormat) — en la práctica el motor tiene
pocas firmas (backbuffer, HDR, shadow, water, IBL). El `VKRenderTarget` referencia su render pass
compartido (owned por el device, no por el target).

### 3.4 Los métodos que hoy son stubs
`vk_device.cpp:69-72` solo define `createBuffer` con cuerpo vacío. El resto (`updateBuffer`, `uploadBuffer`,
`copyBuffer`, `createTexture`, `createSampler`, `createPipeline`, `createRenderTarget`, `getColorTexture`,
`getDepthTexture`, `native*`, `mappedData`, `updateCubemapFace`, todos los `destroy`, `beginFrame`,
`endFrame`, `readPixels`) **ni siquiera están declarados** en el .cpp — la clase declara los virtuals en
el header pero no hay definiciones → si se compila con `HARUKA_RHI_HAS_VULKAN` y un `VKDevice` concreto,
linkaría igual (el factory solo lo instancia si `ready()`), pero un `VKDevice` es clase abstracta hasta
que existan. La fase 3 completa la implementación entera de la clase.

**Orden interno**: los `destroy()` de VK borran el `VkBuffer`/`VkImage`/`VkSampler`/`VkPipeline`/
`VkFramebuffer`/`VkRenderPass` y reciclan el slot (misma free-list que `gl_device.cpp:578-602`).

---

## 4. Fase 4 — `vk_pipeline.{h,cpp}`: compilación SPIR-V + graphics/compute pipelines

**Ficheros**: `src/rhi/vulkan/vk_pipeline.{h,cpp}` (hoy 0 B).
**Referencia**: `gl_device.cpp:344-450` (createPipeline) + `src/rhi/vulkan/vk_pipeline.cpp` (pipeline mínimo).

### 4.1 Shaders
- `createPipeline(PipelineDesc)` recibe: rutas (`*.spv`), bytes SPIR-V directos, o GLSL source.
- **GLSL source en línea** (`vertexSource`/`fragmentSource`/`computeSource`): Vulkan NO compila GLSL.
  El backend deberá: escribir el source a un fichero temporal `$TMP/haruka_<hash>.spv` invocando
  `glslangValidator` en el primer uso con caché por hash → leer el `.spv`. Documentar que requiere
  glslang en el PATH en dev (es el mismo toolchain que ya usa el build de shaders del motor).
- GLSL → SPIR-V target: `-V -S <stage>`. ⚠️ **NO reutilizar el `.spv` del build del motor**
  (`CMakeLists.txt:404-418`): ese se genera con `--target-env=opengl` / `-G` para el fallback GL
  (gl_device.cpp:133-142), NO es consumible por Vulkan. El `.spv` VK se genera con `-V` y **con los
  shifts de binding de §4.3** (`--shift-ssbo-binding 32 --shift-combined-sampler-binding 64`).
- Los shaders del motor ya son compatibles en gran parte (inicializadores constantes,
  `layout(location=)`, no `gl_*` específicos); el `#include` textual se resuelve ANTES igual que en GL
  (`resolveIncludes`, gl_device.cpp:47-98) — para VK hay que **eliminar** la extensión
  `GL_GOOGLE_include_directive` y usar el `#include` de glslang vía `-I` si se quiere, o dejar el
  resolver textual (más simple y ya probado).
- `VkShaderModule` por etapa desde los bytes `.spv` (una por stage; se destruyen tras crear el pipeline).

### 4.2 Graphics pipeline — TODO el estado de `bindPipeline` GL es aquí INMUTABLE
`gl_context.cpp:105-167` (depth func/write, bias, blend, cull, patchVertices, point size) se traduce a:

| Estado GL | `VkGraphicsPipelineCreateInfo` |
|---|---|
| `glDepthFunc`/`glDepthMask` | `VkPipelineDepthStencilStateCreateInfo::depthTestEnable/depthWriteEnable/depthCompareOp` |
| `glPolygonOffset` | `depthBiasEnable` + `depthBiasConstantFactor`/`depthBiasSlopeFactor` (**reversed-Z: factor positivo**, rhi_resources.h:99-103) |
| `glBlendFunc` | `VkPipelineColorBlendAttachmentState` por attachment (enable, srcColorFactor, dstColorFactor) |
| `glCullFace` | `VkPipelineRasterizationStateCreateInfo::cullMode` |
| `glPatchParameteri` | `VkPipelineTessellationStateCreateInfo::patchControlPoints` (solo topology Patches) |
| `gl_PointSize` habilitado | No existe en VK: el size se escribe en el shader (vertex) siempre — sin `glEnable` |
| VAO (atributos + strides + inputRate) | `VkVertexInputBindingDescription[]` (binding, stride, inputRate de `VertexLayout::rates`) + `VkVertexInputAttributeDescription[]` (location, format, offset, binding). **Directo de `VertexLayout`** (rhi_resources.h:63-74) |
| topology | `VkPipelineInputAssemblyStateCreateInfo::topology` |

### 4.3 Pipeline layout + descriptor sets — ⚠️ LOS BINDINGS COLISIONAN (hallazgo verificado)
Vulkan exige declarar los recursos que el shader usa. El motor usa slots numéricos:
`bindUniformBuffer(slot)`, `bindStorageBuffer(slot)`, `bindTexture(slot, tex, sampler)`.

**Hallazgo (auditoría de `assets/shaders/`, 2026-08-07)**: GL tiene TRES espacios de binding
SEPARADOS (UBO, SSBO, textura — `glBindBufferBase(GL_UNIFORM_BUFFER, slot)` /
`GL_SHADER_STORAGE_BUFFER` / `glBindTextureUnit(slot)`, gl_context.cpp:188-202), así que los
shaders reutilizan el mismo número de slot libremente entre tipos. **Vulkan tiene UN solo espacio
de binding por descriptor set** → la estrategia "un descriptor set con binding = slot" del plan
original es INVÁLIDA. Colisiones reales dentro de UN shader (una sola fuente GLSL):

| Shader | Colisión |
|---|---|
| `terrain_gen.comp` | SSBO `binding=0` (DirsIn) + UBO `binding=0` (TerrainGenParams) |
| `planet.frag` | UBO `binding=8` (TerrainParams) + sampler `binding=8` (u_rockAlbedo) |
| `planet.frag` | SSBO `binding=9/10` + sampler `binding=9/10` (u_rockNormal/u_macroVar) |
| `construction_inst.frag` | UBO `binding=0` (PerFrameData) + sampler `binding=0` (u_matAlbedo) |
| `final.frag` | UBO `binding=0/1` + sampler `binding=0/1` (u_matAlbedo/u_matNormal) |
| `fluid_surface.frag` | UBO `binding=0` (PerFrameData) + sampler `binding=0` (u_depth) |
| `precip.vert` | UBO `binding=0` (PerFrameData) + sampler `binding=0` (u_skyMask) |

Máximos usados (para dimensionar): UBO hasta `21` (tidal.glsl), SSBO hasta `10`, sampler hasta `17`.
Además, `pbr.frag`, `prefilter_env.frag`, `equirect_to_cubemap.frag`, `irradiance_convolution.frag`
ya declaran `layout(set = 0, binding = N)` explícito (se respeta tal cual; set 0).

**Decisión — re-encode en compilación, NO editar los shaders**: el GLSL fuente se deja intacto
(GL vive de ese source, gl_device.cpp:114-142, y en GL los tres espacios SÍ son separados). Para
Vulkan, al compilar GLSL→SPIR-V se aplica un **shift de binding por tipo de recurso** con las flags
de glslangValidator (verificado en StandAlone.cpp: `ProcessBindingBase` y en
`TDefaultGlslIoResolver::resolveBinding` — el shift se SUMA al binding explícito del source):
- `--shift-ubo-binding 0` → UBO queda en `binding = slot`
- `--shift-ssbo-binding 32` → SSBO pasa a `binding = slot + 32`
- `--shift-combined-sampler-binding 64` → texturas pasan a `binding = slot + 64`
  (⚠️ en glslang moderno `--shift-texture-binding` ya NO afecta a los combined samplers `sampler2D`:
  hay que usar `--shift-combined-sampler-binding`, README de glslang; si hay texturas sueltas, ambas).
  Como GL no tiene `layout(set=...)` en su GLSL, NO se toca el source: el shift es solo en el `.spv`.

Consecuencia en el layout VK (shared, acordado una vez, todos los pipelines):
- **Descriptor set 0** con bindings 0..31 UBO, 32..63 SSBO, 64..95 texture
  (`VkDescriptorSetLayoutBinding` por binding, `descriptorType = UNIFORM_BUFFER` / `STORAGE_BUFFER` /
  `COMBINED_IMAGE_SAMPLER`), TODOS con `DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT` (un slot sin binding
  no invalida el set).
- El `VKContext` traduce el slot RHI → binding VK igual que glslang: `bindUniformBuffer(slot)` →
  binding `slot`; `bindStorageBuffer(slot)` → `slot+32`; `bindTexture(slot)` → `slot+64`.
  Esta constante tiene que ser la MISMA en glslang y en el backend → se define una vez en
  `vk_common.h` (p.ej. `constexpr uint32_t kSsboBindingBase = 32; kTexBindingBase = 64;`).
- El pool de descriptor sets lo crea el device con tamaño suficiente (crecimiento bajo demanda).
- `VkDescriptorUpdateTemplate` NO necesario: updates con `vkUpdateDescriptorSets` (set idéntico cada
  frame, se reescribe lo que cambió). N-buffering (§6.1) decide si por-frame.

**Verificación**: tras re-encodear, `spirv-dis`/`glslangValidator -V` debe mostrar Decorations Binding
disjuntos por tipo; un `--shift-*` mal aplicado da "binding collision" en `vkCreatePipelineLayout` o
en la validación. Los `layout(set=0, binding=N)` ya existentes siguen siendo binding N en set 0
(no se les aplica shift — son de los shaders de PBR/IBL que NO mezclan tipos por slot).

### 4.4 Compute
`PipelineDesc` con solo `computePath`/`spirvCompute`/`computeSource` → `VkComputePipelineCreateInfo`
con el mismo pipeline layout. Sin estado de rasterizado ni de mezcla (gl_device.cpp:360-371).

### 4.5 Implementación (hecho, 2026-08-07)
- `vk_pipeline.{h,cpp}` (guard `HARUKA_RHI_HAS_VULKAN`): `compileGlslToSpv` (glslangValidator `-V -S`
  con `--shift-ssbo-binding 32 --shift-combined-sampler-binding 64 --shift-texture-binding 64` +
  caché por hash FNV-1a en `/tmp`), `loadStageSpv` (prioridad GLSL inline > ruta (solo source, el
  `.spv` del build es GL) > bytes), `createSharedSetLayout` (96 bindings, PARTIALLY_BOUND con
  fallback sin el flag si el HW no lo soporta), `createSharedPipelineLayout`, `buildGraphicsPipeline`
  (estado inmutable portado de `gl_context.cpp:105-167` y del `createPipeline` de fase 3) y
  `buildComputePipeline`. Los módulos se crean y destruyen dentro de los builders.
- `VKDevice`: habilitados `descriptorBindingPartiallyBound` + `runtimeDescriptorArray` en
  `createLogicalDevice` (VkPhysicalDeviceVulkan12Features en el pNext). El set + pipeline layout
  COMPARTIDOS (`m_setLayout`/`m_pipelineLayout`) se crean perezosamente en el primer `createPipeline`
  y se liberan en el dtor; `destroy(PipelineHandle)` ya NO libera el layout (compartido). Los
  pipelines gráficos se crean contra el render pass del backbuffer — las variantes por target
  específico son de la fase 5 (VKContext).

---

## 5. Fase 5 — `VKContext`: grabado de command buffers + render pass + frame

**Ficheros**: nuevo `src/rhi/vulkan/vk_context.{h,cpp}`.
**Referencia**: `gl_context.cpp` (300 líneas) + `src/rhi/vulkan/vk_context.cpp` (cmd pool/buffers, sync).

`VKContext : RHI::Context` con `VKDevice*` (igual que `GLContext` guarda `GLDevice*`).

### 5.1 Frame
- `beginFrame()` (en `VKDevice`): `vkAcquireNextImageKHR` (→ imageIndex), `vkResetCommandBuffer`,
  `vkBeginCommandBuffer`, devuelve `VKContext`.
- `endFrame()`: `vkEndCommandBuffer`, `vkQueueSubmit` (imagen + wait en `VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT`),
  `vkQueuePresentKHR`. Devuelve/rehace la fence de la imagen.
- `endRenderPass()` → `vkCmdEndRenderPass`.
- El `VKDevice` mantiene: cmd pool (`RESET_COMMAND_BUFFER_BIT`), N command buffers, semáforos
  imageAvailable/renderFinished, una fence por imagen en vuelo (frame-in-flight).

### 5.2 Traducción de comandos (1:1 con `gl_context.cpp`)

| RHI Context | GL (`gl_context.cpp`) | Vulkan |
|---|---|---|
| `beginRenderPass` | bind FBO + clear | `vkCmdBeginRenderPass` (RenderPass del target, framebuffer, clear values — depth clear **0.0** reversed-Z) |
| `beginRenderPassCubemapFace` | FBO temporal + attach a cara | `vkCmdBeginRenderPass` sobre un framebuffer temporal con la image view de la cara+mip (el device cachea un framebuffer por (cara, mip) si hace falta, o recrea el del cubemap por cara) |
| `setViewport` | `glViewport` | `vkCmdSetViewport` (VK: 0..1 origin — compensar el y-flip: GL viewport y origen en la esquina inferior → VK usa top-left; los shaders del motor asumen GL, así que `viewport.y` se invierte y, si hace falta, `VK_KHR_MAINTENANCE_1` + `negativeViewportHeight` para los targets) |
| `clear` | `glClearColor`+`glClear` | dentro de pass: `vkCmdClearAttachments` |
| `bindPipeline` | estado + `glUseProgram` | `vkCmdBindPipeline` (VK_PIPELINE_BIND_POINT_GRAPHICS/COMPUTE) |
| `bindVertexBuffer` | `glVertexArrayVertexBuffer` | `vkCmdBindVertexBuffers(binding, 1, &buffer, &offset=0)` (el stride ya está en el pipeline) |
| `bindIndexBuffer` | `glVertexArrayElementBuffer` | `vkCmdBindIndexBuffer` (VK_INDEX_TYPE_UINT32 — el motor usa uint32) |
| `bindUniformBuffer` / `bindStorageBuffer` | `glBindBufferBase` | escribir el descriptor (UBO → `UNIFORM_BUFFER`, SSBO → `STORAGE_BUFFER`) en el set y `vkCmdBindDescriptorSets` |
| `bindTexture` | `glBindTextureUnit`+`glBindSampler` | descriptor `COMBINED_IMAGE_SAMPLER` (imageView del slot + sampler) |
| `draw` / `drawIndexed` | `glDrawArrays/Element Instanced` | `vkCmdDraw(vertexCount, instanceCount, firstVertex, 0)` / `vkCmdDrawIndexed(indexCount, instanceCount, 0, first, 0)` |
| `drawIndexedIndirect` | `glMultiDrawElementsIndirect` | `vkCmdDrawIndexedIndirect(buffer, offset, drawCount, stride)` (stride obligatorio; `offset` en bytes) |
| `dispatch` | `glDispatchCompute`+barrier | `vkCmdDispatch(x,y,z)` — **sin barrier automático**: se mueve al `memoryBarrier`/fences explícito (ver §5.3) |
| `blitDepth`/`blitColor` | `glBlitFramebuffer` | `vkCmdBlitImage` entre las imágenes de los targets (con `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL/DST_OPTIMAL` + barrier de layout) |
| `memoryBarrier` | `glMemoryBarrier(GL_ALL)` | `vkCmdPipelineBarrier` (SSBO → `VK_ACCESS_SHADER_WRITE_BIT`→`SHADER_READ_BIT`, storage stages) |
| `signalFence` | `glFenceSync` | `vkQueueSubmit(1, {}, fence)` + `VKFence{fence}` en el pool del device (vk_device.h:40) |
| `waitFence` | `glClientWaitSync` | `vkGetFenceStatus` (timeout 0) / `vkWaitForFences` (timeout>0); UINT64_MAX → `VK_TIMEOUT` nunca (espera infinita) |
| `deleteFence` | `glDeleteSync` | `vkDestroyFence` + release del slot |

### 5.3 Barreras — el trabajo real de VK
Donde GL barría bajo la alfombra (`glMemoryBarrier(GL_ALL)`), VK exige barreras explícitas. Las tres
obligatorias en el motor:
1. **Layout transitions** al empezar/terminar un render pass (imagen → `COLOR_ATTACHMENT_OPTIMAL`, y al
   blit → `TRANSFER_*`). El `VKContext` las emite en `beginRenderPass`/`endRenderPass`/`blit*`.
2. **Pipeline barriers** después de compute (`dispatch`) y antes de leer SSBO (mismo papel que
   `glMemoryBarrier`, gl_context.cpp:245-249, pero por stage/access concretos).
3. **Depth readback / readPixels**: `VK_ACCESS_TRANSFER_READ` tras el render, y `vkQueueWaitIdle`
   antes del memcpy.

---

## 6. Decisiones transversales y riesgos

### 6.1 N-buffering (pendiente del MEMO, `GPUInstancing`)
El GL actual es de un solo framebuffer y `glBufferSubData` por-slot con el driver serializando. VK
permite 2-3 frames en vuelo, pero **esto toca el modelo de datos**: `updateBuffer` escribe en el frame
actual y el siguiente draw lo lee. Con N-buffering, `VKBuffer` pasa a `VkBuffer[N]` (o se dobla el
staging) y el descriptor por-frame. **Decisión**: la fase 5 arranca con 1 frame en vuelo (paridad GL,
simple, correcto); el N-buffering se evalúa después como optimización medible, no al portar. El diseño
deja el hook (índice de frame en `VKDevice`).

### 6.2 Un solo dueño del estado (§0) — revisión de cada `bind*`
En GL, `bindTexture` llama `glBindTextureUnit` y listo. En VK, un descriptor set se RELLENA con
`vkUpdateDescriptorSets` ANTES del draw. El contexto mantiene "qué imagen/sampler hay en cada slot"
para el momento en que se escriba el set (post-`bindPipeline` o al final del frame, con un solo update
por frame — más barato). El orden de los `bind*` del renderer no debe importar: **se actualizan
perezosamente antes del primer draw**.

### 6.3 Reversed-Z y y-flip — las dos trampas del port
- Reversed-Z (rhi_resources.h:79-104, ClearValues.depth=0.0): ya resuelto en el diseño (compare Greater,
  clear 0.0). Es la que MÁS "nada se ve" produce si se olvida.
- Vulkan rasteriza con origen arriba-izquierda; GL abajo-izquierda. Los shaders del motor usan
  `gl_FragCoord`/`gl_Position` GL-style. Opción limpia: `VkViewport::y = -height`, `height = -height`
  (maintenance1), o invertir las coordenadas en el viewport y ajustar `VkViewport` con flip
  (`VK_KHR_MAINTAINANCE1`). Decidir en fase 5 al ver el primer frame; documentar el choque con
  `readPixels` (el readback de GL devuelve filas abajo→arriba).

### 6.4 Los `native*` no aplican a VK
`nativeTexture/Framebuffer/Program/Buffer` devuelven GLuints para ImGui y el ensamblado VAO durante la
migración (rhi_device.h:59-73). En VK: ImGui tiene un `VkDescriptorSet`/atlas propio, así que esos
handles no le sirven. **Revisar llamadores** (`imgui`, `texture.cpp`, `renderer/*`) durante la fase 3:
los que solo piden el id para bindearlo a GL hay que portarlos a slots RHI puros. Si un llamador
obstaculiza el port, es candidato a extraerse del camino de VK (como GL lo hizo con `main.cpp`).

### 6.5 Riesgos de sincronización
- `updateBuffer` en mitad del frame con la GPU aún leyendo el buffer anterior → puede hacer que el
  draw de este frame vea datos nuevos ya validados (hazlo en fase 5: escribe el staging al principio
  y copia ANTES del draw, o dobla el buffer).
- Readback (`Readback` + `mappedData`): VK exige `vkQueueWaitIdle` (o fence) antes de leer el mapeo
  persistente — igual que GL exige esperar la fence del `glFenceSync`. El contrato de `rhi_device.h:45-47`
  ya lo documenta.

### 6.6 Qué NO se porta
- El resolver de `#include` GLSL sí se reutiliza (gl_device.cpp:47-98) para GLSL-source, pero la
  "Opción A: GLSL-first" del GL (fiabilidad Mesa/AMD) **no aplica a VK**: Vulkan SOLO come `.spv`, así
  que el pipeline de shaders del build del motor debe producir `.spv` para VK (el mismo `glslangValidator`
  que ya valida `assets/shaders/`).

---

## 7. Orden de escritura y criterio de "hecho" por fase

| Fase | Entrega | Criterio |
|---|---|---|
| 1 | `vk_common.h` completo | Todos los enums traducidos; compila aislado (sin `HARUKA_RHI_HAS_VULKAN`) |
| 2 | `VKSwapchain` | Crea surface/swapchain/views/framebuffers del backbuffer; destruye todo |
| 3 | `VKDevice` completo | Implementa TODOS los virtuals de `Device`; `ready()` real; pools + free-lists; la clase deja de ser abstracta |
| 4 | `VKPipeline` + shader modules | Graphics + compute + teselación + descriptor layout compartido **con bindings shift-eados** (§4.3) — `spirv-dis` muestra Decorations Binding disjuntos por tipo |
| 5 | `VKContext` + frame | Presenta el primer frame; command buffer grabado; fences/semáforos; barreras |
| 6 | Integración | `Device::create(Vulkan)` vuelve a `ready()` en esta máquina con SDK; el juego arranca con VK y cae limpio a GL si no |

Cada fase termina con su fichero **compilable y sin warnings** aunque no se pueda verificar aquí
(sin toolchain). El `HARUKA_RHI_HAS_VULKAN` gate ya aísla el backend: sin SDK, nada de esto se
compila y el motor sigue 100% GL.

---

## 8. Referencias
- `src/rhi/vulkan/vk_swapchain.cpp` / `vk_device.cpp` / `vk_context.cpp` — setup de instance/surface/
  device/swapchain probado (el spike `tests/vk_resize` se eliminó al quedar integrado en el backend).
- `src/rhi/opengl/gl_common.h` — traducciones GL de referencia (1:1 con fase 1).
- `src/rhi/opengl/gl_device.cpp` / `gl_context.cpp` — el comportamiento a replicar.
- `src/rhi/rhi_device.h` / `rhi_context.h` / `rhi_resources.h` / `rhi_types.h` — API FROZEN.
- `src/rhi/vulkan/vk_device.h` — esqueleto de la clase VKDevice + pools (base de fase 3).
