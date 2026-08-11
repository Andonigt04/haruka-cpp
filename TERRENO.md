# Terreno planetario — pipeline completo

**Qué hay aquí**: cómo se construye y se dibuja un planeta de escala real con relieve de metros, paso
a paso y con el fichero de código de cada etapa. Incluye la gestión de texturas y el clima. Al final,
el estado real y lo que falta.

| Este fichero | Otro fichero |
|---|---|
| El pipeline del terreno: cada etapa, entradas, salidas y el contrato entre CPU y GPU. | [ROADMAP.md](ROADMAP.md) — plan por versión de los tres proyectos. |
| | [TODO.md](TODO.md) — bugs abiertos e ideas. |
| | [docs/HISTORIAL.md](docs/HISTORIAL.md) — el sistema de chunks anterior y las trampas que dejó. |

---

## 0. El número que decide la arquitectura

Guardar la Tierra muestreada cada 2 m son **1,28 × 10¹⁴ muestras ≈ 255 TB**. No hay disco, ni
streaming, ni caché que lo arregle.

Así que la primera decisión no es *cómo lo cargo*, sino que **el detalle de 2 m no puede ser un dato:
tiene que ser una función**. Lo único que se decide es dónde se evalúa y con qué densidad.

De ahí cuelga todo. Lo que sí se guarda es solo la parte lenta:

| Qué | Cuánto |
|---|---|
| Malla base cube-sphere, 6 × 257² vértices | 15,9 MB |
| Campo base (elevación, temperatura, humedad), textura array de 6 capas RGBA32F (RG por compat.) | 6,4 MB |
| **Total, fijo, subido una vez, sin streaming** | **~21 MB** |

Esa retícula tiene un vértice cada **39,1 km**. Todo lo que hay entre dos vértices lo pone una
función pura de (semilla, dirección), evaluada donde hace falta y tirada.

### Por qué no hay chunks

El sistema anterior se eliminó (57 ficheros, ~9 900 líneas) por inestable. Los tres fallos que
costaron sesiones enteras eran **del mismo tipo, y ninguno era de "dividir el terreno"**: cola de
streaming huerfanada, un hueco que tumbaba la cara entera, caché en thrash. Los tres son de
**gestionar residencia en CPU**.

**La conclusión que ordena este diseño: si nada tiene que estar *cargado*, nada puede *faltar*.**

| Fallo que hubo | Por qué no puede volver |
|---|---|
| Cola de streaming huerfanada | no hay cola |
| Agujeros mientras carga | no existe "aún no cargado" |
| Caché en thrash | no hay caché |
| Draw set colapsado a la raíz | el nivel es función pura de la distancia |
| Grietas entre niveles vecinos | el factor se calcula por ARISTA (§2.2) |

Lo que **no** elimina: la paridad CPU↔GPU. Esa vuelve entera, y es §5.

---

## 1. Construcción — lo que pasa una vez, al cargar el planeta

Todo en `SimplePlanetInternal::buildMesh` y `addSimplePlanet`
([planetary_system.cpp](src/game/planetary_system.cpp)).

### 1.1 Retícula cube-sphere

Para cada una de las 6 caras, `(lx, ly)` recorre `[-1,1]` en una rejilla de `faceRes+1` y
`cubeFaceToDir` ([cube_sphere.cpp](src/core/terrain/cube_sphere.cpp)) la lleva a la esfera con el
spherify de Nowell:

```
x = p.x·√(1 − y²/2 − z²/2 + y²z²/3)     (y análogas)
```

Reparte la distorsión mucho mejor que normalizar el cubo: las esquinas no se estiran.

⚠️ **Una sola proyección en todo el motor.** Hubo dos `cube_sphere.h` vivos con fórmulas distintas
(la otra era un estiramiento radial `t = 1 + a(1 − 1/r)`); se borró la muerta. Dos descripciones de
la misma superficie es la receta de "dos terrenos".

### 1.2 Altura base

`heightFn(dir)` = `geology.elevationModifier(dir)`, salvo que el planeta traiga **mapa de zonas**, en
cuyo caso manda el autor y `landFraction` deja de tener sentido.

La geología ([geology.h](src/core/planet/geology.h)) es tectónica de placas con `seed` explícita.
⚠️ Antes usaba `random_device` y el mismo mundo salía distinto en cada arranque.

### 1.3 Clima, horneado por vértice

Con la altura ya sabida, cada vértice guarda `temp` y `humid` (§4).

### 1.4 Normal del terreno, no de la esfera

Por diferencias centrales sobre la retícula de la cara. ⚠️ Antes era `normalize(dir)` y eso tenía dos
consecuencias visibles: el relieve **no existía** en la iluminación, y `slope = 1 − dot(n, up)` daba
**0 en todo el planeta**, así que ningún material de pendiente (la roca) podía elegirse jamás.

### 1.5 Lo que se sube a la GPU

| Recurso | Contenido | Dónde |
|---|---|---|
| Vertex buffer | pos, normal, color de bioma, `(elev, temp, humid)` | — |
| Índice de triángulos | camino de respaldo sin teselar | — |
| **Índice de PARCHES** | los 4 vértices del quad, mismo VB | — |
| **Campo base** | array 6 capas RGBA32F = `(elev m, temp °C, humedad, 0)` | unidad 15 |
| Mapa de zonas | equirectangular RGBA8, filtro **Nearest** | unidad 14 |
| Mapa de biomas | equirectangular, color del clima | unidad 11 |
| Macro-variación | equirectangular, rompe la repetición a gran escala | unidad 10 |
| Arrays de terreno | albedo y normal, una capa por material | unidades 12 y 13 |
| Tabla de materiales | UBO std140 | binding 12 |

⚠️ El índice de parches reutiliza el MISMO vertex buffer: un parche son los 4 vértices del quad en
vez de los 6 de dos triángulos, así que reusar los vértices es gratis. **El orden del parche es el
ANILLO (a,b,d,c)**, no el de los triángulos: el eval interpola con `mix(mix(p0,p1,u), mix(p3,p2,u), v)`
y con el orden de triángulos el parche sale cruzado.

