/**
 * @file precip.vert
 * @brief PRECIPITACIÓN EN EL MUNDO: gotas y copos como geometría, no como una capa de pantalla.
 *
 * ── POR QUÉ NO ES UN PASE FULLSCREEN ────────────────────────────────────────────────────────────
 * El pase anterior (`rain.frag`) pintaba rayas sobre la imagen ya compuesta: sin profundidad, sin
 * posición, sin mundo. Llovía dentro de una cueva, detrás de una pared y bajo un techo, porque la
 * lluvia no estaba en ninguna parte — era un filtro. Aquí cada gota es un quad con posición real
 * que se somete al DEPTH BUFFER: si hay roca, tronco o muro entre la gota y la cámara, no se ve.
 *
 * ── SIN BUFFERS: LA GOTA SALE DE `gl_VertexID` ──────────────────────────────────────────────────
 * No hay VBO ni instancias que subir: 6 vértices por gota, el índice de gota es `gl_VertexID/6` y su
 * posición sale de un hash de ese índice. Cero tráfico CPU→GPU por frame, y el número de gotas es
 * un `draw(6*N)`. Mover una nube de partículas por CPU cada frame era justo el coste que no queremos.
 *
 * ── LA REJILLA QUE ENVUELVE A LA CÁMARA ─────────────────────────────────────────────────────────
 * Las gotas viven en una celda cúbica de lado `u_box` centrada en el observador, y se ENVUELVEN
 * (mod) al salir: siempre hay la misma cantidad alrededor, se mueva el jugador o no. El truco está
 * en restar la posición de la cámara MÓDULO la celda (`u_camCell`, que calcula la CPU en double):
 * así la rejilla queda anclada al MUNDO y no arrastrada por el jugador. Sin eso, la lluvia viajaría
 * pegada a ti y al correr se vería estática — el efecto "estoy dentro de una pecera".
 *
 * Todo el pase trabaja en coordenadas RELATIVAS A LA CÁMARA, como el resto de la escena (el planeta
 * mide 6.4e6 m: en absolutas, un float no resuelve una gota).
 */
#version 450 core

layout(location = 0) out vec3  vRel;      // posición relativa a la cámara (para la niebla/fade)
layout(location = 1) out vec2  vQuadUV;   // [-1,1]² dentro del quad (para la forma)
layout(location = 2) out float vFade;     // 0..1 alfa por distancia/borde de la celda

layout(std140, binding = 5) uniform PrecipParams {
    vec4 u_up;        // xyz = cénit local (unitario) · w = tamaño de la celda (m)
    vec4 u_vel;       // xyz = velocidad de caída + viento (m/s, mundo) · w = intensidad [0,1]
    vec4 u_camCell;   // xyz = cámara MÓDULO la celda, YA en el marco local (e1,up,e2) · w = tiempo (s)
    vec4 u_look;      // xyz = dirección de vista · w = 1 si es NIEVE (copo) en vez de lluvia (raya)
    vec4 u_tint;      // rgb = color de la gota (tomado de la luz del sol) · a = alcance de fundido (m)
    vec4 u_misc;      // x = alto del viewport en PÍXELES · y = 1 si hay máscara cenital · zw = libre
    mat4 u_skySpace;  // matriz de la MÁSCARA DE EXPOSICIÓN AL CIELO (ortográfica desde el cénit)
};

// Profundidad de lo que hay ENCIMA (pase cenital). Lo dibuja el mismo hook que el shadow map del
// sol, pero con la cámara en el cénit.
layout(binding = 0) uniform sampler2D u_skyMask;

// ⚠️ ORDEN: `view` va PRIMERO. Es el layout real del UBO per-frame del motor (ver planet.vert):
// declararlo al revés no da error de compilación, simplemente lee la proyección donde está la vista
// y viceversa → todo se transforma con basura y no se rasteriza NADA. Así se perdió el primer intento.
layout(std140, binding = 0) uniform PerFrameData {
    mat4 view;
    mat4 projection;
    vec3 _cameraPos;      float _pad0;
    vec3 _sunDirection;   float _pad1;
    vec3 _sunLightColor;  float ambientStrength;
    int  _enableHDR;
};

// Hash 3D barato y determinista: la gota N siempre nace en el mismo sitio de la celda.
vec3 hash31(float n) {
    vec3 p = fract(vec3(n * 0.1031, n * 0.1030, n * 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yzz) * p.zyx);
}

