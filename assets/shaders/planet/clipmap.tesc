#version 460 core
layout(vertices = 4) out;
layout(location = 0) in vec2 cLocal[]; layout(location = 0) out vec2 eLocal[];
layout(std140, binding = 0) uniform SimplePlanetUBO {
    mat4 uMVP; vec4 uCenter; vec4 uLightDir; vec4 uLightColor; vec4 uAmbient; vec4 uExtra; vec4 uDebug;
    vec4 uTexAnchor;   // ancla planetaria de las UV de terreno (la usa biome.frag; ver planet.cpp)
};
layout(std140, binding = 13) uniform ClipParams {
    vec4 uClipOrigin;   // xyz = dirección del centro de la rejilla, w = tamaño de parche (m)
    vec4 uClipTanU;     // xyz = tangente este
    vec4 uClipTanV;     // xyz = tangente norte
    vec4 uClipCover;    // x = semi-lado (m) del clipmap, y/z = inicio/fin del anillo de mezcla,
                        // w = semi-lado del HUECO central (0 = sin hueco)
};
// Mismo criterio que el de la malla del planeta: factor a partir de la ARISTA. Aquí las aristas son
// mucho más cortas, así que el factor satura a 32 cerca y baja solo en el borde de la rejilla.
float edgeFactor(vec2 a, vec2 b) {
    // OJO: `patch` es palabra RESERVADA en GLSL (el calificador `patch out` de teselación). Usarla
    // como nombre de variable da "syntax error, unexpected PATCH" sin más pista.
    vec2  m = (a + b) * 0.5;
    // Distancia aproximada a la cámara EN EL PLANO: la cámara está sobre el origen local.
    float d = max(length(m), 1.0);
    float arc = length(a - b);
    // Tope 32 (antes 64): los parches cercanos a la cámara saturaban a 64 → quads de 2 m en los
    // primeros ~500 m, que es el grueso del coste de teselación (cada vértice evalúa ~15 ruidos de
    // detalle). Con 32 los quads cercanos son de 4 m — la octava de 22 m sigue con 5,5 muestras por
    // onda, el suelo se ve igualmente suave, y la teselación cuesta 4× menos. El fragmento (relleno)
    // es por píxel, no cambia con esto.
    // ⚠️ NIVEL EN POTENCIAS DE DOS, y no es cosmético: es lo que hace posible la paridad absoluta.
    //
    // Con `equal_spacing` (ver la nota de clipmap.tese) un nivel n reparte el parche en n tramos
    // iguales de `arc/n`. Si n es potencia de dos, ese tramo es 128/32 = 4 m, 8 m, 16 m… o sea que los
    // vértices SIEMPRE caen en múltiplos del quad, y el conjunto de un nivel grueso es un
    // SUBCONJUNTO del fino. Con un nivel arbitrario (31,4) los vértices caen en 4,076 m y no coinciden
    // con nada: la malla de colisión no puede compartirlos por mucho que se afine.
    //
    // Se redondea hacia ABAJO (`floor` del log2): el nivel resultante nunca es más fino que el que
    // pedía la fórmula, así que el coste no sube — baja un poco y el error se queda en el rango que
    // mide `terrain_chord_error`.
    float lvl = clamp(arc / (d * 0.004), 1.0, 32.0);
    return clamp(exp2(floor(log2(lvl))), 1.0, 32.0);
}
void main() {
    // ESTIRADO por altitud (`uClipTanU.w` = 2^k): la rejilla es la misma, solo cubre más. Se aplica
    // aquí y en el tese sobre la MISMA coordenada, así que las dos etapas ven el mismo parche.
    float clipScale = max(uClipTanU.w, 1.0);
    eLocal[gl_InvocationID] = cLocal[gl_InvocationID] * clipScale;
    if (gl_InvocationID == 0) {
        // Las esquinas YA ESTIRADAS: las usan tanto el test del hueco como el factor de teselación.
        // Si el factor se midiera sobre la rejilla sin estirar, un parche de 4 km pediría el mismo
        // nivel que uno de 128 m y saldrían quads gigantes justo donde se estiró para cubrir más.
        vec2 c0 = cLocal[0] * clipScale, c1 = cLocal[1] * clipScale;
        vec2 c2 = cLocal[2] * clipScale, c3 = cLocal[3] * clipScale;
        // ── HUECO PARA EL SUELO QUE VIENE DE LA COLISIÓN ────────────────────────────────────────
        //
        // Dentro del hueco el suelo lo dibuja el anillo cercano, con los MISMOS vértices y los
        // MISMOS triángulos que Jolt colisiona. Si el clipmap dibujara también, habría dos
        // superficies sobre el mismo suelo separadas solo por un sesgo de profundidad: z-fighting, y
        // una línea en la frontera.
        //
        // El descarte es por PARCHE ENTERO (un parche no se puede recortar a medias), y por eso el
        // semi-lado del hueco tiene que caer en un borde de parche — ver `TERRAIN_CLIP_HOLE_M`, que
        // explica por qué son 192 m y no los 256 del anillo.
        //
        // Nivel 0 descarta el parche sin rasterizar nada: es más barato que dibujarlo y taparlo.
        float hole = uClipCover.w;
        if (hole > 0.0) {
            vec2 lo = min(min(c0, c1), min(c2, c3));
            vec2 hi = max(max(c0, c1), max(c2, c3));
            if (lo.x >= -hole && hi.x <= hole && lo.y >= -hole && hi.y <= hole) {
                gl_TessLevelOuter[0] = 0.0; gl_TessLevelOuter[1] = 0.0;
                gl_TessLevelOuter[2] = 0.0; gl_TessLevelOuter[3] = 0.0;
                gl_TessLevelInner[0] = 0.0; gl_TessLevelInner[1] = 0.0;
                return;
            }
        }
        float e0 = edgeFactor(c3, c0);
        float e1 = edgeFactor(c0, c1);
        float e2 = edgeFactor(c1, c2);
        float e3 = edgeFactor(c2, c3);
        gl_TessLevelOuter[0]=e0; gl_TessLevelOuter[1]=e1;
        gl_TessLevelOuter[2]=e2; gl_TessLevelOuter[3]=e3;
        gl_TessLevelInner[0]=max(e1,e3); gl_TessLevelInner[1]=max(e0,e2);
    }
}
