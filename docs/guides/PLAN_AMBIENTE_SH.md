# Plan de luz ambiente: SH desde el cielo procedural (decisión 2026-08-06)

## 0. La decisión, en una línea

Sustituir la luz ambiente actual (constantes por sitio + IBL que nadie muestrea) por un **ambiente
procedural en armónicos esféricos** integrado desde el MISMO cielo que se dibuja, y repartirlo a todos
los shaders con **un solo UBO de coeficientes SH**. Esto cierra de raíz el problema de las "tres
constantes desincronizadas" y hace que el IBL (código pesado sin consumidor) deje de ser deuda.

**Recomendada frente a las alternativas** (detalle en §6): IBL por cubemaps exige 3 convoluciones
(irradiance + prefilter + BRDF LUT), 4 texturas, pipelines y targets que hoy no se conectan a nada,
para una estética **toon** que ni siquiera usa BRDF de PBR. SH de orden 2 (9 coeficientes) es el
mínimo correcto para irradiancia difusa y cuesta 1 UBO de 48 B.

---

## 1. Qué hay hoy (auditoría, 2026-08-06)

### 1.1 Tres "ambientes" que se ignoran entre sí

| Sitio | Código | Qué es |
|---|---|---|
| `final.frag:87` | `shadowCol = baseColor * vec3(0.40, 0.46, 0.60) * ao` | tint de sombra TOON del pase de objetos |
| `sky.frag:139` | `cShad = vec3(0.46, 0.52, 0.67)` | base fría de las nubes (color de dibujado) |
| `planet.cpp:2402` | `ubo.uAmbient = ambientStrength * vec3(0.55, 0.65, 0.85)` | UBO del terreno (clipmap/simple/textured/biome/water) |

Más el escalar `ambientStrength` del `PerFrameData` (`application_render.cpp:811` = `mix(0.11, 0.28, day)`,
`application_render.cpp:1965` = `0.25`), que llega a todos los shaders como `ambientStrength` pero que
**cada consumidor interpreta como le da la gana** (`final.frag` ni siquiera lo usa para la sombra: lo
multiplica `prop_inst.frag`, `softbody.frag`, `fluid_particle.frag`, `water.frag:283`, `preview.frag:56`
con `max(ambientStrength, 0.15)`, etc.).

Resultado: el término ambiente del terreno (`vec3(0.55,0.65,0.85)` fijo) no se parece al tint de sombra
de los objetos (`vec3(0.40,0.46,0.60)` fijo) ni al cielo que se ve (`sky.frag`). Misma hora del día en
tres colores distintos. **No es un bug que se vea aislado**: es una desincronización que se nota en
cuanto dos superficies comparten plano.

### 1.2 IBL: código que genera texturas que nadie lee

`src/renderer/ibl.cpp` (328 líneas) construye toda la tubería:

- `setupCubemap()` → `hEnv` (cubemap RGB16F 512³)
- `generateDefaultSky()` → rellena `hEnv` con un cielo planimétrico
- `generateIrradianceMap()` → `hIrradiance` (convolución difusa)
- `generatePrefilterMap()` → `hPrefilter` (prefiltered env por mip)
- `generateBRDFLUT()` → `hBrdf` (LUT RG16F 512²)

Pero **ningún shader de `assets/shaders/` muestra `uIrradiance`, `uPrefilter` ni `uBrdf`** (grep
exhaustivo, 2026-08-06: cero coincidencias). `final.frag` declara `enableIBL` en el `PerFrameData`
(`final.frag:30`) y `application_render.cpp:839` lo rellena, pero **no se usa en el shader**. El único
uso real de `_ibl` es construirlo (`application_render.cpp` → `application.h:482`) y destruirlo
(`application.cpp:275`). El README ya lo marca como "no conectado"; esta auditoría lo confirma: **es
deuda sin consumidor, no una feature a medias**.

### 1.3 La semilla del plan ya existe: `lib/sky_palette.glsl`

`assets/shaders/lib/sky_palette.glsl` (escrito por el usuario, en working copy sin commit) define:

