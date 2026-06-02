# Plan: Fluidos híbridos + Softbody (XPBD unificado)

> Estado: PROPUESTA (sin implementar). Documento de planificación para revisión.
> Objetivo del usuario: fluido completo híbrido (océano + ríos + chapoteo) y un
> solver XPBD general para softbody (tela, cuerdas, volúmenes).

## 0. Insight central que reduce el alcance

**Softbody y fluido-de-partículas comparten solver.** XPBD (Extended
Position-Based Dynamics) resuelve *constraints* sobre posiciones de partículas:
- distancia → tela y cuerdas
- volumen/forma → softbody 3D
- **densidad (PBF)** → fluido de partículas (chapoteo)

Es el modelo de NVIDIA FleX: **un solo "UnifiedParticleSolver"** que hace rigid,
soft, cloth, rope y fluid. Por eso construir XPBD primero desbloquea DOS de las
peticiones a la vez.

El líquido NO es un solo sistema, son **tres dominios** que coexisten:

| Dominio | Técnica | Escala | Estado |
|---|---|---|---|
| Mar global | Gerstner (superficie) | planeta | ✅ HECHO |
| Ríos / lagos / inundación | Shallow-water heightfield | regional (chunks) | plan |
| Chapoteo / recipientes / cascada | PBF (partículas, sobre XPBD) | local | plan |

Lo difícil no es cada dominio aislado — es el **acoplamiento** entre ellos
(que un río desemboque en el mar, que las partículas de una cascada se fundan
en el heightfield del lago de abajo).

---

## 1. Decisiones arquitectónicas (transversales)

### 1.1 CPU vs GPU compute
- **XPBD softbody**: CPU al inicio (cientos-miles de partículas, debuggable),
  con diseño *data-oriented* (SoA) para migrar a compute después sin reescribir
  la lógica. El motor ya tiene compute shaders (`planet_generation.comp`,
  `frustum_cull.comp`) → la ruta GPU existe.
- **PBF fluido**: GPU desde el principio si se quieren >50k partículas; CPU si es
  solo chapoteo cosmético (<5k). Decisión por presupuesto.
- **Shallow-water**: GPU compute ideal (es una textura 2D de alturas por chunk),
  pero CPU viable para regiones pequeñas.

### 1.2 Escala planetaria / floating origin
Todo se simula en **espacio local cámara-relativo** (igual que el océano y el
terreno ya hacen). Las partículas y heightfields viven en coordenadas locales de
una región activa; al hacer `shiftOrigin` se trasladan con el resto del mundo.
Nada se simula en coordenadas absolutas (1e8 m) → sin pérdida de precisión float.

### 1.3 Acoplamiento de dominios (el problema real)
- **Una sola "altura de agua" autoritativa** por punto: `waterLevel(worldPos)` =
  max(océano, heightfield local). Render y física la consultan.
- **Transiciones por emisión/absorción**: una cascada emite partículas PBF; al
  caer en una celda de heightfield con agua, las partículas se "absorben"
  (suman su volumen a la celda y se borran). Conservación de masa aproximada.
- El océano (Gerstner) es **frontera de Dirichlet** para los heightfields
  costeros: el borde de un río en la costa toma la altura del mar.

### 1.4 Integración con el PhysicsEngine actual
El `PhysicsEngine` actual (rigid spheres + static boxes, octree) **no se toca**.
El solver XPBD es un sistema **paralelo** que:
- lee colisionadores estáticos del PhysicsEngine (terreno, cajas) como límites,
- expone sus partículas/cuerpos para que el rígido reaccione (flotabilidad,
  empuje) vía fuerzas, no resolución conjunta (acoplamiento débil, estable).

---

## 2. Plan por fases (orden por dependencias)

Cada fase compila y se demuestra sola. Estimaciones en "sesiones de trabajo",
no días de calendario.

### FASE A — Núcleo XPBD (base de softbody Y fluido) — GRANDE
Sin esto no hay nada. Es el cimiento.
- `core/physics/xpbd/particle_system.h` — SoA: posición, posición previa,
  velocidad, inverseMass, fase/grupo. Cámara-relativo.
- `xpbd_solver.{h,cpp}` — bucle: predict (gravedad+inercia) → iterar constraints
  (substeps) → actualizar velocidad. Substepping XPBD (Müller 2020) para rigidez
  estable sin explotar.
- Colisión partícula↔terreno reusando `getTerrainHeightAt` / static boxes.
- **Constraint base: distancia** (con compliance α) — ya permite cuerdas.
- Demo: una cuerda colgando y cayendo sobre el terreno.
- Entregable testeable: cuerda estable, sin explosión, sobre el planeta.

