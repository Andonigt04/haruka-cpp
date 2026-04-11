#pragma once

#include <glm/glm.hpp>
#include <glm/ext.hpp>
#include <vector>
#include <memory>
#include <glad/glad.h>

/**
 * @brief Efficient GPU instancing helper with double-precision support.
 *
 * Supports:
 * - double precision path for astronomical-scale scenes
 * - single precision fallback
 * - SSBO-backed instance buffers
 */

/** @brief Double-precision instance payload. */
struct InstanceDataDouble {
    glm::dmat4 model;          // Matriz de transformación (double)
    glm::dvec3 position;       // Posición (double) 
    double _pad1;              // Padding
    glm::vec4 color;           // Color RGBA
    glm::vec3 scale;           // Escala
    float _pad2;               // Padding para alineación 256-byte
};

/** @brief Single-precision instance payload fallback. */
struct InstanceDataFloat {
    glm::mat4 model;           // Matriz de transformación
    glm::vec4 color;           // Color RGBA
    glm::vec3 scale;           // Escala
    float _padding;            // Padding para alineación
};

class GPUInstancing {
public:
    /** @brief Precision mode used by the instancer. */
    enum PrecisionMode {
        PRECISION_DOUBLE = 0,   // Para astronomía (double 64-bit)
        PRECISION_FLOAT = 1     // Para otras cosas (float 32-bit)
    };

    /** @brief Constructs instancing helper with chosen precision mode. */
    GPUInstancing(PrecisionMode mode = PRECISION_DOUBLE);
    ~GPUInstancing();

    /** @brief Allocates instance buffers for the given capacity. */
    void init(int maxInstances = 10000);

    /** @brief Adds one double-precision instance. */
    void addInstanceDouble(
        const glm::dvec3& position,
        const glm::vec3& scale = glm::vec3(1.0f),
        const glm::vec4& color = glm::vec4(1.0f),
        const glm::dvec3& rotation = glm::dvec3(0.0)
    );

    /** @brief Adds one single-precision instance. */
    void addInstanceFloat(
        const glm::vec3& position,
        const glm::vec3& scale = glm::vec3(1.0f),
        const glm::vec4& color = glm::vec4(1.0f),
        const glm::vec3& rotation = glm::vec3(0.0f)
    );

    /** @brief Uploads pending instance data to GPU buffers. */
    void updateBuffer();

    /** @brief Renders all queued instances with the provided base mesh. */
    void render(GLuint VAO, GLuint indexCount);

    /** @brief Clears all instance data and marks buffers dirty. */
    void clear();

    /** @brief Returns total queued instances. */
    int getInstanceCount() const { return instancesDouble.size() + instancesFloat.size(); }
    /** @brief Returns configured max instance capacity. */
    int getMaxInstances() const { return maxInstances; }
    /** @brief Returns a coarse count-based reduction factor. */
    float getReductionFactor() const {
        return static_cast<float>(getInstanceCount());
    }
    /** @brief Returns the active precision mode. */
    PrecisionMode getPrecisionMode() const { return precisionMode; }

private:
    PrecisionMode precisionMode;
    
    std::vector<InstanceDataDouble> instancesDouble;
    std::vector<InstanceDataFloat> instancesFloat;
    
    GLuint instanceVBO = 0;
    GLuint instanceVAO = 0;
    int maxInstances = 0;
    bool bufferDirty = false;

    /** @brief Builds a model matrix using double precision. */
    glm::dmat4 createModelMatrixDouble(
        const glm::dvec3& pos,
        const glm::vec3& scale,
        const glm::dvec3& rotation
    ) const;

    /** @brief Builds a model matrix using float precision. */
    glm::mat4 createModelMatrixFloat(
        const glm::vec3& pos,
        const glm::vec3& scale,
        const glm::vec3& rotation
    ) const;

    /** @brief Configures VAO/VBO layout for instanced rendering. */
    void setupInstanceBuffer();
};