---

## 2. Render — dos pasadas que describen la misma superficie

No se puede bajar de 39 km a 2 m en un paso: `GL_MAX_TESS_GEN_LEVEL` es **64**, así que un parche de
39,1 km da como mucho 611 m. Por eso hay dos.

```
                39,1 km ──teselación──► 611 m        malla del planeta (todo el globo)
                  128 m ──teselación──►   2 m        clipmap (4 km alrededor de la cámara)
```

### 2.1 Pasada A — malla del planeta

`terrain.vert` → `terrain.tesc` → `terrain.tese` → `biome.frag`, en `assets/shaders/planet/`.

### 2.2 Control: cuánto subdividir

El factor de cada arista se calcula **solo a partir de esa arista**:

```glsl
float edgeFactor(vec3 a, vec3 b) {
    vec3  mid = (a + b) * 0.5 + uCenter.xyz;   // punto medio, relativo a la CÁMARA
    float d   = max(length(mid), 1.0);
    float arc = length(a - b);
    return clamp(arc / (d * 0.012), 1.0, 64.0);
}
```

**Esta es la propiedad que sustituye a toda la maquinaria de cosido**: dos parches vecinos comparten
arista, le pasan los mismos dos extremos, obtienen el mismo número → **no puede haber grietas por
construcción**.

⚠️ Meter cualquier dato del PARCHE (su centro, su normal, su nivel) rompe la simetría y las grietas
vuelven.
⚠️ El interior es el máximo de las aristas. Si fuera menor, el parche se rompe por dentro al no poder
conectar aristas más finas que su relleno.
⚠️ El orden de `gl_TessLevelOuter` en un quad es: 0 = arista v0-v3, 1 = v0-v1, 2 = v1-v2, 3 = v2-v3.
Equivocarlo no da grietas —los factores siguen siendo simétricos— pero subdivide en la dirección
contraria, y el síntoma es "se ve mal" sin nada que lo explique.

### 2.3 Evaluación: dónde va cada vértice

```
dir  = normalize(bilineal de las esquinas)
h    = bilineal de las alturas  +  terrainDetail(dir, R+h, triM)
pos  = dir * (R + h)
```

⚠️ **Se interpolan dirección y altura por SEPARADO, no la posición 3D.** Interpolar la posición metía
la sagita de la cuerda dentro de la altura: a 39 km de parche sobre 6371 km de radio son ~30 m de
hundimiento en el centro. Eso no es relieve, es un artefacto de la malla — y obligaba a la CPU a
replicar la misma cuerda para coincidir.

⚠️ **La normal sale de derivadas de la propia función**, no de los vértices vecinos: dentro del tess
eval no se puede consultar a los contiguos sin geometry shader.

### 2.4 Pasada B — el clipmap, para llegar a 2 m

`clipmap.vert` → `clipmap.tesc` → `clipmap.tese` → `biome.frag`.

**31×31 parches de 128 m = 3968 m de lado**, teselados hasta 32 → **4 m por triángulo**. La malla del
planeta ya tiene 393 000 parches; estos 961 son ruido.

⚠️ **Era 64 → 2 m, y bajó a 32 → 4 m** para que la teselación costara 4× menos (la octava de 22 m
sigue con 5,5 muestras por onda, así que el suelo se ve igual). Lo que NO se propagó fue el piso de
`triM`, que se quedó en 2 m: la octava de 4,5 m seguía entrando al 12,5 % sobre vértices separados
4 m, o sea **sub-Nyquist — exactamente el hervido que describe §3.1**. Hoy el piso se DERIVA del quad
(`TERRAIN_TRIM_FLOOR = TERRAIN_CLIP_PATCH_M / TERRAIN_CLIP_TESS_CAP`, en
[terrain_lod.h](src/core/planet/terrain_lod.h)) y `terrain_lod_invariants` lo vigila.

⚠️ **Ningún tope de teselación puede pasar de 64.** `GL_MAX_TESS_GEN_LEVEL` vale 64 en todo el
hardware de escritorio — es el teselador de función fija, el mismo silicio en GL, Vulkan y D3D.
Pedir más **no da error**: el driver recorta en silencio y el shader hace algo distinto de lo que
dice su comentario. Pasó: el `ringCap` de `terrain.tesc` pidió 128 durante un tiempo, el quad nunca
bajó de ~305 m, y de propina la rampa por anillos no empezaba a los ~12 km sino a los ~32 km.

Y conserva la propiedad por la que se quitaron los chunks: **la rejilla es fija en coordenadas de
CÁMARA**. No se regenera nunca — se reorienta con el marco tangente (`origin`, `tanU`, `tanV`) que se
le pasa por UBO cada frame. Nada que pueda faltar.

Cómo lee la misma altura que la malla: descompone `dir` → `(cara, lx, ly)` con
`harukaDirToCubeFace` ([lib/cube_face.glsl](assets/shaders/lib/cube_face.glsl)) y muestrea el campo
base con **bilineal a mano usando `texelFetch`**.

⚠️ **A mano y no con el filtrado del hardware**: el bilineal de GL usa pesos de precisión limitada
(8 bits en varias GPU) y eso bastaría para que el suelo del clipmap y el de la malla difieran en
centímetros — un escalón justo en el borde de la rejilla.

### 2.5 Cómo se cosen las dos pasadas

Tres cosas, y hacen falta las tres:

1. **Orden**: el clipmap va DESPUÉS de la malla.
2. **Sesgo de profundidad** (`DepthState::biasConstant/biasSlope`, añadido al RHI para esto): las dos
   superficies son coplanares a propósito, así que lo que decide cuál gana es el sesgo, y tiene que
   ganar la rejilla fina. Positivo porque en reversed-Z "más cerca" es profundidad mayor.
3. **Convergencia del detalle en el borde**: el clipmap mezcla su `triM` hacia el de la malla entre
   1400 y 1900 m, de modo que en el borde exterior evalúa **exactamente las mismas octavas** y las dos
   alturas coinciden. Sin esto habría un escalón anular donde acaba la rejilla.

