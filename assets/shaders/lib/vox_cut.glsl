/**
 * @file lib/vox_cut.glsl
 * @brief The vox terrain-cut window lookup, ONE copy for terrain, water and grass.
 *
 * The cut texture can be an ATLAS: the coarse window (±320 m, 4 m texels) in its lower-left corner
 * and the FINE window (±16 m, 10 cm texels) of the decimetre holes to its right, full height. The
 * fine window is read within `info.z` metres of the eye, and only where its ALPHA says it was
 * evaluated (under fine bricks that are already uploaded: a hole is never cut before its wall is
 * drawn); everywhere else the coarse window answers.
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
            const vec4 fine = textureLod(tex, vec2(atlas.z + atlas.w * f.x, f.y), 0.0);
            if (fine.a > 0.5) return fine;   // evaluated here; else the coarse window (below)
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

/** @brief The WATER BED (m under the ground) in a cut-window sample: the fine window stores it in
 *  tenths of a metre and says so in B; the coarse one (B = 0) in whole metres. */
float harukaVoxCutBedM(vec4 cut) {
    return cut.b > 0.5 ? cut.g * 25.5 : cut.g * 255.0;
}
