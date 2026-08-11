#version 460 core
/** @file debug_lines.frag  @brief Color plano para el alambre de depuración. */
layout(std140, binding = 0) uniform DebugLinesUBO {
    mat4 uViewProjRotOnly;
    vec4 uColor;
};
layout(location = 0) out vec4 fragColor;
void main() { fragColor = uColor; }
