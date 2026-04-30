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
    // Crear VAO si no existe
    if (instanceVAO == 0) {
        glGenVertexArrays(1, &instanceVAO);
    }
    
    // Crear VBO para instancias
    if (instanceVBO == 0) {
        glGenBuffers(1, &instanceVBO);
    }
    
    glBindVertexArray(instanceVAO);
    glBindBuffer(GL_COPY_WRITE_BUFFER, instanceVBO);
    
    // Calcular tamaño correcto basado en precision mode
    size_t elementSize = (precisionMode == PRECISION_DOUBLE) ? 
                         sizeof(InstanceDataDouble) : 
                         sizeof(InstanceDataFloat);
    
    glBufferData(GL_COPY_WRITE_BUFFER, 
                 maxInstances * elementSize, 
                 nullptr, 
                 GL_DYNAMIC_DRAW);
    
    // Para SSBO (Shader Storage Buffer Object), no necesitamos vertex attributes
    // El shader accederá directamente al buffer
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, instanceVBO);
    
    glBindVertexArray(0);
}

void GPUInstancing::updateBuffer() {
    if (!bufferDirty) return;
    
    glBindBuffer(GL_COPY_WRITE_BUFFER, instanceVBO);
    
    if (precisionMode == PRECISION_DOUBLE && !instancesDouble.empty()) {
        glBufferSubData(GL_COPY_WRITE_BUFFER, 
                        0,
                        instancesDouble.size() * sizeof(InstanceDataDouble),
                        instancesDouble.data());
    } else if (precisionMode == PRECISION_FLOAT && !instancesFloat.empty()) {
        glBufferSubData(GL_COPY_WRITE_BUFFER, 
                        0,
                        instancesFloat.size() * sizeof(InstanceDataFloat),
                        instancesFloat.data());
    }
    
    bufferDirty = false;
}

void GPUInstancing::render(GLuint VAO, GLuint indexCount) {
    int instanceCount = 0;
    
    if (precisionMode == PRECISION_DOUBLE) {
        instanceCount = instancesDouble.size();
    } else {
        instanceCount = instancesFloat.size();
    }
    
    if (instanceCount == 0) return;
    
    updateBuffer();
    
    // Bind tanto el mesh VAO como el instancing VBO
    glBindVertexArray(VAO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, instanceVBO);
    
    // Renderizar con instancing
    glDrawElementsInstanced(GL_TRIANGLES, 
                           indexCount, 
                           GL_UNSIGNED_INT, 
                           0, 
                           instanceCount);
    
    glBindVertexArray(0);
}

void GPUInstancing::clear() {
    instancesDouble.clear();
    instancesFloat.clear();
    bufferDirty = true;
}
