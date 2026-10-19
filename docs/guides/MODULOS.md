# Módulos de compilación {#guia_modulos}

El motor se parte en módulos que se encienden o apagan al compilar, para enviar solo lo que un juego usa: menos binario y nada de código muerto. Todo sale de una máscara de bits y de una sola cabecera generada, `src/Haruka/Core/Modules.h`.

## Cómo funciona

- **La lista es fija y el orden es el bit**: `FLUIDS`=0, `DEFORM`=1, `SOFTBODY`=2, `NETWORK`=3, `AUDIO`=4, `PARTICLES`=5, `TERRAIN`=6, `POSTFX`=7, `SHADOWS`=8, `PHYSICS`=9.
- **La máscara** `-DMODULES=` lleva un carácter por módulo (`1` encendido, `0` apagado). Una posición que falta cuenta como **encendida**.
- **CMake genera `Modules.h`** con un `#define HARUKA_MOD_<NOMBRE> 1` por módulo encendido. Se escribe dentro del árbol de fuentes a propósito: el juego lo incluye y ve los mismos interruptores.
- **Un módulo apagado desaparece de dos formas**: sus `.cpp` y shaders se quitan de la compilación, y el código compartido que lo toca está guardado con `#ifdef HARUKA_MOD_<NOMBRE>` con un camino vacío.
- **Dentro de un módulo no hace falta `#ifdef`**: si está apagado no se compila. Solo se guarda donde el resto del motor lo usa.
- **Dependencias**: `set(<MOD>_REQUIRES OTRO)` enciende el requisito con un aviso si falta. Hoy no hay ninguna declarada.

## Archivos principales

| Archivo | Qué hace |
|---|---|
| `cmake/HarukaModules.cmake` | Lista, máscara y dependencias |
| `CMakeLists.txt` | Quita fuentes de módulos apagados |
| `cmake/modules.h.in` | Plantilla de `Modules.h` |
| `src/Haruka/Core/Modules.h` | Interruptores generados (no editar) |
| `cmake/check_modules_include.cmake` | Comprueba includes por módulo |

## Opciones

| Opción | Efecto |
|---|---|
| `-DMODULES=1111111111` | Todo encendido (por defecto) |
| `-DMODULES=0011111111` | Sin fluidos ni deformación |
| `./<juego>/build.sh MODULES=…` | Misma máscara para motor y juego |

## Cómo usarlo

Compilar sin fluidos ni deformación:

```bash
cmake -S . -B build -DMODULES=0011111111
```

Motor y juego **tienen que compilarse con la misma máscara**: reconfigurar con otra reescribe `Modules.h`.

Añadir un módulo nuevo:

1. Añádelo **al final** de `HARUKA_MODULE_LIST` en `cmake/HarukaModules.cmake`. Insertarlo en medio desplaza todos los bits y rompe las máscaras guardadas.
2. Pon su código en una ruta reconocible y quítalo cuando esté apagado (junto a los otros bloques de `CMakeLists.txt`):
   ```cmake
   if(NOT HARUKA_MOD_WEATHER)
       list(FILTER ENGINE_SOURCES EXCLUDE REGEX "weather/.*\\.cpp$")
   endif()
   ```
3. Guarda con `#ifdef HARUKA_MOD_WEATHER` el código compartido que lo usa (miembros, creación, llamadas por frame) y deja un camino vacío.
4. Añade el bit a la tabla de esta guía y del `README.md`.
