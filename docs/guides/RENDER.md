# Pases de render {#guia_render}

Cómo se dibuja un frame: una lista fija de pases, cada uno un módulo con sus propios recursos, que comparten una sola respuesta a "cuánta luz hay y de qué color". Estilo anime con luz física debajo: el color sale de la física y el sombreado es cel suave.

## Cómo funciona

- **El orden del frame** está en `Application::renderFrameContent()`:
  1. `frameBegin` — tamaño, cadena de post, mecánica celeste, near plane, props.
  2. `passCompute` — cómputo de terreno, antes de abrir ningún render pass (requisito de Vulkan).
  3. `passSky` — cielo, clima del frame, sol y aire.
  4. `passSceneSetup` — UBO por frame, luces, física, tonemapping.
  5. `passSceneObjects` — objetos de escena y piezas de construcción instanciadas.
  6. `passProps` — props del mundo.
  7. `passPlanetUpdate` — LOD y streaming del planeta.
  8. `passShadows` — sombra del sol, máscara cenital, suelo mojado.
  9. `passPlanet` — terreno y agua.
  10. `passGameWorld` — el `onRenderWorld` del juego.
  11. `passPrecipitation`, `passFluid`, `passClouds`.
  12. Post — FXAA, bloom y escalado a pantalla.
- **Cada pase es un módulo** (`SkyPass`, `ScenePass`, `ShadowPass`, `CloudPass`, `PostPass`…) con sus recursos dentro. `Application` solo los llama en orden.
- **Luz física única**: `SkyLight` sube en el binding 31 la luz del Sol y la Luna ya atenuada por la atmósfera y ya expuesta. Todos los pases la leen; ningún shader sabe de exposición. Ver @ref guia_cielo.
- **UBOs gemelos**: los bloques std140 de C++ (`SceneUBO.h`, `SkyLight.h`) tienen su gemelo en `assets/shaders/lib/`. Un campo nuevo se añade en los dos, en el mismo sitio; `static_assert` vigila el tamaño.
- **Shaders genéricos**: librerías reutilizables en `assets/shaders/lib/` (`toon_shade`, `surface_shade`, `aerial`, `sky_light`…), no un shader por feature.
- **Datos CPU+GPU** pasan por `ResourceStore`: una revisión por recurso, una subida por revisión, presupuesto de subida por frame y borrado diferido hasta que los frames en vuelo terminan.
- **Escala de render**: la escena se dibuja a escala (`renderScale`) y el post la sube a pantalla.

## Archivos principales

| Archivo | Qué hace |
|---|---|
| `src/Haruka/Core/ApplicationRender.cpp` | Orden de pases del frame |
| `src/Haruka/Renderer/SkyPass.{h,cpp}` | Fondo de cielo y clima |
| `src/Haruka/Renderer/ScenePass.{h,cpp}` | Objetos y piezas instanciadas |
| `src/Haruka/Renderer/ShadowPass.{h,cpp}` | Sombra solar y cenital |
| `src/Haruka/Renderer/CloudPass.{h,cpp}` | Nubes volumétricas |
| `src/Haruka/Renderer/PrecipitationRenderer.{h,cpp}` | Lluvia y nieve en mundo |
| `src/Haruka/Renderer/PostPass.{h,cpp}` | Bloom, FXAA, escalado |
| `src/Haruka/Renderer/SkyLight.{h,cpp}` | Luz física del frame |
| `src/Haruka/Renderer/FrameLights.{h,cpp}` | Sol, luna y ambiente |
| `src/Haruka/Renderer/SceneUBO.h` | UBOs de escena (std140) |
| `src/Haruka/Renderer/ResourceStore.{h,cpp}` | Subidas CPU→GPU con revisión |
| `src/Haruka/Renderer/GPUInstancing.{h,cpp}` | Una malla, N instancias |
| `src/Haruka/Renderer/GroundField.{h,cpp}` | Marcas en el suelo |
| `src/Haruka/Renderer/Shader.{h,cpp}` | Carga GLSL con includes |
| `assets/shaders/lib/toon_shade.glsl` | Sombreado cel común |

## Opciones

| Opción | Efecto |
|---|---|
| Ajustes `renderScale`, `bloom`, `antialiasing`, `shadowQuality` | Calidad del frame |
| `HARUKA_RENDER_SCALE=<f>` | Fuerza la escala de render |
| `HARUKA_NOBLOOM=1` | Sin bloom |
| `HARUKA_EXPOSURE=<f>` | Exposición fija (anula la automática) |
| `HARUKA_COLLISION_WIRE=1` | Malla de colisión en alambre verde |
| `HARUKA_SHOT=<png>`, `HARUKA_SHOT_SEC`, `HARUKA_SHOT_AFTER` | Captura de pantalla automática |

## Cómo usarlo

Dibujar algo propio en el mundo: implementa `onRenderWorld` en tu `GameInterface`. Se llama después del terreno y el agua, con el UBO por frame ya puesto:

```cpp
static void onRenderWorld(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& camPos) {
    // posiciones relativas a la cámara: worldPos - camPos
}
```

Para que proyecte sombra, implementa también `onRenderShadow(lightSpace, camPos)`.

Añadir un pase al motor: crea un módulo en `src/Haruka/Renderer/` con sus recursos, envuélvelo en `HARUKA_GPU_SCOPE`, llámalo en su sitio de `renderFrameContent()` y lee la luz de `lib/sky_light.glsl` en vez de inventar colores.
