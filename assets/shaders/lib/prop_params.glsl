/**
 * @file prop_params.glsl
 * @brief El UBO del pase de props (binding 6), compartido por `prop_inst.vert/.frag` y
 *        `prop_burst.vert`. Gemelo de `PropParams` en PropSystem.cpp (std140, 96 B): un campo de más
 *        o de menos no da error de compilación, desplaza todo lo que viene detrás.
 */
#ifndef HARUKA_PROP_PARAMS_GLSL
#define HARUKA_PROP_PARAMS_GLSL

layout(std140, binding = 6) uniform PropParams {
    vec3  u_wind;   float u_time;      // viento del clima (mundo, m/s) · segundos
    vec4  u_matPBR;                    // x=metallic y=roughness z=ao w=máscara de texturas (bits)
    // ORIGEN del scatter relativo a la cámara. Las instancias van RELATIVAS AL ORIGEN (fijo hasta el
    // siguiente scatter), no a la cámara: así se construyen una vez y no por frame.
    vec4  u_originRel;
    vec4  u_aerial;     // perspectiva aérea: x = 1/L · y = día (lib/aerial.glsl)
    // CLIMA que pesa sobre la vegetación: x = nieve acumulada · y = suelo mojado · z = lluvia ·
    // w = nevada (todo 0..1, integrado por SkyPass).
    vec4  u_weather;
    // x = 1 si el backend es Vulkan (gl_InstanceIndex YA incluye firstInstance; en GL no) · yzw libres.
    vec4  u_flags;
};

#endif // HARUKA_PROP_PARAMS_GLSL
