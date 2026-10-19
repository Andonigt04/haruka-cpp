# Red y servidor (DGS) {#guia_red}

Multijugador con un servidor **autoritativo** (DGS) que valida con el **mismo código** que corre el cliente: el mismo suelo, las mismas reglas de construcción y el mismo reloj del mundo. El cliente predice; el servidor decide.

## Cómo funciona

- **`DGSBridge`** es la API de red del juego: `sendState` (lo que el jugador dice de sí mismo, con un payload propio del juego), chat, acciones (colocar pieza, soltar objeto) y sus respuestas, y el uuid de la sesión.
- **Un reloj para todos**: si el cluster reparte reloj (`hasWorldTime`/`worldTimeSeconds`), órbitas, clima y marea usan ese tiempo, y dos jugadores ven la misma hora y la misma tormenta.
- **Entidades remotas** (`EntitySync`): vacía la cola del cliente, crea un objeto de escena por cada jugador u objeto que llega, lo mueve y lo caduca por TTL (nada en la red dice "se ha ido"). El jugador local no se duplica (`setLocalPlayerUuid`).
- **El suelo del servidor** (`HeightField`): el mismo campo de altura horneado que pisa el cliente, sin GL. Sin él el servidor juzgaba contra una esfera lisa y expulsaba a quien estuviera en un valle.
- **Reglas como módulo**: `haruka_rules.so` (ABI `dgs_game_module_v1`) se carga en el servidor con `dlopen`. Se apoya en `haruka_simbase` (base sin GL: suelo, clima) y `HarukaConstruction`.
- **Determinismo**: física en un hilo, clima como función pura, construcción sin aleatorio — por eso el servidor puede validar sin replicar estado.

## Archivos principales

| Archivo | Qué hace |
|---|---|
| `src/Haruka/Network/DGSBridge.{h,cpp}` | API de red del juego |
| `src/Haruka/Network/EntitySync.{h,cpp}` | Entidades remotas en escena |
| `src/Haruka/Network/HeightField.h` | Suelo horneado del servidor |
| `src/Haruka/Network/DefaultRules.cpp` | Módulo de reglas por defecto |
| `src/Haruka/Core/ApplicationNetwork.cpp` | Enganche red ↔ Application |
| `external/dgs` | SDK del cliente DGS |
| `tools/BenchDGSBandwidth.cpp` | Banco de ancho de banda |

## Opciones

| Opción | Efecto |
|---|---|
| Módulo `NETWORK` | Compila o no el cliente de red |
| `HARUKA_RULES_HEIGHT_FIELD=<fichero>` | Campo de altura para el servidor |
| `HARUKA_RULES_PIECE_CATALOG=<fichero>` | Catálogo de piezas para validar |

## Cómo usarlo

Desde el juego, cada frame:

```cpp
namespace Net = Haruka::Network;
Net::setLocalPlayerUuid(Net::sessionUuid());
Net::sendState(Net::sessionUuid(), pos, rot, payload.data(), payload.size());   // size 0 = sin cambios
for (auto& ack : Net::pollActionResults()) { /* ¿aceptó el servidor la pieza? */ }
if (Net::hasWorldTime()) { /* usar Net::worldTimeSeconds() */ }
```

En el servidor, deja junto al binario el campo de altura y el catálogo (o pásalos con las variables de arriba): sin catálogo, `validatePlace` rechaza todo.

Tests: `./build/haruka_tests --group Network` y el banco `./build/bench_dgs_bandwidth`.
