/**
 * @file gpu_instancing.h
 * @brief Instancing: dibuja UNA malla N veces con transformaciones distintas, en UN draw.
 *
 * Para qué: miles de árboles/rocas del mismo tipo. Hoy cada prop se dibuja por separado (y peor: con
 * su PROPIA malla generada). Con instancing, el prop pasa a ser "prototipo + transformación + tinte"
 * → un draw por (tipo, LOD) en vez de miles, y la CPU deja de construir una malla por objeto.
 *
 * ⚠️ CÓMO SE USA (cambió al migrar al PSO):
 *  - Los atributos de INSTANCIA (matriz, color, escala) son parte del **VertexLayout del PSO**, en su
 *    propio binding con `InputRate::Instance`. El divisor ya no lo pone el instancer a mano: es
 *    estado del pipeline. (Antes se ponía con glVertexAttribDivisor y había que RESETEARLO, o se
 *    quedaba pegado al VAO y contaminaba los draws siguientes.)
 *  - El PSO y la malla base los pone el LLAMADOR; esta clase solo aporta el buffer de instancias.
 *
 * El layout que espera el shader (ver PSO del llamador):
 *    location 3..6 = mat4 model (4 columnas)   ·   7 = vec4 color   ·   8 = vec3 scale
 */
#pragma once

#include <glm/glm.hpp>
#include <glm/ext.hpp>
#include <vector>
#include "rhi/rhi_types.h"
#include "rhi/rhi_resources.h"

namespace Haruka { namespace RHI { class Context; } }

namespace Haruka { namespace Renderer {

/** @brief Datos por INSTANCIA (el buffer que avanza una vez por objeto, no por vértice). */
struct InstanceDataFloat {
    glm::mat4 model;           // transformación completa
    glm::vec4 color;           // tinte RGBA (la variedad sale de aquí, no de geometría nueva)
    glm::vec3 scale;           // escala (para normales/efectos que la necesiten)
    float     _padding;
};

class GPUInstancing {
public:
    ~GPUInstancing();

    /** @brief Reserva el buffer de instancias. */
    void init(int maxInstances = 10000);

    /** @brief Encola una instancia. La VARIEDAD (tamaño, giro, tinte) va aquí — la malla se comparte. */
    void addInstance(const glm::vec3& position,
                     const glm::vec3& scale    = glm::vec3(1.0f),
                     const glm::vec4& color    = glm::vec4(1.0f),
                     const glm::vec3& rotation = glm::vec3(0.0f));

    /** @brief Encola una instancia con matriz ya construida (p.ej. orientada a la normal del suelo). */
    void addInstance(const glm::mat4& model,
                     const glm::vec4& color = glm::vec4(1.0f),
                     const glm::vec3& scale = glm::vec3(1.0f));

    /**
     * @brief Sube lo pendiente y dibuja TODAS las instancias en un solo draw.
     * @param ctx              contexto RHI (el PSO y la malla base ya deben estar atados).
     * @param indexCount       índices de la malla base.
     * @param instanceBinding  binding del VertexLayout marcado como InputRate::Instance.
     */
    void render(Haruka::RHI::Context* ctx, uint32_t indexCount, uint32_t instanceBinding = 1);

    /** @brief Vacía la cola (se rellena cada frame). */
    void clear();

    int  getInstanceCount() const { return (int)m_instances.size(); }
    int  getMaxInstances()  const { return m_maxInstances; }
    /** @brief Handle del buffer de instancias (para atarlo a mano si el llamador prefiere). */
    Haruka::RHI::BufferHandle buffer() const { return m_buf; }

    /** @brief El layout de los atributos de instancia, para que el llamador lo meta en su PSO.
     *  Devolverlo desde aquí evita que cada usuario lo reescriba (y se equivoque en un offset). */
    static void appendInstanceLayout(Haruka::RHI::VertexLayout& layout, uint32_t binding);

private:
    std::vector<InstanceDataFloat> m_instances;
    Haruka::RHI::BufferHandle      m_buf;
    int  m_maxInstances = 0;
    bool m_dirty = false;

    void upload();
};

}} // namespace Haruka::Renderer

namespace Haruka { using Renderer::InstanceDataFloat; using Renderer::GPUInstancing; }
