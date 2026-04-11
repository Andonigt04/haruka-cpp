#version 460 core
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D scene;
uniform sampler2D bloom;
uniform float bloomStrength;

void main()
{
    vec3 sceneColor = texture(scene, TexCoords).rgb;
    vec3 bloomColor = texture(bloom, TexCoords).rgb;
    
    vec3 result = sceneColor + bloomColor * bloomStrength;
    
    // Tone mapping final
    result = result / (result + vec3(1.0));
    result = pow(result, vec3(1.0/2.2));
    
    FragColor = vec4(result, 1.0);
}