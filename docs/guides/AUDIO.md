# Audio {#guia_audio}

Que el mundo suene por lo que pasa en él, sin un solo fichero de audio: cada hecho (una rama que se rompe, una pisada) o cada cosa que suena mientras algo le ocurre (un árbol al viento, la rompiente) es un **punto de sonido**; los que suenan igual se agrupan, y el sonido rodea paredes y se tapa con el terreno.

## Cómo funciona

- **Tres capas**: `AudioSystem` es OpenAL puro (contexto, oyente, pool de voces); `AudioLoader` da los buffers; `AudioManager` es la API alta (oyente = cámara, fuentes del mundo, propagación).
- **Puntos de sonido** (`SoundPoints`): un punto es un sonido, una posición y una potencia. Los bucles se crean con `add` y se mueven con `set`; los hechos se disparan con `emit` (los del mismo frame y sector se suman).
- **Agrupación**: el espacio alrededor del oído se parte en sectores (12 de azimut × 3 anillos). Los puntos con el mismo sonido en el mismo sector son **una voz** en su centroide con la energía sumada: cien árboles a 40 m son "un bosque al este", no cien fuentes. Los grupos más fuertes ganan el presupuesto de voces.
- **Atenuación propia**: energía = potencia · (ref/d)². OpenAL va con rolloff 0. El mismo número sirve para reproducir y para preguntar "¿cuánto se oye aquí?" (sigilo, IA).
- **Sonidos sintetizados** (`SoundSynth`): viento, hojas, rompiente, chapoteo, bajo el agua, pisadas por material, chapuzón, crujido. Bucles con número entero de periodos para que no hagan clic. Sin OpenAL: se prueba en headless.
- **Propagación** (`Propagation`): encuentra el camino de A a B rodeando obstáculos y mide cuánta señal llega (oclusión por terreno + rodeo). La comparten el audio y los proyectiles que rodean paredes.

## Archivos principales

| Archivo | Qué hace |
|---|---|
| `src/Haruka/Audio/AudioManager.{h,cpp}` | API de audio del juego |
| `src/Haruka/Audio/SoundPoints.{h,cpp}` | Puntos y agrupación por sector |
| `src/Haruka/Audio/SoundSynth.{h,cpp}` | Sonidos sintetizados |
| `src/Haruka/Audio/AudioSystem.{h,cpp}` | Backend OpenAL |
| `src/Haruka/Audio/AudioLoader.{h,cpp}` | Buffers de audio |
| `src/Haruka/Core/Propagation.{h,cpp}` | Caminos y oclusión del sonido |

## Opciones

| Opción | Efecto |
|---|---|
| Ajustes `masterVolume`, `musicVolume`, `sfxVolume`, `ambientVolume` | Volúmenes |
| Ajustes `outputDevice`, `inputDevice` | Dispositivos de audio |
| Módulo `AUDIO` | Compila o no el audio |

## Cómo usarlo

```cpp
auto& audio = /* AudioManager */;
audio.setListener(camPos, camFwd, camUp);                       // cada frame
audio.points().emit(pieWorld, Haruka::Audio::Sound::StepGrass, 1.0f);   // un hecho
auto id = audio.points().add(arbolWorld, Haruka::Audio::Sound::Leaves, 0.4f);  // un bucle
audio.points().setPower(id, viento);                            // sube y baja con el viento
audio.setSubmerged(bajoElAgua);
audio.update();
```

Tests: `./build/haruka_tests --group Audio` (síntesis y agrupación, sin tarjeta de sonido).