void main() {
    const float box = u_up.w;
    const int   id  = gl_VertexID / 6;
    const int   crn = gl_VertexID % 6;

    // Marco local: `up` es el cénit (radial en un planeta), y dos tangentes cualesquiera.
    vec3 up = normalize(u_up.xyz);
    vec3 e1 = normalize(cross(up, abs(up.y) < 0.9 ? vec3(0, 1, 0) : vec3(1, 0, 0)));
    vec3 e2 = cross(up, e1);

    // Semilla de la gota + desplazamiento por el tiempo (la CPU ya lo entrega envuelto en la celda).
    vec3 seed = hash31(float(id) + 0.5) * box;
    vec3 disp = u_vel.xyz * u_camCell.w;

    // A coordenadas de la celda, restando la cámara MÓDULO celda → rejilla anclada al mundo.
    vec3 local = vec3(dot(disp, e1), dot(disp, up), dot(disp, e2));
    // `u_camCell` YA viene en el marco local y envuelto: la CPU proyecta la posición absoluta sobre
    // (e1,up,e2) y hace el módulo EN DOUBLE. Proyectar aquí lo ya envuelto por ejes de mundo daría
    // un anclaje distinto en cada orientación — la rejilla se arrastraría al girar el planeta.
    vec3 q = mod(seed + local - u_camCell.xyz, vec3(box)) - vec3(box * 0.5);

    // De vuelta a mundo (relativo a la cámara).
    vec3 rel = e1 * q.x + up * q.y + e2 * q.z;

    // Orientación del quad: LARGO en la dirección de caída (la raya de la gota es su estela) y
    // ANCHO perpendicular a la vista → siempre se ve de canto ancho, nunca desaparece de perfil.
    vec3 vdir = normalize(u_vel.xyz);
    vec3 side = cross(vdir, normalize(rel));
    float sl  = length(side);
    side = (sl > 1e-4) ? side / sl : e1;

    const bool snow = u_look.w > 0.5;
    // La NIEVE es un copo: corto, ancho y lento. La LLUVIA es una estela: larga y fina. Esa
    // diferencia de FORMA es lo que hace que se distingan de un vistazo, más que el color.
    float len = snow ? 0.10 : 0.85;
    float wid = snow ? 0.09 : 0.016;

    // ⚠️ ANCHO MÍNIMO DE UN PÍXEL Y PICO. Una gota real mide milímetros: 1.6 cm a 10 m son 0.0016 rad
    // = un TERCIO de píxel a 256 px de alto. Un triángulo que no cubre ninguna muestra no se rasteriza
    // → la lluvia desaparecía entera (así salió: 0 píxeles en el test, y en pantallas normales habría
    // sido un parpadeo sucio). El tamaño en MUNDO se sube al que ocupa ~1.4 px a esa distancia; la
    // ilusión no sufre (una gota se ve por su estela, no por su grosor) y deja de depender de la
    // resolución. `projection[1][1] = 1/tan(fovY/2)` → radianes por píxel = 2/(p11·alto).
    float dist    = max(length(rel), 0.05);
    float mPerPx  = dist * 2.0 / (max(projection[1][1], 1e-4) * max(u_misc.x, 1.0));
    wid = max(wid, mPerPx * 1.4);
    len = max(len, mPerPx * 2.0);

    // Quad de 2 triángulos: (-1,-1) (1,-1) (1,1) / (-1,-1) (1,1) (-1,1)
    vec2 c = vec2((crn == 1 || crn == 2 || crn == 4) ? 1.0 : -1.0,
                  (crn == 2 || crn == 4 || crn == 5) ? 1.0 : -1.0);
    vec3 world = rel + side * (c.x * wid) + vdir * (c.y * len);

    // Fundido: en el BORDE de la celda (o aparecerían/desaparecerían de golpe al envolverse) y con
    // la distancia. La gota de al lado de la cara es la que se ve; las del borde solo dan sensación.
    float edge = 1.0 - max(max(abs(q.x), abs(q.y)), abs(q.z)) / (box * 0.5);
    float d    = length(rel);
    vFade = clamp(edge * 3.0, 0.0, 1.0) * (1.0 - smoothstep(u_tint.a * 0.5, u_tint.a, d));

    // ── ¿ESTÁ ESTA GOTA BAJO CUBIERTO? ──────────────────────────────────────────────────────────
    // La pregunta es "¿hay algo ENCIMA?", y no la puede responder el depth de la escena: bajo un
    // tejado, la gota que cae delante de tu cara no tiene NADA delante. Se responde con la máscara
    // cenital: si lo que hay arriba está más cerca del cielo que la gota, la gota está tapada.
    // Se decide POR GOTA, así que en el borde de un alero llueve fuera y no dentro, en la MISMA
    // imagen — un interruptor global no podría hacer eso.
    if (u_misc.y > 0.5) {
        vec4 sp = u_skySpace * vec4(rel, 1.0);
        // Con glClipControl(ZERO_TO_ONE) la z de clip YA sale en [0,1]; solo x/y van en [-1,1].
        // (Mismo convenio que el shadow map del sol en planet.frag — remapear la z aquí la hundiría
        // y saldría "todo tapado".)
        vec3 sc = vec3(sp.xy / sp.w * 0.5 + 0.5, sp.z / sp.w);
        if (sc.z <= 1.0 && sc.x > 0.0 && sc.x < 1.0 && sc.y > 0.0 && sc.y < 1.0) {
            float above = texture(u_skyMask, sc.xy).r;
            // El sesgo evita que el propio tejado se "auto-tape" por el grosor del téxel; y el
            // fundido suave impide un corte recto en la vertical del alero.
            vFade *= 1.0 - smoothstep(0.0, 0.004, sc.z - above - 0.0015);
        }
    }

    vRel    = rel;
    vQuadUV = c;
    // Misma convención que el resto de la escena: vista SIN traslación sobre posiciones ya relativas.
    gl_Position = projection * mat4(mat3(view)) * vec4(world, 1.0);
}
