/**
 * @file compute_postprocess.h
 * @brief Compute-shader post-processing pipeline (bloom, tone mapping, color grading).
 */
#pragma once

#include <glm/glm.hpp>
#include <glad/glad.h>
#include <memory>
#include <string>

/**
 * @brief Compute-shader based post-processing pipeline.
 *
 * Supports bloom, tone mapping, and color grading passes using SSBO-backed compute workloads.
 */

class ComputePostProcess {
public:
    /** @brief Tone mapping operators supported by the pipeline. */
    enum ToneMapMode {
        TONE_LINEAR,
        TONE_REINHARD,
        TONE_ACES,
        TONE_FILMIC
    };

    /** @brief Constructs an uninitialized post-process pipeline. */
    ComputePostProcess();
    /** @brief Releases post-process GPU resources. */
    ~ComputePostProcess();

    /**
     * @brief Initializes the compute post-processing pipeline.
     * @param width Screen width.
     * @param height Screen height.
     */
    void init(int width, int height);

    /** @brief Applies bloom extraction/blur using compute shaders. */
    void bloomCompute(
        GLuint inputTexture,
        GLuint outputTexture,
        float threshold = 1.0f,
        float strength = 1.0f
    );

    /** @brief Applies tone mapping to an HDR input texture. */
    void toneMappingCompute(
        GLuint inputTexture,
        GLuint outputTexture,
        float exposure = 1.0f,
        ToneMapMode mode = TONE_ACES
    );

    /** @brief Applies color grading in-place to an RGBA16F texture. */
    void colorGradingCompute(
        GLuint texture,
        float saturation = 1.0f,
        float contrast = 1.0f,
        float brightness = 0.0f
    );

    /** @brief Runs the full post-processing chain. */
    void processAll(
        GLuint inputTexture,
        GLuint outputTexture,
        float exposure = 1.0f,
        float bloomThreshold = 1.0f,
        float bloomStrength = 1.0f,
        ToneMapMode toneMode = TONE_ACES,
        float saturation = 1.0f,
        float contrast = 1.0f,
        float brightness = 0.0f
    );

    /** @brief Compute pipeline statistics snapshot. */
    struct ComputeStats {
        int dispatchWidth;
        int dispatchHeight;
        int localGroupSize;
        float estimatedSpeedup;  // vs fragment shader
    };

    /** @brief Returns the current compute pipeline statistics. */
    ComputeStats getStats() const { return stats; }

private:
    GLuint bloomShader = 0;
    GLuint toneMappingShader = 0;
    GLuint colorGradingShader = 0;

    int screenWidth = 0;
    int screenHeight = 0;
    int localGroupSize = 8;  // 8x8 compute groups

    ComputeStats stats;

    /** @brief Compiles one compute shader source into a program. */
    GLuint compileComputeShader(const std::string& source);
    /** @brief Dispatches a compute shader over the given dimensions. */
    void dispatchCompute(GLuint shader, int width, int height);
};