### FASE B — Softbody: tela y volúmenes — MEDIO
Sobre el núcleo XPBD.
- **Tela**: constraints de distancia (estructura) + bending (curvatura) sobre una
  rejilla. Viento (reusa el `u_windDir/strength` del océano → coherencia).
- **Volumen 3D**: constraints de distancia (tetraedros) + *volume preservation*.
  Carga de malla → tetrahedralización simple (o rejilla de partículas).
- Render: actualizar VBO desde posiciones de partículas cada frame (o skinning
  GPU). Normales recalculadas.
- Colisión softbody↔rigid (acoplamiento débil con el PhysicsEngine).
- Demo: bandera ondeando + cubo de jelly que cae y rebota.

### FASE C — PBF fluido de partículas (chapoteo local) — MEDIO/GRANDE
El MISMO solver XPBD + density constraint.
- Vecindad espacial (grid hash uniforme) para densidad — el caro de PBF.
- Constraint de densidad (Macklin & Müller 2013) + viscosidad XSPH + vorticity
  confinement (para que no se vea "pegajoso").
- Render de superficie: screen-space fluid (depth → blur → normales) o
  metaballs. Empezar con partículas-como-esferas para validar la física.
- Emisores/sumideros (grifo, fuente, cascada).
- Decisión GPU vs CPU según presupuesto de partículas (ver 1.1).
- Demo: verter agua en un recipiente; salpica y se asienta.

### FASE D — Shallow-water heightfield (ríos/lagos/inundación) — MEDIO
Independiente de XPBD; capa de chunks.
- Por chunk de terreno con agua: textura 2D `(altura_agua, flujo_x, flujo_y)`.
- Integración shallow-water (pipe model o SWE) en compute shader: el agua fluye
  cuesta abajo según el gradiente del terreno (que ya generamos).
- Render: reusa el `WaterRenderer`/shader de agua, con la altura del heightfield
  en vez de (o sumada a) el nivel del mar. Olas Gerstner se atenúan en agua poco
  profunda/quieta.
- Lluvia = fuente uniforme; manantiales = fuentes puntuales; evaporación =
  sumidero. Erosión hidráulica opcional (modifica el heightmap → ríos tallan
  valles reales).
- Demo: llueve sobre una montaña, el agua baja, forma un río y llena un lago.

### FASE E — Acoplamiento híbrido (lo que lo hace "completo") — MEDIO
Unir los tres dominios.
- `waterLevel(worldPos)` autoritativo = max(Gerstner, heightfield).
- Río→mar: borde costero del heightfield fijado a la altura del océano.
- Cascada→lago: emisor PBF arriba, absorción PBF→heightfield abajo (suma volumen).
- Flotabilidad unificada: rigid bodies y el player consultan `waterLevel` (el
  player ya tiene el hook de `getSeaSurface` — se generaliza).
- Demo: cascada que cae a un lago que desagua en un río que llega al mar.

### FASE F — Optimización y LOD de simulación — CONTINUO
- Simular solo regiones cerca de la cámara/jugadores (presupuesto fijo).
- Partículas PBF: dormir grupos en reposo; LOD por distancia.
- Heightfield: solo chunks activos; congelar agua quieta.
- Sleeping de softbodies en reposo.

---

## 3. Riesgos y notas honestas

- **Es trabajo de meses, no de días.** Cada fase A-E es sustancial; A y C son las
  más grandes. Conviene parar tras cualquier fase con algo jugable.
- **El acoplamiento (E) es lo verdaderamente difícil**; A-D son técnicas
  conocidas con referencias claras. Si el tiempo aprieta, A+B (softbody) y D
  (ríos) dan el 80% del valor visible; C (chapoteo) y E (híbrido total) son el
  largo plazo.
- **GPU compute** será necesario para fluido a escala; diseñar SoA desde el día 1
  evita reescrituras.
- El `PhysicsEngine` actual es muy básico; se mantiene como está y XPBD vive al
  lado con acoplamiento débil (más estable que un solver monolítico).

## 4. Referencias
- Müller et al. "XPBD: Position-Based Simulation of Compliant Constrained Dynamics"
- Macklin & Müller "Position Based Fluids" (PBF, 2013)
- Macklin et al. "Unified Particle Physics for Real-Time Applications" (FleX, 2014)
- Müller "Detailed Rigid Body Simulation with XPBD" (substepping, 2020)
- Mei et al. "Fast Hydraulic Erosion Simulation" (shallow-water + erosión)

## 5. Orden recomendado de arranque
**A → B → D → C → E**, con F aplicada de forma continua.
(Softbody y ríos primero por mejor relación valor/coste; chapoteo y acoplamiento
total al final.)
