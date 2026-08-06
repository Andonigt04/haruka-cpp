/**
 * @file sky.frag
 * @brief Cielo procedural: gradiente cénit→horizonte por elevación solar,
 *        resplandor de amanecer/atardecer, disco + halo del sol, y estrellas
 *        que aparecen de noche y en el espacio. Se mezcla a negro espacial
 *        según el grosor de atmósfera (u_atmo: 1 en superficie, 0 en órbita).
 *
 * Pase de fondo: se dibuja tras limpiar, SIN escribir profundidad, de modo
 * que el terreno/objetos se pintan encima.
 */
#version 450 core

layout(location = 0) in vec3 vRayDir;
layout(location = 0) out vec4 FragColor;

// UBO compartido con sky.vert (binding 5) — antes uniforms sueltos (glUniform3fv/1f), que NO
// existen en Vulkan → van por UBO (ruta PSO/RHI). Declaración IDÉNTICA a la de sky.vert (GLSL lo
// exige entre etapas del mismo programa). Truco std140: vec3 (align 16, size 12) + float siguiente
// comparten el mismo slot de 16 B → por eso van emparejados.
layout(std140, binding = 5) uniform SkyParams {
    mat4  u_invViewProjRot; // inv(proj * mat4(mat3(view))) — lo usa el vertex
    vec3  u_sunDir;         // hacia el Sol (mundo, normalizado)
    float u_sunElev;        // dot(sunDir, up): elevación del sol
    vec3  u_up;             // cénit local del observador (mundo)
    float u_atmo;           // 1=superficie ... 0=espacio
    vec3  u_sunColor;       // color de la luz solar
    float u_time;           // segundos (movimiento de nubes)
    vec4  u_weather;        // x=humedad · y=tempC · z=precipitación · w=COBERTURA de nube (del mundo)
    vec4  u_wind;           // x=este(m/s) · y=norte(m/s) · z=racha · w=1 si es NIEVE
};

// Hash 3D barato para el campo de estrellas.
float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

