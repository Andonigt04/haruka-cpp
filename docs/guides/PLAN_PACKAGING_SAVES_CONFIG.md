# Plan: Empaquetado de assets, saves por jugador y config/presets

> Estado: PROPUESTA (sin implementar). Para revisión antes de tocar código.
> Cubre tres frentes que la pregunta del usuario mezclaba pero son distintos.

## 0. Las tres categorías de datos (NO confundir)

| Categoría | Quién escribe | Dónde vive | Formato | Va en el build? |
|---|---|---|---|---|
| **Config / presets gráficos** | jugador (menú) | carpeta usuario SO | texto legible | NO (es del jugador) |
| **Saves de partida** | juego en runtime | carpeta usuario SO, por slot | binario+comprimido | NO |
| **Assets + escenas** | dev (al exportar) | junto al .exe | .pak/embebido, no legible | SÍ |

**Regla de oro:** lo que se *escribe en runtime* (config, saves) va a la carpeta de
usuario del SO y nunca dentro del .pak (que es solo-lectura). Lo que es *igual para
todos y de solo lectura* (assets, escenas) se empaqueta.

Rutas de usuario por SO (usar SDL_GetPrefPath("Haruka","Survival") → portable):
- Linux:   ~/.local/share/Survival/
- Windows: %APPDATA%/Survival/
- macOS:   ~/Library/Application Support/Survival/
Estructura: `settings.json`, `saves/slot1.sav`, `keybinds.json`.

---

## 1. Decisión de compresión: **zstd** (justificada)

Evalué tres opciones:
- **none + XOR**: trivial pero archivos enormes; no "profesional".
- **miniz** (header-only DEFLATE): zero-dep, fácil, ratio ~zip. Suficiente pero
  ratio/velocidad mediocres para assets grandes.
- **zstd**: estándar de la industria (UE5, Steam, muchos engines). Mejor
  ratio Y velocidad de descompresión que DEFLATE (la descompresión es lo que
  importa en runtime). Niveles ajustables (rápido para iterar, máx para release).

**Elijo zstd.** Es lo que pediste ("profesional"). Se vendoriza en
`third_party/zstd` (amalgamación de un solo .c/.h disponible upstream) → sin
dependencia de sistema, compila en cualquier sitio. Si el build se complicara,
el wrapper (sección 3) permite caer a miniz/none cambiando una línea.

---

## 2. Pieza fundacional: capa de I/O virtual (VFS)

**Sin esto no se puede empaquetar nada** — hoy el motor llama `stbi_load(ruta)` y
`assimp ReadFile(ruta)` directo, rutas de disco hardcodeadas.

`io/vfs.h` — una API única que el resto del motor usa:
```
namespace Haruka::vfs {
  bool   exists(const std::string& logicalPath);
  Bytes  read(const std::string& logicalPath);   // devuelve bytes ya descomprimidos
  void   mountLoose(const std::string& rootDir);  // dev: lee de disco
  void   mountPak(const std::string& pakPath);    // release: lee de .pak
  void   mountEmbedded(const void* data, size_t);  // embebido en exe
}
```
- `read("models/ship.glb")` busca en orden de montaje: loose → pak → embedded.
- Dev monta loose (assets/ del proyecto). Release monta el .pak. El código del
  motor **no cambia** entre modos.
- Refactor necesario: `texture.cpp`, `model.cpp`, `scene_loader.cpp`, `shader.h`
  pasan de rutas a `vfs::read()`. assimp necesita su `IOSystem` custom (lee de
  memoria); stb_image ya tiene `stbi_load_from_memory`.

---

## 3. Formato .pak

`io/pak.h` + `pak_builder` (herramienta de exportado):
```
[Header]  magic "HRKP", version, flags(compression, obfuscation), entryCount
[TOC]     por entrada: hash(logicalPath) u64, offset u64, sizeComp u32,
                       sizeRaw u32, compression u8
[Blobs]   cada asset: zstd-comprimido, luego XOR con keystream derivado de una
                       clave del proyecto (ofuscación ligera — frena lectura
                       casual a ojo; NO es seguridad criptográfica fuerte, ver §6)
```
- Lookup O(1) por hash del path lógico. Sin nombres en claro en el TOC (hashes) →
  no se puede listar el contenido leyendo el archivo.