---

## 3. La función de altura

`terrainDetail(dir, radius, minFeatureM)`, en dos ficheros GEMELOS que se cambian **a la vez**:
[terrain_detail.h](src/core/planet/terrain_detail.h) (GL-free) y
[lib/terrain_detail.glsl](assets/shaders/lib/terrain_detail.glsl).

5 octavas de value noise 3D sobre la dirección:

| λ | amplitud | qué aporta |
|---|---|---|
| 2857 m | 260 m | grandes ondulaciones, valles |
| 625 m | 70 m | colinas |
| 111 m | 14 m | lomas |
| 22 m | 3 m | **el relieve que se camina** |
| 4,5 m | 0,7 m | badenes |

### 3.1 El freno de Nyquist, que no es opcional

```glsl
octaveWeight(λ, triM) = clamp(λ / (2·triM) − 1, 0, 1)
```

`triM` es lo que va a medir el triángulo que lleva ese vértice. Sin el freno, una octava de 4,5 m
evaluada en vértices separados 611 m **no se dibuja: se muestrea mal y HIERVE** al mover la cámara.
Es la diferencia entre relieve y ruido.

⚠️ `triM` sale de la DISTANCIA a la cámara, no del factor de teselación del parche. El factor es
discreto por parche, y usarlo dibujaba **una rejilla de 128 m sobre el terreno** — el salto del peso
en la frontera entre parches.

**Consecuencia honesta**: con `triM = 2 m` la octava de 22 m entra entera (3 m de relieve) y la de
4,5 m entra al 12,5 % (≈ 9 cm). O sea, **2 m es el tamaño de triángulo; el relieve más fino que se ve
de verdad es de ~22 m**. Los granos y la hierba no son geometría a ninguna escala razonable: son la
capa de material (§6).

### 3.2 Atenuación bajo el agua

El recorte de zona separa mar y tierra por ±40 m, pero el detalle suma ±151 m: sin atenuar, en una
zona pintada de agua asomaba terreno por encima del mar.

⚠️ **Atenuar por |altura| NO funciona, y se probó**: el campo de placas es casi binario —fondo
oceánico o meseta continental— así que casi toda la tierra está exactamente en el valor del recorte.
La atenuación la redujo al 20 % en todas partes y el terreno volvió a verse plano. La versión buena
atenúa **solo bajo el agua y por profundidad**: sobre tierra el detalle va entero, y si un valle baja
del nivel del mar eso es un lago, que es lo que se quiere.

---

## 4. Clima — y cómo llega al planeta

### 4.1 Dónde se calcula

[climate.h](src/core/planet/climate.h), evaluado **por vértice de la malla base** durante la
construcción y horneado en el vertex buffer y en el campo base.

⚠️ **Hay DOS climas en el motor.** Éste hornea el mapa de biomas (`BiomeClassifyNode`); el que lee el
shader por píxel es el campo de `PlanetFields::computeClimate` (SSBO binding 10). **Tienen que
describir el MISMO planeta** o el color del terreno contradice a la humedad con la que se elige la
textura y con la que se colocan los props. Hoy comparten constantes a propósito.

### 4.2 Temperatura

```
t = 27,0 − 0,0060·latDeg²  −  6,5·max(0, elevKm)
```

≈ 27 °C en el ecuador, ≈ 10 °C a 53°, ≈ −21 °C en el polo, más el gradiente vertical real de 6,5 °C
por km.

⚠️ **El perfil va en GRADOS, no en `|dir.y|`** (que es el *seno* de la latitud). La rampa lineal
anterior daba 15 °C de máximo en TODO el planeta —12 °C por debajo del ecuador real—, así que ninguna
fila cálida podía activarse y el mapa de biomas **no tenía ni sabana ni selva en ninguna latitud**.

### 4.3 Humedad

Sobre el mar, 1,0: el mar *es* la fuente. Sobre tierra, un perfil zonal por celdas de circulación:

```
h = 0,30 + 0,62·gauss(lat, 0°, 12°)      ITCZ, húmedo    → selva ecuatorial
         − 0,24·gauss(lat, 25°, 12°)     subtropical seco → cinturón de desiertos
         + 0,34·gauss(lat, 52°, 14°)     templado húmedo  → bosque
h *= 0,5 + 0,5·clamp((t + 10)/40, 0, 1)  el aire frío retiene menos vapor → seca los polos
si elevKm > 2: sombra de lluvia de altura
```

⚠️ La versión anterior daba **0,3 fijo** sobre tierra, que tras modular por temperatura quedaba en
0,15–0,24 en todo el planeta: por debajo de `forestEdge0` (0,46), así que **bosque y selva eran
inalcanzables** y toda la superficie emergida salía desierto o estepa. De ahí que el planeta se viera
liso y monótono.

### 4.4 Cómo el clima afecta a lo que se ve

Tres caminos distintos, y conviene no confundirlos:

| Camino | Qué decide | Dónde |
|---|---|---|
| **Color** | el mapa de biomas, horneado del clima | textura equirectangular, unidad 11 |
| **Material** | qué textura/tinte/grano se aplica | tabla de materiales, UBO 12 (§6) |
| **Geometría** | nada — el clima **no** mueve el terreno | — |

El clima viaja al fragment por `vClimate = (elev, temp, humedad)`, interpolado desde los vértices o
—en el clipmap— leído del campo base.

---

## 5. Paridad CPU↔GPU — el suelo que se pisa

