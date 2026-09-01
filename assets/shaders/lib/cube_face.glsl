// ===========================================================================================
// INVERSA cubo↔esfera — gemelo GLSL de `dirToCubeFaceClosed` en src/core/terrain/cube_sphere.cpp.
//
// La malla del planeta se construye recorriendo (lx,ly) en [-1,1] por cara y llamando a
// `cubeFaceToDir`. Cualquier shader que quiera leer esa retícula desde una DIRECCIÓN tiene que
// deshacer exactamente esa proyección. Hacerlo "casi" no vale: la versión anterior de este código
// usaba la proyección gnómica a secas (dividir por la componente dominante) y encima con el signo
// de v invertido, así que el clipmap leía la altura de OTRO PUNTO DEL PLANETA — normalmente fondo
// oceánico, con lo que la rejilla se hundía kilómetros y no se dibujaba ni un píxel. El síntoma no
// era un error de GL ni un shader que no compila: era simplemente que no se veía nada.
//
// La derivación de la forma cerrada está en cube_sphere.h.
// ⚠️ CUALQUIER CAMBIO VA EN LOS DOS FICHEROS A LA VEZ.
// ===========================================================================================
#ifndef HARUKA_CUBE_FACE_GLSL
#define HARUKA_CUBE_FACE_GLSL

// dir unitaria → (cara 0..5, uv=(lx,ly) en [-1,1]). Las caras son el enum PlanetFace:
// 0 FRONT(+X) · 1 BACK(-X) · 2 TOP(+Y) · 3 BOTTOM(-Y) · 4 RIGHT(+Z) · 5 LEFT(-Z).
void harukaDirToCubeFace(vec3 d, out int face, out vec2 uv) {
    vec3 ad = abs(d);

    // (sj,sk) = las dos componentes NO dominantes, en el orden en que cubeFaceToDir las coloca.
    int   dom;
    float sj, sk;
    if (ad.x >= ad.y && ad.x >= ad.z) { dom = 0; sj = d.y; sk = d.z; }
    else if (ad.y >= ad.z)            { dom = 1; sj = d.x; sk = d.z; }
    else                              { dom = 2; sj = d.x; sk = d.y; }

    float j2 = sj * sj, k2 = sk * sk;
    float T  = j2 + k2;
    float D  = 2.0 * (j2 - k2);                       // = A - B
    float Q  = max(9.0 - 12.0 * T + D * D, 0.0);
    // Forma CONJUGADA de `3 - sqrt(Q)`: en el centro de la cara T→0 y sqrt(Q)→3, y la resta directa
    // se lleva por delante los dígitos significativos (en float32, todos).
    float S  = (12.0 * T - D * D) / (3.0 + sqrt(Q));

    float a = sign(sj) * sqrt(max((S + D) * 0.5, 0.0));
    float b = sign(sk) * sqrt(max((S - D) * 0.5, 0.0));

    if (dom == 0) {
        if (d.x > 0.0) { face = 0; uv = vec2(-b,  a); }   // p = ( 1, ly, -lx)
        else           { face = 1; uv = vec2( b,  a); }   // p = (-1, ly,  lx)
    } else if (dom == 1) {
        if (d.y > 0.0) { face = 2; uv = vec2( a, -b); }   // p = (lx,  1, -ly)
        else           { face = 3; uv = vec2( a,  b); }   // p = (lx, -1,  ly)
    } else {
        if (d.z > 0.0) { face = 4; uv = vec2( a,  b); }   // p = (lx, ly,  1)
        else           { face = 5; uv = vec2(-a,  b); }   // p = (-lx,ly, -1)
    }
}

#endif // HARUKA_CUBE_FACE_GLSL
/**
 * @brief RAÍZ EN DOBLE QUE NO PASA POR `sqrt(double)` DEL DRIVER. Newton desde una semilla en float.
 *
 * ⚠️ EXISTE POR UN BUG DE DRIVER MEDIDO, NO POR PRURITO. El compilador de GLSL del driver de OpenGL
 * de AMD degrada las transcendentes de doble a precisión de float: `inversesqrt(double)` sale con
 * **3,4e-8 de error relativo** (medido con entradas opacas en `fp64_probe.comp`). Como esta función
 * hace tres `sqrt(double)` por dirección y la dirección se multiplica luego por 6 371 000 m, eso son
 * **0,0235 m de superficie** — exactamente el peor error del bake en esa combinación (0,0253 m),
 * contra 0,0001 m en AMD+Vulkan y en NVIDIA con los dos backends.
 *
 * Newton para `sqrt(a)` es `x <- (x + a/x)/2` y DUPLICA los dígitos correctos en cada paso. Desde una
 * semilla en float (~7 dígitos) dos pasos dan ~28, más de los 16 que un double guarda. Y sólo usa
 * SUMA, MULTIPLICACIÓN Y DIVISIÓN en doble, que es lo que ese driver sí hace bien — los casos (1) y
 * (2) de la sonda pasan en las cuatro combinaciones.
 *
 * ⚠️ El resultado puede diferir en 1 ulp del `sqrt` correctamente redondeado de la CPU. Eso son 1e-16
 * relativos, o sea **6e-10 m de superficie**: seis órdenes por debajo del 0,0001 m con el que el bake
 * ya casa. No mueve la paridad.
 *
 * ⚠️ El dominio aquí es `[1/3, 1]` (el radicando del spherify de Cobb), así que la semilla en float
 * ni desborda ni se aplana. Fuera de ese rango habría que revisar la semilla.
 */
