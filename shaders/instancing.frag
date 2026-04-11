#version 460 core

in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoord;
in vec4 InstanceColor;

out vec4 FragColor;

uniform sampler2D texture_diffuse1;
uniform vec3 viewPos;
uniform vec3 lightPos;
uniform vec3 lightColor;

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
