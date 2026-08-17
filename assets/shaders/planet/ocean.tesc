#version 460 core
layout(vertices = 4) out;
layout(location = 0) in  vec2 cLocal[];
layout(location = 0) out vec2 eLocal[];
layout(std140, binding = 0) uniform SimplePlanetUBO {
    mat4 uMVP; vec4 uCenter; vec4 uLightDir; vec4 uLightColor; vec4 uAmbient; vec4 uExtra; vec4 uDebug;
    vec4 uTexAnchor;
};
layout(std140, binding = 13) uniform ClipParams {
    vec4 uClipOrigin; vec4 uClipTanU; vec4 uClipTanV; vec4 uClipCover;
};

// TESELACIÓN DEL AGUA: la decide la OLA MÁS CORTA que se quiera ver, no el tamaño en pantalla.
//
// La ola más corta del tren mide 8,7 m (ver `ocean_wave.glsl`). Nyquist pide al menos dos muestras
// por longitud de onda, así que el quad tiene que bajar de ~4 m para que esa ola exista como
// geometría en vez de como muaré. Con parches de 128 m eso son 32 divisiones — justo el tope del
// clipmap del terreno, que es de donde sale la rejilla.
//
// Y se recorta con la DISTANCIA, porque una ola a 2 km ya es subpíxel: por debajo de ese umbral la
// amplitud la desvanece el tese y aquí basta la subdivisión mínima para que el plano siga la esfera.
float edgeF(vec2 a, vec2 b) {
    // `rad` = distancia al centro del clipmap, que está bajo la cámara: sirve de distancia al ojo
    // para el recorte, y a diferencia de la distancia real es función SOLO de la arista (lo que
    // garantiza que dos parches vecinos obtengan el mismo factor y no haya grietas).
    float rad = length((a + b) * 0.5);
    float arc = length(a - b);
    float lvl = arc / 4.0;                            // 4 m por segmento: la ola de 8,7 m con 2 muestras
    // Un quad de 4 m mide ~2 px a 2 km, así que la subdivisión plena solo hace falta cerca; de ahí
    // en adelante se desvanece hasta 1. El límite lo pone el tamaño en pantalla, no el gusto.
    lvl *= 1.0 - smoothstep(1200.0, 9000.0, rad);
    return clamp(lvl, 1.0, 32.0);
}

void main() {
    eLocal[gl_InvocationID] = cLocal[gl_InvocationID];
    if (gl_InvocationID == 0) {
        // Parche ENTERO fuera del hueco del anillo interior: se descarta como en el clipmap del
        // terreno, o los anillos se solaparían y el agua se mezclaría consigo misma (alfa doble).
        float hole = uClipCover.w;
        if (hole > 0.0) {
            vec2 c0 = cLocal[0], c1 = cLocal[1], c2 = cLocal[2], c3 = cLocal[3];
            float mx = max(max(abs(c0.x), abs(c1.x)), max(abs(c2.x), abs(c3.x)));
            float my = max(max(abs(c0.y), abs(c1.y)), max(abs(c2.y), abs(c3.y)));
            if (mx <= hole && my <= hole) {
                gl_TessLevelOuter[0] = gl_TessLevelOuter[1] = 0.0;
                gl_TessLevelOuter[2] = gl_TessLevelOuter[3] = 0.0;
                gl_TessLevelInner[0] = gl_TessLevelInner[1] = 0.0;
                return;
            }
        }
        // Factor por ARISTA y solo a partir de sus dos extremos: dos parches vecinos comparten
        // arista y obtienen el MISMO número, así que no puede haber grietas (misma propiedad que el
        // terreno; meter aquí cualquier dato del parche la rompe).
        float e0 = edgeF(cLocal[3], cLocal[0]);
        float e1 = edgeF(cLocal[0], cLocal[1]);
        float e2 = edgeF(cLocal[1], cLocal[2]);
        float e3 = edgeF(cLocal[2], cLocal[3]);
        gl_TessLevelOuter[0] = e0; gl_TessLevelOuter[1] = e1;
        gl_TessLevelOuter[2] = e2; gl_TessLevelOuter[3] = e3;
        gl_TessLevelInner[0] = max(e1, e3);
        gl_TessLevelInner[1] = max(e0, e2);
    }
}
