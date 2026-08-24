# La superficie del planeta: materiales, zonas, estratos y props

Guía práctica de **cómo se decide qué hay en cada punto del suelo** y cómo declararlo desde una
escena. Escrita después de una sesión larga en la que varias de estas cosas se pisaban entre sí; las
trampas del final son todas reales y están medidas.

---

## 1. Las tres preguntas

El suelo responde a tres preguntas distintas, y meterlas en el mismo saco es de donde salen casi
todos los problemas:

| Pregunta | Mecanismo | Ejemplo |
|---|---|---|
| **¿De qué está hecho?** | material con **bandas** | roca, arena, tierra |
| **¿Dónde mando yo?** | **zona** (pintada o geométrica) | un pueblo, un cráter, un oasis |
| **¿Qué hay ENCIMA?** | **props** | árboles, piedras, casas |

Y una cuarta, vertical, que atraviesa a las anteriores:

| | |
|---|---|
| **¿Qué hay DEBAJO?** | **estratos**: lecho y cobertura |

---

## 2. Materiales: el mundo por defecto

Un material se declara en `surfaceConfig.materials`. Sus **bandas** dicen dónde vive:

```json
{
  "name": "taiga",
  "albedo": "grass_albedo.png",
  "normal": "grass_normal.png",
  "color":  [0.17, 0.26, 0.18],
  "tempC":  [-25, 10],
  "elevKm": [0.0, 2.6]
}
```

Cuatro ejes, todos opcionales: `humidity` [0,1], `tempC` (°C), `slope` (0 llano … 1 vertical) y
`elevKm` (km sobre el nivel del mar). Los bordes se difuminan con `feather` (y `elevFeatherKm` para
la altura, en km, porque compartir el mismo número daría 80 m de degradado en una transición que
necesita cientos).

> ⚠️ **Un material SIN bandas pesa 1 en todo el planeta.** Los valores por defecto son el rango
> completo (`humedad 0..1`, `temp ±1000`, `pendiente 0..1`, `altura ±1000 km`). Eso está bien para
> **uno** —el suelo por defecto— y es un desastre para siete: sus colores se promedian en cada píxel.
> Medido en esta escena, con siete materiales sueltos el color de fondo del planeta era
> **(0.30, 0.38, 0.41)**, un gris-verde-azulado que no es de ninguno, con los azules del mar
> promediados hasta en el desierto.

**Regla:** exactamente **un** material sin bandas (el fallback). Todo lo demás, acotado.

### El color y la textura

- `color` + `colorWeight` — sustituye al color del mapa de biomas donde manda ese material.
- `albedo`/`normal` — nombre del PNG. **Dos materiales con el mismo PNG comparten capa** del array
  de texturas (se deduplica por fichero), así que repetir `grass_albedo.png` en cuatro materiales no
  cuesta memoria.
- `tint` multiplica al color del bioma; `grain` y `detail` gradúan cuánto aportan la textura y su
  normal map.

---

## 3. Estratos: lecho y cobertura

El suelo es una **columna**, no una lista de candidatos. Cada material declara su papel:

```json
{ "name": "rock", "role": "bedrock", "albedo": "rock_albedo.png" }
{ "name": "sand", "role": "cover",   "elevKm": [-0.4, 0.35] }     // "cover" es el valor por defecto
```

- **`bedrock`** — la roca de debajo. Asoma donde el sedimento no se agarra.
- **`cover`** — el manto suelto de encima (arena, tierra, regolito), con **espesor**.

El espesor sale de la física del sitio (`core/planet/terrain_strata.h`), no de una regla de arte:

- **pendiente** — el sedimento resbala. Ángulo de reposo ~34° (`slope01 ≈ 0.17`); pasado 0.35 la
  cobertura es 0. *Por eso un cortado se ve de roca.*
- **humedad** — el suelo vivo genera manto: ×0,35 en roca desnuda, ×1,0 en pradera.
- **cuenca** — bajo el nivel del mar el sedimento se acumula (×2,6). *Por eso la plataforma es arena.*

