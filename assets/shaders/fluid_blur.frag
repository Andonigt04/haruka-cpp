/**
 * @file fluid_blur.frag
 * @brief Screen-space fluid — separable bilateral (edge-preserving) blur of the
 *        eye-depth target. Smooths the bumpy per-particle spheres into a single
 *        surface while keeping the silhouette crisp (depth-difference weighting).
 *        Run once horizontally then once vertically.
 *
 * Empty pixels (no fluid) are stored as 0 and ignored so the surface edge stays
 * sharp instead of bleeding into the background.
 */
#version 450 core

layout(location = 0) out float FragDepth;
layout(location = 0) in  vec2 TexCoords;

layout(binding = 0) uniform sampler2D u_depth;
// Mismo bloque (binding 8) que el resto de shaders del fluido. Ver fluid_particle.vert.
layout(std140, binding = 8) uniform FluidParams {
    vec2  u_blurDir;        // (1/w,0) o (0,1/h)
    vec2  u_texel;
    float u_depthFalloff;   // m; menor = bordes más nítidos
    float u_refractScale;
    float u_radius;
    float u_viewportH;
};
#define u_dir u_blurDir

void main() {
    float c = texture(u_depth, TexCoords).r;
    if (c <= 0.0) { FragDepth = 0.0; return; } // no fluid here

    float sum = 0.0, wsum = 0.0;
    const int R = 6;
    for (int i = -R; i <= R; ++i) {
        vec2 uv = TexCoords + u_dir * float(i);
        float s = texture(u_depth, uv).r;
        if (s <= 0.0) continue;                 // skip empty
        float spatial = exp(-float(i * i) / (2.0 * float(R) * float(R) / 4.0));
        float dz = (s - c) / max(u_depthFalloff, 1e-4);
        float range = exp(-dz * dz);            // bilateral range term
        float w = spatial * range;
        sum  += s * w;
        wsum += w;
    }
    FragDepth = (wsum > 0.0) ? (sum / wsum) : c;
}
