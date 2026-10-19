# Assets, escenas y guardado {#guia_assets}

Dónde vive cada dato del juego y cómo viaja: legible y plano mientras se desarrolla, empaquetado y ofuscado en la versión final, sin que los cargadores sepan cuál de los dos es. Las escenas describen el mundo; los bakes son caché regenerable; las partidas son del jugador.

## Cómo funciona

- **Tres clases de datos que no se mezclan**: contenido del juego (escenas, shaders, texturas: va en el build), caché (`bakes/`: se regenera desde la semilla) y partidas y ajustes del jugador (los escribe él).
- **`AssetPaths` centraliza la disposición**: en *dev* los assets están planos junto al ejecutable (`shaders/`, `scenes/`); en *release* (`-DHARUKA_RELEASE`) bajo `assets/` (`assets/shaders/`, `assets/maps/`). Ningún cargador escribe rutas a mano. El ejecutable hace `cd` a su carpeta al arrancar.
- **Escenas**: `.scene` es JSON legible (dev); `.hmap` es la misma escena comprimida con zstd y ofuscada (release). `SceneLoader` decide por la extensión; `SceneValidator` comprueba el JSON.
- **`BlobCodec`**: zstd + XOR con una clave que va en el binario. Frena la lectura y edición casual; **no** es DRM.
- **Bakes**: altura, biomas, cuevas… se guardan en `bakes/` con una clave hash de todo lo que los afecta. Si la clave cambia, se rehornean solos.
- **Proyecto**: `project.hrk` (JSON) agrupa escenas, assets y ajustes de exportación; `startScene` es la escena de arranque.
- **Escritura de PNG propia** (`ImageWriter`): 8 y 16 bits con compresión, sin depender de zlib (el bake de altura necesita 16 bits).
- **Textos**: `assets/lang/<código>.json`; los idiomas se detectan del directorio.

## Archivos principales

| Archivo | Qué hace |
|---|---|
| `src/Haruka/Core/AssetPaths.h` | Rutas dev vs release |
| `src/Haruka/Core/Scene/SceneLoader.{h,cpp}` | Carga `.scene` y `.hmap` |
| `src/Haruka/Core/Scene/SceneValidator.{h,cpp}` | Valida el JSON de escena |
| `src/Haruka/Core/Scene/SceneManager.h` | Objetos de la escena |
| `src/Haruka/IO/BlobCodec.{h,cpp}` | Comprime y ofusca blobs |
| `src/Haruka/IO/ImageWriter.{h,cpp}` | PNG 8/16 bits |
| `src/Haruka/Core/Project.{h,cpp}` | Proyecto `project.hrk` |
| `src/Haruka/Core/Locale.{h,cpp}` | Textos por idioma |
| `src/Haruka/Core/BuildInfo.{h,cpp}` | Versión, build y commit |
| `tools/Mapc.cpp` | Empaqueta `.scene` → `.hmap` |
| `cmake/HarukaShaders.cmake` | Copia shaders al build |

## Opciones

| Opción | Efecto |
|---|---|
| `-DHARUKA_RELEASE` | Disposición de release (`assets/…`) |
| `HARUKA_INI=<ruta>` | Fichero de ajustes de esta instancia |
| `HARUKA_VOX_SAVE_DIR=<dir>` | Dónde guardar ediciones del mundo |

## Cómo usarlo

Cargar rutas sin escribirlas a mano:

```cpp
#include "Haruka/Core/AssetPaths.h"
std::string escena = Haruka::Core::AssetPaths::maps() + "menu";      // .scene en dev, .hmap en release
std::string datos  = Haruka::Core::AssetPaths::data() + "prefabs/";
```

Empaquetar un mapa para release:

```bash
./build/mapc scenes/main.scene             # → scenes/main.hmap
```

Para regenerar un mundo desde cero, borra su carpeta de `bakes/` (o cambia la semilla): todo lo que hay ahí se vuelve a hornear.
