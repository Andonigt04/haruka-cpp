#version 450 core

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D uTexture;

layout(std140, binding = 0) uniform PanelTransform {
    mat4  model;
    mat4  view;
    mat4  proj;
    float focused;
    float _pad0, _pad1, _pad2;
};

void main() {
    vec4 col = texture(uTexture, vUV);

    if (focused > 0.5) {
        float bw  = 0.008;
        float rim = 0.0;
        if (vUV.x < bw || vUV.x > 1.0 - bw || vUV.y < bw || vUV.y > 1.0 - bw)
            rim = 1.0;
        col.rgb = mix(col.rgb, vec3(0.4, 0.8, 1.0), rim * 0.7);
    }

    fragColor = col;
}
