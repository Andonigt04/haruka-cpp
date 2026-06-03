# Plan: Terreno deformable (cavar / cráteres / construir)

> Estado: PROPUESTA (sin implementar). Visión del usuario: cavar, o un misil
> impacta y genera un agujero real → la malla se recalcula con vértices nuevos.
> Las tres metas por fases: (1) colocar/quitar objetos, (2) modificar terreno
> (cavar/construir), (3) destruir/minar terreno con drops.
> Enfoque: Opción B — terreno procedural + capa de deformaciones (NO voxels).
> Referencia: No Man's Sky / Astroneer (terreno suave editable).

## 0. El insight central

El terreno NO se rehace. Se INTERCEPTA su función de altura:

    alturaFinal(pos) = calculateHeight(pos) + deformaciones(pos)

Una deformación = una "brocha" {centro, radio, fuerza, tipo}. Cráter = resta;
construir = suma. Cuando un chunk se regenera, suma las deformaciones que lo
solapan → la malla nueva sale ya con el agujero/montículo y vértices nuevos,
reutilizando TODO el sistema actual (cube-sphere, LOD, morph, colisión, agua).

Esto funciona porque el chunk ya se genera bajo demanda; solo añadimos un término.

## 1. Estructura de datos: el campo de deformación

`core/terrain/deformation_field.{h,cpp}` (engine):
- Lista de brochas: `struct Brush { dvec3 centerWorld; float radius; float strength; uint8 type; }`
  - type: SUBTRACT (cavar/cráter), ADD (construir/elevar), FLATTEN (aplanar).
- Índice espacial (grid hash por celdas grandes) para que `sample(pos)` solo
  evalúe las brochas cercanas, no todas → O(1) amortizado.
- `float sample(const dvec3& worldPos)`: suma la contribución de las brochas que
  cubren pos. Caída suave (smoothstep desde el centro al borde) → bordes limpios,
  no escalones. Devuelve metros de desplazamiento (±).
- Thread-safe (read) — el generador corre en workers async.

## 2. Integración con el generador

`TerrainGenerator::generateChunk` y `calculateHeight`/`sampleHeightAt`:
- Tras calcular la altura procedural de un vértice, sumar
  `deformField->sample(worldPosDelVertice)` (convertido a fracción del radio).
- El bounding sphere del chunk ya cubre el relieve; un cráter profundo necesita
  recomputar el bounding sphere desde los vértices (ya lo hacemos para el cull).
- Las normales analíticas: el epsilon de muestreo debe incluir la deformación
  (muestrear deformField en los puntos ±eps) para que el cráter tenga sombreado
  correcto en sus paredes.

## 3. Re-mallado dinámico (lo que el usuario pide)

Cuando se añade/modifica una brocha:
1. Calcular qué chunks (en GPU ahora mismo) solapa el AABB de la brocha.
2. Marcarlos "sucios" → el streaming los regenera (async) y re-sube a GPU.
3. El chunk nuevo trae los vértices deformados. Reemplaza al viejo sin parpadeo
   (doble buffer: subir el nuevo, luego soltar el viejo).
- Coste: solo los chunks tocados, solo cuando editas. Un cráter de misil toca
  1-4 chunks → barato.
- Sub-fase opcional: para impactos en tiempo real (misiles), regenerar solo el
  parche afectado a alta resolución sin esperar al streaming completo.

## 4. Persistencia

Las brochas SON el diff del mundo (encaja con el save plan):
- Guardar la lista de brochas {center, radius, strength, type} en el save.
- Al cargar, repoblar el deformField → el terreno se regenera ya deformado.
- Tamaño minúsculo: el planeta es procedural, solo guardas las ediciones.
- Streaming: las brochas viven en CPU; un chunk que se descarga y recarga vuelve
  a aplicar las brochas de su zona → las ediciones persisten aunque te alejes.

## 5. Colisión y agua (coherencia)

- `getTerrainHeightAt` (usado por player, agua, raycast del sandbox) DEBE sumar
  también `deformField->sample` → así caes dentro del cráter, el agua fluye al
  agujero, y el cursor del sandbox apunta a la superficie real editada.
- Una sola fuente de verdad: `alturaFinal()` la usan render Y física.

## 6. Las tres fases del usuario

### FASE 1 — Objetos/estructuras (lo más alcanzable, NO toca terreno)
- Sistema de entidades colocables: items, mesas, props sobre el terreno.
- Reusa SceneManager (addLoadedObject) + el raycast del sandbox (ya hecho en
  Fase 1 del sandbox) para apuntar dónde colocar.
- Inventario simple (qué tienes para colocar) + colocar/recoger.
- Render: mallas normales (Model/primitive), ya soportado.
- Entregable: colocar una mesa en el suelo, recogerla.

### FASE 2 — Deformación de terreno (cavar/construir/cráteres)
- deformation_field + integración generador + re-mallado (secciones 1-3).
- getTerrainHeightAt suma deformaciones (sección 5).
- Herramientas sandbox: cavar (SUBTRACT brush al apuntar+clic), construir (ADD),
  aplanar (FLATTEN). Misil = SUBTRACT brush grande en el impacto.
- Entregable: cavas un agujero / un misil deja un cráter, persiste.

### FASE 3 — Minar/recolectar (deformación + drops)
- Pegar al terreno → SUBTRACT brush pequeño + spawn de item-recurso (piedra/tierra
  según la zona) → al inventario.
- Reusa Fase 1 (items) + Fase 2 (deformación).
- Entregable: minas piedra y la recoges; el agujero queda.

## 7. Riesgos / honestidad

- Es trabajo grande pero NO requiere rehacer el terreno (esa es la victoria de la
  Opción B). Fase 1 es relativamente acotada; Fase 2 es la sustancial.
- El re-mallado debe ser sin parpadeo (doble buffer) y barato (solo chunks
  tocados). El LOD complica: editar a LOD alto y luego alejarse (LOD baja) debe
  preservar la edición → el deformField es independiente del LOD (se muestrea a
  cualquier resolución), así que escala bien.
- Límite de resolución: una brocha más pequeña que el espaciado de vértices del
  chunk no se ve hasta acercarse (más LOD). Cavar detalle fino necesita forzar
  LOD alto local o un parche de alta-res. Aceptable para empezar.
- Voxels darían bloques perfectos pero cambian el look a cúbico y son otro motor.
  La Opción B mantiene el planeta suave y reutiliza lo construido.

## 8. Orden recomendado
FASE 1 (objetos) primero — alcanzable, da el bucle de "apuntar+colocar" y reusa
el raycast del sandbox ya hecho. Luego FASE 2 (deformación, el núcleo técnico).
Luego FASE 3 (minar) que combina ambas.
