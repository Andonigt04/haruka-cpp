/**
 * @file shadow.frag
 * @brief Depth-only pass for the directional shadow map.
 *
 * Empty body — depth is written automatically by the fixed-function
 * depth test. Color output is intentionally suppressed (no color attachment
 * on the shadow FBO). Paired with shadow.vert.
 */
#version 450 core

void main()
{
}