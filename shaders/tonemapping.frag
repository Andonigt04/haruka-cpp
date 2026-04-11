#version 460 core
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D scene;
uniform sampler2D bloom;
uniform float exposure;
uniform float bloomStrength;

void main()
{
    vec3 hdrColor = texture(scene, TexCoords).rgb;
    vec3 bloomColor = texture(bloom, TexCoords).rgb;
    
    // Tone mapping
    vec3 mapped = vec3(1.0) - exp(-hdrColor * exposure);

    // Bloom
    mapped += bloomColor * bloomStrength;

    // Gamma correction
    mapped = pow(mapped, vec3(1.0 / 2.2));

    FragColor = vec4(mapped, 1.0);
}