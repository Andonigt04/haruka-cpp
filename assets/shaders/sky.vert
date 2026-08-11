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

// UBO compartido con sky.frag (binding 5) — antes eran uniforms sueltos (glUniformMatrix4fv/
// glUniform3fv/1f). Los glUniform* NO existen en Vulkan → van por UBO (ruta PSO/RHI). El bloque
// debe declararse IDÉNTICO en ambas etapas. Truco std140: un vec3 (align 16, size 12) deja 4 B
// libres → el float que le sigue entra en el mismo slot de 16 B (por eso van emparejados).
layout(std140, binding = 5) uniform SkyParams {
    mat4  u_invViewProjRot; // inv(proj * mat4(mat3(view)))
    vec3  u_sunDir;         // hacia el Sol (mundo, normalizado)
    float u_sunElev;        // dot(sunDir, up): elevación del sol
    vec3  u_up;             // cénit local del observador (mundo)
    float u_atmo;           // 1=superficie ... 0=espacio
    vec3  u_sunColor;       // color de la luz solar
    float u_time;           // segundos (movimiento de nubes)
    vec4  u_weather;        // x=humedad · y=tempC · z=precipitación · w=COBERTURA de nube (del mundo)
    vec4  u_wind;           // x=este(m/s) · y=norte(m/s) · z=racha · w=1 si es NIEVE
    // ⚠️ AUNQUE EL VERTEX NO LO USE. GLSL exige que el bloque sea IDÉNTICO en las dos etapas del
    // mismo programa; declararlo solo en el fragment da `struct type mismatch between shaders` al
    // enlazar y el cielo entero deja de dibujarse.
    vec4  u_planet;         // x=radio(m) · y=altitud del ojo(m) · z=base de nube(m)
};

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
