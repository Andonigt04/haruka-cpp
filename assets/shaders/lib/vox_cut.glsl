/**
 * @file lib/vox_cut.glsl
 * @brief The vox terrain-cut window lookup, ONE copy for terrain, water and grass.
 *
 * The cut texture can be an ATLAS: the coarse window (±320 m, 4 m texels) in its lower-left corner
 * and the FINE window (±16 m, 10 cm texels) of the decimetre holes to its right, full height. The
 * fine window is read within `info.z` metres of the eye — the radius in which the fine wall bricks
 * are all uploaded, the same sphere `vox.frag` uses to hand over from the 4 m walls to the fine ones —
 * so a hole is never cut where its wall is not drawn yet.
 *
 * info:  x = window valid · y = debug mode (terrain) · z = fine radius (m, 0 = none) · w = unused
 * atlas: x, y = coarse window size / atlas size · z = fine window's x offset / atlas width ·
 *        w = fine window width / atlas width (it is atlas-high). atlas.x == 0: the whole texture is
 *        the coarse window (the layout before the atlas).
 * `textureLod` because vertex and compute stages have no derivatives (the texture has no mips).
 */
vec4 harukaVoxCutLookup(sampler2D tex, mat4 coarseSpace, mat4 fineSpace, vec4 info, vec4 atlas, vec3 posRelEye,
                        bool allowFine, out bool inside) {
    const vec2 ts = vec2(textureSize(tex, 0));
    if (atlas.x > 0.0 && allowFine && length(posRelEye) < info.z) {
        vec2 f = (fineSpace * vec4(posRelEye, 1.0)).xy;
        if (f.x > 0.0 && f.x < 1.0 && f.y > 0.0 && f.y < 1.0) {
            inside = true;
            const vec2 half_ = 0.5 / vec2(atlas.w * ts.x, ts.y);   // half a texel of the fine window, in its uv
            f = clamp(f, half_, vec2(1.0) - half_);
            return textureLod(tex, vec2(atlas.z + atlas.w * f.x, f.y), 0.0);
        }
    }
    vec2 c = (coarseSpace * vec4(posRelEye, 1.0)).xy;
    inside = c.x > 0.0 && c.x < 1.0 && c.y > 0.0 && c.y < 1.0;
    if (!inside) return vec4(0.0);
    if (atlas.x > 0.0) {
        const vec2 half_ = 0.5 / (atlas.xy * ts);   // keep bilinear inside the coarse window
        c = clamp(c, half_, vec2(1.0) - half_) * atlas.xy;
    }
    return textureLod(tex, c, 0.0);
}
