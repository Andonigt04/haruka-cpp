#include "renderer/gpu_instancing.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"

#include <glm/gtc/matrix_transform.hpp>
#include <cstddef>

namespace Haruka { namespace Renderer {

GPUInstancing::~GPUInstancing() {
    if (RHI::valid(m_buf))
        if (RHI::Device* dev = RHI::device()) dev->destroy(m_buf);
}

void GPUInstancing::init(int maxInstances) {
    m_maxInstances = maxInstances > 0 ? maxInstances : 1;
    m_instances.reserve((size_t)m_maxInstances);
    RHI::Device* dev = RHI::device();
    if (!dev) return;
    if (RHI::valid(m_buf)) dev->destroy(m_buf);
    // Vertex buffer (no SSBO): lo consume el ensamblador de vértices como binding por-instancia.
    m_buf = dev->createBuffer(RHI::BufferUsage::Vertex,
                              (size_t)m_maxInstances * sizeof(InstanceDataFloat),
                              nullptr, RHI::BufferMemory::Dynamic);
}

void GPUInstancing::addInstance(const glm::vec3& position, const glm::vec3& scale,
                                const glm::vec4& color, const glm::vec3& rotation) {
    glm::mat4 m(1.0f);
    m = glm::translate(m, position);
    if (rotation.x != 0.0f) m = glm::rotate(m, rotation.x, glm::vec3(1, 0, 0));
    if (rotation.y != 0.0f) m = glm::rotate(m, rotation.y, glm::vec3(0, 1, 0));
    if (rotation.z != 0.0f) m = glm::rotate(m, rotation.z, glm::vec3(0, 0, 1));
    m = glm::scale(m, scale);
    addInstance(m, color, scale);
}

void GPUInstancing::addInstance(const glm::mat4& model, const glm::vec4& color, const glm::vec3& scale) {
    if ((int)m_instances.size() >= m_maxInstances) return;   // cota dura: el buffer es fijo
    m_instances.push_back({ model, color, scale, 0.0f });
    m_dirty = true;
}

void GPUInstancing::upload() {
    if (!m_dirty || m_instances.empty() || !RHI::valid(m_buf)) return;
    if (RHI::Device* dev = RHI::device())
        dev->updateBuffer(m_buf, 0, m_instances.size() * sizeof(InstanceDataFloat), m_instances.data());
    m_dirty = false;
}

void GPUInstancing::render(RHI::Context* ctx, uint32_t indexCount, uint32_t instanceBinding) {
    if (!ctx || m_instances.empty() || indexCount == 0) return;
    upload();
    // El divisor NO se toca aquí: vive en el PSO (InputRate::Instance). El llamador ya ató su
    // pipeline y la malla base; nosotros solo añadimos el stream de instancias y disparamos el draw.
    ctx->bindVertexBuffer(m_buf, instanceBinding);
    ctx->drawIndexed(indexCount, /*first*/0, (uint32_t)m_instances.size());
}

void GPUInstancing::clear() {
    m_instances.clear();
    m_dirty = true;
}

void GPUInstancing::appendInstanceLayout(RHI::VertexLayout& layout, uint32_t binding) {
    if (layout.strides.size() <= binding) layout.strides.resize(binding + 1, 0);
    if (layout.rates.size()   <= binding) layout.rates.resize(binding + 1, RHI::InputRate::Vertex);
    layout.strides[binding] = (uint32_t)sizeof(InstanceDataFloat);
    layout.rates[binding]   = RHI::InputRate::Instance;   // ← avanza una vez por OBJETO

    // mat4 = 4 columnas vec4 en locations 3..6 (así lo declara el shader).
    for (uint32_t col = 0; col < 4; ++col)
        layout.attributes.push_back({ 3 + col, (uint32_t)(col * sizeof(glm::vec4)),
                                      RHI::Format::RGBA32F, binding });
    layout.attributes.push_back({ 7, (uint32_t)offsetof(InstanceDataFloat, color),
                                  RHI::Format::RGBA32F, binding });
    layout.attributes.push_back({ 8, (uint32_t)offsetof(InstanceDataFloat, scale),
                                  RHI::Format::RGB32F, binding });
}

}} // namespace Haruka::Renderer