Resultado típico: ~2,5 m de manto en llano templado, ~9 m en cuenca costera, 0 en un cortado.

Se eligen **un lecho y una cobertura por separado** y se mezclan por el espesor. No se promedian
entre grupos. Y si en un punto **no hay ninguna cobertura candidata**, asoma el lecho — que es lo que
hace que la llanura abisal sea roca sin declararlo en ninguna parte.

---

## 4. Zonas: donde mandas tú

Hay **dos mecanismos distintos** con el mismo nombre. Conviene tenerlos separados en la cabeza.

### 4.1 Zona pintada (color en el zoneMap)

El material declara un color; donde el `zoneMap` tenga ese color, manda ese material.

```json
"zoneMap": "assets/textures/earth_zones_8192.png",
"materials": [
  { "name": "oasis", "zone": [42, 106, 186], "albedo": "grass_albedo.png" }
]
```

- Colores en **0-255**. Tolerancia de ~28 por canal (absorbe el antialias del pincel y la
  recompresión del PNG). **Negro = sin pintar**, por convención explícita.
- El muestreo es bilineal y luego cae al color de paleta más cercano: la línea de costa de una zona
  no sale cuadriculada.

> ⚠️ **Declarar `zone` significa "SOLO aquí".** Un material con zona no compite fuera de ella. Antes
> la zona solo sumaba un bonus dentro y no restaba fuera —media regla—, y por eso materiales pensados
> para pintarse a mano acababan de fondo en todo el planeta.

**Consecuencia:** `zone` y bandas juntas son ambiguas. Si le pones zona a un material con bandas de
clima, sus bandas dejan de tener efecto. Elige uno:

- **bandas = el mundo** (aparece por clima, en todas partes donde toque)
- **zona = tu excepción** (aparece solo donde pintes)

### 4.2 Zona geométrica (círculo o polígono)

Independiente de los materiales: existe aunque ningún material se llame así. Va en `surfaceConfig.zones`.

```json
"zones": [
  { "name": "pueblo", "center": [49.83, -41.13], "radiusM": 600.0 },
  { "name": "puerto", "perimeter": [[10.1, 20.2], [10.4, 20.9], [9.8, 21.1]] }
]
```

- `center` es **[lat, lon] en grados**; `radiusM` en metros.
- `perimeter` es una lista de [lat, lon] — forma libre.
- **Varias zonas pueden compartir nombre**: quien las referencia usa el nombre, no la instancia. Diez
  entradas `"pueblo"` = diez pueblos.
- Puede tener `color` además de geometría: entonces es pintada **y** geométrica.

Este es el mecanismo para "esto va aquí porque lo digo yo", y el que un generador automático debería
escribir el día que lo haya: solo tiene que añadir entradas a este array.

---

## 5. Props: lo que hay encima

Se declaran en `surfaceConfig.propLayers`. Tienen **bandas propias** (mismos cuatro ejes) más dos
filtros de zona:

```json
{
  "name": "pueblo", "mesh": "house",
  "when": "zone == pueblo",
  "slopeMin": 0.0, "slopeMax": 0.2,
  "density": 0.5, "claimRadius": 3.0,
  "scaleMin": 0.8, "scaleMax": 1.0
}
```

- **`zones`** (lista de nombres) — veto **duro**: si está y el punto no cae en ninguna, no instala.
  Sin zona declarada en el planeta → denegado, no "por defecto sí".
- **`when`** (expresión booleana) — sobre las dos identidades del punto: `layer` (el material) y
  `zone` (la zona). Soporta `&&`, `||`, `!`, `==`, `!=` y paréntesis:

  ```
  when: "zone == oasis"                     solo en el oasis
  when: "layer != sand"                     en cualquier sitio menos arena
  when: "layer != sand || zone == oasis"    no en arena, pero sí en el oasis
  ```

  Se evalúa **además** de `zones`: con los dos, hay que pasar los dos.
