/**
 * @file sky.vert
 * @brief Cielo atmosférico de pantalla completa (un solo triángulo).
 *
 * No usa VBO: genera el triángulo que cubre el viewport a partir de
 * gl_VertexID (0,1,2). Reconstruye el rayo de vista en MUNDO usando la
 * inversa de (proyección * vista-solo-rotación) para que el frag pinte el
 * gradiente cénit→horizonte, el disco solar y las estrellas por dirección.
 */
#version 450 core

layout(location = 0) uniform mat4 u_invViewProjRot; // inv(proj * mat4(mat3(view)))

layout(location = 0) out vec3 vRayDir; // dirección de vista en espacio mundo

void main()
{
    // Triángulo fullscreen: NDC (-1,-1),(3,-1),(-1,3) cubre todo el clip.
    vec2 ndc = vec2((gl_VertexID == 1) ? 3.0 : -1.0,
                    (gl_VertexID == 2) ? 3.0 : -1.0);
    gl_Position = vec4(ndc, 1.0, 1.0); // z=1 → plano lejano (detrás de todo)

    // Rayo de mundo: punto en el plano lejano reproyectado sin traslación.
    vec4 far = u_invViewProjRot * vec4(ndc, 1.0, 1.0);
    vRayDir = far.xyz / far.w;
}
