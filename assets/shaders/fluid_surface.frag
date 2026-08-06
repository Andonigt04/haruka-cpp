/**
 * @file fluid_surface.frag
 * @brief Screen-space fluid — surface composite (v2). From the smoothed eye-depth
 *        it reconstructs view position + normal, then shades a water surface that
 *        REFRACTS the scene behind it (sampled from a copy of the scene colour)
 *        and adds a fresnel reflection + sun specular. Opaque output — the scene
 *        shows through via the refraction sample, so no alpha blend is needed.
 *
 * Scene-depth occlusion is handled in the depth pass (the particle depth buffer
 * is seeded with the scene depth), so this pass only runs where fluid is in front.
 */
#version 450 core

layout(location = 0) out vec4 FragColor;
layout(location = 0) in  vec2 TexCoords;

layout(binding = 0) uniform sampler2D u_depth;   // smoothed eye-depth (positive m)
layout(binding = 1) uniform sampler2D u_scene;   // copy of the scene colour behind

// Mismo bloque (binding 8) que el resto de shaders del fluido. Ver fluid_particle.vert.
layout(std140, binding = 8) uniform FluidParams {
    vec2  u_blurDir;
    vec2  u_texel;          // 1/resolución
    float u_depthFalloff;
    float u_refractScale;   // fuerza de refracción en unidades uv
    float u_radius;
    float u_viewportH;
};

layout(std140, binding = 0) uniform PerFrameData {
    mat4 view;
    mat4 projection;
    vec3 _cameraPos;     float _pad0;
    vec3 sunDirection;   float _pad1;
    vec3 sunLightColor;  float ambientStrength;
    int  enableHDR;
};

vec3 viewPosFromUV(vec2 uv, float d) {
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc.x * d / projection[0][0],
                ndc.y * d / projection[1][1],
                -d);
}

void main() {
    float dC = texture(u_depth, TexCoords).r;
    if (dC <= 0.0) discard;                       // no fluid here

    vec3 P = viewPosFromUV(TexCoords, dC);

    // Normal from neighbouring depths (smaller-difference to avoid silhouette spikes).
    float dR = texture(u_depth, TexCoords + vec2(u_texel.x, 0)).r;
    float dL = texture(u_depth, TexCoords - vec2(u_texel.x, 0)).r;
    float dU = texture(u_depth, TexCoords + vec2(0, u_texel.y)).r;
    float dD = texture(u_depth, TexCoords - vec2(0, u_texel.y)).r;
    vec3 ddx = (dR > 0.0) ? (viewPosFromUV(TexCoords + vec2(u_texel.x,0), dR) - P)
                          : (P - viewPosFromUV(TexCoords - vec2(u_texel.x,0), dL));
    vec3 ddy = (dU > 0.0) ? (viewPosFromUV(TexCoords + vec2(0,u_texel.y), dU) - P)
                          : (P - viewPosFromUV(TexCoords - vec2(0,u_texel.y), dD));
    vec3 N = normalize(cross(ddx, ddy));
    if (N.z < 0.0) N = -N;

    vec3 V  = normalize(-P);
    vec3 Lv = normalize(mat3(view) * sunDirection);
    vec3 H  = normalize(Lv + V);
    float spec = pow(max(dot(N, H), 0.0), 80.0);
    float fres = pow(1.0 - max(dot(N, V), 0.0), 5.0);

    // Refraction: distort the scene-behind sample by the surface normal.
    vec2 refrUV = clamp(TexCoords + N.xy * u_refractScale, vec2(0.0), vec2(1.0));
    vec3 behind = texture(u_scene, refrUV).rgb;

    // Water absorption: tint what's behind toward deep blue-green.
    vec3 deep = vec3(0.05, 0.20, 0.32);
    vec3 refracted = mix(behind, deep, 0.55);

    // Fresnel reflection (cheap: sky/sun tint), plus sun glints.
    vec3 reflectCol = sunLightColor * 0.6 + vec3(0.10, 0.20, 0.30);
    vec3 color = mix(refracted, reflectCol, clamp(fres, 0.0, 0.8))
               + sunLightColor * 0.7 * spec;

    FragColor = vec4(color, 1.0);
}
