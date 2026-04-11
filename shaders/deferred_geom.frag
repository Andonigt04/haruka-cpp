#version 460 core
layout (location = 0) out vec4 gPosition;
layout (location = 1) out vec4 gNormal;
layout (location = 2) out vec4 gAlbedoSpec;
layout (location = 3) out vec4 gEmissive;

in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoord;

uniform sampler2D texture_diffuse1;
uniform sampler2D texture_specular1;
uniform sampler2D texture_emissive1;

void main()
{
    gPosition = vec4(FragPos, 1.0);
    gNormal = vec4(normalize(Normal), 1.0);

    // Albedo - con fallback a gris
    vec3 albedo = texture(texture_diffuse1, TexCoord).rgb;
    if(length(albedo) < 0.01) albedo = vec3(0.7);

    // Specular - con fallback a 0.5
    vec4 specTex = texture(texture_specular1, TexCoord);
    float spec = (length(specTex.rgb) > 0.01) ? specTex.r : 0.5;

    gAlbedoSpec = vec4(albedo, spec);
    
    // Emissive - con fallback a negro
    vec3 emissive = texture(texture_emissive1, TexCoord).rgb;
    gEmissive = vec4(emissive, 1.0);
}