- `compression` por-entrada: archivos ya comprimidos (png/jpg/ogg) se guardan
  STORED (sin recomprimir); el resto zstd. El builder decide por extensión.
- Wrapper `compress()/decompress()` aísla zstd → cambiar a miniz/none = 1 archivo.

---

## 4. Modos de exportado (el dev elige)

Panel/CLI de exportado con 3 modos:
1. **Loose (dev)**: copia assets/ + scenes/ sin tocar. Iteración rápida, legible.
2. **Packed (release)**: genera `game.pak` (zstd+ofuscado) junto al exe. Monta pak.
3. **Embedded**: incrusta el .pak como sección de datos del binario
   (objcopy/incbin) → un único .exe sin archivos sueltos. Monta embedded.

Selección por flag de build/exportado (`HARUKA_ASSET_MODE=loose|pak|embedded`) o
campo en `project.hrk`. El runtime detecta qué montar al arrancar (si existe
game.pak → pak; si hay sección embebida → embedded; si no → loose).

Escenas: en loose son .scene JSON; en pak/embedded se serializan a binario
comprimido dentro del .pak (mismo pipeline que cualquier asset) → no legibles.

---

## 5. Config + presets gráficos

`settings/graphics_presets.h`:
- Presets nombrados: Low / Medium / High / Ultra = constantes `GraphicsSettings`
  (shipean con el juego, en código). Custom = lo que el jugador edita.
- Panel ImGui de Ajustes → Gráficos: combo de preset + controles individuales
  (texturas, sombras, AA, render scale, vsync, ssao, bloom, motion blur, FOV,
  + waterQuality que ya dejé como hook). Editar cualquiera → preset pasa a Custom.
- Persistencia: `settings.json` en carpeta de usuario (SDL_GetPrefPath). Migrar
  de imgui.ini (hoy) a un json propio para gráficos+audio+keybinds.
- `applyGraphicsSettings()` (ya existe parcial en application.cpp) se completa
  para aplicar cada opción en caliente.

---

## 6. Saves de partida (por jugador)

`io/save_system.h`:
- Carpeta usuario `saves/slotN.sav`. Binario serializado + zstd (reusa §3 wrapper).
- Contenido (el mundo es procedural → el save es pequeño): seed del planeta,
  versión, posición/orientación/estado del jugador, inventario, y DIFFS del mundo
  (bloques colocados/destruidos, entidades) — NO el terreno entero (se regenera
  del seed).
- Header con versión para migración futura. Autosave + slots manuales.
- Multijugador: el servidor DGS es la autoridad; el save local es para single
  player / cache. (Fuera de alcance inicial.)

---

## 7. Seguridad — expectativa honesta

"No legible a ojo humano" SÍ se consigue: zstd + ofuscación XOR + hashes en el TOC
→ abrir el .pak en un editor no muestra nada útil, y no se listan los nombres.

Lo que esto **NO** es: protección fuerte contra un atacante motivado. La clave de
ofuscación viaja en el binario; alguien con un depurador puede extraerla. Eso es
cierto para TODOS los juegos (los assets acaban en la GPU/RAM). Para DRM real
haría falta cifrado con clave de servidor + streaming, que es otro proyecto y
rompe el juego offline. Recomiendo ofuscación (frena el 99%) y no invertir en
DRM fuerte salvo necesidad de negocio concreta.

---

## 8. Orden de implementación recomendado

1. **VFS** (fundacional — desbloquea todo lo demás). Refactor a `vfs::read`.
2. **Config + presets** (independiente, alto valor visible, no necesita VFS).
3. **zstd wrapper + .pak + builder** (sobre VFS).
4. **Modos de exportado** (loose/pak/embedded).
5. **Saves** (reusa el wrapper de compresión).

Cada paso compila y es testeable solo. 1 y 2 son independientes → se pueden
hacer en cualquier orden. 3-5 dependen de 1.

## 9. Estado actual (punto de partida)
- Config: SettingsManager usa imgui.ini (ImGui handlers). GraphicsSettings struct
  existe; applyGraphicsSettings() parcial.
- Assets: rutas de disco directas (stbi_load, assimp ReadFile). Sin VFS.
- Escenas: .scene JSON texto plano (nlohmann).
- Compresión: ninguna lib presente. zstd a vendorizar en third_party/.
