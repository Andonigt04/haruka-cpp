#include "gpu_instancing.h"
#include <glm/gtc/matrix_transform.hpp>

GPUInstancing::GPUInstancing(PrecisionMode mode) : precisionMode(mode) {}

GPUInstancing::~GPUInstancing() {
    if (instanceVBO) glDeleteBuffers(1, &instanceVBO);
    if (instanceVAO) glDeleteVertexArrays(1, &instanceVAO);
}

void GPUInstancing::init(int maxInst) {
    maxInstances = maxInst;
    instancesDouble.reserve(maxInstances);
    instancesFloat.reserve(maxInstances);
    setupInstanceBuffer();
}

glm::dmat4 GPUInstancing::createModelMatrixDouble(
    const glm::dvec3& pos,
    const glm::vec3& scale,
    const glm::dvec3& rotation) const {
    
    glm::dmat4 model = glm::dmat4(1.0);
    
    // Translación (double precision)
    model = glm::translate(model, pos);
    
    // Rotación (Euler angles)
    model = glm::rotate(model, rotation.x, glm::dvec3(1.0, 0.0, 0.0));
    model = glm::rotate(model, rotation.y, glm::dvec3(0.0, 1.0, 0.0));
    model = glm::rotate(model, rotation.z, glm::dvec3(0.0, 0.0, 1.0));
    
    // Escala
    model = glm::scale(model, glm::dvec3(scale));
    
    return model;
}

glm::mat4 GPUInstancing::createModelMatrixFloat(
    const glm::vec3& pos,
    const glm::vec3& scale,
    const glm::vec3& rotation) const {
    
    glm::mat4 model = glm::mat4(1.0f);
    
    // Translación
    model = glm::translate(model, pos);
    
    // Rotación (Euler angles)
    model = glm::rotate(model, rotation.x, glm::vec3(1.0f, 0.0f, 0.0f));
    model = glm::rotate(model, rotation.y, glm::vec3(0.0f, 1.0f, 0.0f));
    model = glm::rotate(model, rotation.z, glm::vec3(0.0f, 0.0f, 1.0f));
    
    // Escala
    model = glm::scale(model, scale);
    
    return model;
}

void GPUInstancing::addInstanceDouble(
    const glm::dvec3& position,
    const glm::vec3& scale,
    const glm::vec4& color,
    const glm::dvec3& rotation) {
    
    if (instancesDouble.size() >= maxInstances) {
        return;  // Buffer lleno
    }
    
    InstanceDataDouble instance;
    instance.model = createModelMatrixDouble(position, scale, rotation);
    instance.position = position;
    instance.color = color;
    instance.scale = scale;
    
    instancesDouble.push_back(instance);
    bufferDirty = true;
}

void GPUInstancing::addInstanceFloat(
    const glm::vec3& position,
    const glm::vec3& scale,
    const glm::vec4& color,
    const glm::vec3& rotation) {
    
    if (instancesFloat.size() >= maxInstances) {
        return;  // Buffer lleno
    }
    
    InstanceDataFloat instance;
    instance.model = createModelMatrixFloat(position, scale, rotation);
    instance.color = color;
    instance.scale = scale;
    
    instancesFloat.push_back(instance);
    bufferDirty = true;
}

void GPUInstancing::setupInstanceBuffer() {
    if (instanceVBO == 0) glGenBuffers(1, &instanceVBO);

    glBindBuffer(GL_ARRAY_BUFFER, instanceVBO);
    glBufferData(GL_ARRAY_BUFFER,
                 maxInstances * sizeof(InstanceDataFloat),
                 nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void GPUInstancing::updateBuffer() {
    if (!bufferDirty || instancesFloat.empty()) return;

    glBindBuffer(GL_ARRAY_BUFFER, instanceVBO);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    instancesFloat.size() * sizeof(InstanceDataFloat),
                    instancesFloat.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    bufferDirty = false;
}

void GPUInstancing::render(GLuint VAO, GLuint indexCount) {
    int instanceCount = (int)instancesFloat.size();
    if (instanceCount == 0) return;

    updateBuffer();

    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, instanceVBO);

    // mat4 model: 4 consecutive vec4 columns at locations 3-6
    constexpr GLsizei stride = sizeof(InstanceDataFloat);
    for (int col = 0; col < 4; ++col) {
        GLuint loc = 3 + col;
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, stride,
                              (void*)(col * 16));
        glVertexAttribDivisor(loc, 1);
    }
    // vec4 color at location 7
    glEnableVertexAttribArray(7);
    glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, stride,
                          (void*)offsetof(InstanceDataFloat, color));
    glVertexAttribDivisor(7, 1);
    // vec3 scale at location 8
    glEnableVertexAttribArray(8);
    glVertexAttribPointer(8, 3, GL_FLOAT, GL_FALSE, stride,
                          (void*)offsetof(InstanceDataFloat, scale));
    glVertexAttribDivisor(8, 1);

    glDrawElementsInstanced(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, 0, instanceCount);

    // Reset divisors so non-instanced draws of this VAO aren't affected
    for (int col = 0; col < 4; ++col) glVertexAttribDivisor(3 + col, 0);
    glVertexAttribDivisor(7, 0);
    glVertexAttribDivisor(8, 0);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

void GPUInstancing::clear() {
    instancesDouble.clear();
    instancesFloat.clear();
    bufferDirty = true;
}
