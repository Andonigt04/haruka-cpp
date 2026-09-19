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
    /// MÁSCARA DE PARTES ROTAS de esta instancia (bit p = la parte p ya no está). Ocupa el hueco
    /// que antes era relleno, así que el stream de instancia NO crece. Va como float con el VALOR
    /// entero (no el patrón de bits): un índice de parte cabe exacto en float hasta 2^24, y el
    /// patrón de bits de un entero pequeño es un denormal que la GPU puede vaciar a cero.
    /// Los pases que no rompen nada (construcción) la dejan en 0 = nada roto.
    float     breakMask = 0.0f;
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

    /** @brief Lote por ÍNDICES: copia `idx.size()` instancias de `src` (ya transformadas) al lote.
     *  Es el camino del pase de props: las instancias viven transformadas en la Application y el
     *  bucket de cada (prototipo, LOD) es una lista de índices, no una copia de 96 B por instancia. */
    void setInstancesGather(const InstanceDataFloat* src, const std::vector<uint32_t>& idx);

    /** @brief Encola una instancia con matriz ya construida (p.ej. orientada a la normal del suelo). */
    void addInstance(const glm::mat4& model,
                     const glm::vec4& color = glm::vec4(1.0f),
                     const glm::vec3& scale = glm::vec3(1.0f));

    /** @brief Sustituye la cola por `data` (recortando al máximo del buffer). Evita el bucle
     *  addInstance del llamador cuando el conjunto se monta de una vez (props del scatter). */
    void setInstances(const std::vector<InstanceDataFloat>& data);

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
    Haruka::RHI::BufferHandle buffer() const { return m_arena; }

    /** @brief El layout de los atributos de instancia, para que el llamador lo meta en su PSO.
     *  Devolverlo desde aquí evita que cada usuario lo reescriba (y se equivoque en un offset). */
    static void appendInstanceLayout(Haruka::RHI::VertexLayout& layout, uint32_t binding);

private:
    std::vector<InstanceDataFloat> m_instances;
    /// ⚠️ UN ARENA POR FRAME, no un buffer por draw. `render()` sube las instancias y dibuja; con
    /// un único buffer eso vale en OpenGL —cada draw se ejecuta al vuelo— pero en Vulkan los comandos
    /// se GRABAN y se ejecutan al final, así que TODOS los draws leerían el ÚLTIMO lote subido.
    /// Primero fue un anillo de 16 buffers de 20 000 instancias (30 MB fijos) que se daba la vuelta a
    /// mitad de frame en cuanto hubo más de 16 draws (la hierba cercana no salía); después una entrada
    /// por draw del tamaño del lote, que costaba **22 ms por frame** en `updateBuffer`: cada entrada
    /// se recreaba cuando su lote crecía (el orden de los draws cambia con el cull) y crear+mapear
    /// memoria de Vulkan por draw no es gratis.
    ///
    /// Ahora hay UN buffer host-visible por frame y cada draw se sub-asigna LINEALMENTE dentro
    /// (`bindVertexBuffer` con offset): un memcpy contiguo por lote, cero creaciones en régimen. Si
    /// el arena se queda corto a mitad de frame se crea otro mayor y el viejo se RETIRA hasta el
    /// frame siguiente (los draws ya grabados lo referencian). Entre frames es seguro: el device
    /// espera a la GPU en `endFrame`.
    Haruka::RHI::BufferHandle m_arena;
    size_t m_arenaCap  = 0;    ///< capacidad del arena (instancias)
    size_t m_arenaUsed = 0;    ///< instancias ya sub-asignadas este frame
    size_t m_lastOff   = 0;    ///< offset (bytes) del lote actual dentro del arena
    int    m_draws     = 0;    ///< lotes subidos este frame (diagnóstico)
    std::vector<Haruka::RHI::BufferHandle> m_retired;   ///< arenas viejos: se destruyen al frame siguiente
    int  m_maxInstances = 0;
    bool m_dirty = false;

public:
    /// Sube el lote actual a SU entrada del anillo (lo llama `render`; público para el banco RHI).
    void upload();
    /// Al empezar cada frame: el arena vuelve a cero y se sueltan los arenas retirados.
    void beginFrame();
    /// Lotes subidos en el último frame (diagnóstico: cuántos draws instanciados hubo).
    int ringSize() const { return m_draws; }
    /// Bytes reservados en buffers de instancias (host-visible: cuentan como RAM).
    size_t hostBytes() const;
};

}} // namespace Haruka::Renderer

namespace Haruka { using Renderer::InstanceDataFloat; using Renderer::GPUInstancing; }
