#pragma once
/**
 * @file terrain_lod.h
 * @brief Las CIFRAS de LOD del terreno, en un solo sitio. GL-free y puro.
 *
 * Por qué existe este fichero
 * ---------------------------
 * `terrain_detail.h` resolvió bien el problema de "una función, dos lenguajes": la altura vive en
 * dos ficheros gemelos que se cambian a la vez (§3 de TERRENO.md). Pero el LOD —el `triM` con el
 * que se evalúa esa altura, y la rejilla con la que la física la tesela— se quedó fuera: la misma
 * fórmula estaba ESCRITA A MANO en cuatro sitios (`clipmap.tese`, `terrain.tese`,
 * `world_system_provider.h` y, otra vez, dentro del test que decía comprobarla).
 *
 * ⚠️ Ese es exactamente el fallo que hizo inútil a `detail_triM_parity`: el test se escribía su
 * propia copia de `max(rad·0.002, floor)` y la comparaba con otra copia suya. Comparaba `f(x)` con
 * `f(x)`, daba 0.0000 m y NO PODÍA FALLAR — habría seguido en verde aunque el motor usara una
 * fórmula distinta. Un test que copia la fórmula que audita no audita nada.
 *
 * Regla: si un número de LOD aparece en un shader Y en C++, su definición vive AQUÍ y el shader es
 * su gemelo declarado (igual que `terrain_detail.glsl` lo es de `terrain_detail.h`). Los tests
 * llaman a estas funciones; nunca las reescriben.
 */

#include <cmath>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

