#version 460 core
#extension GL_GOOGLE_include_directive : require
/**
 * @file grass.vert
 * @brief UNA BRIZNA POR INSTANCIA, SIN MALLA: 7 vertices que este shader coloca sobre una Bezier.
 *
 * El registro (`Blade`, escrito por `grass_gen.comp`) trae la raiz relativa a la camara, la altura,
 * la normal del suelo y un hash. De ahi sale todo lo demas: la orientacion (hash), la anchura
 * (hash + LOD), el color (hash + altura), la fase del viento (hash) y la INCLINACION, que suma tres
 * cosas:
 *
 *   · el viento del clima (`u_wind`, el mismo que mueve arboles y nubes), con un vaiven por brizna;
 *   · LA PRESION: el mapa `uPress` (ver grass_press.frag) dice, en cada texel del suelo, hacia donde
 *     y cuanto esta aplastada la hierba. Se muestrea en la RAIZ (no por vertice: una brizna es rigida
 *     en la base) y tumba la brizna en esa direccion hasta casi el suelo;
 *   · la curvatura propia (cada brizna se dobla un poco hacia su lado).
 *
 * La brizna es una tira de 3 segmentos (7 vertices: 3 pares + la punta) sobre una Bezier cuadratica
 * `raiz -> control -> punta`; el control se desplaza con la inclinacion y la punta sigue. Sin
 * textura ni alpha: el frag sombrea por normal y altura. Es lo que la hace barata — medio millon de
 * briznas son 3,5 M de vertices sin una sola lectura de textura en el vertex.
 */
#include "lib/backend.glsl"   // INSTANCE_INDEX / VERTEX_INDEX

layout(std140, binding = 0) uniform GrassDraw {
    mat4  uMVP;         // proyeccion · rotacion de la vista (posiciones ya relativas a la camara)
    vec4  uWind;        // xyz = viento (m/s, mundo) · w = tiempo (s)
    vec4  uSun;         // xyz = direccion al Sol · w = intensidad
    vec4  uPressA;      // xyz = ancla del mapa de presion (relativa a la camara) · w = lado del mapa (m)
    vec4  uPressE;      // xyz = eje E del mapa
    vec4  uPressN;      // xyz = eje N del mapa
    vec4  uShade;       // x = ambiente · y = anchura media (m) · z = altura media (m) · w = (libre)
    vec4  uAerial;      // (frag) perspectiva aerea, la misma que el terreno
    vec4  uUp;          // (frag) xyz = arriba en la camara
};
struct Blade { vec4 pos; vec4 up; };
layout(std430, binding = 3) readonly buffer Blades { Blade uBlades[]; };
layout(binding = 5) uniform sampler2D uPress;   // rg = vector de aplastado (E, N) · 0 = de pie

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vColor;
layout(location = 2) out float vT;       // 0 raiz · 1 punta
layout(location = 3) out vec3 vFragPos;

