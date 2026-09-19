/**
 * @file cloud_vol.vert
 * @brief Triángulo de pantalla completa del pase VOLUMÉTRICO de nubes.
 *
 * A diferencia de `sky.vert`, este pase NO es fondo: se dibuja DESPUÉS de la escena y necesita
 * saber a qué distancia está la geometría para no pintar nube por delante de una montaña. Por eso
 * el rayo se reconstruye igual, pero el fragment además lee la profundidad de la escena.
 */
#version 450 core

layout(std140, binding = 5) uniform CloudParams {
    // Una sola inversa, la de ROTACIÓN: la escena se dibuja cámara-relativa (`view` sin traslación),
    // así que es a la vez el rayo de vista y lo que deshace la profundidad. Ver `cloud_vol.frag`.
    mat4  u_invViewProjRot;
    vec4  u_planetC;         // xyz = centro del planeta RELATIVO a la cámara (m) · w = radio (m)
    vec4  u_slab;            // x = base(m) · y = techo(m) · z = cobertura[0,1] · w = precipitación
    vec4  u_sun;             // xyz = hacia el Sol · w = elevación (dot con el cénit)
    vec4  u_sunColor;        // rgb = color del sol · a = día[0,1]
    vec4  u_wind;            // xy = deriva del campo · z = tiempo(s) · w = altitud del ojo (m)
    vec4  u_misc;            // x = atmósfera[0,1] · y = pasos · z = escala del campo · w = extinción
    // ⚠️ MISMO BLOQUE QUE EN `cloud_vol.frag`, ENTERO. OpenGL enlaza vert+frag y un bloque con el
    // mismo `binding` y distinta definición es un error de link: al crecer el UBO con los embudos
    // solo se tocó el frag y en GL el pase de nubes dejó de existir (el banco: "pipeline creado"
    // en FAIL). Vulkan no enlaza etapas entre sí y no avisó.
    vec4  u_vortexPos[4];
    vec4  u_vortexInfo[4];
    vec4  u_vortexN;
    vec4  u_aerial;    // (solo lo usa el fragmento; el bloque tiene que coincidir con el .frag)
    vec4  u_blend;
};

layout(location = 0) out vec3 vRayDir;
layout(location = 1) out vec2 vNdc;

void main()
{
    vec2 ndc = vec2((gl_VertexID == 1) ? 3.0 : -1.0,
                    (gl_VertexID == 2) ? 3.0 : -1.0);
    gl_Position = vec4(ndc, 1.0, 1.0);
    vNdc = ndc;

    vec4 far = u_invViewProjRot * vec4(ndc, 1.0, 1.0);
    vRayDir = far.xyz / far.w;
}