precise double harukaSqrtD(double a) {
    if (!(a > 0.0LF)) return 0.0LF;
    precise double x = double(sqrt(float(a)));
    x = 0.5LF * (x + a / x);
    x = 0.5LF * (x + a / x);
    return x;
}

/**
 * @brief (cara, lx, ly) → dirección unitaria. Spherify de Cobb. **Gemelo de `cubeFaceToDir`**
 *        (core/terrain/cube_sphere.cpp) — cualquier cambio va en los dos.
 *
 * ⚠️ Faltaba: este fichero solo tenía la INVERSA, porque hasta ahora ningún shader necesitaba ir de
 * cara a dirección — el clipmap parte de un marco tangente, no de coordenadas de cara. El quadtree
 * del v5 sí: su téxel se identifica por (cara, nivel, i, j, u, v) y la dirección se DERIVA de ahí.
 *
 * En DOUBLE a propósito. La coordenada de cara del nodo es exacta (sale de enteros) y redondearla a
 * float aquí tiraría justo lo que ese diseño compra: a radio terrestre un ulp de dirección unitaria
 * son 0,38-0,76 m de superficie.
 */
dvec3 harukaCubeFaceToDir(int face, double lx, double ly) {
    // `precise`: prohíbe al compilador del driver reasociar o CONTRAER (`a*b+c` → FMA) estas
    // expresiones. Sin ello el resultado depende del driver, y el gemelo C++ se compila con
    // `-ffp-contract=off` (ver CMakeLists) — o sea que sin esto los dos lados hacen aritméticas
    // distintas a propósito y no pueden coincidir bit a bit.
    precise dvec3 p;
    if      (face == 0) p = dvec3( 1.0LF,   ly,   -lx);    // FRONT
    else if (face == 1) p = dvec3(-1.0LF,   ly,    lx);    // BACK
    else if (face == 2) p = dvec3(   lx, 1.0LF,   -ly);    // TOP
    else if (face == 3) p = dvec3(   lx,-1.0LF,    ly);    // BOTTOM
    else if (face == 4) p = dvec3(   lx,    ly, 1.0LF);    // RIGHT
    else                p = dvec3(  -lx,    ly,-1.0LF);    // LEFT
    precise double x2 = p.x * p.x, y2 = p.y * p.y, z2 = p.z * p.z;
    // ⚠️ `harukaSqrtD` Y NO `sqrt`: ver su nota. Con el `sqrt(double)` del driver, esta línea daba
    // 0,0235 m de error de superficie en AMD+OpenGL y 6e-10 m en todo lo demás.
    precise dvec3 r = dvec3(p.x * harukaSqrtD(1.0LF - y2 / 2.0LF - z2 / 2.0LF + y2 * z2 / 3.0LF),
                            p.y * harukaSqrtD(1.0LF - z2 / 2.0LF - x2 / 2.0LF + z2 * x2 / 3.0LF),
                            p.z * harukaSqrtD(1.0LF - x2 / 2.0LF - y2 / 2.0LF + x2 * y2 / 3.0LF));
    return r;
}

/**
 * @brief La MISMA proyección en FLOAT. Existe para el vertex shader del quadtree, y el motivo es de
 *        rendimiento MEDIDO, no de estilo.
 *
 * ⚠️ El `sqrt` en doble precisión va a 1/32 - 1/64 del ritmo del float en GPU de consumo. Tres por
 * vértice, sobre ~1,1 M de vértices por frame, es lo que tenía el pase de nodos en 24 ms dibujando
 * solo 2,1 M de triángulos — coste que no cuadraba con la geometría.
 *
 * ⚠️ Y NO vale para todo: la coordenada de cara `lx` SÍ tiene que calcularse en double (sale de
 * enteros y de ella depende que el vértice caiga sobre su téxel). Lo que se degrada aquí es solo la
 * proyección cara→esfera, cuya entrada ya es exacta. El error que introduce se mide en
 * `terrain_node_render`.
 */
vec3 harukaCubeFaceToDirF(int face, float lx, float ly) {
    vec3 p;
    if      (face == 0) p = vec3( 1.0,   ly,  -lx);
    else if (face == 1) p = vec3(-1.0,   ly,   lx);
    else if (face == 2) p = vec3(  lx,  1.0,  -ly);
    else if (face == 3) p = vec3(  lx, -1.0,   ly);
    else if (face == 4) p = vec3(  lx,   ly,  1.0);
    else                p = vec3( -lx,   ly, -1.0);
    float x2 = p.x * p.x, y2 = p.y * p.y, z2 = p.z * p.z;
    return vec3(p.x * sqrt(1.0 - y2 * 0.5 - z2 * 0.5 + y2 * z2 / 3.0),
                p.y * sqrt(1.0 - z2 * 0.5 - x2 * 0.5 + z2 * x2 / 3.0),
                p.z * sqrt(1.0 - x2 * 0.5 - y2 * 0.5 + x2 * y2 / 3.0));
}
