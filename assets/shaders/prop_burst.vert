/**
 * @file prop_burst.vert
 * @brief LA CAPA MASIVA: dibuja las matas de un BROTE reconstruyéndolas aquí (ver
 *        `World/Props/MassBurst.h`). Sin stream de instancia: cada brote visible es UN comando
 *        indirecto con `instanceCount` = sus matas y `firstInstance` = su índice en `uBursts`.
 *
 * Comparte fragment con los props (`prop_inst.frag`) y saca LAS MISMAS varyings: misma luz, misma
 * perspectiva aérea, mismo material del prototipo.
 *
 * ── QUÉ MATA SOY ─────────────────────────────────────────────────────────────────────────────────
 * El brote es `gl_BaseInstanceARB` (= firstInstance) en los dos backends. La mata `k` no: en GL
 * `gl_InstanceID` empieza en 0 en cada comando, pero en Vulkan `gl_InstanceIndex` (al que el backend
 * reescribe `gl_InstanceID`) YA lleva sumado firstInstance. El backend no define nada que permita
 * distinguirlos en el preprocesador, así que lo dice el UBO (`u_flags.x`).
 *
 * ── CLIMA ────────────────────────────────────────────────────────────────────────────────────────
 * Viento: `harukaPropSway`, el mismo que los props. PESO: la nieve acumulada y el agua aplastan la
 * mata (se acorta y se vence hacia su lado) y la nieve la blanquea desde la punta. Pisada: el estado
 * `Flat` la tumba entera. Destruida: colapsa a un punto (área cero, sin fragmentos).
 */
#version 460 core
#extension GL_GOOGLE_include_directive : require
#extension GL_ARB_shader_draw_parameters : require

layout(location = 0)  in vec3 aPos;
layout(location = 1)  in vec4 aNormal;     // RGB10A2_SNORM
layout(location = 2)  in vec4 aColorPk;    // RGBA8: rgb × (a·4)
layout(location = 9)  in vec2 aUv;

#include "lib/frame_ubo.glsl"     // PerFrameData (binding 0)
#include "lib/prop_params.glsl"   // PropParams (binding 6)
#include "lib/prop_sway.glsl"
#include "lib/mass_burst.glsl"

layout(std430, binding = 2) readonly buffer MassBursts { HarukaMassBurst uBursts[]; };
layout(std430, binding = 3) readonly buffer MassState  { uint uMassState[]; };

layout(location = 0) out vec3 Normal;
layout(location = 1) out vec3 FragPos;
layout(location = 2) out vec3 Color;
layout(location = 3) out vec2 TexCoord;

void main() {
    const uint bi = uint(gl_BaseInstanceARB);
    const uint k  = uint(gl_InstanceID) - (u_flags.x > 0.5 ? bi : 0u);
    const HarukaMassBurst B = uBursts[bi];
    const vec3 center = B.a.xyz;

    // Estado de ESTA mata (2 bits). Solo los brotes tocados tienen palabras.
    uint st = 0u;
    if (B.c.z != HARUKA_MASS_NO_STATE) {
        const uint bit = k * 2u;
        st = (uMassState[B.c.z + (bit >> 5)] >> (bit & 31u)) & 3u;
    }
    if (st == 2u) {   // destruida: colapsa a un punto (ver prop_inst.vert: NO se expulsa del NDC)
        gl_Position = projection * view * vec4(center + u_originRel.xyz, 1.0);
        Normal = vec3(0.0, 1.0, 0.0); FragPos = center + u_originRel.xyz; Color = vec3(0.0); TexCoord = vec2(0.0);
        return;
    }

    const HarukaMassElement e = harukaMassElement(B.c.x, k, uintBitsToFloat(B.c.w), B.a.w, B.b.w);
    const vec3 up = B.b.xyz;
    vec3 u, v;
    harukaMassTangents(up, u, v);
    // Base de la mata: X girada por su yaw, Y = vertical local, Z = X × Y (mano derecha, como el
    // prototipo).
    const vec3 X = u * cos(e.yaw) + v * sin(e.yaw);
    const vec3 Z = cross(X, up);
    const vec3 root = center + u * e.offset.x + v * e.offset.y;

    // PESO y PISADA: se acorta y se vence hacia su +X (que el yaw ya hace aleatorio por mata).
    const float h = clamp(aPos.y, 0.0, 4.0);
    const float weight = clamp(u_weather.x * 0.75 + u_weather.y * 0.25, 0.0, 1.0);
    const float bend = (st == 1u) ? 0.85 : weight * 0.55;
    vec3 p = aPos * e.scale;
    p.y *= 1.0 - 0.75 * bend;
    p.x += h * e.scale * bend * 0.9;

    vec3 world = root + X * p.x + up * p.y + Z * p.z;
    // VIENTO (en el mundo): menos cuanto más vencida está — la mata tumbada no ondea.
    const float ph = root.x * 0.15 + root.z * 0.15 + float(k) * 0.37;
    world += harukaPropSway(u_wind, u_time, ph, h * e.scale) * (1.0 - bend);

    const vec3 aColor = aColorPk.rgb * (aColorPk.a * 4.0);
    vec3 col = aColor * e.tint;
    // NIEVE: blanquea desde la punta, más cuanta más haya acumulada.
    col = mix(col, vec3(0.92, 0.94, 0.97), u_weather.x * smoothstep(0.0, 0.35, h) * 0.7);

    const vec3 camRel = world + u_originRel.xyz;
    FragPos  = camRel;
    Normal   = normalize(X * aNormal.x + up * aNormal.y + Z * aNormal.z);   // escala uniforme
    Color    = col;
    TexCoord = aUv;
    gl_Position = projection * view * vec4(camRel, 1.0);
}