Es la parte delicada, y el motor ya la pagó una vez ("había TRES suelos y por eso se veían dos
terrenos"). `SimplePlanetInternal::sampleHeight(dir)` reproduce **exactamente** lo que dibuja el tess
eval, y `sampleTerrainHeight` lo consulta antes que la superficie de referencia.

⚠️ **Antes del paso 5 no había suelo en absoluto**: la cadena acababa en `sampleTerrainV2`, un stub
que devolvía 0. Se caminaba sobre una esfera perfectamente lisa viendo montañas.

**Medido**: 4096 direcciones repartidas por la esfera (espiral de Fibonacci, no lat/lon, que
agruparía en los polos) → media 4 µm, **peor caso 34 µm**. El requisito histórico del motor es 1 cm.

### Las minas, todas pagadas ya una vez

- **Hash con aritmética ENTERA.** `fract(p * 0.3183099)` **no puede funcionar a escala planetaria**:
  en la octava fina la coordenada vale ~57 000 y el ulp de float32 ahí es ~0,004, así que `fract`
  conserva dos o tres dígitos. Un ulp de diferencia entre CPU y GPU —que lo hay siempre— daba un hash
  completamente distinto: **39 saltos de hasta 41 m** en 4096 muestras. Con `uint`, que envuelve
  módulo 2³² igual en los dos lenguajes, es bit-exacto.
- **Reducción de celda en DOUBLE**, para que el índice coincida cuando el punto cae justo en el borde
  (1 ulp de `dir` son ~0,38 m sobre la Tierra).
  ⚠️ **Esta sola NO arregla nada, y se probó.** Era la hipótesis obvia, pero al medir salió
  **exactamente el mismo número** antes y después. *Cuando un arreglo no mueve la cifra ni un dígito,
  no es que sea insuficiente: es que no toca la causa.*
- **Escribir `a + (b−a)*t`, nunca `mix`**: el driver puede implementar `mix` con un FMA y eso cambia
  el último bit.
- **`-ffast-math` es una mina**: Release lo activa y autoriza reasociar y fundir en FMA → la paridad
  se rompe **solo en Release**, con los tests en verde. Los ficheros del camino de altura llevan
  `-fno-fast-math -ffp-contract=off`.
- **Sin raíces ni divisiones en el camino común** si se puede evitar: el `sqrt` de double del driver
  (Mesa/ACO) es ~1e-8, no IEEE.

### La inversa cubo→esfera, en forma cerrada

El clipmap necesita `dir → (cara, lx, ly)` **por vértice teselado**, y un Newton con jacobiano
numérico son 12 evaluaciones del spherify. Resulta que sí hay forma cerrada (el fichero decía que no):
con `(s_j, s_k)` las componentes no dominantes,

```
T = s_j² + s_k²      D = 2(s_j² − s_k²)      S = (12T − D²) / (3 + √(9 − 12T + D²))
a = sign(s_j)·√((S+D)/2)        b = sign(s_k)·√((S−D)/2)
```

⚠️ La forma directa `S = 3 − √(9−12T+D²)` **se cancela catastróficamente** en el centro de la cara,
donde T→0 y la raíz→3. De ahí la conjugada.

Validada contra el Newton en el test `cube_sphere_inverse`: **1e-8 en (lx,ly)**, o sea 1e-6 téxeles
sobre una retícula de 256.

⚠️ **El fallo que costó media sesión, y que este test ahora previene**: el clipmap invertía con la
proyección gnómica a secas (y con el signo de `v` cambiado en las seis caras). Leía la altura de
**otro punto del planeta** —normalmente fondo oceánico—, se hundía kilómetros y **no pintaba un solo
píxel**. Ni error de GL, ni shader que no compila, ni draw que falle: terreno sin relieve y ninguna
pista. Se cazó anulando dependencias del shader una a una hasta que la imagen cambió.

---

## 6. Gestión de texturas

### 6.1 El material es una REGLA, no una textura

[terrain_material.h](src/core/planet/terrain_material.h). Un material declara **dónde se aplica**
(rangos de humedad, temperatura y pendiente, con `feather` para que los límites sean degradados y no
líneas rectas) y **cómo se ve** (tinte, grano, detalle, textura, color propio).

La tabla viaja a la GPU en un UBO (binding 12, máx. 16 materiales × 4 vec4 std140) y la recorre
[lib/terrain_material.glsl](assets/shaders/lib/terrain_material.glsl). **Añadir un material es un
objeto más en el JSON de la escena, no un `if` más en GLSL.**

⚠️ El motor **no** conoce ninguna lista de "sand/grass/land/rock". Los defaults no traen ninguna
textura: una escena que no declare `surface.materials` sale con color de bioma y sin grano, que es
honesto — en vez de que el motor invente nombres de PNG que el proyecto quizá no tiene.

### 6.2 Zona pintada: el mapa manda

Si el planeta trae `surface.zoneMap` (equirectangular de **colores planos**, leído con **Nearest**),
la zona gana a las reglas de clima: donde el mapa dice agua, es agua.

⚠️ El PNG **no puede tener degradados ni antialias**: cada píxel tiene que ser exactamente uno de los
colores de la paleta, porque el motor empareja el color con el material más cercano.
⚠️ `submerged` es lo que hace creíble el mapa: una zona de agua tiene que estar **por debajo del nivel
del mar en el campo de alturas**, o asomaría terreno por encima del océano pintado.
⚠️ `baseColor` existe porque la zona decidía material y elevación pero no el tono, así que una isla
pintada salía con forma de isla y **color de desierto** — ganaba el clima de esa latitud.

### 6.3 Capas: el índice es la POSICIÓN

`assignLayers()` asigna a cada material con textura su capa = su posición en la tabla.

⚠️ Que la capa **no** se declare en el JSON quita de en medio toda una clase de fallo: un índice
escrito a mano puede apuntar a la capa de otro material, y el síntoma sería "la roca tiene textura de
hierba" sin nada que lo explique.

Las capas se cargan en un `GL_TEXTURE_2D_ARRAY` (albedo unidad 12, normal unidad 13). La primera capa
que cargue fija la resolución; una capa que falte o no cuadre entra como **gris neutro** y avisa por
log.

### 6.4 Niveles de calidad

`loadTextureArray` empieza en el tier de la calidad elegida y cae hacia abajo:

```
src → 16k → 8k → 4k → hd          Ultra=src  High=16k  Medium=8k  Low=4k
```

Busca primero en los assets del motor y luego en los del proyecto.
⚠️ Antes arrancaba siempre en el primero, así que el ajuste de calidad **no hacía nada**: con calidad
Baja se cargaban igualmente las texturas del tier más alto.

### 6.5 Cómo se aplican: triplanar

**No hay UV en el terreno.** El fragment proyecta la textura desde la posición del mundo en los tres
ejes y las mezcla pesadas por la normal (`triplanarArr` en `biome.frag`). Eso evita tener que
parametrizar una esfera y funciona igual en un acantilado vertical.

`tiling` son **metros por tile** (100 por defecto), y el shader usa `tileScale = 1/tiling`.
⚠️ Estaba al revés (`vFragPos * tiling`), lo que daba tiles de centímetros y anulaba todo el grano.

### 6.6 Los tiles TIENEN que ser sin costura

Un tile que se repite cada 100 m sobre todo un planeta enseña su junta como **una rejilla de 100 m
sobre el suelo**. El generador (`Survival/tools/gen_texture_tiers.sh`) usaba fBm normal, que no puede
teselar.

Ahora el ruido es **periódico**: las celdas de la retícula se envuelven módulo el periodo, así que el
borde derecho interpola contra el mismo vértice que el izquierdo y la junta desaparece **por
construcción**, no por difuminado. Medido en `grass_albedo.png`: **costura 14,05 → 0,05** (dos
columnas contiguas difieren 0,16).

⚠️ Detalles que también rompían la periodicidad: `u = x/(res−1)` duplica la última fila —hay que usar
`x/res`— y la bicúbica de reescalado recortaba el índice en los bordes en vez de envolverlo.

### 6.7 Grano fino — dónde está hoy y qué haría falta

El detalle por debajo de la geometría vive aquí, no en la malla. Pero conviene ser exacto con lo que
da la configuración actual:

| Configuración | m/téxel | Qué se ve |
|---|---|---|
| **Hoy**: tile 4096², `tiling` = 100 m | **24 mm** | grano de terrón, no de grano de arena |
| tile 4096², `tiling` = 4 m | 0,98 mm | grano de verdad, pero la repetición cada 4 m canta |
| dos capas: la de 100 m + una de detalle a 2 m | ~0,5 mm | lo que hace falta para "mirar los granos" |

O sea: **para el objetivo de agacharse y ver el grano falta una segunda capa de detalle**, no subir la
resolución del tile. Bajar `tiling` a secas cambia un problema por otro — la repetición se hace
evidente. La macro-variación (unidad 10) ya rompe la repetición a gran escala; lo que falta es la
capa fina.

---

## 7. Escala real: las dos cosas sin las que nada de esto funciona

- **Render camera-relative**: al shader le llega el centro del planeta *relativo a la cámara*
  (`uCenter = centro − cam`) y una VP **solo de rotación**. La GPU opera a escala de radio planetario
  (~6371 km) y no de coordenadas astronómicas (~150 × 10⁶ km). En float32 la diferencia es entre
  centímetros y kilómetros de error.
- **Reversed-Z con proyección infinita**: precisión de profundidad donde hace falta (cerca) y sin
  plano lejano, que es lo que permite tener a la vez un guijarro a 2 m y el horizonte a 62 km.

---

## 8. Estado

| # | Paso | Estado |
|---|---|---|
| 1 | Etapas de teselación en el RHI (`Patches`, tess control/eval, `patchVertices`) | ✅ |
| 2 | Shaders de control + evaluación, factor por arista | ✅ |
| 3 | Malla base a parches | ✅ |
| 4 | `height()` en GLSL idéntica a la de C++ | ✅ 34 µm medidos |
| 5 | La física muestrea la misma superficie que dibuja la GPU | ✅ |
| 6 | Clipmap centrado en la cámara — los 2 m | ✅ |
| 7 | Coserlo con la malla (sesgo de profundidad + convergencia de `triM`) | ✅ |
| 8 | Anillos concéntricos — LOD real en la pasada A (§9) | ◐ implementado, sin verificar |
| 9 | Per-pixel — rellena el hueco de 2 km → horizonte (§9) | ◐ implementado, sin verificar |
| 10 | Colisión local por anillos, misma estructura que el LOD (§9) | ◐ plan |

**Coste medido**, 1024², solo terreno: **16–26 ms/frame** según altitud. El peor caso son los 26 ms
con la cámara a 300 m, que es cuando el clipmap está entero en pantalla. La evaluación es **por
vértice teselado, no por píxel**: hasta 3,9 M de vértices en el peor caso del clipmap.

### Lo que falta

- **Verificarlo a pie.** Se ve relieve a altura de vuelo bajo; a altura de ojo (~1,7 m) no lo ha
  mirado nadie todavía. El listón de calidad se fija ahí, no en órbita. (Fase 4 del plan §9.)
- **El plan de la mezcla** (anillos concéntricos + per-pixel + culling, con el clipmap intacto, y
  colisión local por anillos) está en §9. Pasos 8 y 9 **implementados en los shaders** (anillos en
  `terrain.tesc.ringCap`, per-pixel en `biome.frag.reanchorToFine`) pero **sin verificar** en el
  motor; el paso 10 (colisión) sigue en plan.
- ~~**Bajar el coste por vértice.**~~ ✅ Hecho: **15 → 5 evaluaciones de ruido por vértice**. El ruido
  es una trilineal con pesos `smoothstep`, así que su derivada es EXACTA y sale de los mismos 8
  hashes que ya se leen para el valor: `terrainDetailGrad` / `harukaTerrainDetailGrad` devuelven
  altura y gradiente juntos, y las dos evaluaciones desplazadas que sacaban la normal por diferencias
  finitas desaparecen. Lo usan `terrain.tese`, `clipmap.tese` y el per-pixel de `biome.frag` (donde
  el ahorro pesa más: es por PÍXEL, no por vértice).

  Además la normal sale **mejor**, no solo más barata: la diferencia finita medía la pendiente MEDIA
  sobre `eps` metros (12 m en la malla base) y aplanaba el relieve que curvara dentro de ese paso.

  ⚠️ Y resuelve de raíz un parche: el `eps` mezclado (3 → 12 m) de `clipmap.tese` existía porque la
  rejilla y la malla sombreaban con pasos distintos, y entonces la rejilla se veía como una CAPA
  aparte flotando sobre el terreno (los "dos terrenos" al subir). Con el gradiente no hay paso que
  igualar: las dos derivan la misma función con el mismo `triM` en el borde.

  Lo vigila `terrain_detail_gradient`, y ese test tuvo que aprender tres cosas antes de servir.

  Las dos primeras: a escala planetaria un paso pequeño es puro redondeo (1 ulp de `dir` en `vec3` son
  0,76 m sobre la Tierra), y el ruido **solo es C¹** — su segunda derivada salta en las fronteras de
  celda, y ninguna centrada que las cruce puede coincidir con el gradiente del centro. Por eso la
  matemática se audita en el espacio propio del ruido, lejos de las fronteras: error medio **0,016 %**.

  La tercera costó un 1,5 %. La comprobación a escala planetaria daba **98,5 %** de acuerdo y eso NO
  era del código, era de la medida, por dos motivos independientes:

  - **El paso.** Estaba en 200 m. Hay una ventana entre dos errores que compiten —el truncamiento de
    la centrada (∝ eps²) y el redondeo de `dir` (0,76 m)— y barriéndola sale una U con óptimo nítido:
    `200 m → 98,50 %` · `50 m → 99,90 %` · `5 m → 98,65 %` · `0,5 m → 85,00 %`. A 200 m mandaba el
    truncamiento; 50 m es 66× el redondeo y 0,018 λ, los dos errores en su mínimo a la vez.
  - **El criterio.** Era *relativo a la pendiente local*, con un suelo de `1e-4` puesto a dedo. Eso
    está mal planteado en los extremos de la octava: allí la pendiente analítica es **exactamente 0** y
    la secante de 100 m no lo es, así que dividir por ~0 hace que cualquier residuo parezca infinito y
    **ningún gradiente puede aprobar**. Los 2 puntos que seguían fallando con 50 m tenían pendiente al
    0,00 % y al 0,82 % del pico: eran exactamente eso.

  El criterio correcto es **absoluto contra la pendiente pico de la octava** — un listón físico fijo en
  vez de un cociente que explota — y el signo solo se exige donde la pendiente existe (>5 % del pico).
  Con eso: **magnitud 2000/2000 · signo 1643/1643 · peor desvío 0,86 % del pico**.

  Y es **más estricto** que el anterior, no más laxo: un gradiente escalado un 3 % —error que el 50 %
  relativo dejaba pasar en silencio— lo suspende (1941/2000, peor desvío 3,01 %). Verificado por
  mutación.
- Los bugs abiertos del terreno están en [TODO.md](TODO.md).

### Deuda de arquitectura que toca este pipeline

`PlanetarySystem` **construye el planeta en vez de que el planeta se construya a sí mismo**: el
planeta real es `SimplePlanetInternal`, un struct anidado y privado dentro del `.cpp` al que
`addSimplePlanet` y `renderSimplePlanet` acceden por `p->` en 27 miembros distintos. Lo único que el
planeta hace por su cuenta es `sampleHeight` — y no es casualidad que sea la pieza que menos
problemas ha dado. La descomposición pendiente es `Planet` (se construye solo) + `PlanetRenderer`
(los pipelines, que hoy son `static` dentro del planeta precisamente porque no son suyos) +
`PlanetarySystem` (órbitas y escena, que es lo que dice su nombre).

---

## 9. La mezcla: clipmap (intacto) + anillos concéntricos + per-pixel + colisión local

**Objetivo**: quitar el único "punto de coste" del terreno —un solo clipmap de 2 m que llega a ~2 km
y de ahí salta a triángulos de 611 m sin octavas finas— sustituyéndolo por tres capas que se reparten
la distancia. El clipmap **no se toca**: sigue siendo quien da la geometría real de 2 m cerca del
jugador (silueta y oclusión verdaderas). Lo que se añade son las capas que hacen el resto.

```
        ┌──────── clipmap (intacto) ────────┐
   2 m ▮ ▮ ▮ ▮ ▮ ▮ ▮ ▮ ▮ ▮ ▮ ▮ ▮ ▮ ▮ ▮ ▮ │ 0 ─ 2 km : geometría real, 128 m→2 m
                                          │
   anillos concéntricos (LOD de la malla)  │ 2 km ─ 60 km : malla base teselada por anillos
   + per-pixel (fragment)                  │              (128 m, 512 m, 2048 m…), relieve
                                          │              fino por SHADING, no por geometría
                                          │
   esfera base, sin detalle                │ > 60 km : como hoy, con culling
```

### Por qué es correcto (y qué problema resuelve cada pieza)

| Problema de hoy | Pieza que lo resuelve |
|---|---|
| Una sola rejilla fina → o la pagas entera cerca o no existe | **Anillos concéntricos**: LOD de la malla base por distancia a la cámara |
| El relieve fino (octavas de 22 m y 4,5 m) muere a 611 m por el freno de Nyquist (§3.1) | **Per-pixel**: el fragment muestrea el heightfield; el tamaño de muestra es el píxel, no el triángulo, así que el freno de Nyquist deja de aplicar |
| Dos superficies coplanares cosidas con sesgo de profundidad = acoplamiento frágil | **Per-pixel**: el clipmap ya no necesita ganarle a la malla base; la malla base *es* la que pinta el detalle fino fuera del clipmap |
| Malla de colisión de 96 m que se reconstruye por movimiento | **Colisión local por anillos**: parche de ~200-300 km con resolución por anillos (densa cerca, gruesa lejos), misma `sampleHeight` que el render |

### La decisión clave: per-pixel en vez de más teselación

El clipmap actual **ya** llega a 2 m de triángulo. La limitación no es la densidad: es el **radio**.
Extender el clipmap a 8 km costaría 16× su área de rejilla por un detalle que el ojo, a esa distancia,
no distingue de un triángulo de 10 m. La alternativa natural —más anillos de clipmap— multiplica el
coste de la pasada B y vuelve a introducir costuras.

El per-pixel evita ambas cosas: el fragment, al evaluar la misma `heightFunction` que la CPU, puede
devolver relieve fino **sin generar vértices**. El coste pasa de "por vértice teselado" a "por píxel
visible", que se recorta con culling y con LOD de pasos del raymarch por distancia.

⚠️ **Honestidad sobre el per-pixel**: no mueve la silueta (el contorno del terreno contra el cielo
sigue siendo la malla teselada, que en el rango de anillos es de ~10-50 m → un brete lejano se ve
poligonal, no suave). Ese es el límite aceptado de la mezcla; si más adelante se quiere silueta fina
también, se añade un anillo más de clipmap sin tocar el diseño.

### Plan de implementación (por fases, cada una verificable a pie)

1. **Anillos concéntricos (pasada A)**. La malla base deja de teselar "por distancia a un parche" y
   pasa a teselar **por anillo**: factor de teselación por rango de distancia a la cámara (cerca alto,
   lejos bajo). El `terrain.tese` ya tiene el `triM` por distancia (§3.1); lo que cambia es la cota
   superior del factor por anillo y el *culling* (no enviar parches enteros fuera del cono o más allá
   de un radio). Resultado verificable: la malla base sola ya no muestra la línea de los 611 m.
2. **Per-pixel (pasada A + biome.frag)**. En el fragment, raymarch corto sobre el heightfield del
   campo base (unidad 15) para re-anclar `vFragPos` a la superficie fina antes de triplanar e iluminar.
   Con **LOD de pasos por distancia**: 8-16 pasos cerca, 2-4 lejos. El clipmap sigue pintando por
   delante (coplanares, con su sesgo). Resultado verificable: desde 1,7 m de ojo, el grano fino se ve
   hasta el horizonte, sin línea de corte a 2 km.

**Cómo se implementó** (biome.frag, `reanchorToFine`): el fragmento re-evalúa la MISMA
   `harukaTerrainDetail` pero con `triM` de PÍXEL (`dist·0.002`, piso 0,5 m), y un sphere-trace de
   Newton sobre el rayo de la vista hasta cruzar la superficie fina `R + baseH + detail(pixelTriM)`,
   con salida temprana (|Δ| < 5 cm) y pasos LOD por distancia (mix 16→2 entre 3000 y 15000 m). Se
   limita **fuera de la caja del clipmap** (el mismo test `inClip` de `terrain.tese`, con
   `uClipCover.x` y `uDebug.w`) y con bake presente (`uDebug.z > 0.5`): dentro de la rejilla fina ya
   da el relieve real y repintarlo descosería el sesgo. Tras re-anclar, la normal se re-evalúa por
   diferencias finitas de la misma función (`eps` = triM de píxel) y la elevación en km usa el
   relieve fino (`baseH + hFine`), así que orilla y arena siguen el grano. Sources nuevos para el
   fragmento: `uHeightTex` (binding 16, ya enlazada en planet.cpp:2592) y `ClipParams` (UBO binding
   13, mismo bloque que usan los tese; no choca con el sampler2DArray 13 porque UBO y textura son
   espacios de binding distintos). El coste es **por píxel visible de la malla base**, no por vértice
   teselado: la pasada del clipmap NO lo paga (se descarta dentro de la caja), así que solo pesa en
   la zona 2 km → horizonte.
3. **Colisión local por anillos (física)**. `terrainMesh` (world_system_provider.h) pasa de la rejilla
   uniforme de 96 m a un parche de ~200-300 km con resolución por anillos (densa en el centro, gruesa
   en el borde). Misma `sampleHeight` → la física sigue pisando exactamente lo que se dibuja.
   Verificable: caminar hasta el borde de la malla sin caerse y sin ver terreno distinto del que se pisa.
4. **Culling por anillo** (puede ir con la 1). Parches fuera del cono de visión o más allá de su radio
   de anillo no se envían.

### Colisión local por anillos, en detalle

Hoy (physics_engine.cpp:349-363): parche de 96 m, rejilla uniforme 32×32, reconstruido en worker al
moverse el jugador >48 m o al refinarse el terreno >0,5 m. Se amplía a un parche de ~200-300 km con la
MISMA estructura que los anillos visuales (rejilla geométrica: paso ~3 m al pie, ×1,12 por anillo):

- **Interior** (lo que se pisa): paso ~3 m (el centro del parche).
- **Medio**: el paso crece ×1,12 por anillo de distancia.
- **Exterior** (~hasta 200-300 km): paso kilométrico — el horizonte lejano, que la colisión alcanza
  igual que lo dibuja el render (decisión: ampliar el parche de 2 km a 200-300 km, "la colisión llega
  donde llega el render"; el resto de fases no lo necesita).

El parche nuevo se construye igual en un worker y se intercambia sin hueco. El coste de muestreo
crece logarítmicamente con el radio (la rejilla uniforme de 300 km a paso 3 m serían 10¹⁰ vértices;
por anillos, ~25×10³ — medido: 159×159 = 25 281 vértices / ~50 k triángulos). ⚠️ Los anillos tienen
que compartir la **misma `triM`→paso** que el render, o la física pisará un suelo distinto del
dibujado — la regla de paridad CPU↔GPU de §5 no se negocia.

### ⚠️ La disparidad real, y por qué el test anterior no la veía

`detail_triM_parity` decía medir esto y daba **0.0000 m**. Era una tautología: calculaba el `triM` de
la física y el del clipmap **con la misma expresión escrita dos veces**, evaluaba `terrainDetail` con
ambas y comprobaba que coincidían. Comparaba `f(x)` con `f(x)` — no podía fallar, y habría seguido en
verde con el motor cambiado debajo, porque el test **nunca llamaba al código del motor**: se escribía
su propia copia de la fórmula.

Y medía el sitio equivocado. La disparidad no está entre dos evaluaciones de la función continua:
está entre las dos **teselaciones** de ella. El render dibuja quads de 4 m y la colisión una rejilla
cuyo paso crece ×1,12; dentro de cada celda las dos son planos, así que se separan de la función —y
entre sí— por la **sagita** de la celda. Medido por `terrain_chord_error`:

| Dónde | Paso de la colisión | Sagita |
|---|---|---|
| En el pie (render vs colisión) | 3 m | **6,2 cm** peor · 0,9 cm típico |
| 84 m | 13,1 m | 0,23 m |
| 340 m – 1840 m (tope `TERRAIN_RING_MAX_STEP_IN_BOX`) | 25,0 m | 0,44 – 0,90 m |
| **Peor caso DENTRO de la caja del clipmap (±1984 m)** | 25,0 m | **1,20 m** |

`TERRAIN_RING_MAX_STEP_IN_BOX = 25` ya recortó esto: sin el tope, a 1,6 km el paso llegaba a 198,7 m y
la sagita a **10,18 m**. Con el tope, el peor caso dentro de la caja es 1,20 m.

**Al jugador caminando no le afecta** —la malla se recentra en él y se reconstruye al derivar >48 m,
así que siempre pisa la zona de paso 3 m—, pero sí a todo lo que colisiona lejos: objetos lanzados,
vehículos, otras entidades y el anclado de construcción.

#### ✅ EL LISTÓN DE 1 CM, SUPERADO: es CERO

El listón declarado era 1 cm y no se cumplía: 6,2 cm en el peor caso a pie, 1,20 m en el borde de la
caja. Y no se arreglaba apretando un número, porque la causa era estructural:

> **El render y la colisión teselan la misma función con retículas DISTINTAS.** El render usa quads
> uniformes de 4 m (`TERRAIN_CLIP_QUAD_M`) en toda la caja; la colisión usa anillos geométricos de
> 3 m creciendo ×1,12 con tope en 25 m. Dentro de cada celda las dos son planos, así que se separan
> por la sagita de la celda MÁS GRUESA. Mientras las retículas no coincidan, la disparidad no puede
> bajar de ahí.

Bajar el crecimiento de los anillos hasta que la sagita llegue a 1 cm exige celdas ~11× más finas
(≈2,3 m), y eso no es el arreglo bueno: **si las dos retículas COINCIDEN, la disparidad no es 1 cm,
es exactamente 0** — mismos vértices, misma interpolación bilineal, mismo marco anclado (que
`terrainClipFrame(..., snapRadius)` ya proporciona y que la colisión ya usa).

Implementado así: `TERRAIN_RING_INNER_STEP` pasa a **ser** `TERRAIN_CLIP_QUAD_M` (era 3,0 m, un número
redondo sin relación con el render) y la rejilla gana un **bloque uniforme** de radio
`TERRAIN_COLLIDE_UNIFORM_M = 256 m` en el que el paso no crece, así que todos sus nodos caen en
múltiplos exactos del quad. Tres regímenes: uniforme dentro del bloque, ×1,12 topado a 25 m dentro de
la caja del clipmap, libre fuera.

| Radio del bloque uniforme a 4 m | Vértices | Triángulos |
|---|---|---|
| ±128 m | 4 225 | 8 192 |
| **±256 m — elegido** | **16 641** | **32 768** |
| ±512 m | 66 049 | 131 072 |
| ±1984 m (toda la caja) | 986 049 | 1 968 128 |

Toda la caja es inviable (2 M de triángulos por reconstrucción de la `MeshShape`). ±256 m basta con
margen de 5× porque la malla se reconstruye en cuanto el jugador deriva 48 m: no puede salirse del
bloque fino entre reconstrucciones.

**Resultado medido:**

```
en el pie · render(quad 4 m) vs función: 0.0843 m · colisión(rejilla real) vs función: 0.0843 m
en el pie · RENDER vs COLISIÓN: peor 0.000000 m · media 0.000000 m
```

Cero exacto, y las dos se separan de la función por la misma cantidad — que es la firma de que teselan
igual. La sagita de 1,20 m queda relegada a donde el jugador no puede estar (el peor caso dentro de la
caja baja a 1,05 m, fuera del bloque); allí siguen los anillos geométricos, que es lo correcto para
objetos lanzados y vehículos a kilómetros.

**El coste, medido en la rejilla completa** (no solo el bloque):

| | Vértices | Triángulos |
|---|---|---|
| Antes (paso 3 m, sin bloque) | 187² = 34 969 | 69 192 |
| Ahora (bloque ±256 m) | 297² = **88 209** | **175 232** |
| | | **×2,52** |

Asumible porque la `MeshShape` se construye fuera del hilo de física, pero es un coste real: subir el
radio a ±512 m lo pondría en ×5.

⚠️ **Y el test tuvo que cambiar para no volverse una tautología.** Con
`TERRAIN_RING_INNER_STEP == TERRAIN_CLIP_QUAD_M`, comparar `fPoly(QUAD)` contra `fPoly(INNER_STEP)`
es escribir la misma expresión dos veces — exactamente el pecado de `detail_triM_parity`, y un cero
así no probaría nada. Ahora la celda de la colisión se localiza por búsqueda binaria sobre la rejilla
que `terrainRingGrid` construye de verdad, y `terrain_ring_grid` comprueba el invariante estructural:
cada nodo del bloque es múltiplo EXACTO del quad, el paso no crece dentro, y el radio cubre la deriva
de reconstrucción. Verificado por mutación — volver al paso de 3 m reproduce exactamente los 0,0622 m
de antes y suspende cinco aserciones.

### Regla: las cifras de LOD viven en un solo sitio

Lo que hizo posible la tautología es que la fórmula del `triM` y la de la rejilla estaban **escritas a
mano en cuatro sitios** (`clipmap.tese`, `terrain.tese`, `world_system_provider.h` y el propio test).
`terrain_detail.h` ya había resuelto bien ese problema para la altura con los dos ficheros gemelos
(§3); lo que faltaba era aplicarlo al LOD. Hoy `terrain_lod.h` es la única definición de
`terrainTriM`, la rejilla por anillos y el marco tangente del clipmap — el motor y los tests llaman a
las mismas funciones, y los shaders son sus gemelos declarados.