- los 4 colores de la cúpula (`HARUKA_ZENITH_NIGHT/DAY`, `HARUKA_HORIZ_NIGHT/DAY`)
- `harukaSkyDay(sunElev)` → factor día/noche
- `harukaSkyBase(t, day)` → gradiente horizonte→cénit (interpolado noche↔día)

y **`sky.frag` ya lo usa** (`sky.frag:76-77`). Su cabecera declara ser gemelo de
`src/core/sky_ambient.{h,cpp}` — **pero ese fichero no existe** (glob 2026-08-06). El plan no inventa
una pieza nueva: rellena la mitad del contrato que el propio shader ya declara.

---

## 2. Arquitectura objetivo

```
src/core/sky_ambient.{h,cpp}   ← NUEVO (el gemelo CPU que sky_palette.glsl ya promete)
  · harukaSkyBase(t, day)       · mismo gradiente que el GLSL (mismas constantes, doble del GLSL)
  · skyDiffuseIrradiance(dir)   · integra el cielo a SH (ver §3) o por muestreo directo
  · coeffs() → 9 × vec3         · coeficientes SH de banda 0..2, actualizados por frame

PerFrameData (UBO, binding 0)   ← SE AMPLÍA
  + vec4 shCoef0;                y  shCoef1..shCoef8 → 9 vec4 = 144 B (std140: vec3+pad)

final.frag / prop_inst / softbody / fluid / water / planet/* / preview
  ← consumen:  float ndlAmbient = dot(shCoeff(dir, normal))   (o irradianceDiffuse(N))
  ← suman al término ambiente en vez de su constante hardcodeada
```

Clave del diseño: **el que dibuja el cielo y el que ilumina los objetos comparten la MISMA paleta**,
porque ambos parten de `harukaSkyBase`/`harukaSkyDay`. Cualquier retoque de hora del día se propaga a
los dos lados — se acaba la desincronización.

---

## 3. La integración a SH (dos opciones, elegir al implementar)

### Opción B1 — SH por muestreo de `harukaSkyBase` (recomendada)

La irradiancia difusa en un punto con normal `N` es `∫ L(ω) max(ω·N,0) dω`. Con los 9 coeficientes SH
de banda 0..2 basta para luz difusa. Procedimiento por frame (CPU, en `sky_ambient.cpp`):

1. Construir las **funciones de transferencia convolucionadas** una vez (constantes, solo dependen del
   kernel `max(ω·N,0)` y de la base SH): 9 coeficientes fijos.
2. Integrar el cielo actual: muestrear `harukaSkyBase` sobre la esfera (suma de Monte Carlo o
   cuadratura por bandas del cénit, ~100–500 puntos), factorizando `harukaSkyBase(t, day)` con
   `t = cos θ` (el gradiente solo depende de la elevación → integral 1D por banda, trivial).
3. Cargar los 9 `vec3` al UBO `PerFrameData` una vez por frame (barato: ya se sube el UBO cada frame).

Ventaja: es **barato** (integral casi analítica por simetría azimutal), exacto con la paleta, y no
depende de la cámara.

### Opción B2 — SH resuelto en shader (deferido)

Variante que deja `sky_ambient.cpp` fuera: un compute/lookup por texel. NO recomendada: duplica el
costo y complica la paridad CPU↔GPU. Se documenta por si el 1D por bandas diera problemas de
convergencia (no debería).

> La forma concreta (`skyDiffuseIrradiance` evaluando SH en el shader vs pasando los 9 coefs y
> evaluando el polinomio) se decide en implementación; el plan exige **una sola fuente de verdad**:
> el CPU integra, el GPU evalúa.

---

## 4. Cambios por fichero

### 4.1 NUEVO `src/core/sky_ambient.{h,cpp}` — el gemelo CPU
- Constantas de paleta idénticas a `sky_palette.glsl` (en `double` donde proceda; el resto del motor
  ya mezcla double/flota con criterio, ver `terrain_sampler_v2.cpp`).