- **`claimRadius`** — radio de exclusión que deja cada instancia; las capas posteriores que caigan
  dentro se descartan. 0 para hierba o líquen, varios metros para una casa.

> Para una zona pintada a mano, **quítale las bandas de clima al prop**. Dentro de un círculo de
> 600 m la humedad es casi constante: o pasa la zona entera o no pasa ninguna instancia, y el fallo
> no deja más rastro que un contador en el log.

---

## 6. Ejemplo trabajado: "quiero una zona de musgo"

La pregunta correcta no es "¿qué material creo?" sino **qué es el musgo**:

- **No es sustrato.** Musgo sobre roca *es roca con musgo encima*. Gastar una capa del array de
  texturas (67 MB a 4K) en un tinte verde es pagar memoria por algo que resuelven la geometría y el
  color.
- **Es cubierta.** O sea: props + tinte +, si quieres, deformación del terreno.

Receta:

1. **Zona geométrica** donde va, en `zones`: `{ "name": "musgo", "perimeter": [...] }`.
2. **Capa de props** con `when: "zone == musgo"`, `claimRadius: 0` (no reclama, deja pasar otras
   capas) y densidad alta.
3. Si además quieres que el suelo tire a verde, **un material con `zone`** del mismo nombre y sin
   bandas — recuerda: con zona, solo pinta ahí.

Y lo que **no** hay que hacer: crear un material `musgo` sin bandas y sin zona. Eso lo mete en el
promedio de todo el planeta.

---

## 7. Lo que NO es una capa

Dos cosas que parecen material y no lo son:

- **Agua.** El mar, los lagos y los ríos son **una superficie** alimentada por un campo
  (`profundidad = nivel del agua − cota del suelo`). Un material marcado `submerged` se **salta** en
  la selección del terreno: su sitio es el sombreado del agua, no el albedo del suelo.
- **Nieve, fango, humedad.** Son **estado dinámico** que modula lo que haya debajo, no un estrato.
  Cambian en minutos y no se pueden hornear. Su sitio es `uWet` (la CPU integra: mojarse ~30 s,
  secarse ~4 min).

---

## 8. Trampas conocidas (todas pisadas de verdad)

| Síntoma | Causa | Arreglo |
|---|---|---|
| Todo el planeta tiene un velo de color raro | varios materiales sin bandas → **promedio** | acotar; dejar UNO sin bandas |
| Una textura aparece donde no toca | empate de `score` → gana el **primero declarado** | dar bandas o `priority` |
| Un material pintado sale también de fondo | tenía `zone` **y** bandas | elegir uno |
| El fondo del mar se ve de hierba | la cobertura por defecto reclama bajo el agua | acotar su `elevKm` a la tierra emergida |
| Una capa de props no aparece nunca | `when` referencia una zona inexistente | crear la zona o quitar el `when` |
| Props ausentes "por humedad" | el aviso mide el **parche** local, no el planeta | comprobar el rango real del planeta antes de tocar bandas |
| El array de texturas ocupa el triple | el mismo PNG en varios materiales | ya se deduplica por fichero |
| El suelo cercano se ve **por triángulos** | la normal era per-vértice dentro del clipmap (cada 4 m) | arreglado 2026-08-18: `biome.frag` la calcula por píxel también dentro |
| La sombra es del mismo gris a cualquier hora | el color de sombra era una **constante** en 4 de 5 shaders | arreglado 2026-08-18: `lib/surface_shade.glsl` lo deriva del ambiente real |
| Tocas un shader y no cambia nada | su `.spv` no se regeneró y el motor usa el GLSL del driver | comprobar que el `.spv` existe; `cmake -S . -B build` si el shader es nuevo |

Y la meta-trampa, que costó más que todas juntas: **cuando algo no cambia al tocarlo, comprueba que
tu código llega a ejecutarse** —build de todos los repos, binario recién enlazado— antes de buscar la
causa en el render.
