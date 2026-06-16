/**
 * @file post_present.frag
 * @brief Final present pass for the standalone post-processing stack.
 *
 * Samples the offscreen HDR scene target and writes it to the screen. This is
 * the upscale point for Render Scale (the scene texture may be smaller than the
 * window; linear filtering upscales it here) and the place where screen-space
 * post effects compose:
 *   - Bloom: add a blurred bright-pass texture (u_bloom != 0).
 *   - FXAA:  luma-based edge anti-aliasing on the resolved image (u_fxaa != 0).
 *
 * Kept look-preserving by default: with every toggle off it is a plain copy, so
 * routing the scene through the offscreen target does not change the image.
 */
#version 450 core

layout(location = 0) out vec4 FragColor;
layout(location = 0) in  vec2 TexCoords;

layout(binding = 0) uniform sampler2D u_scene;   // HDR scene color
layout(binding = 1) uniform sampler2D u_bloomTex; // blurred bright-pass

layout(location = 0) uniform int   u_fxaa;        // 0 = off
layout(location = 1) uniform vec2  u_texel;       // 1.0 / sceneResolution
layout(location = 2) uniform int   u_bloom;       // 0 = off
layout(location = 3) uniform float u_bloomStrength;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

// Compact luma-based FXAA (NVIDIA-style edge blend). Operates on resolved color.
vec3 fxaa(vec2 uv) {
    vec3 rgbM  = texture(u_scene, uv).rgb;
    vec3 rgbNW = texture(u_scene, uv + vec2(-u_texel.x, -u_texel.y)).rgb;
    vec3 rgbNE = texture(u_scene, uv + vec2( u_texel.x, -u_texel.y)).rgb;
    vec3 rgbSW = texture(u_scene, uv + vec2(-u_texel.x,  u_texel.y)).rgb;
    vec3 rgbSE = texture(u_scene, uv + vec2( u_texel.x,  u_texel.y)).rgb;

    float lM = luma(rgbM), lNW = luma(rgbNW), lNE = luma(rgbNE),
          lSW = luma(rgbSW), lSE = luma(rgbSE);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));

    // Skip flat regions — no edge to soften.
    if (lMax - lMin < 0.0625) return rgbM;

    vec2 dir;
    dir.x = -((lNW + lNE) - (lSW + lSE));
    dir.y =  ((lNW + lSW) - (lNE + lSE));
    float reduce = max((lNW + lNE + lSW + lSE) * 0.03125, 0.0078125);
    float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcpMin, vec2(-8.0), vec2(8.0)) * u_texel;

    vec3 a = 0.5 * (texture(u_scene, uv + dir * (1.0/3.0 - 0.5)).rgb +
                    texture(u_scene, uv + dir * (2.0/3.0 - 0.5)).rgb);
    vec3 b = a * 0.5 + 0.25 * (texture(u_scene, uv + dir * -0.5).rgb +
                               texture(u_scene, uv + dir *  0.5).rgb);
    float lB = luma(b);
    return (lB < lMin || lB > lMax) ? a : b;
}

void main() {
    vec3 color = (u_fxaa != 0) ? fxaa(TexCoords) : texture(u_scene, TexCoords).rgb;

    if (u_bloom != 0)
        color += texture(u_bloomTex, TexCoords).rgb * u_bloomStrength;

    FragColor = vec4(color, 1.0);
}