- `harukaSkyBase(float t, float day)` y `harukaSkyDay(float sunElev)` — mismo cuerpo que el GLSL.
- `struct SkySH { glm::vec3 coef[9]; }` + `skySH(sunElev)` que integra y rellena.
- Nota de paridad: comentario doble en `sky_palette.glsl` y en este fichero, tipo
  "CUALQUIER CAMBIO EN LA PALETA VA EN LOS DOS" (ya existe la advertencia en el GLSL; ahora se cumple).

### 4.2 `assets/shaders/lib/sky_palette.glsl`
- Sin cambios de contenido (ya es la fuente del shader). Solo se cierra el contrato de paridad que ya
  declara.

### 4.3 UBO `PerFrameData` (los ~10 shaders que lo declaran)
- Añadir `vec4 shCoef[9];` al final (std140). CUIDADO con el offset: cada `vec4` 16 B → 144 B de
  campo nuevo. Declaración IDÉNTICA en vert+frag del mismo programa (la disciplina de sky.vert/frag
  ya lo exige).
- Shaders afectados: `final.frag`, `prop_inst.frag`, `softbody.frag`, `fluid_particle.frag`,
  `water.frag`, `preview.frag`, `shallow_water.frag`, `planet.frag`, `construction_inst.frag`, etc.
  (todos los que hoy declaran `PerFrameData` con `ambientStrength`).

### 4.4 CPU que rellena el UBO — `application_render.cpp` (~línea 811)
- Sustituir `frameData.ambientStrength = mix(0.11f, 0.28f, day)` por:
  - `SkySH sh = skySH(sunElev);` → volcar los 9 coeficientes al `PerFrameData`.
  - `ambientStrength` se mantiene como **gainer** final (escalar) o se elimina; decisión en §5.2.
- `PerFrameData` está en `application_render.cpp` (~línea 60) → añadir los 9 `vec4`.

### 4.5 Consumidores — quitar constantes hardcodeadas
- `final.frag:87` `vec3(0.40, 0.46, 0.60)` → `shadowCol = baseColor * amb(N) * ao` donde
  `amb(N)` evalúa el SH.
- `sky.frag:139` `vec3(0.46, 0.52, 0.67)` es el color de las nubes, NO ambiente: **NO se toca** (ver
  §5.1 — es un arte del dibujado, y el SH no debe alimentarlo para no contar el sol dos veces).
- `planet.cpp:2402` `uAmbient = ambientStrength * vec3(0.55, 0.65, 0.85)` → los shaders del terreno
  (`planet/simple.frag:17`, `textured.frag:25`, `biome.frag:245`, `water.frag:123`) leen el SH del
  `PerFrameData` y usan `uAmbient` como fallback.
- `preview.frag:56` `max(ambientStrength, 0.15)` → SH.

### 4.6 IBL — decidir al implementar (§6.1)
- Conservar `ibl.cpp` (sirve de referencia y para un futuro render PBR de verdad) pero **no es
  necesario para este plan**; el SH difuso lo cubre para la estética toon.
- El README ya lo lista como no conectado → el plan puede dejarlo marcado "fuera de alcance" o
  borrarlo; decisión del autor.

---

## 5. Decisiones que el plan cierra (y las que NO cierra)

### 5.1 Cerradas
- **La luz ambiente sale de la MISMA paleta que el cielo** (paridad por contrato, no por
  aproximación).
- **El sol NO entra en el SH** (ya es luz directa; meterlo sería contarlo dos veces — el propio
  `sky_palette.glsl` lo documenta). El halo/disco/estrellas tampoco (no aportan irradiancia difusa).
- **El rebote de suelo sí entra** bajo el horizonte (una cara que mira abajo recoge el color del
  bioma, no azul de horizonte) — es la divergencia deliberada que ya anuncia `sky_palette.glsl:18-20`.
- **`sky.frag:139` (color de nubes) NO es ambiente** y no se toca: cambiar las sombras por un valor
  que alimente el color de las nubes las teñiría de forma inconsistente con el día.

