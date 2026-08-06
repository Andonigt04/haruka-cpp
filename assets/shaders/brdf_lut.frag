/**
 * @file brdf_lut.frag
 * @brief Pre-integrates the GGX split-sum BRDF lookup table.
 *
 * Implements the split-sum approximation from Karis 2013 (Epic Games).
 * For each (NdotV, roughness) texel, 1024 importance-sampled GGX microfacet
 * directions are integrated to produce two scale/bias terms (A, B) stored in
 * RG16F. ibl.frag reads this LUT to reconstruct the specular reflectance.
 *
 * TexCoords.x → NdotV,  TexCoords.y → roughness
 * Out: FragColor.rg = (A, B) — Fresnel scale and bias
 */
#version 450 core

layout(location = 0) out vec2 FragColor;
layout(location = 0) in vec2 TexCoords;

const float PI = 3.14159265359;

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r*r) / 8.0;
    float nom = NdotV;
    float denom = NdotV * (1.0 - k) + k;
    return nom / denom;
}

float GeometrySmith(float NdotV, float NdotL, float roughness)
{
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

vec2 IntegrateBRDF(float NdotV, float roughness)
{
    const uint SAMPLE_COUNT = 1024u;
    vec3 V = vec3(sqrt(1.0 - NdotV*NdotV), 0.0, NdotV);

    float A = 0.0;
    float B = 0.0;

    for(uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        float Xi1 = float(i)/float(SAMPLE_COUNT);
        float Xi2 = fract(sin(float(i)*12.9898)*43758.5453);

        float phi = 2.0 * PI * Xi1;
        float cosTheta = sqrt((1.0 - Xi2) / (1.0 + (roughness*roughness - 1.0) * Xi2));
        float sinTheta = sqrt(1.0 - cosTheta*cosTheta);

        vec3 H = vec3(cos(phi)*sinTheta, sin(phi)*sinTheta, cosTheta);
        vec3 L = normalize(2.0 * dot(V,H) * H - V);

        float NdotL = max(L.z, 0.0);
        float NdotH = max(H.z, 0.0);
        float VdotH = max(dot(V,H), 0.0);

        if(NdotL > 0.0)
        {
            float G = GeometrySmith(NdotV, NdotL, roughness);
            float G_Vis = (G * VdotH) / (NdotH * NdotV + 0.0001);
            float Fc = pow(1.0 - VdotH, 5.0);

            A += (1.0 - Fc) * G_Vis;
            B += Fc * G_Vis;
        }
    }

    A /= float(SAMPLE_COUNT);
    B /= float(SAMPLE_COUNT);
    return vec2(A, B);
}

void main()
{
    vec2 integratedBRDF = IntegrateBRDF(TexCoords.x, TexCoords.y);
    FragColor = integratedBRDF;
}