namespace Haruka {
namespace Planet {

// =================================================================================================
// Geometría del clipmap — gemelo de clipmap.tesc + planet.cpp (llenado de ClipParams)
// =================================================================================================

/// Lado del parche de la rejilla del clipmap, en metros. Gemelo de `planet.cpp` (PATCH) y del
/// `uClipOrigin.w` que se sube en ClipParams.
inline constexpr double TERRAIN_CLIP_PATCH_M = 128.0;

/// Tope de teselación del clipmap. Gemelo de `clipmap.tesc` (`clamp(..., 1.0, 32.0)`).
/// ⚠️ Bajó de 64 a 32 para que la teselación costara 4× menos. El efecto colateral —que el quad
/// pasara de 2 m a 4 m— es el que NO se propagó al `triM`, y es lo que arregla `TERRAIN_TRIM_FLOOR`.
inline constexpr double TERRAIN_CLIP_TESS_CAP = 32.0;

/// Lo que MIDE de verdad un triángulo del clipmap delante de la cámara. Derivado, no escrito a mano:
/// si alguien toca el parche o el tope, esto y el piso de `triM` se mueven con él.
inline constexpr double TERRAIN_CLIP_QUAD_M = TERRAIN_CLIP_PATCH_M / TERRAIN_CLIP_TESS_CAP;

/// Tope de teselación de la MALLA BASE. Gemelo del techo de `ringCap` en `terrain.tesc`.
/// ⚠️ Estuvo en 128 —por encima del máximo del hardware— y el driver lo recortaba en silencio, así
/// que el shader hacía algo distinto de lo que su comentario afirmaba. `terrain_lod_invariants` lo
/// vigila ahora: cualquier tope por encima de `TERRAIN_MAX_TESS_GEN_LEVEL` es un no-op disfrazado.
inline constexpr double TERRAIN_MESH_TESS_CAP = 64.0;

/// El tope de teselación que el HARDWARE concede, no el que se pida. Es de silicio: el mismo en
/// OpenGL, Vulkan y D3D, y el mínimo que el spec garantiza. Un `gl_TessLevel*` por encima de esto
/// NO da error — se recorta en silencio, así que el shader hace algo distinto de lo que dice.
/// Comprobado en la máquina de desarrollo: `GL_MAX_TESS_GEN_LEVEL = 64`.
inline constexpr double TERRAIN_MAX_TESS_GEN_LEVEL = 64.0;

// =================================================================================================
// `triM` — el tamaño de triángulo con el que se evalúa la altura
// =================================================================================================

/// Pendiente de `triM` con la distancia tangente. Gemelo de `clipmap.tese` (`rad·0.002`) y de la
/// malla de colisión. Es un ángulo: el triángulo crece con lo que abarca en pantalla.
inline constexpr double TERRAIN_TRIM_SLOPE = 0.002;

/// Piso de `triM` en el campo cercano — y, con él, la definición de LA SUPERFICIE que se pisa.
///
/// ⚠️ No es una constante de antialiasing: la altura DEPENDE de `triM` (el freno de Nyquist de §3.1
/// apaga las octavas más finas que el triángulo), así que este número define qué superficie existe.
/// Por eso se DERIVA del quad real en vez de escribirse: con el piso en 2.0 y quads de 4 m, la
/// octava de 4,5 m entraba al 12,5 % (±8,7 cm) en vértices separados 4 m — sub-Nyquist, o sea el
/// hervido que §3.1 describe. Derivado, el piso sigue al quad y eso no puede volver a desalinearse.
inline constexpr double TERRAIN_TRIM_FLOOR = TERRAIN_CLIP_QUAD_M;

/**
 * @brief Piso de `triM` para el SOMBREADO per-píxel. **A propósito distinto del de la geometría.**
 *
 * ⚠️ EXISTE PORQUE LA OCTAVA MÁS FINA DEL TERRENO ESTABA MUERTA. La escalera de `terrain_detail.h`
 * acaba en λ = 4,5 m (±0,7 m) y su guarda es `minFeatureM < 2.25`. Con el piso de la GEOMETRÍA
 * (`TERRAIN_CLIP_QUAD_M` = 128/32 = **4 m**) esa condición no se cumple nunca, en ningún punto del
 * planeta y a ninguna distancia: el rasgo más fino que el terreno podía tener era λ = 22 m. De pie,
 * el bulto más pequeño del suelo medía 22 m de ancho — el "se ve basto desde todas partes".
 *
 * Cómo se murió: el tope de tesela bajó de 64 a 32 (4× menos teselación, decisión de coste
 * documentada en `clipmap.tesc`). Eso llevó el quad de 2 m a 4 m y el piso con él. La octava estaba
 * dimensionada para el quad de 2 m —`2.0 < 2.25` la activa, `4.0` no— y el cambio la apagó en
 * silencio. El comentario del tope comprobó que λ = 22 m sobrevivía; nadie miró la siguiente.
 *
 * POR QUÉ PUEDEN SER DISTINTOS, que es la pregunta obvia: geometría y sombreado se muestrean a
 * frecuencias distintas. La geometría vive en los VÉRTICES (4 m de separación) y detalle más fino que
 * eso es sub-Nyquist — el hervido de §3.1, y por eso su piso NO se toca. El sombreado vive en los
 * PÍXELES, y un píxel a 10 m del jugador abarca centímetros: ahí caben cuatro octavas más sin
 * acercarse a su límite. Atarlos era confundir dos tasas de muestreo distintas.
 *
 * DE DÓNDE SALE EL VALOR, que NO es "el más grueso que la enciende": el peso de una octava es
 * `octaveWeight(λ, triM) = clamp(λ/(2·triM) − 1, 0, 1)`, o sea una RAMPA, no un interruptor. Con la
 * guarda justo rozada (`triM` = 2.0) la octava entra al **12,5 %** — ±8,75 cm en vez de sus ±0,7 m.
 * Encenderla así es casi no encenderla. El peso llega a 1 en `λ/4`:
 *
 *     octaveWeight(4.5, 1.125) = 4.5/2.25 − 1 = 1.0
 *
 * De ahí el 1.125: es **λ_fina / 4**, el valor derivado en el que la octava aporta su amplitud
 * completa. Bajar de ahí no compra nada (el peso ya está topado en 1) y solo pagaría evaluaciones.
 *
 * Y es seguro para el sombreado: λ = 4,5 m a 1 km son ~4,5 píxeles, y más cerca aún más. El
 * sub-Nyquist que este piso NO puede provocar es el de la MALLA, y la malla no lo usa.
 *
 * ⚠️ NO ENTRA EN LA PARIDAD. El contrato es sobre la ALTURA (lo que se pisa contra lo que se dibuja);
 * esto solo cambia la NORMAL con la que se ilumina. La colisión no lo lee.
 *
 * Gemelo de `harukaPixelTriM` en `planet/biome.frag`. Lo vigila `test_terrain_finest_octave`.
 */
inline constexpr double TERRAIN_TRIM_FLOOR_PIXEL = 1.125;   // = lambda_fina/4 (ver arriba)

/**
 * @brief `triM` a una distancia tangente `radM` de la cámara/jugador, en metros.
 *
 * LA fuente de este número para el motor entero: clipmap, malla base, colisión y tests. Los
 * shaders `clipmap.tese` / `terrain.tese` son sus gemelos y llevan la misma expresión.
 */
inline float terrainTriM(double radM) {
    const double t = radM * TERRAIN_TRIM_SLOPE;
    return (float)(t > TERRAIN_TRIM_FLOOR ? t : TERRAIN_TRIM_FLOOR);
}

/**
 * @brief ¿Toca RE-ANCLAR el anillo cercano? Con banda muerta, que es lo que le faltaba.
 *
 * El problema que resuelve (medido en el juego, 2026-08-14)
 * --------------------------------------------------------
 * El anclaje se cuantiza con un `round` duro (ver `terrainClipFrame`) y el disparador comparaba las
 * anclas por IGUALDAD EXACTA. Con el jugador parado justo sobre un borde de celda, un micrón de
 * temblor bastaba para que el ancla saltara de una celda a la otra **en cada frame**: 69
 * reconstrucciones en 8 segundos alternando entre exactamente dos mallas (9 409 vértices y 18 432
 * triángulos cada una, 5-6 ms), con el personaje quieto.
 *
 * Y se REALIMENTA, que es lo que lo volvía permanente: las dos anclas dan superficies ligeramente
 * distintas (el twist del quad medía 0,040 m en una y 0,044 m en la otra), así que el suelo empujaba
 * al personaje de vuelta al otro lado del borde. Un ciclo límite, y la causa más probable del
 * "no me quedo quieto en el sitio".
 *
 * Por qué la banda muerta va AQUÍ y no en `terrainClipFrame`
 * ---------------------------------------------------------
 * Porque aquella es una función PURA de la posición, compartida por render, colisión y tests: es lo
 * que garantiza que los tres describan el mismo suelo. Una banda muerta es, por definición, memoria
 * del camino recorrido. Metida ahí envenenaría a los tres; aquí solo decide CUÁNDO se reconstruye,
 * no QUÉ se construye.
 *
 * El umbral
 * ---------
 * Un punto puede estar a `quad·0,707` de su propia ancla (la diagonal de la celda), así que el
 * umbral tiene que superar eso o se saltaría estando aún dentro de la celda de siempre. `0,75·quad`
 * (3 m con quads de 4 m) lo cubre con margen. Retrasa el re-anclaje como mucho 3 m sobre un anillo
 * de ±192 m: irrelevante para lo que cubre, decisivo para que deje de temblar.
 *
 * @param curAnchor  ancla vigente (dirección unitaria desde el centro del planeta)
 * @param posDir     dirección actual del jugador (no hace falta normalizar)
 * @param radiusM    radio del planeta, para pasar el ángulo a metros
 */
inline bool terrainAnchorShouldJump(const glm::dvec3& curAnchor, const glm::dvec3& posDir,
                                    double radiusM) {
    const double lc = glm::length(curAnchor), lp = glm::length(posDir);
    if (lc < 1e-12 || lp < 1e-12) return true;
    const double c = glm::clamp(glm::dot(curAnchor / lc, posDir / lp), -1.0, 1.0);
    const double distM = std::acos(c) * radiusM;      // separación TANGENCIAL en metros
    return distM > TERRAIN_CLIP_QUAD_M * 0.75;
}

// =================================================================================================
// ANILLOS ANIDADOS del clipmap — gemelo de `clipmap.tesc` (edgeFactor) y del bucle de `planet.cpp`
// =================================================================================================

/// Nivel de teselación de una arista de longitud `arcM` a distancia `dM`. GEMELO EXACTO de
/// `edgeFactor` en `clipmap.tesc`, redondeo a potencia de dos incluido.
///
/// ⚠️ El redondeo NO es cosmético y ahora carga con una segunda propiedad. Ya era lo que mantenía
/// los vértices en múltiplos del quad (paridad con la colisión); con los anillos anidados es además
/// lo que impide las GRIETAS entre ellos: en la frontera, el anillo de fuera tiene aristas 2× más
/// largas y pide un nivel 2× mayor, y solo porque el nivel se redondea a potencia de dos sale el
/// MISMO quad por los dos lados. Con un redondeo cualquiera, los dos lados se subdividen distinto y
/// aparecen T-junctions — rendijas por las que se ve el espacio.
inline double terrainClipEdgeLevel(double arcM, double dM) {
    const double d = dM > 1.0 ? dM : 1.0;
    double lvl = arcM / (d * TERRAIN_TRIM_SLOPE * 2.0);   // 0.004 = TERRAIN_TRIM_SLOPE·2
    if (lvl < 1.0) lvl = 1.0;
    if (lvl > TERRAIN_CLIP_TESS_CAP) lvl = TERRAIN_CLIP_TESS_CAP;
    double p = std::exp2(std::floor(std::log2(lvl)));
    if (p < 1.0) p = 1.0;
    if (p > TERRAIN_CLIP_TESS_CAP) p = TERRAIN_CLIP_TESS_CAP;
    return p;
}

/// Índice del anillo que dibuja la distancia tangente `dM`, siendo `cover0` el semi-lado del
/// nivel 0. Cada nivel dobla escala y alcance, así que es el logaritmo en base dos.
inline int terrainClipRingIndex(double dM, double cover0) {
    if (dM <= cover0) return 0;
    const int r = (int)std::ceil(std::log2(dM / cover0));
    return r > 0 ? r : 0;
}

/// Lado real del quad dibujado a distancia `dM` con anillos anidados.
inline double terrainClipRingQuadM(double dM, double cover0) {
    const int    r   = terrainClipRingIndex(dM, cover0);
    const double arc = TERRAIN_CLIP_PATCH_M * std::exp2((double)r);
    return arc / terrainClipEdgeLevel(arc, dM);
}

// =================================================================================================
// Rejilla por anillos de la malla de COLISIÓN (§9 Fase 3)
// =================================================================================================

/// Paso de la rejilla de colisión al pie del jugador, en metros.
///
/// ⚠️ ES EL QUAD DEL RENDER, y eso no es una coincidencia: es la única forma de que la disparidad
/// entre lo que se dibuja y lo que se pisa sea CERO en vez de pequeña. Ver
/// `TERRAIN_COLLIDE_UNIFORM_M`. Valía 3.0 m —un número redondo sin relación con el render— y con él
/// las dos superficies eran poligonales sobre retículas incompatibles, así que se separaban por la
/// sagita de la celda por mucho que se afinara: 6,2 cm en el peor caso a pie.
inline constexpr double TERRAIN_RING_INNER_STEP = TERRAIN_CLIP_QUAD_M;

/// Radio del bloque UNIFORME de la colisión, en metros. Dentro de él el paso es exactamente
/// `TERRAIN_CLIP_QUAD_M` y los nodos caen en múltiplos de esa medida.
///
/// El porqué
/// ---------
/// El render y la colisión teselan la MISMA función, y dentro de cada celda las dos son planos. Si las
/// retículas no coinciden, las dos superficies se separan por la sagita de la celda más gruesa y no
/// hay forma de bajar de ahí sin afinar sin fin: con celdas de 3 m contra quads de 4 m la separación
/// era de 6,2 cm a pie y de 1,20 m en el borde de la caja del clipmap.
///
/// Con las retículas COINCIDIENDO el resultado no es "1 cm": es **exactamente 0**. Mismos vértices,
/// misma bilineal, mismo marco anclado (`terrainClipFrame(..., snapRadius)`), misma función de altura.
/// No es una aproximación mejor, es la misma superficie.
///
/// Por qué 256 m y no toda la caja
/// -------------------------------
/// Una retícula uniforme de 4 m sobre los ±1984 m de la caja son 993² = 986 049 vértices y 1,97 M de
/// triángulos por reconstrucción de la `MeshShape` de Jolt: inviable. El radio no tiene que cubrir la
/// caja, tiene que cubrir **hasta dónde puede llegar el jugador entre reconstrucciones**, y la malla
/// se reconstruye en cuanto deriva 48 m del centro. Medido:
///
///     ±128 m ->   4 225 vért ·   8 192 tris     (margen 2,7× sobre los 48 m)
///     ±256 m ->  16 641 vért ·  32 768 tris     (margen 5,3×)   <- elegido
///     ±512 m ->  66 049 vért · 131 072 tris
///
/// ⚠️ EL COSTE REAL de la rejilla COMPLETA, medido (no el del bloque suelto):
///
///     antes (paso 3 m, sin bloque):  187² =  34 969 vért ·  69 192 tris
///     ahora (bloque de ±256 m):      297² =  88 209 vért · 175 232 tris   →  **×2,52**
///
/// O sea 2,5× vértices, no "apenas el doble" como daba a entender contar solo el bloque. Es asumible
/// porque la `MeshShape` se construye FUERA del hilo de física (ver `physics_engine.cpp`: hacerlo
/// dentro daba un tirón al avanzar 48 m), pero es un coste real y hay que saberlo antes de subir el
/// radio: ±512 m lo pondría en ×5.
///
/// Fuera del bloque siguen los anillos geométricos, que es lo correcto para lo que colisiona a
/// kilómetros (objetos lanzados, vehículos) y donde el render también es grueso.
inline constexpr double TERRAIN_COLLIDE_UNIFORM_M = 256.0;

/// Cuánto crece el paso por anillo de distancia. ⚠️ Este número decide la disparidad entre lo que
/// se DIBUJA y lo que se PISA lejos del jugador: la malla es lineal dentro de cada celda, así que
/// la sagita crece con el paso. Lo mide `terrain_chord_error`; no es un ajuste libre.
inline constexpr double TERRAIN_RING_GROWTH = 1.12;

/// Semi-lado del clipmap en la calidad más baja (31 parches). Dentro de esa caja el render dibuja
/// geometría fina, así que la colisión NO puede ir por celdas de cientos de metros.
inline constexpr double TERRAIN_CLIP_COVER_MIN_M = 31.0 * TERRAIN_CLIP_PATCH_M * 0.5;

/// Tope del paso de la colisión DENTRO de la caja del clipmap.
///
/// El crecimiento ×1,12 arranca en el pie del jugador, así que a 1,6 km la celda ya medía 199 m
/// mientras el render dibujaba quads de ~8 m: **10,18 m de separación entre el suelo que se ve y el
/// que se pisa, dentro de la propia caja del clipmap**. El arreglo no es bajar el crecimiento (para
/// llegar a ~3 m haría falta 1,06 → 3,2× vértices) sino TOPAR el paso donde el render es fino.
/// Medido: con tope de 25 m la disparidad baja a **1,20 m** por un 38 % más de vértices; con 50 m
/// queda en 2,34 m y **no cuesta ni un vértice más**. Fuera de la caja el paso crece libre, que es
/// donde el render también es grueso y la colisión solo tiene que llegar.
inline constexpr double TERRAIN_RING_MAX_STEP_IN_BOX = 25.0;

/// Crecimiento de la celda de COLISIÓN con la distancia, antes de redondear a potencia de dos.
///
/// Elegido para que la celda valga el quad exacto (4 m) justo hasta el borde del bloque uniforme y
/// duplique a partir de ahí: 4 m hasta 256 m, 8 hasta 512, 16 hasta 1024, 32 hasta el borde de la caja.
///
/// ⚠️ Es UN MANDO DE COSTE, y el coste es superlineal. Medido sobre la rejilla completa:
///
///     0.016 (este)  ->  449 nodos/eje ·  201 k vert ·  401 k tris     (×2,3 sobre los 175 k de antes)
///     0.008         ->  637 nodos/eje ·  406 k vert ·  810 k tris     (×4,6)
///     copiar el render literalmente (4 m hasta 1 km) -> 1,12 M tris    (×6,4)
///
/// Esa malla se reconstruye entera cada vez que el jugador deriva 48 m del centro, así que el número
/// de triángulos no es una cifra de memoria: es el tiempo de construir el árbol AABB de Jolt.
inline constexpr double TERRAIN_COLLIDE_SLOPE = 0.016;

/// Tope de la celda de colisión dentro de la caja del clipmap, en metros.
///
/// ⚠️ TIENE que ser `TERRAIN_CLIP_QUAD_M · 2^k`. Valía 25 m (`TERRAIN_RING_MAX_STEP_IN_BOX`), que no es
/// múltiplo de 4: al aplicarlo con un `min` rompía justo la propiedad que hace que las retículas
/// compartan vértices, y la rejilla volvía a cruzarse con el render en vez de tocarlo.
///
/// Por qué 16 y no 32. Con el tope en 32 m la sagita peor DENTRO de la caja subía a **1,63 m** —peor
/// que los 1,05 m de la rejilla ×1,12 que esto sustituye—, porque la sagita va con la celda AL
/// CUADRADO. Medido con `terrain_chord_error`:
///
///     tope 32 m  ->  373 k tris  ·  sagita peor en la caja 1,63 m
///     tope 16 m  ->  508 k tris  ·  sagita peor en la caja 0,68 m     <- elegido
///
/// O sea: 1,4× más triángulos compran 3,6× menos separación. El near-field es 0 exacto en los dos.
inline constexpr double TERRAIN_COLLIDE_CELL_CAP = TERRAIN_CLIP_QUAD_M * 4.0;   // 16 m

/**
 * @brief Lado del quad que el CLIPMAP dibuja a una distancia tangente `radM`, en metros.
 *
 * GEMELO de `edgeFactor` en `clipmap.tesc`: el nivel es `clamp(arc/(d·0.004), 1, 32)` redondeado hacia
 * abajo a POTENCIA DE DOS, y el lado del quad es `arc/nivel`. Existe aquí, y no como un número escrito
 * a mano, porque es la regla que la malla de colisión tiene que seguir para compartir vértices con el
 * render: potencias de dos hacen que la retícula gruesa sea un SUBCONJUNTO de la fina, así que las dos
 * superficies coinciden en los puntos que comparten en vez de aproximarse.
 */
inline double terrainClipQuadAt(double radM) {
    const double arc = TERRAIN_CLIP_PATCH_M;
    const double d   = std::max(radM, 1.0);
    double lvl = arc / (d * 0.004);
    lvl = std::min(std::max(lvl, 1.0), TERRAIN_CLIP_TESS_CAP);
    lvl = std::exp2(std::floor(std::log2(lvl)));
    lvl = std::min(std::max(lvl, 1.0), TERRAIN_CLIP_TESS_CAP);
    return arc / lvl;
}

/**
 * @brief Celda de la malla de COLISIÓN a distancia `radM`, en metros. Siempre `quad · 2^k`.
 *
 * No es `terrainClipQuadAt` a secas, y la diferencia es una decisión de coste explícita:
 *
 *   · Copiar el render literalmente (4 m hasta 1 km, 8 m hasta el borde) da 749 nodos por eje —
 *     561 001 vértices y **1,12 M de triángulos** por reconstrucción de la `MeshShape` de Jolt, 6,4×
 *     lo que costaba. Se reconstruye cada vez que el jugador deriva 48 m: no cabe.
 *   · Lo que hace falta NO es copiar el render en todas partes, es que los vértices CAIGAN EN SU
 *     RETÍCULA. Como la celda es siempre `quad · 2^k`, la rejilla gruesa es un SUBCONJUNTO de la del
 *     render: las dos superficies se TOCAN en cada vértice compartido en vez de cruzarse. Entre
 *     vértices queda la cuerda, que es el mismo error que el render ya tiene contra la función.
 *
 * Dentro de `TERRAIN_COLLIDE_UNIFORM_M` la celda es el quad exacto y la disparidad es 0 — que es todo
 * el radio donde el jugador y los props tocan el suelo, o sea donde la paridad se puede observar.
 */
inline double terrainCollideCellAt(double radM) {
    const double want = std::max(radM * TERRAIN_COLLIDE_SLOPE, TERRAIN_RING_INNER_STEP);
    const double k    = std::exp2(std::ceil(std::log2(want / TERRAIN_RING_INNER_STEP)));
    const double cell = TERRAIN_RING_INNER_STEP * k;
    return std::min(cell, TERRAIN_COLLIDE_CELL_CAP);
}

// =================================================================================================
// El BLOQUE como HEIGHTFIELD (§9) — el trozo que Jolt puede representar sin malla
// =================================================================================================

/// Número de muestras por lado del heightfield de colisión.
///
/// ⚠️ POTENCIA DE DOS por exigencia de Jolt: `HeightFieldShapeSettings` divide la rejilla en bloques
/// de `mBlockSize` y documenta que `mSampleCount / mBlockSize` debe ser ≥ 2 y, para no desperdiciar
/// memoria ni rendimiento, potencia de dos.
///
/// ⚠️ CORRECCIÓN (medida): aquí ponía que «129 muestras es impar y Jolt no lo admite». **Es falso, y
/// la verdad es peor.** Jolt ACEPTA `mSampleCount = 129` sin error y lo REDONDEA EN SILENCIO al
/// múltiplo siguiente de `mBlockSize`: `GetSampleCount()` devuelve 130 y el AABB local llega a
/// +260 m en vez de a +256. O sea que aparece una fila y una columna de geometría que el motor no
/// escribe, justo en el BORDE — el sitio donde una superficie de más es una repisa fantasma. Un
/// tamaño impar no falla ruidosamente: miente. Ver `TERRAIN_RING_SAMPLES` para el tamaño con el que
/// sí se puede tener cobertura simétrica.
inline constexpr uint32_t TERRAIN_HF_SAMPLES = 128;

/// Coordenada tangente del PRIMER nodo del heightfield, en metros.
///
/// ⚠️ ASIMÉTRICO POR UNA CELDA, y es inevitable: un número PAR de muestras no puede repartirse
/// simétricamente alrededor del 0 sobre una retícula que tiene un nodo EN el 0. Se elige empezar en
/// `-TERRAIN_COLLIDE_UNIFORM_M` y terminar una celda antes por el lado positivo, en vez de centrarlo,
/// porque así **todos los nodos siguen siendo múltiplos exactos del quad** — que es la propiedad que
/// `clipmap_vertex_lattice` exige y sin la cual el heightfield dejaría de compartir vértices con el
/// render.
inline constexpr double TERRAIN_HF_LO = -TERRAIN_COLLIDE_UNIFORM_M;                     // -256 m

/// Coordenada tangente del ÚLTIMO nodo. Derivada, no escrita: sigue a las muestras y al quad.
inline constexpr double TERRAIN_HF_HI =
    TERRAIN_HF_LO + (double)(TERRAIN_HF_SAMPLES - 1) * TERRAIN_CLIP_QUAD_M;             // +252 m

/// ¿Cae la celda `[a,b]` ENTERAMENTE dentro del heightfield? La malla por anillos tiene que dejar ese
/// hueco: si las dos geometrías se solapan, Jolt genera contactos DOBLES en la misma superficie y el
/// personaje rebota o se queda enganchado en la frontera.
inline bool terrainHeightFieldCovers(double a, double b) {
    return a >= TERRAIN_HF_LO - 1e-9 && b <= TERRAIN_HF_HI + 1e-9;
}

// =================================================================================================
// ANILLOS CONCÉNTRICOS DE HEIGHTFIELD — la malla lejana sin árbol de triángulos
// =================================================================================================
//
// POR QUÉ, con los números que lo justifican (medidos en el juego, no estimados)
// ------------------------------------------------------------------------------
// La malla lejana (`terrainMesh`) entrega a Jolt 257 049 vértices y 479 814 triángulos cada vez que
// el jugador deriva 48 m. Desglose real de una reconstrucción, medido con la sonda de
// `physics_engine.cpp` sobre el planeta bakeado:
//
//     muestreo del terreno (257 049 × sampleTerrainHeight) ....  454-640 ms
//     MeshShape::Create (árbol AABB sobre 479 814 tris) ....... 802-1274 ms   ← el 65 %
//     heightfield del bloque cercano (128×128) ................    29-52 ms
//     TOTAL .................................................. 1300-1900 ms
//
// El grueso NO es evaluar el terreno: es construir el árbol de triángulos. Y un `HeightFieldShape`
// no construye ninguno. Medido en aislado con el mismo Jolt del build:
//
//     MeshShape de la rejilla completa .......  700-740 ms
//     11 HeightFieldShape (±256 m … ±262 km) ..     18,3 ms   → 38× más barato
//
// Con MENOS muestras (180 224 contra 257 049 vértices) y MÁS alcance.
//
// LO QUE NO SE PUEDE HACER, y por qué no es esto
// ----------------------------------------------
// La otra idea era trocear la malla en parches y rehacer solo el perímetro. No funciona: el marco de
// `terrainClipFrame` se RE-ANCLA con el jugador y los nodos son offsets tangentes relativos a ese
// ancla, así que al derivar se mueven todos. Medido sobre la rejilla real, coincidencias a 1 mm tras
// derivar 48 m: **8 de 257 049** hacia el este, 9 126 hacia el norte. El este es peor porque el
// anclaje se cuantiza en (lat, lon) y el paso longitudinal vale `quad·cos(lat)` — 3,30 m a latitud
// 0,6 rad, no 4 m —, o sea inconmensurable con la retícula tangente. No hay parche que sobreviva.
//
// EL PRECIO, dicho claro
// ----------------------
// La rejilla por anillos de `terrainRingGrid` es un producto tensorial: el paso crece pero filas y
// columnas se comparten, así que NO tiene ni una costura. Los anillos SÍ: en la frontera entre el
// nivel k y el k−1 el lado fino tiene un nodo en el punto medio de la arista gruesa, y ahí las dos
// superficies discrepan por la sagita de la celda gruesa. No es un agujero (se tocan en los nodos
// compartidos) pero sí un ESCALÓN. Lo mide `terrain_ring_seam`; no es un ajuste libre.

/// Semi-lado del HUECO que el clipmap deja para que lo rellene el anillo cercano, en metros.
///
/// ⚠️ NO son los 256 m del anillo, y el motivo es geométrico, no una elección. El clipmap es una
/// rejilla de `NC × NC` parches de 128 m con las esquinas en `(i − NC/2)·128`, y `NC` vale 31, 47, 63
/// o 95 — **siempre impar**. Con NC impar los bordes de parche caen en múltiplos IMPARES de 64:
/// …−192, −64, +64, +192, +320… **256 no es borde de parche.**
///
/// Y el borde del hueco tiene que serlo, porque un parche se descarta entero o no se descarta:
///
///   · recortando "parches enteros dentro de 256" el trozo 192-256 lo dibujarían LOS DOS → dos
///     superficies sobre el mismo suelo, que es el z-fighting que este paso viene a quitar;
///   · recortando los que TOCAN el hueco, entre 256 y 320 no dibujaría nadie → un agujero real por
///     el que se ve el espacio.
///
/// Así que el hueco es el mayor cuadrado alineado a parche que cabe en el anillo: `1,5 · 128` = 192 m.
/// La banda 192-256 m la sigue dibujando el clipmap (el anillo la tiene, pero no la dibuja), o sea que
/// ahí la paridad sigue siendo la de antes. Es a 192 m del jugador: no es donde se camina.
inline constexpr double TERRAIN_CLIP_HOLE_M = TERRAIN_CLIP_PATCH_M * 1.5;   // 192 m

/// Semi-anchura en NODOS de un anillo NORMAL (fuera de la caja del clipmap). Los anillos de dentro
/// de la caja la suben para poder topar su celda sin dejar de cubrir el doble que el de dentro.
inline constexpr int TERRAIN_RING_BASE_HALF = 64;

/// Muestras por lado de un anillo. Ver el ⚠️ de `TERRAIN_HF_SAMPLES`: un tamaño impar NO da error,
/// Jolt lo redondea en silencio y aparece geometría fantasma en el borde.
///
/// Por qué 130 y no 128. La cobertura tiene que ser SIMÉTRICA para que la costura entre niveles caiga
/// exactamente donde acaba el nivel de dentro: el nivel k−1 llega a `±64·celda(k−1)` = `±32·celda(k)`,
/// y el hueco del nivel k tiene que valer eso mismo. Con 128 muestras la cobertura es asimétrica por
/// media celda gruesa (ver `TERRAIN_HF_LO`) y la costura queda o solapada —contactos DOBLES, que es
/// el fallo que `terrainHeightFieldCovers` existe para evitar— o con un hueco por el que se cae.
/// Con 130 el índice 0 sobra: se marca SIN COLISIÓN y quedan 129 nodos útiles, `-64·celda … +64·celda`.
inline constexpr uint32_t TERRAIN_RING_SAMPLES = 2 * TERRAIN_RING_BASE_HALF + 2;   // 130

/// Geometría de UN anillo. El tamaño NO es igual en todos: ver `terrainRingLayout`.
struct TerrainRingSpec {
    double   cell    = 0.0;   ///< Lado de la celda, m. Siempre múltiplo del quad del render.
    int      half    = 0;     ///< Semi-anchura en nodos → cubre `±half·cell`.
    double   extent  = 0.0;   ///< `half·cell`, en metros.
    double   hole    = 0.0;   ///< Semi-anchura del hueco central = `extent` del anillo de dentro.
    uint32_t samples = 0;     ///< Muestras por lado = `2·half + 2` (la fila 0 sobra, ver arriba).
};

/// Coordenada tangente del nodo `i` de un anillo. El índice 0 es el sobrante (queda una celda por
/// fuera de la cobertura útil) y por eso va marcado sin colisión.
inline double terrainRingNode(uint32_t i, const TerrainRingSpec& r) {
    return ((double)i - (double)(r.half + 1)) * r.cell;
}

/// ¿Cae el nodo `(x,z)` DENTRO del hueco del anillo? El hueco es justo lo que cubre el anillo de
/// dentro, y la condición es ESTRICTA a propósito: un nodo en `|x| == hole` es el borde COMPARTIDO y
/// tiene que seguir teniendo superficie en los dos niveles, o la costura se abre y se cae por ella.
inline bool terrainRingHole(const TerrainRingSpec& r, double x, double z) {
    return r.hole > 0.0 && std::abs(x) < r.hole - 1e-9 && std::abs(z) < r.hole - 1e-9;
}

/**
 * @brief La pila de anillos que cubre `±halfExtent` metros.
 *
 * Cada anillo cubre el DOBLE que el de dentro, así que llegar a 200 km cuesta 11 niveles. La celda
 * sale de dividir el alcance entre `TERRAIN_RING_BASE_HALF`… salvo DENTRO DE LA CAJA DEL CLIPMAP.
 *
 * ⚠️ EL TOPE DENTRO DE LA CAJA NO ES OPCIONAL, y cuesta un número medido. Sin él la celda dobla
 * libremente y a 2 km ya vale 32 m, contra los 16 m a los que la malla la topa (`TERRAIN_COLLIDE_CELL_CAP`).
 * El escalón en las costuras, medido en el juego sobre el terreno bakeado, era:
 *
 *     costura a  256 m -> 0,15 m      costura a 2 km -> 2,4-2,9 m
 *     costura a  512 m -> 0,55 m      costura a 8 km -> 10 m
 *     costura a    1 km -> 1,3-1,7 m  costura a 131 km -> 107-111 m
 *
 * Los de decenas de km dan igual (ahí la malla que esto sustituye tenía celdas de 20 km, peores),
 * pero 1,7 m a 1 km y 2,9 m a 2 km caen DENTRO de la caja, donde la sagita de la malla es 0,68 m. O
 * sea que sin tope los anillos EMPEORAN justo la zona que importa. Con el tope la celda no pasa de
 * 16 m mientras el anillo empiece dentro de la caja; el precio es que ese anillo necesita el doble de
 * nodos por lado (258 en vez de 130) para seguir cubriendo el doble que el de dentro.
 *
 * La integridad de la costura exige que `extent` del anillo de dentro sea múltiplo EXACTO de la celda
 * del de fuera (si no, el borde del hueco no cae en un nodo y queda media celda de nada). Se cumple
 * por construcción: `half` es par y el alcance dobla.
 */
inline std::vector<TerrainRingSpec> terrainRingLayout(
        double halfExtent,
        double boxRadius = TERRAIN_CLIP_COVER_MIN_M,
        double cellCap   = TERRAIN_COLLIDE_CELL_CAP) {
    std::vector<TerrainRingSpec> out;
    double inner = 0.0;                                   // alcance del anillo de dentro
    for (int k = 0; k < 32; ++k) {
        const double extent = (k == 0)
            ? (double)TERRAIN_RING_BASE_HALF * TERRAIN_CLIP_QUAD_M   // ±256 m = el bloque cercano
            : 2.0 * inner;
        double cell = extent / (double)TERRAIN_RING_BASE_HALF;
        // El tope se aplica si el anillo EMPIEZA dentro de la caja: es donde el render dibuja fino.
        if (inner < boxRadius) cell = std::min(cell, cellCap);
        cell = std::max(cell, TERRAIN_CLIP_QUAD_M);
        TerrainRingSpec r;
        r.cell    = cell;
        r.half    = (int)std::llround(extent / cell);
        r.extent  = extent;
        r.hole    = inner;
        r.samples = (uint32_t)(2 * r.half + 2);
        out.push_back(r);
        inner = extent;
        if (inner >= halfExtent) break;
    }
    return out;
}

/// Marca de "aquí no hay superficie" en las muestras de un anillo. Es NaN a propósito: cualquier
/// aritmética que la toque por error se propaga y salta a la vista, en vez de colarse como una altura
/// plausible. El motor de física la traduce al valor que Jolt entiende.
inline bool terrainRingNoSample(float h) { return !(h == h); }

/**
 * @brief Coordenadas de la rejilla NO-uniforme de colisión, ordenadas: [-rm … -r1, 0, r1 … rm].
 *
 * Paso `innerStep` en el centro (lo que se pisa) creciendo `×growth` por anillo hasta cubrir
 * `halfExtent`. Estrictamente creciente y simétrica.
 *
 * `boxRadius` es el semi-lado del clipmap: mientras el anillo cae dentro de él, el paso se TOPA a
 * `maxStepInBox`. Ahí el render dibuja geometría fina y una celda de colisión de cientos de metros
 * separaba el suelo que se pisa del que se ve por hasta 10 m (ver `TERRAIN_RING_MAX_STEP_IN_BOX`).
 *
 * ⚠️ La monotonía NO es cosmética: los índices de triángulo asumen filas y columnas en orden de
 * coordenada, así que una rejilla que no creciera invertiría la orientación de las celdas y la
 * colisión quedaría del revés en media malla. `terrain_ring_grid` lo comprueba.
 */
inline std::vector<double> terrainRingGrid(double halfExtent,
                                           double boxRadius   = TERRAIN_CLIP_COVER_MIN_M,
                                           double maxStepInBox = TERRAIN_RING_MAX_STEP_IN_BOX,
                                           double innerStep   = TERRAIN_RING_INNER_STEP,
                                           double growth      = TERRAIN_RING_GROWTH,
                                           double uniformR    = TERRAIN_COLLIDE_UNIFORM_M) {
    std::vector<double> xs;
    std::vector<double> pos;
    pos.reserve(256);
    double s = innerStep, r = 0.0;
    while (true) {
        // TRES regímenes, en orden:
        //
        //  1. BLOQUE UNIFORME (r < uniformR): paso EXACTAMENTE `innerStep`, que es el quad del render.
        //     Los nodos caen en múltiplos exactos de esa medida, o sea EN la retícula del clipmap, así
        //     que las dos superficies no se aproximan: son la misma. Es lo que lleva la disparidad a 0
        //     en todo el radio que el jugador puede alcanzar entre reconstrucciones de la malla.
        //     `s` NO crece aquí: si creciera, al salir el paso daría un salto y el bloque no sería
        //     uniforme de verdad.
        //  2. Dentro de la caja del clipmap: crece ×growth pero topado a `maxStepInBox`, porque ahí el
        //     render sigue dibujando fino.
        //  3. Fuera de la caja: crece libre. El render también es grueso y la colisión solo tiene que
        //     llegar (objetos lanzados, vehículos).
        double step;
        if (r + 1e-9 < uniformR) {
            step = innerStep;                       // el paso NO crece dentro del bloque
        } else if (r <= boxRadius) {
            // ⚠️ DENTRO DE LA CAJA DEL CLIPMAP LA CELDA ES LA DEL RENDER, no una progresión propia.
            //
            // Antes crecía ×1,12 con un tope de 25 m: 10, 16, 25 m contra quads de 4 m del render. Esos
            // números no son múltiplos del quad, así que las dos retículas NO COMPARTÍAN NINGÚN VÉRTICE
            // a partir de ~256 m y las superficies se cruzaban. Ahora la celda es `quad·2^k` siempre
            // (ver `terrainCollideCellAt`), o sea un subconjunto de la retícula del render.
            //
            // ⚠️ `s` se REASIGNA, no se acumula. Acumulando (`s = max(s,step)*growth`) seguía
            // multiplicándose durante los ~250 anillos del interior, y al salir de la caja el primer
            // paso valía 1,12^250 metros: la rejilla se comía los 200 km de alcance en UN salto y el
            // terreno lejano quedaba con una sola celda. La rejilla terminaba en ±1988 m.
            step = terrainCollideCellAt(r);
            // ⚠️ ALINEACIÓN DE FASE, no solo de tamaño. Que la celda sea `quad·2^k` NO basta: el nodo
            // tiene que caer además en un MÚLTIPLO de esa celda, o la retícula gruesa queda desfasada
            // media celda respecto a la fina y no comparte ni un vértice.
            //
            // Es el fallo que destapó `clipmap_vertex_lattice`: el bloque uniforme acaba en 256, los
            // pasos de 8 m llegan a 504, y 504 no es múltiplo de 16 — así que al doblar a 16 todos los
            // nodos siguientes quedaban en 504, 520, 536… o sea ≡8 (mod 16). 19 256 de 140 625 nodos
            // fuera de la retícula del render, con 8 m de desvío. El tamaño era correcto y la
            // superficie se cruzaba igual.
            //
            // Se resuelve retrasando el salto: mientras `r` no sea múltiplo del paso nuevo, se sigue
            // con el anterior (la mitad). Termina siempre, porque `r` es múltiplo del quad por
            // construcción y el bucle divide por dos hasta llegar a él.
            while (std::fmod(r, step) > 1e-9) step *= 0.5;
            s = step;
        } else {
            step = s;
            s *= growth;
        }
        r += step;
        if (r >= halfExtent) break;
        pos.push_back(r);
    }
    xs.reserve(pos.size() * 2 + 1);
    for (size_t i = pos.size(); i-- > 0;) xs.push_back(-pos[i]);
    xs.push_back(0.0);
    for (double a : pos) xs.push_back(a);
    return xs;
}

// =================================================================================================
// Marco tangente del clipmap / de la malla de colisión
// =================================================================================================

/**
 * @brief Triedro tangente en el sub-punto de la cámara: `up` radial, `tu`/`tv` sobre la superficie.
 *
 * Gemelo del bloque de `planet.cpp` que llena ClipParams y de la reconstrucción que hacen
 * `terrain.tese` (recorte) y `biome.frag` (test `inClip`). Lo usa también la malla de colisión: que
 * las dos rejillas nazcan del MISMO marco es lo que evita que el suelo que se pisa y el que se
 * dibuja estén rotados uno respecto del otro.
 *
 * `(tu, tv, up)` es dextrógiro por construcción (`cross(tu,tv) == up`), lo que fija la orientación
 * de las celdas y, con ella, el winding de la malla de colisión.
 */
inline void terrainClipFrame(const glm::dvec3& cameraPos, const glm::dvec3& planetCenter,
                             glm::dvec3& up, glm::dvec3& tu, glm::dvec3& tv,
                             double snapRadius = 0.0) {
    up = glm::normalize(cameraPos - planetCenter);

    // ── ANCLAJE A RETÍCULA DEL MUNDO ────────────────────────────────────────────────────────────
    //
    // Sin esto, `up` se recalcula cada frame desde la posición EXACTA de la cámara, y como los
    // vértices del clipmap son offsets fijos respecto a él, **toda la retícula de muestreo se
    // desliza contigo**. La superficie dibujada es la interpolación lineal entre esos puntos, así
    // que al andar el terreno se re-muestrea en sitios distintos cada frame y "nada" bajo los pies
    // hasta la cuerda del quad. Y la colisión, que muestrea la función exacta, no se mueve: la
    // paridad no puede bajar de ahí, porque persigue una superficie que cambia cada frame.
    //
    // Con el anclaje, `up` salta en escalones discretos del tamaño del quad: entre salto y salto los
    // vértices están QUIETOS en coordenadas del mundo, la superficie dibujada es estable, y la
    // colisión puede construirse sobre la MISMA retícula (que es lo que lleva la disparidad a 0).
    //
    // Se cuantiza en (lat, lon) porque es donde la retícula del clipmap es regular. El paso
    // longitudinal se encoge con el coseno de la latitud, así que el anclaje NO es uniforme en
    // metros — da igual: lo que hace falta es que no se MUEVA entre saltos, no que sea equiespaciado.
    if (snapRadius > 0.0) {
        const double step = TERRAIN_CLIP_QUAD_M / snapRadius;    // radianes por celda
        if (step > 0.0) {
            const double lat = std::asin(glm::clamp(up.y, -1.0, 1.0));
            const double lon = std::atan2(up.z, up.x);
            const double latS = std::round(lat / step) * step;
            const double lonS = std::round(lon / step) * step;
            const double cl = std::cos(latS);
            up = glm::dvec3(cl * std::cos(lonS), std::sin(latS), cl * std::sin(lonS));
        }
    }
    tu = glm::cross(glm::dvec3(0, 1, 0), up);
    // Caso polar: con la cámara sobre el eje, el producto vectorial es CERO y normalizarlo daría
    // NaN (no un vector corto). Hay que elegir el eje auxiliar ANTES de normalizar.
    if (glm::length(tu) < 1e-6) tu = glm::cross(glm::dvec3(1, 0, 0), up);
    tu = glm::normalize(tu);
    tv = glm::cross(up, tu);
}

} // namespace Planet
} // namespace Haruka
