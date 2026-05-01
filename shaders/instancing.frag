/**
 * @file instancing.frag
 * @brief Blinn-Phong shading for GPU-instanced objects.
 *
 * Applies a single point light with per-instance color tinting.
 * Alpha is taken from instanceColor.a to support transparent instances.
 *
 * In:  FragPos, Normal, TexCoord, InstanceColor (from instancing.vert)
 * Out: FragColor
 * Sampler: texture_diffuse1
 * UBO: Params { viewPos, lightPos, lightColor }
 */
#version 450 core

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 3) in vec4 InstanceColor;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 1) uniform Params {
    vec3 viewPos;
    vec3 lightPos;
    vec3 lightColor;
};

void main() {
    // Ambient
    float ambientStrength = 0.3;
    vec3 ambient = ambientStrength * InstanceColor.rgb;
    
    // Diffuse
    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(lightPos - FragPos);
    float diff = max(dot(norm, lightDir), 0.0);
    vec3 diffuse = diff * lightColor;
    
    // Specular
    float specularStrength = 0.5;
    vec3 viewDir = normalize(viewPos - FragPos);
    vec3 reflectDir = reflect(-lightDir, norm);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32.0);
    vec3 specular = specularStrength * spec * lightColor;
    
    vec3 result = (ambient + diffuse + specular) * InstanceColor.rgb;
    
    FragColor = vec4(result, InstanceColor.a);
}
