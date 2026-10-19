# Planeta {#guia_planeta}

Un planeta que se crea a sí mismo desde su configuración: el relieve es el **resultado de un proceso** (tectónica → hidrología y erosión → clima → biomas), no una suma de ruido. Los campos horneados alimentan a la vez el render, la física, el agua y los props, y son deterministas por semilla.

## Cómo funciona

- **`PlanetarySystem` orquesta**: decide qué planeta planificar, construir, actualizar y dibujar, resuelve las cadenas de órbitas y delega el resto en cada `TerrestrialPlanet`.
- **`TerrestrialPlanet` se construye solo** con su identidad y el JSON crudo de `surfaceConfig` del objeto de escena.
- **Proceso de generación** (cada paso necesita el anterior):
  1. **Tectónica**: placas, tipos de borde y modificadores de elevación (`PlanetGeology`).
  2. **Relleno de depresiones** (priority-flood): las cuencas sin salida se llenan → **lagos**.
  3. **Rutado de flujo** (D8) y acumulación → **caudal y ríos**.
  4. **Erosión hidráulica** (stream power) → valles en V y redes de drenaje.
  5. **Erosión térmica** → taludes al ángulo de reposo.
  6. **Clima** (temperatura y humedad por latitud, altura, inclinación, orografía) y **biomas**.
- **La altura es un campo, no una fórmula**: todo el mundo muestrea el mismo bake (`PlanetFields::sample(dir)`), así no hay copias de la fórmula que diverjan.
- **El nivel del mar se deriva** de `landFraction` (cuantil de alturas), no al revés.
- **Caché en disco**: los bakes van a `bakes/` con una clave hash de todo lo que los afecta; con caché fría crear un mundo tarda ~20 s.
- **Órbitas**: Kepler analítico con elementos que precesan para los cuerpos masivos; N-cuerpos solo dentro de las esferas de influencia de lo que el jugador toca (`SOI.h`).
- **Materiales y props como dato**: `materials`, `zones` y `propLayers` del JSON son reglas sobre clima/forma; el motor no conoce ningún material ni capa concreta.

## Archivos principales

| Archivo | Qué hace |
|---|---|
| `src/Haruka/World/Planet/PlanetarySystem.{h,cpp}` | Orquesta planetas y órbitas |
| `src/Haruka/World/Planet/Planet.{h,cpp}` | Planeta que se construye solo |
| `src/Haruka/World/Planet/PlanetGeology.{h,cpp}` | Tectónica de placas |
| `src/Haruka/World/Planet/PlanetFields.{h,cpp}` | Erosión, hidrología, campos |
| `src/Haruka/World/Planet/Climate.h` | Temperatura y humedad |
| `src/Haruka/World/Planet/Biomes.h` | Biomas desde el clima |
| `src/Haruka/World/Planet/Orbit.h` | Órbitas keplerianas precesantes |
| `src/Haruka/World/Planet/SOI.h` | Esferas de influencia, N-cuerpos |
| `src/Haruka/World/Planet/GroundView.h` | Suelo dibujado para la GPU |
| `src/Haruka/World/WorldSystem.{h,cpp}` | Cuerpos celestes, origen flotante |
| `src/Haruka/World/WorldSystemProvider.h` | Mundo visto por la física |

## Opciones

Campos de `surfaceConfig` del planeta en la escena:

| Campo | Efecto |
|---|---|
| `seed` | Semilla de todo el proceso |
| `landFraction` | Fracción de tierra (deriva el nivel del mar) |
| `elevationMap`, `elevationRange` | PNG de elevación del autor y su rango en m |
| `zoneMap` | PNG de zonas por color |
| `materials` | Reglas de material del terreno (ver @ref guia_terreno) |
| `zones` | Zonas con perímetro (ciudades, oasis…) |
| `propLayers` | Capas de props (ver @ref guia_props) |
| `biomePalette` | Colores por bioma |
| `texRes`, `macroRes`, `tiling` | Resolución de texturas procedurales |

| Variable | Efecto |
|---|---|
| `HARUKA_TEX_SERIAL=1` | Hornea texturas sin hilos |
| `HARUKA_PLANET_DEBUG` | Vistas de depuración del planeta |
| `HARUKA_TERRAIN_PROBE`, `HARUKA_WATER_PROBE` | Sondas de altura y agua al log |

## Cómo usarlo

Consultar el planeta desde el juego:

```cpp
auto& ps = /* PlanetarySystem de la Application */;
double h   = ps.sampleTerrainHeight(worldPos);   // altura del suelo
auto   s   = ps.sampleSurface(worldPos);         // TerrainSample: altura, material…
double agua = ps.sampleWaterLevel(worldPos);     // cota del agua (mar o lago)
auto   w   = ps.weatherAt(worldPos);             // clima del punto
```

Para un mundo nuevo, cambia `seed` en la escena: el bake se rehace y se cachea. Para dibujar el continente a mano, da un `elevationMap` y deja que la erosión haga el resto.

Tests: `./build/haruka_tests --group World/Planet`.