### 5.2 Pendientes de decidir con el autor (para no inventar look)
- ¿`ambientStrength` (escalar) se mantiene como gainer global o desaparece? Hoy cada shader lo
  interpreta distinto. Propuesta: mantenerlo como **multiplicador final único** (el artista ajusta un
  solo número), definido en un sitio central.
- ¿Los shaders del terreno migran al SH o conservan `uAmbient` como única vía? (El terreno tiene su
  propio UBO `planet.*`: `uAmbient` de `planet.cpp:2402`.) Propuesta: migrar, dejando `uAmbient`
  como fallback si falta el `PerFrameData`.
- ¿EL SH se evalúa por normal en el fragment (coste 9 mults/px, trivial) o se pasa irradiance de
  banda baja? Propuesta: por fragment, es el estándar.

---

## 6. Comparación con alternativas (por qué no IBL "de verdad")

| | **SH difuso (elegido)** | IBL cubemap completo |
|---|---|---|
| Costo estático | 1 UBO de 9 vec4 | 4 texturas + 3 convoluciones + pipelines + targets |
| Costo por frame | integral 1D por simetría azimutal | genera/actualiza cubemap + prefilter (o estático) |
| Encaje estética toon | perfecto (difusa, sin BRDF) | overkill (el modelo NO es PBR) |
| Estado hoy | paleta ya existe y la usa `sky.frag` | código sin consumidor (verificado) |
| Riesgo | bajo, cambio incremental | alto: hay que conectar todo y mantenerlo |

La "Física correcta" de un IBL completo no compra nada visual en un renderer toon donde `final.frag`
ni siquiera evalúa un BRDF. SH es el paso que **resuelve el síntoma real** (tres constantes
desincronizadas) con el menor cambio.

---

## 7. Orden de implementación (fases verificables)

1. **Fase 1 — gemelo CPU**: `src/core/sky_ambient.{h,cpp}` con las mismas constantes y funciones que
   `sky_palette.glsl`. Verificación: test unitario que muestrea `harukaSkyBase` y compara contra el
   GLSL para los mismos inputs (día/noche, cénit/horizonte). Paridad numérica exacta en `float`.
2. **Fase 2 — UBO**: ampliar `PerFrameData` con `shCoef[9]` en TODOS los shaders y rellenarlo en
   `application_render.cpp`. Verificación: se compila, no cambia la imagen (el campo no se consume aún).
3. **Fase 3 — integrar SH**: `skySH(sunElev)` en CPU + volcar al UBO. Verificación: con
   `HARUKA_DEBUG=1` (o el flag de debug del renderer) imprimir `shCoef[0]` — debe ser el color medio
   del cielo (≈ `lerp(horiz, zenith, 0.5)` para `day=1`).
4. **Fase 4 — consumidores**: migrar `final.frag` (sombra toon) y luego los demás shaders al SH.
   Verificación: al atardecer la sombra se vuelve cálida/cercana al horizonte sin tocar la cámara; al
   mediodía, fría y azul. Comparar contra la imagen anterior con `ambientStrength` fija.
5. **Fase 5 — limpieza**: decidir `ambientStrength`, `uAmbient` fallback, y el futuro de `ibl.cpp`.

Cada fase es independiente y comprobable; el plan NO depende de poder renderizar (las fases 1-3 se
verifican por test/valor impreso; la 4 necesita pantalla).

---

## 8. Relación con otros planes/doc

- `assets/shaders/lib/sky_palette.glsl` — la fuente del gradiente; este plan rellena su gemelo CPU.
- `docs/ESTADO_MOTOR.md` §2.5.b — el fix sRGB es ORTOGONAL: toca formatos/pipeline, no la luz. Pero
  conviene hacerlo antes de la Fase 4 (si se cambia el formato del albedo, la medición visual de la
  sombra SH no se contamina con el cambio de gamma).
- `docs/ESTADO_MOTOR.md` §2.2/2.3 — SSAO/IBL "no conectados": el plan retira IBL del camino y deja el
  README honesto.
- `README.md` línea "PBR Materials with IBL" — revisar tras Fase 5.
