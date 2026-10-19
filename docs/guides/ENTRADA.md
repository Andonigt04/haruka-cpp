# Ajustes, entrada e interfaz {#guia_entrada}

Los ajustes del jugador, los controles y la interfaz: acciones tipadas que valen igual con teclado que con mando, ajustes guardados en un solo fichero por instancia, y paneles de interfaz que pueden vivir en el mundo 3D.

## Cómo funciona

- **Acciones, no teclas**: el juego registra acciones (`Move`, `Jump`…) y lee su valor; no sabe si vienen de WASD o del stick. Cada fuente (`DirectBinding`, `Axis1DBinding`, `Axis2DBinding`) lleva sus teclas **y** sus botones/ejes de mando. Botones: cualquiera pulsado; ejes: gana el de mayor magnitud; un gatillo por encima del umbral cuenta como botón.
- **Mando**: `Gamepad` abre el primero que haya, sigue los que entran y salen y llena el estado de cada frame.
- **Ajustes**: `GameSettings` (gráficos, audio, controles) se guarda en `imgui.ini` como secciones `[GameSettings]` y `[KeyGroup]`. Hay presets de calidad y un LOD adaptativo que mueve el objetivo en píxeles.
- **Panel de ajustes** (`SettingsPanel`): ImGui inmediato, con reasignación de teclas.
- **Interfaz en el mundo** (`UISystem`, `UIWorldPanel`): paneles ImGui dibujados a una textura y proyectados en un quad plano o curvado en el mundo; `UIInteractionSystem` hace raycast contra ellos para foco e interacción.
- **Textos traducidos**: `tr("clave")` desde `assets/lang/<código>.json` (ver @ref guia_assets).

## Archivos principales

| Archivo | Qué hace |
|---|---|
| `src/Haruka/Input/InputAction.{h,cpp}` | Acciones teclado + mando |
| `src/Haruka/Input/Gamepad.{h,cpp}` | Abre y lee el mando |
| `src/Haruka/Settings/GameSettings.h` | Todos los ajustes |
| `src/Haruka/Settings/SettingsManager.{h,cpp}` | Guarda ajustes, registra acciones |
| `src/Haruka/UI/SettingsPanel.{h,cpp}` | Panel de ajustes |
| `src/Haruka/UI/UISystem.{h,cpp}` | Paneles en el mundo |
| `src/Haruka/UI/UIWorldPanel.{h,cpp}` | Panel 3D plano o curvo |
| `src/Haruka/UI/UIInteractionSystem.{h,cpp}` | Raycast a paneles |
| `src/Haruka/UI/UIStyles.h` | Paleta y tipografía |

## Opciones

Ajustes de gráficos (`GameSettings`): `windowMode`, resolución, monitor, `renderBackend`, `textureQuality`, `shadowQuality`, `antialiasing` (None/FXAA/TAA), `waterQuality`, `terrainQuality`, `foliageQuality`, `grass`, `fov`, `renderScale`, `vsync`, `ssao`, `bloom` (+ umbral, fuerza, iteraciones), `hdr`, `terrainShadows`, `fog`, `chunkMemoryMB`, `maxFps`, `adaptiveLOD`, `lodTargetPx`. Audio: volúmenes y dispositivos.

| Variable | Efecto |
|---|---|
| `HARUKA_INI=<ruta>` | Fichero de ajustes propio (varias ventanas a la vez) |
| `HARUKA_GAMEPAD=0` | Ignora el mando |
| `HARUKA_UI_SCALE_LOG=1` | Traza la escala de la interfaz |

## Cómo usarlo

Registrar y leer una acción:

```cpp
auto& sm = Haruka::Settings::SettingsManager::get();
sm.registerAction("Jump", "Saltar", "Movimiento", { SDL_SCANCODE_SPACE });   // + botón en el binding
// cada frame (lo hace la Application):
if (sm.readValue<bool>("Jump")) { /* saltar */ }
auto mover = sm.readValue<glm::vec2>("Move");                                 // WASD o stick
```

Leer un ajuste: `sm.graphics().fov`. Dos instancias a la vez sin pisarse: `HARUKA_INI=/tmp/a.ini ./juego` y `HARUKA_INI=/tmp/b.ini ./juego`.

Tests: `./build/haruka_tests --group Input` y `--group Settings`.
