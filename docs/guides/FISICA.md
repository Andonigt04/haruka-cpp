# Física {#guia_fisica}

Física determinista a escala planetaria: gravedad real de los cuerpos celestes, suelo igual al que se dibuja, cuerpos rígidos con Jolt y blandos con XPBD. Corre en **un solo hilo** a propósito, para que el servidor (DGS) reproduzca exactamente el mismo resultado y pueda validar.

## Cómo funciona

- **Aislada del motor**: `HarukaPhysics` es una librería estática que solo depende de glm y de `IWorldProvider`. No ve GL, ni renderer, ni assets; por eso compila también en el servidor.
- **`IWorldProvider`** es la ventana al mundo: gravedad (posición, masa, radio y esfera de influencia de cada cuerpo), nivel del mar (flotación) y altura del terreno. En el cliente lo implementa `WorldSystemProvider` sobre el planeta; en el servidor, su propio estado sin GL.
- **Doble precisión** en las posiciones. La gravedad de una esfera decrece hacia el centro, así atravesar el suelo un frame no dispara al jugador al infinito.
- **Jolt** hace la fase ancha y el solver de rígidos. Los cuerpos son esferas o cajas (una caja vuelca sobre su arista de verdad), con estado angular opcional.
- **Raíles, no animaciones**: puertas, rampas, torretas y suspensiones son cuerpos restringidos a un grado de libertad (`Hinge`, `Slider`, `Rope`, `Weld`) con límites y motor o muelle opcionales. Una restricción choca, cede y se rompe sola; una animación no.
- **Suelo**: colisión contra la malla de terreno cercana (la misma que se dibuja) y contra las paredes de cueva (@ref guia_cuevas).
- **Personaje**: `Character` (estados, cámara, pasos arriba/abajo) sobre el cuerpo físico.
- **XPBD**: solver por subpasos para cuerpos blandos (tela, gelatina) y base de las partículas de fluido.
- **Hilos**: la física no tiene pool propio; recibe el del motor con `setTaskRunner`.

## Archivos principales

| Archivo | Qué hace |
|---|---|
| `src/Haruka/Physics/PhysicsEngine.{h,cpp}` | Rígidos, Jolt, raíles |
| `src/Haruka/Physics/WorldProvider.h` | Mundo visto por la física |
| `src/Haruka/World/WorldSystemProvider.h` | Proveedor del cliente |
| `src/Haruka/Game/Character.{h,cpp}` | Controlador de jugador/NPC |
| `src/Haruka/Physics/WindField.h` | Viento por zonas |
| `src/Haruka/Physics/XPBD/XPBDSolver.{h,cpp}` | Solver XPBD por subpasos |
| `src/Haruka/Physics/XPBD/Softbody.{h,cpp}` | Tela y cajas blandas |
| `src/Haruka/Physics/XPBD/Constraints.h` | Restricciones XPBD |
| `src/Haruka/Renderer/SoftbodyRenderer.{h,cpp}` | Dibuja cuerpos blandos |
| `src/Haruka/Renderer/CollisionWire.{h,cpp}` | Alambre de colisión |

## Opciones

| Opción | Efecto |
|---|---|
| `HARUKA_JOLT=0` | Solver propio en vez de Jolt |
| `HARUKA_COLLISION_WIRE=1` | Dibuja la malla de colisión |
| `HARUKA_CHAR_STEPUP=<m>`, `HARUKA_CHAR_STEPDOWN=<m>` | Altura de escalón del personaje |
| `HARUKA_FAR_GROUND_QUERY=0` | Sin consulta de suelo lejano |
| `HARUKA_GROUND_BENCH=<n>` | Mide el coste del suelo N frames |
| `HARUKA_WIND_DIAG=1` | Traza el viento sobre el personaje |
| Módulos `PHYSICS`, `SOFTBODY` | Compilan o no física y XPBD |

## Cómo usarlo

Un cuerpo y una puerta con bisagra:

```cpp
PhysicsEngine& ph = /* de la Application */;
auto marco  = std::make_shared<RigidBody>();
marco->name = "marco";  marco->anchored = true;   // en Jolt, pero nada lo mueve
auto puerta = std::make_shared<RigidBody>();
puerta->name = "puerta"; puerta->mass = 40.0;      // + posición, forma…
ph.addBody(marco);
ph.addBody(puerta);

PhysicsEngine::RailSpec r;
r.dof    = PhysicsEngine::RailSpec::Dof::Hinge;
r.anchor = bisagraWorld;
r.axis   = {0, 1, 0};
r.limits = {0.0, 100.0};                               // grados
ph.addRail("marco", "puerta", r);
```

Ver la colisión mientras juegas: `HARUKA_COLLISION_WIRE=1 ./juego`. Tests: `./build/haruka_tests --group Physics`.
