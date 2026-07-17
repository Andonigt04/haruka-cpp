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
    float _padSky;
};

// Hash 3D barato para el campo de estrellas.
float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
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
