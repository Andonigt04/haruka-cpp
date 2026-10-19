# RHI: OpenGL y Vulkan {#guia_rhi}

Una sola interfaz de GPU para todo el motor, con dos backends detrás: OpenGL 4.6 y Vulkan. El código de los pases no sabe con cuál corre; el backend es un ajuste y se puede forzar por ejecución para comparar los dos.

## Cómo funciona

- **`Device`** vive todo el programa: crea y destruye buffers, texturas, samplers, pipelines y render targets, y gobierna el frame. `Device::create` elige `GLDevice` o `VKDevice`.
- **`Context`** son los comandos de **un** frame (`beginRenderPass`, binds, draws, dispatch). En GL se ejecutan al momento; en Vulkan se graban en el command buffer del frame. Quien llama no nota la diferencia.
- **Los recursos se piden con "recetas"** (`RHIResources.h`): structs de datos sin lógica que se rellenan y se pasan a `create*`. Los handles son índices en tablas del backend.
- **GL y VK solo existen dentro de su carpeta.** `<glad/glad.h>` y `<vulkan/vulkan.h>` se incluyen únicamente en `RHI/OpenGL/` y `RHI/Vulkan/`; fuera no hay ni una constante de GL o VK.
- **Shaders**: GLSL con `#include` (raíz `assets/shaders/lib/`). En Vulkan se compilan a SPIR-V con glslang y todos los pipelines comparten **un** descriptor set layout, así los bindings son los mismos números en los dos backends.
- **El backend se decide antes de crear la ventana**, porque el tipo de ventana (Vulkan u OpenGL) depende de él.
- **`HARUKA_GPU_SCOPE("nombre")`** mide un tramo de GPU con timestamps (RAII, anidable). Se pone por pase, no por draw.
- La API de `RHIDevice.h`, `RHIContext.h`, `RHIResources.h` y `RHITypes.h` está **congelada**: no se rompe sin subir versión mayor.

## Archivos principales

| Archivo | Qué hace |
|---|---|
| `src/Haruka/RHI/RHIDevice.h` | Crea recursos, gobierna frame |
| `src/Haruka/RHI/RHIContext.h` | Comandos de un frame |
| `src/Haruka/RHI/RHIResources.h` | Recetas de recursos |
| `src/Haruka/RHI/RHITypes.h` | Enums y handles comunes |
| `src/Haruka/RHI/RHIGPUScope.h` | Medición de GPU por pase |
| `src/Haruka/RHI/OpenGL/GLDevice.{h,cpp}` | Backend OpenGL |
| `src/Haruka/RHI/Vulkan/VKDevice.{h,cpp}` | Backend Vulkan |
| `src/Haruka/RHI/Vulkan/VKContext.{h,cpp}` | Graba el command buffer |
| `src/Haruka/RHI/Vulkan/VKPipeline.{h,cpp}` | GLSL→SPIR-V y pipelines |
| `src/Haruka/RHI/Vulkan/VKSwapchain.{h,cpp}` | Surface y swapchain |
| `cmake/HarukaShaders.cmake` | Copia y compila shaders |

## Opciones

| Opción | Efecto |
|---|---|
| Ajuste `RenderBackend` (`imgui.ini`) | Backend por defecto (requiere reinicio) |
| `HARUKA_BACKEND=vulkan\|opengl` | Fuerza backend sin tocar ajustes |
| `HARUKA_VK_GPU=<n>` | Qué GPU usa Vulkan |
| `HARUKA_VK_FRAMES=<n>` | Frames en vuelo de Vulkan |
| `HARUKA_NO_VSYNC=1` | Sin vsync en Vulkan |
| `HARUKA_GL_SPIRV=1` | GL carga SPIR-V en vez de GLSL |
| `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` | Capas de validación de Vulkan |

El backend Vulkan solo se compila si se encuentra `<vulkan/vulkan.h>`.

## Cómo usarlo

Dibujar desde un pase:

```cpp
auto* dev = Haruka::RHI::device();
Haruka::RHI::PipelineDesc pd{};          // shaders, vértices, depth, blend…
auto pipe = dev->createPipeline(pd);

Haruka::RHI::Context* ctx = /* el del frame */;
HARUKA_GPU_SCOPE("mi_pase");
ctx->beginRenderPass(target, clear);
ctx->bindPipeline(pipe);
// bind de buffers/texturas en los mismos bindings que el GLSL
ctx->draw(/* … */);
ctx->endRenderPass();
```

Comparar backends: `HARUKA_BACKEND=opengl ./juego` y `HARUKA_BACKEND=vulkan ./juego`. Los tests de paridad: `./build/haruka_tests_rhi gl|vk|all`.
