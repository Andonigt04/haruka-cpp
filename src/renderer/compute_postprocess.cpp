#include "compute_postprocess.h"
#include <glm/glm.hpp>
#include <iostream>

ComputePostProcess::ComputePostProcess() {}

ComputePostProcess::~ComputePostProcess() {
    if (bloomShader) glDeleteProgram(bloomShader);
    if (toneMappingShader) glDeleteProgram(toneMappingShader);
    if (colorGradingShader) glDeleteProgram(colorGradingShader);
}

GLuint ComputePostProcess::compileComputeShader(const std::string& source) {
    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    int success;
    char infoLog[512];
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        std::cerr << "Compute shader compilation failed: " << infoLog << "\n";
        glDeleteShader(shader);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, shader);
    glLinkProgram(program);

    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(program, 512, nullptr, infoLog);
        std::cerr << "Shader program linking failed: " << infoLog << "\n";
        glDeleteProgram(program);
        glDeleteShader(shader);
        return 0;
    }

    glDeleteShader(shader);
    return program;
}

void ComputePostProcess::init(int width, int height) {
    screenWidth = width;
    screenHeight = height;

    // Compilar bloom shader
    const char* bloomSource = R"(
#version 460 core
layout(local_size_x = 8, local_size_y = 8) in;

layout(rgba16f, binding = 0) uniform image2D inputImg;
layout(rgba16f, binding = 1) uniform image2D outputImg;

uniform float bloomThreshold = 1.0;
uniform float bloomStrength = 1.0;

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    
    vec4 color = imageLoad(inputImg, pixel);
    float brightness = dot(color.rgb, vec3(0.299, 0.587, 0.114));
    
    vec4 bloom = vec4(0.0);
    if (brightness > bloomThreshold) {
        bloom = color * bloomStrength;
    }
    
    imageStore(outputImg, pixel, bloom);
}
)";

    bloomShader = compileComputeShader(bloomSource);

    // Compilar tone mapping shader (ACES)
    const char* toneMappingSource = R"(
#version 460 core
layout(local_size_x = 8, local_size_y = 8) in;

layout(rgba16f, binding = 0) uniform image2D inputImg;
layout(rgba8, binding = 1) uniform image2D outputImg;

uniform float exposure = 1.0;
uniform int toneMode = 2;  // 2 = ACES

vec3 acesToneMapping(vec3 color) {
    const float A = 2.51;
    const float B = 0.03;
    const float C = 2.43;
    const float D = 0.59;
    const float E = 0.14;
    
    return clamp((color*(A*color+B))/(color*(C*color+D)+E), 0.0, 1.0);
}

vec3 reinhardToneMapping(vec3 color) {
    return color / (color + vec3(1.0));
}

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    
    vec4 hdrColor = imageLoad(inputImg, pixel);
    vec3 color = hdrColor.rgb * exposure;
    
    vec3 mapped;
    if (toneMode == 1) {
        mapped = reinhardToneMapping(color);
    } else {
        mapped = acesToneMapping(color);
    }
    
    vec4 ldrColor = vec4(mapped, hdrColor.a);
    imageStore(outputImg, pixel, ldrColor);
}
)";

    toneMappingShader = compileComputeShader(toneMappingSource);

    // Compilar color grading shader
    const char* colorGradingSource = R"(
#version 460 core
layout(local_size_x = 8, local_size_y = 8) in;

layout(rgba8, binding = 0) uniform image2D inputImg;
layout(rgba8, binding = 1) uniform image2D outputImg;

uniform float saturation = 1.0;
uniform float contrast = 1.0;
uniform float brightness = 0.0;

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    
    vec4 color = imageLoad(inputImg, pixel);
    vec3 rgb = color.rgb;
    
    // Brightness
    rgb += brightness;
    
    // Contrast
    rgb = (rgb - 0.5) * contrast + 0.5;
    
    // Saturation (desaturate and recombine)
    float gray = dot(rgb, vec3(0.299, 0.587, 0.114));
    rgb = mix(vec3(gray), rgb, saturation);
    
    imageStore(outputImg, pixel, vec4(rgb, color.a));
}
)";

    colorGradingShader = compileComputeShader(colorGradingSource);

    stats.dispatchWidth = (screenWidth + localGroupSize - 1) / localGroupSize;
    stats.dispatchHeight = (screenHeight + localGroupSize - 1) / localGroupSize;
    stats.localGroupSize = localGroupSize;
    stats.estimatedSpeedup = 1.2f;  // ~20% speedup

    std::cout << "✓ Compute Post-Processing initialized\n";
    std::cout << "  Resolution: " << screenWidth << "x" << screenHeight << "\n";
    std::cout << "  Dispatch: " << stats.dispatchWidth << "x" << stats.dispatchHeight << "\n";
}

void ComputePostProcess::dispatchCompute(GLuint shader, int width, int height) {
    glUseProgram(shader);
    
    int dispatchX = (width + localGroupSize - 1) / localGroupSize;
    int dispatchY = (height + localGroupSize - 1) / localGroupSize;
    
    glDispatchCompute(dispatchX, dispatchY, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void ComputePostProcess::bloomCompute(
    GLuint inputTexture,
    GLuint outputTexture,
    float threshold,
    float strength) {
    
    if (!bloomShader) return;

    glUseProgram(bloomShader);
    
    glBindImageTexture(0, inputTexture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
    glBindImageTexture(1, outputTexture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    
    glUniform1f(glGetUniformLocation(bloomShader, "bloomThreshold"), threshold);
    glUniform1f(glGetUniformLocation(bloomShader, "bloomStrength"), strength);
    
    dispatchCompute(bloomShader, screenWidth, screenHeight);
}

void ComputePostProcess::toneMappingCompute(
    GLuint inputTexture,
    GLuint outputTexture,
    float exposure,
    ToneMapMode mode) {
    
    if (!toneMappingShader) return;

    glUseProgram(toneMappingShader);
    
    glBindImageTexture(0, inputTexture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
    glBindImageTexture(1, outputTexture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    
    glUniform1f(glGetUniformLocation(toneMappingShader, "exposure"), exposure);
    glUniform1i(glGetUniformLocation(toneMappingShader, "toneMode"), mode);
    
    dispatchCompute(toneMappingShader, screenWidth, screenHeight);
}

void ComputePostProcess::colorGradingCompute(
    GLuint inputTexture,
    GLuint outputTexture,
    float saturation,
    float contrast,
    float brightness) {
    
    if (!colorGradingShader) return;

    glUseProgram(colorGradingShader);
    
    glBindImageTexture(0, inputTexture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    glBindImageTexture(1, outputTexture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    
    glUniform1f(glGetUniformLocation(colorGradingShader, "saturation"), saturation);
    glUniform1f(glGetUniformLocation(colorGradingShader, "contrast"), contrast);
    glUniform1f(glGetUniformLocation(colorGradingShader, "brightness"), brightness);
    
    dispatchCompute(colorGradingShader, screenWidth, screenHeight);
}

void ComputePostProcess::processAll(
    GLuint inputTexture,
    GLuint outputTexture,
    float exposure,
    float bloomThreshold,
    float bloomStrength,
    ToneMapMode toneMode,
    float saturation,
    float contrast,
    float brightness) {
    
    // Pipeline: Bloom -> Tone Mapping -> Color Grading
    bloomCompute(inputTexture, outputTexture, bloomThreshold, bloomStrength);
    toneMappingCompute(outputTexture, outputTexture, exposure, toneMode);
    colorGradingCompute(outputTexture, outputTexture, saturation, contrast, brightness);
}
