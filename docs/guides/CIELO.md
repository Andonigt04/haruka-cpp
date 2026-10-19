# Cielo, luz y clima {#guia_cielo}

El cielo, la luz y el tiempo salen de la física, no de colores puestos a mano. El Sol está quieto y el planeta gira; la atmósfera dispersa la luz; las nubes aparecen donde el aire se satura; y la lluvia solo cae de nubes que existen. Aspecto: anime tipo *Tenki no Ko*, con nubes volumétricas y sombreado cel suave.

## Cómo funciona

- **Efemérides**: `SkyEphemeris` calcula dónde están el Sol y la Luna vistos desde el planeta. El Sol no se mueve; el planeta rota (un día = 24 h de simulación) y la Luna orbita.
- **Atmósfera**: Rayleigh + Mie + ozono con dispersión simple a lo largo del rayo. De ahí sale el azul, el atardecer y la perspectiva aérea. Se hornea en una LUT.
- **Radiometría**: el Sol por su radiación, la Luna por lo que refleja, las estrellas por su magnitud, y la exposición de la cámara que lo pasa a pantalla. Noche sin luna = negro; luna llena = gris tenue.
- **`SkyLight` (UBO 31)** lleva todo eso, ya expuesto, a todos los pases. Una sola respuesta a "cuánta luz hay aquí".
- **Clima**: `WeatherSystem` es una **función pura** de (semilla, tiempo, dirección). Unos frentes recorren el planeta; la cobertura es la suma de los que tapan un punto, modulada por la humedad. Dos clientes con la misma semilla y reloj ven la misma tormenta.
- **Invariante**: la precipitación sale de la cobertura. Sin nube encima no llueve (lo vigila un test).
- **Nubes**: no hay capas fijas. `CloudColumn` describe la columna de aire y la nube está donde el aire se satura: la base es el nivel de condensación, el techo donde se acaba el vapor; altocúmulos y cirros salen a la cota de su temperatura. `CloudPass` marcha el volumen a resolución reducida y lo compone a resolución completa.
- **Movimiento de nubes**: la deriva con el viento se integra (no `viento × tiempo`) y se funde entre horneados para que no vayan a tirones.
- **CPU = GPU**: `CloudColumn` y `lib/cloud_column.glsl` son la misma función; los tests miden una contra otra.

## Archivos principales

| Archivo | Qué hace |
|---|---|
| `src/Haruka/World/Planet/SkyEphemeris.h` | Posición de Sol y Luna |
| `src/Haruka/World/Planet/Atmosphere.h` | Dispersión de la luz |
| `src/Haruka/World/Planet/SkyRadiometry.h` | Cuánta luz y exposición |
| `src/Haruka/Renderer/SkyLight.{h,cpp}` | UBO de luz (binding 31) |
| `src/Haruka/Renderer/SkyPass.{h,cpp}` | Fondo, sol, estrellas, clima |
| `src/Haruka/World/WeatherSystem.{h,cpp}` | Frentes y precipitación |
| `src/Haruka/World/CloudColumn.h` | Dónde se forma nube |
| `src/Haruka/World/CloudMotion.h` | Deriva y fundido de nubes |
| `src/Haruka/Renderer/CloudPass.{h,cpp}` | Marcha volumétrica de nubes |
| `src/Haruka/Renderer/PrecipitationRenderer.{h,cpp}` | Lluvia y nieve |
| `assets/shaders/sky.frag` | Shader del cielo |
| `assets/shaders/cloud_vol.frag` | Marcha de nubes |
| `assets/shaders/lib/atmosphere.glsl` | Atmósfera en GLSL |
| `assets/shaders/lib/sky_light.glsl` | Gemelo GLSL de SkyLight |

## Opciones

| Opción | Efecto |
|---|---|
| `HARUKA_SIM_TIME=<s>` | Congela el reloj de simulación en ese instante |
| `HARUKA_DAY_ANGLE=<rad>` | Fija el giro del planeta (0 = hora de la escena, π = 12 h después) |
| `HARUKA_EXPOSURE=<f>` | Exposición fija |
| `HARUKA_RAIN=<-1..1>` | Fuerza la lluvia (negativo = la decide el clima) |
| `HARUKA_CLOUD_VOL=0` | Apaga las nubes volumétricas |
| `HARUKA_CLOUD_COVER=<0..1>` | Fuerza la cobertura |
| `HARUKA_CLOUD_STEPS=<n>` | Pasos de la marcha |
| `HARUKA_CLOUD_RESDIV=<n>`, `HARUKA_CLOUD_RESDIV_Y` | Divisor de resolución de nubes |
| `HARUKA_CLOUD_MODES` | Modos de depuración de nubes |
| `HARUKA_WATER_CYCLE_SPEED=<f>` | Acelera el ciclo del agua |

## Cómo usarlo

Probar otra hora del día con lluvia, sin esperar:

```bash
HARUKA_DAY_ANGLE=1.4 HARUKA_RAIN=0.8 ./juego
```

Preguntar el clima desde el juego: `PlanetarySystem::weatherAt(worldPos)` devuelve cobertura, precipitación y viento de fondo del punto. En un shader, lee la luz con `#include "lib/sky_light.glsl"`; no añadas colores de cielo propios.

Tests: `./build/haruka_tests --group World` (frentes, formación de nubes) y `./build/haruka_tests_rhi vk` (iluminación y nubes en GPU).
