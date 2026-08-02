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
| Campo base (elevación, temperatura, humedad), textura array de 6 capas RGB32F | 4,8 MB |
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
| **Campo base** | array 6 capas RGB32F = `(elev m, temp °C, humedad)` | unidad 15 |
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

**31×31 parches de 128 m = 3968 m de lado**, teselados hasta 64 → **2 m por triángulo**. La malla del
planeta ya tiene 393 000 parches; estos 961 son ruido.

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

**Coste medido**, 1024², solo terreno: **16–26 ms/frame** según altitud. El peor caso son los 26 ms
con la cámara a 300 m, que es cuando el clipmap está entero en pantalla. La evaluación es **por
vértice teselado, no por píxel**: hasta 3,9 M de vértices en el peor caso del clipmap.

### Lo que falta

- **Verificarlo a pie.** Se ve relieve a altura de vuelo bajo; a altura de ojo (~1,7 m) no lo ha
  mirado nadie todavía. El listón de calidad se fija ahí, no en órbita.
- **Anillos concéntricos.** El clipmap llega a ~2 km y a partir de ahí se vuelve a 611 m de triángulo
  sin octavas finas. En pantalla se nota como una línea donde se acaba el relieve. Lo natural son
  anillos (128 m, 512 m, 2048 m…) en vez de una sola rejilla.
- **Bajar el coste por vértice.** 15 evaluaciones de ruido por vértice (3 muestras × 5 octavas: altura
  más las dos diferencias finitas de la normal). Hay margen obvio.
- Los bugs abiertos del terreno están en [TODO.md](TODO.md).

### Deuda de arquitectura que toca este pipeline

`PlanetarySystem` **construye el planeta en vez de que el planeta se construya a sí mismo**: el
planeta real es `SimplePlanetInternal`, un struct anidado y privado dentro del `.cpp` al que
`addSimplePlanet` y `renderSimplePlanet` acceden por `p->` en 27 miembros distintos. Lo único que el
planeta hace por su cuenta es `sampleHeight` — y no es casualidad que sea la pieza que menos
problemas ha dado. La descomposición pendiente es `Planet` (se construye solo) + `PlanetRenderer`
(los pipelines, que hoy son `static` dentro del planeta precisamente porque no son suyos) +
`PlanetarySystem` (órbitas y escena, que es lo que dice su nombre).