// Value-noise + fBm para las NUBES pintadas (anime).
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash13(vec3(i, 0.0));
    float b = hash13(vec3(i + vec2(1.0, 0.0), 0.0));
    float c = hash13(vec3(i + vec2(0.0, 1.0), 0.0));
    float d = hash13(vec3(i + vec2(1.0, 1.0), 0.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
float fbm(vec2 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 5; ++i) { v += a * vnoise(p); p *= 2.0; a *= 0.5; }
    return v;
}

// Densidad de nube: DOMAIN WARP (deforma las coords con otro ruido → formas orgánicas billowy, no blobs) +
// erosión de detalle en los bordes (desgarrados). `wind` = deriva temporal.
float cloudField(vec2 p, vec2 wind) {
    vec2  warp = vec2(fbm(p * 0.6 + wind * 0.5), fbm(p * 0.6 + 5.2 - wind * 0.5));
    float d    = fbm(p * 1.3 + warp * 1.4 + wind);
    d -= 0.20 * fbm(p * 4.0 - wind * 2.0);
    return d;
}

void main()
{
    vec3 dir = normalize(vRayDir);
    float t  = dot(dir, u_up);              // 1=cénit, 0=horizonte, <0 bajo el horizonte
    float sunAmt = max(dot(dir, u_sunDir), 0.0);

    // --- Paleta por elevación solar (noche → día) ---
    float day = smoothstep(-0.12, 0.22, u_sunElev);

    vec3 zenithNight = vec3(0.012, 0.016, 0.045);
    vec3 horizNight  = vec3(0.020, 0.024, 0.055);
    vec3 zenithDay   = vec3(0.18,  0.40,  0.82);
    vec3 horizDay    = vec3(0.62,  0.74,  0.92);

    vec3 zenithC = mix(zenithNight, zenithDay, day);
    vec3 horizC  = mix(horizNight,  horizDay,  day);

    // Gradiente vertical: horizonte → cénit. Bajo el horizonte se aplana al
    // color de horizonte (el terreno suele taparlo de todos modos).
    float h = smoothstep(0.0, 0.55, max(t, 0.0));
    vec3 sky = mix(horizC, zenithC, h);

    // --- Resplandor cálido de amanecer/atardecer ---
    // Máximo con el sol cerca del horizonte; concentrado en su acimut y abajo.
    float twilight = exp(-(u_sunElev * u_sunElev) / 0.020)
                   * smoothstep(-0.25, 0.05, u_sunElev);
    float glowAz   = pow(sunAmt, 4.0);                 // pegado al acimut del sol
    float glowLow  = 1.0 - smoothstep(0.0, 0.35, abs(t)); // ceñido al horizonte
    sky = mix(sky, vec3(0.95, 0.45, 0.22), twilight * glowAz * glowLow * 0.9);

    // Halo difuso alrededor del sol — SOLO dentro de la atmósfera (dispersión de Mie). En el
    // espacio (atmo=0) no hay halo → así el Sol se ve IGUAL dentro y fuera (su cuerpo es la
    // corona-objeto emisiva; el cielo ya NO dibuja un disco solar aparte que se veía distinto
    // contra cielo azul vs negro = "dos soles/diferentes").
    sky += u_sunColor * pow(sunAmt, 8.0) * 0.35 * day * u_atmo;

    // Con LLUVIA el cielo se CUBRE y agrisa (overcast) → el azul soleado desaparece.
    sky = mix(sky, vec3(0.55, 0.58, 0.63) * (0.65 + 0.35 * day), u_weather.z * 0.7 * u_atmo);

    // --- NUBES cinematográficas: VOLUMEN (domain warp + auto-sombra en varios pasos hacia el sol = gradiente
    //     luz→sombra "de película"), ESCALA por zonas (un campo de masa de baja frecuencia → cúmulos grandes,
    //     zonas despejadas y puffs pequeños), CICLO del planeta y SILVER-LINING a contraluz. Plano tangente
    //     LOCAL (es un planeta → "arriba" = u_up). ---
    if (u_atmo > 0.5 && t > 0.02) {
        vec3 e1 = normalize(cross(u_up, abs(u_up.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
        vec3 e2 = cross(u_up, e1);
        vec2 base = vec2(dot(dir, e1), dot(dir, e2)) / (t + 0.22);       // proyección al domo
        // DERIVA = el viento del CLIMA (m/s en este/norte), no una constante. Es el mismo vector que
        // inclina la lluvia y (fase futura) ondea el follaje: la nube va hacia donde sopla.
        vec2 wind = u_wind.xy * 0.0022 * u_time;

        // MASA a baja frecuencia → varía el TAMAÑO/cobertura por zonas (grandes cúmulos ↔ despejado ↔ puffs).
        float mass    = fbm(base * 0.30 + wind * 0.25);
        float rain    = u_weather.z;   // 0..1 precipitación (la que el MUNDO dice que está cayendo aquí)

        // ⚠️ LA COBERTURA VIENE DEL MUNDO (`WeatherSystem`), NO de un fbm(u_time) local.
        // Aquí había `weather = fbm(vec2(u_time*0.008, 3.7))`, una segunda opinión sobre el tiempo
        // que no tenía nada que ver con la que usaba la CPU para decidir la lluvia → nubarrones sin
        // gota y lluvia con cielo azul. El shader ya NO decide CUÁNTA nube hay: eso lo dice el frente
        // que está pasando por encima. Lo que el shader sigue decidiendo —y hace bien— es la FORMA.
        float cloudiness = clamp(u_weather.w, 0.0, 1.0)
                         * smoothstep(0.16, 0.72, mass + 0.15 + 0.5 * rain);   // con lluvia cubre hasta lo despejado

        float dens = cloudField(base, wind);
        float lo   = mix(0.70, 0.32, cloudiness);                        // umbral: más cobertura = más nube
        float cov  = smoothstep(lo, lo + 0.26, dens);
        cov = cov * cov * (3.0 - 2.0 * cov);                             // remap "puffy"

        float band  = smoothstep(0.03, 0.30, t) * (1.0 - smoothstep(0.55, 0.99, t)); // ni horizonte ni cénit
        float cloud = cov * band * day * u_atmo;

        // Iluminación VOLUMÉTRICA (Beer): acumula densidad en 3 pasos HACIA el sol (proyectado al plano) →
        // gradiente fuerte luz→sombra (base oscura, cara al sol brillante) = el "pop" 3D de cine.
        vec3  sunT = normalize(u_sunDir - dot(u_sunDir, u_up) * u_up);
        vec2  ls   = vec2(dot(sunT, e1), dot(sunT, e2)) * 0.20;
        float sh   = 0.0;
        for (int i = 1; i <= 3; ++i) sh += smoothstep(lo, lo + 0.26, cloudField(base + ls * float(i), wind));
        float light = exp(-(sh / 3.0) * 1.9);                            // Beer-Lambert

        // SIN silver-lining/powder en las nubes (el usuario: el halo, en el cielo sí, en las nubes no).
        // Cima blanca-MATE (no 1.0 → no se sobreexpone), base fría en sombra. Con LLUVIA → nubes GRISES.
        vec3  cLit  = mix(vec3(0.80, 0.82, 0.86), u_sunColor, 0.10 + 0.18 * pow(sunAmt, 2.0));
        vec3  cShad = vec3(0.46, 0.52, 0.67);                            // base fría (sombra propia)
        // Nube de LLUVIA = gris plomo con contraste. Nube de NIEVE = gris CLARO y PLANO (la nevada
        // apaga el relieve del cielo: no hay claroscuro, todo es una lámina lechosa). Distinguirlas
        // es lo que hace que "va a nevar" se lea desde lejos, sin más información que el cielo.
        float snowy = u_wind.w;
        vec3  pLit  = mix(vec3(0.55, 0.57, 0.61), vec3(0.78, 0.79, 0.83), snowy);
        vec3  pShad = mix(vec3(0.30, 0.32, 0.36), vec3(0.63, 0.65, 0.70), snowy);
        cLit  = mix(cLit,  pLit,  rain);
        cShad = mix(cShad, pShad, rain);
        vec3  cloudCol = mix(cShad, cLit, light);
        sky = mix(sky, cloudCol, cloud * 0.92);
    }

    // disc solo para ATENUAR estrellas cerca del Sol (el cuerpo del Sol lo dibuja la corona-objeto).
    float disc = smoothstep(0.9994, 0.9998, dot(dir, u_sunDir));

    // --- Estrellas (noche + espacio) ---
    float starVis = (1.0 - day) * (1.0 - 0.6 * disc); // ocultas por el día y el sol
    if (starVis > 0.001) {
        vec3  cell = floor(dir * 350.0);
        float r    = hash13(cell);
        float star = smoothstep(0.9975, 1.0, r);       // pocas celdas encienden
        float tw   = 0.6 + 0.4 * sin(hash13(cell + 7.0) * 100.0); // titileo
        sky += vec3(star * tw * starVis);
    }

    // --- Mezcla a espacio negro según grosor de atmósfera ---
    vec3 spaceC = vec3(0.004, 0.004, 0.010);
    vec3 outC   = mix(spaceC, sky, u_atmo);
    // (El cuerpo del Sol = corona-objeto emisiva, se dibuja en el pase de objetos sobre este
    //  fondo, igual dentro y fuera de la atmósfera → sin "dos soles".)

    FragColor = vec4(outC, 1.0);
}