void main() {
    const Blade b = uBlades[INSTANCE_INDEX];
    const vec3  up   = normalize(b.up.xyz);
    const uint  bits = floatBitsToUint(b.up.w);
    const float hash = float(bits >> 16) / 65535.0;
    const float wid  = float(bits & 0xFFFFu) / 65535.0 * 4.0;    // ensanche del LOD (1 = normal)

    // Marco de la brizna: `side` (ancho) y `fwd` (hacia donde se dobla), girados por el hash.
    const float ang = hash * 6.2831853;
    vec3 t1 = normalize(abs(up.y) < 0.99 ? cross(up, vec3(0, 1, 0)) : cross(up, vec3(1, 0, 0)));
    vec3 t2 = cross(up, t1);
    // (la brizna `blade` de la mata gira 120° y cambia de hash: ver mas abajo)
    const int   bladeK = int(VERTEX_INDEX) / 7;
    const float hashB  = fract(hash * (1.0 + 2.71 * float(bladeK)) + 0.37 * float(bladeK));
    const float angB   = ang + 2.0943951 * float(bladeK) + (hashB - 0.5) * 0.9;
    const vec3 side = t1 * cos(angB) + t2 * sin(angB);
    const vec3 fwd  = cross(up, side);
    const vec3 root = b.pos.xyz + (t1 * (hashB - 0.5) + t2 * (fract(hashB * 4.3) - 0.5)) * (0.04 * float(bladeK > 0));
    const float H   = b.pos.w * (0.8 + 0.4 * fract(hashB * 6.1));

    // ── INCLINACION: viento + presion + curvatura propia, como vector en el plano del suelo ────
    vec3 lean = fwd * (0.15 + 0.25 * fract(hashB * 7.31));
    {
        const vec3  w   = uWind.xyz - up * dot(uWind.xyz, up);
        const float ws  = length(w);
        if (ws > 1e-3) {
            const float gust = 0.6 + 0.4 * sin(uWind.w * 1.9 + hashB * 12.0 + dot(root, w) * 0.08)
                             + 0.15 * sin(uWind.w * 4.1 + hashB * 40.0);
            lean += (w / ws) * clamp(ws / 10.0, 0.0, 1.2) * gust * 0.9;
        }
    }
    // Presion: el texel del mapa bajo la raiz. Fuera del mapa, `clamp` lee el borde (que esta a 0).
    {
        const vec3  d  = root - uPressA.xyz;
        const vec2  uv = vec2(dot(d, uPressE.xyz), dot(d, uPressN.xyz)) / uPressA.w + 0.5;
        const vec2  p  = texture(uPress, clamp(uv, 0.0, 1.0)).rg;
        const float pm = length(p);
        if (pm > 1e-3) {
            const vec3 pd = normalize(uPressE.xyz * p.x + uPressN.xyz * p.y);
            // Aplastada del todo con |p| = 1: la punta a 4 cm del suelo.
            lean = mix(lean, pd * 2.6, clamp(pm, 0.0, 1.0));
        }
    }

    // ── LA BEZIER: raiz -> control -> punta. La longitud se conserva a ojo (el control baja). ────
    const float L    = H;
    const float lm   = length(lean);
    const float bend = clamp(lm, 0.0, 2.6) / 2.6;                       // 0 de pie · 1 tumbada
    const vec3  ldir = (lm > 1e-4) ? lean / lm : fwd;
    const vec3  tip  = root + up * (L * cos(bend * 1.45)) + ldir * (L * sin(bend * 1.45));
    const vec3  ctrl = root + up * (L * 0.55) + ldir * (L * 0.25 * bend);

    // Vertice: 0..5 = pares (izq/der) en t = 0, 0.45, 0.8 · 6 = punta. Y UNA MATA DE TRES: los
    // vertices 7..13 y 14..20 son la segunda y tercera brizna, giradas 120° y con su propio hash, a
    // 2-4 cm de la raiz. Triplica la cobertura del suelo por el mismo compute y el mismo registro.
    const int  viAll = int(VERTEX_INDEX);
    const int  blade = viAll / 7;
    const int  vi    = viAll - blade * 7;
    const int  pair  = min(vi / 2, 3);
    const float ts[4] = float[4](0.0, 0.45, 0.8, 1.0);
    const float t   = ts[pair];
    const float sgn = (vi == 6) ? 0.0 : ((vi & 1) == 0 ? -1.0 : 1.0);
    const float u   = 1.0 - t;
    const vec3  p   = u * u * root + 2.0 * u * t * ctrl + t * t * tip;
    const vec3  tng = normalize(2.0 * u * (ctrl - root) + 2.0 * t * (tip - ctrl));
    // Anchura: la media × hash × LOD, afinando hacia la punta.
    const float wBase = uShade.y * (0.7 + 0.6 * fract(hashB * 3.77)) * wid;
    const vec3  pw    = p + side * (sgn * 0.5 * wBase * (1.0 - t * t));

    vFragPos = pw;
    // Normal: perpendicular a la tangente y al ancho, con un abombado hacia los bordes para que
    // la luz cruce la brizna en vez de dejarla plana.
    vec3 n = normalize(cross(side, tng));
    n = normalize(n + side * (sgn * 0.35));
    vNormal = n;
    vT      = t;
    // Color: verde con matiz por brizna y algo mas seco cuanto mas alta; la raiz oscura la pone el frag.
    const float hue = fract(hashB * 5.13);
    vColor = mix(vec3(0.16, 0.34, 0.08), vec3(0.42, 0.52, 0.16), hue) * (0.85 + 0.3 * fract(hashB * 9.7));
    gl_Position = uMVP * vec4(pw, 1.0);
}
