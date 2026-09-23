#include "renderer/gpu_instancing.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"

#include <glm/gtc/matrix_transform.hpp>
#include <cstddef>
#include "tools/profiler.h"

namespace Haruka { namespace Renderer {

GPUInstancing::~GPUInstancing() {
    if (RHI::Device* dev = RHI::device()) {
        if (RHI::valid(m_arena)) dev->destroy(m_arena);
        for (auto& b : m_retired) if (RHI::valid(b)) dev->destroy(b);
    }
}

void GPUInstancing::init(int maxInstances) {
    m_maxInstances = maxInstances > 0 ? maxInstances : 1;
    m_instances.reserve((size_t)m_maxInstances);
    RHI::Device* dev = RHI::device();
    if (!dev) return;
    if (RHI::valid(m_arena)) dev->destroy(m_arena);
    for (auto& b : m_retired) if (RHI::valid(b)) dev->destroy(b);
    m_retired.clear();
    m_arena = {}; m_arenaCap = 0; m_arenaUsed = 0; m_lastOff = 0; m_draws = 0;
    // El arena nace al primer uso y del tamaño que haga falta (ver la nota de la cabecera).
}

void GPUInstancing::beginFrame() {
    if (RHI::Device* dev = RHI::device())
        for (auto& b : m_retired) if (RHI::valid(b)) dev->destroy(b);
    m_retired.clear();
    m_arenaUsed = 0;
    m_draws = 0;
}

size_t GPUInstancing::hostBytes() const {
    return m_arenaCap * sizeof(InstanceDataFloat);
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

void GPUInstancing::setInstances(const std::vector<InstanceDataFloat>& data) {
    const size_t n = data.size() < (size_t)m_maxInstances ? data.size() : (size_t)m_maxInstances;
    m_instances.assign(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(n));
    m_dirty = true;
}

void GPUInstancing::setInstancesGather(const InstanceDataFloat* src, const std::vector<uint32_t>& idx) {
    const size_t n = idx.size() < (size_t)m_maxInstances ? idx.size() : (size_t)m_maxInstances;
    m_instances.resize(n);
    for (size_t i = 0; i < n; ++i) m_instances[i] = src[idx[i]];
    m_dirty = true;
}

void GPUInstancing::reserve(size_t capacity) {
    if (capacity <= m_arenaCap) return;
    RHI::Device* dev = RHI::device();
    if (!dev) return;
    // Los draws de este frame ya grabados pueden seguir leyendo el arena viejo: se retira como en
    // `upload` (se destruye en el beginFrame siguiente, cuando la GPU ya termino el frame).
    const size_t c = capacity < (size_t)m_maxInstances ? (size_t)m_maxInstances : capacity;
    if (RHI::valid(m_arena)) m_retired.push_back(m_arena);
    m_arena = dev->createBuffer(RHI::BufferUsage::Vertex, c * sizeof(InstanceDataFloat),
                                nullptr, RHI::BufferMemory::Dynamic);
    m_arenaCap = c;
    m_arenaUsed = 0;
}

void GPUInstancing::upload() {
    if (m_instances.empty()) return;
    RHI::Device* dev = RHI::device();
    if (!dev) return;
    const size_t need = m_instances.size();
    if (!RHI::valid(m_arena) || m_arenaUsed + need > m_arenaCap) {
        // No cabe: arena nuevo del tamaño EXACTO de lo sub-asignado + lo que viene (el viejo se
        // retira hasta el frame siguiente — los draws ya grabados este frame lo siguen leyendo).
        // Potencias de dos hacian explotar el arena: cada frame que cruzaba el tope recreaba AL
        // DOBLE un buffer host-visible (una allocacion que en este portatil hibrido cuesta cientos
        // de ms y dobla cada frame: 157 -> 278 -> 671 ms). Con el tamaño exacto, si vuelve a cruzar
        // crece por el DELTA, nunca al doble, y lo retenido cubre al frame siguiente.
        size_t c = m_arenaUsed + need;
        if (c < (size_t)m_maxInstances) c = (size_t)m_maxInstances;
        if (RHI::valid(m_arena)) m_retired.push_back(m_arena);
        m_arena = dev->createBuffer(RHI::BufferUsage::Vertex, c * sizeof(InstanceDataFloat),
                                    nullptr, RHI::BufferMemory::Dynamic);
        m_arenaCap = c;
        // Lo ya sub-asignado vive en el arena retirado; el nuevo empieza limpio.
        m_arenaUsed = 0;
    }
    if (!RHI::valid(m_arena)) return;
    m_lastOff = m_arenaUsed * sizeof(InstanceDataFloat);
    dev->updateBuffer(m_arena, m_lastOff, need * sizeof(InstanceDataFloat), m_instances.data());
    m_arenaUsed += need;
    ++m_draws;
    m_dirty = false;
}

void GPUInstancing::render(RHI::Context* ctx, uint32_t indexCount, uint32_t instanceBinding) {
    if (!ctx || m_instances.empty() || indexCount == 0) return;
    { HARUKA_PROFILE("inst.upload(memcpy)"); upload(); }
    // El divisor NO se toca aquí: vive en el PSO (InputRate::Instance). El llamador ya ató su
    // pipeline y la malla base; nosotros solo añadimos el stream de instancias y disparamos el draw.
    HARUKA_PROFILE("inst.bind+draw");
    ctx->bindVertexBuffer(m_arena, instanceBinding, m_lastOff);
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
    // Máscara de partes rotas (loc 10). Va en el stream de INSTANCIA porque cada árbol pierde sus
    // propias ramas mientras comparte la malla prototipo con todos los demás.
    layout.attributes.push_back({ 10, (uint32_t)offsetof(InstanceDataFloat, breakMask),
                                  RHI::Format::R32F, binding });
}

}} // namespace Haruka::Renderer
