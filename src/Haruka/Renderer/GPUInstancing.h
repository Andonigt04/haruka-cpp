/**
 * @file GPUInstancing.h
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
 * El layout que espera el shader (`assets/shaders/lib/instance.glsl`, ver PSO del llamador):
 *    3 = vec3 posición · 4 = vec4 cuaternión (snorm16) · 5 = vec3 escala · 7 = color RGBA8 · 10 = máscara
 */
#pragma once

#include <glm/glm.hpp>
#include <glm/ext.hpp>
#include <glm/gtc/packing.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cmath>
#include <cstdint>
#include <vector>
#include "Haruka/RHI/RHITypes.h"
#include "Haruka/RHI/RHIResources.h"

namespace Haruka { namespace RHI { class Context; } }

namespace Haruka { namespace Renderer {

/**
 * @brief Datos por INSTANCIA (el buffer que avanza una vez por objeto, no por vértice). 40 B.
 *
 * ⚠️ ERA UNA mat4 + vec4 + vec3 + float = 96 B, y la mitad era redundante: una transformación de
 * prop o de pieza es TRASLACIÓN · GIRO · ESCALA (sin cizalla), y eso son 10 números, no 16. Con
 * 300 000 props eran 29 MB en RAM más lo que se sube cada frame; ahora 12 MB. Y el shader ya no
 * necesita `inverse()` por vértice para la normal: con giro y escala separados es `R · (n / s)`.
 *
 * Lo que NO se ha comprimido, a propósito: la posición y la escala siguen en float32. Las piezas de
 * construcción encajan unas con otras y un error de milímetros en un sillar de 10 m se ve como una
 * rendija; en half lo habría.
 *
 * Se construye con `make()` o `fromMatrix()`; los campos empaquetados no se escriben a mano.
 */
struct InstanceDataFloat {
    glm::vec3 position{0.0f};  // traslación (relativa al origen del pase)
    uint32_t  rot[2] = { 0u, 0x7FFF0000u };   // cuaternión (x,y,z,w) en snorm16 — identidad por defecto
    glm::vec3 scale{1.0f};     // escala por eje (construcción: no uniforme)
    /// Tinte RGBA8 con la ESCALA en el alfa: rgb × (a·4). Los tintes de variedad del scatter llegan a
    /// 1,15 y un RGBA8 normal los saturaría (mismo esquema que el color de vértice de `PropVertex`).
    uint32_t  color = 0x40FFFFFFu;   // blanco, escala 1
    /// MÁSCARA DE PARTES ROTAS de esta instancia (bit p = la parte p ya no está). Ocupa el hueco
    /// que antes era relleno, así que el stream de instancia NO crece. Va como float con el VALOR
    /// entero (no el patrón de bits): un índice de parte cabe exacto en float hasta 2^24, y el
    /// patrón de bits de un entero pequeño es un denormal que la GPU puede vaciar a cero.
    /// Los pases que no rompen nada (construcción) la dejan en 0 = nada roto.
    float     breakMask = 0.0f;

    /// Tinte → RGBA8 con la escala en el alfa (hasta 4,0). El alfa del tinte de entrada se ignora:
    /// ningún pase instanciado lo usaba.
    static uint32_t packTint(const glm::vec3& c) {
        const glm::vec3 cc = glm::max(c, glm::vec3(0.0f));
        const float want = glm::clamp(glm::max(glm::max(cc.r, cc.g), glm::max(cc.b, 1.0f)), 1.0f, 4.0f);
        // La escala se CUANTIZA PRIMERO (hacia arriba, para que rgb/s no pase de 1) y el color se
        // divide por la escala ya cuantizada: si no, el error del alfa (pasos de 4/255) se suma al
        // de los canales.
        const float a = std::ceil(want * 0.25f * 255.0f - 1e-4f) / 255.0f;
        return glm::packUnorm4x8(glm::vec4(glm::min(cc / (a * 4.0f), glm::vec3(1.0f)), a));
    }
    static InstanceDataFloat make(const glm::vec3& pos, const glm::quat& q, const glm::vec3& scl,
                                  const glm::vec3& tint, float breakMask = 0.0f) {
        InstanceDataFloat d;
        d.position = pos;
        const glm::quat n = glm::normalize(q);
        const uint64_t pk = glm::packSnorm4x16(glm::vec4(n.x, n.y, n.z, n.w));   // x en los bits bajos
        d.rot[0] = (uint32_t)(pk & 0xFFFFFFFFull);
        d.rot[1] = (uint32_t)(pk >> 32);
        d.scale = scl;
        d.color = packTint(tint);
        d.breakMask = breakMask;
        return d;
    }
    /// Desde una matriz TRS (sin cizalla). Una matriz con espejo (determinante < 0) se representa
    /// con la escala X negativa, que es lo único que un cuaternión no puede llevar.
    static InstanceDataFloat fromMatrix(const glm::mat4& m, const glm::vec3& tint, float breakMask = 0.0f) {
        glm::vec3 c0(m[0]), c1(m[1]), c2(m[2]);
        glm::vec3 s(glm::length(c0), glm::length(c1), glm::length(c2));
        if (glm::dot(glm::cross(c0, c1), c2) < 0.0f) s.x = -s.x;
        const auto safe = [](float v) { return std::abs(v) > 1e-20f ? v : 1e-20f; };
        const glm::mat3 r(c0 / safe(s.x), c1 / safe(s.y), c2 / safe(s.z));
        return make(glm::vec3(m[3]), glm::quat_cast(r), s, tint, breakMask);
    }
};
static_assert(sizeof(InstanceDataFloat) == 40, "InstanceDataFloat: el layout compacto son 40 B");

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

    /** @brief Encola una instancia ya empaquetada. */
    void addInstance(const InstanceDataFloat& inst);
    /** @brief Encola una instancia con matriz ya construida (p.ej. orientada a la normal del suelo). */
    void addInstance(const glm::mat4& model,
                     const glm::vec4& color = glm::vec4(1.0f),
                     const glm::vec3& scale = glm::vec3(1.0f));

    /** @brief Sustituye la cola por `data` (recortando al máximo del buffer). Evita el bucle
     *  addInstance del llamador cuando el conjunto se monta de una vez (props del scatter). */
    void setInstances(const std::vector<InstanceDataFloat>& data);

    /** @brief Garantiza que el arena cabe `capacity` instancias SIN recrearse a mitad de frame.
     *  Parte del coste de `inst.upload` no es el memcpy: es RECREAR el arena host-visible cuando el
     *  total sub-asignado del frame cruza su potencia de dos, y cada recreacion llama a
     *  `vkAllocateMemory` de un buffer que puede duplicar el anterior en tamaño. Quien conozca el
     *  tope ANTES de dibujar (el anillo de props: la suma de todos los buckets cabe en el anillo) lo
     *  reserva de una vez al empezar el pase y el bucle de draws no vuelve a cruzar el tope.
     *  Con `setHardCap` (tope duro) el arena NACE a ese tamaño la primera vez y jamás se reasigna:
     *  cada `vkAllocateMemory` host-visible cuesta ~110 ms en este portátil híbrido, y las
     *  re-siembras del mundo (que agrandan el anillo) disparaban un golpe por re-siembra. */
    void reserve(size_t capacity);

    /** @brief Tope duro del arena (instancias): la primera reserva salta directamente a él y
     *  `upload` NUNCA recrea por debajo. Si un frame pide más, se escribe lo que quepa en el arena
     *  (un aviso la primera vez) en vez de asignar. 0 = sin tope (comportamiento antiguo). */
    void setHardCap(size_t capacity) { m_hardCap = capacity; }

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
    uint32_t m_lastCount = 0;  ///< instancias del lote actual (puede ser menor que `m_instances` si el tope duro recortó)
    int    m_draws     = 0;    ///< lotes subidos este frame (diagnóstico)
    std::vector<Haruka::RHI::BufferHandle> m_retired;   ///< arenas viejos: se destruyen al frame siguiente
    size_t m_frameDemand = 0;       ///< instances asked for this frame (in every arena, retired ones too)
    size_t m_lastFrameDemand = 0;   ///< ...and in the whole previous frame: what a new arena must hold
    int  m_maxInstances = 0;
    size_t m_hardCap   = 0;    ///< tope duro del arena (0 = sin tope, se crece como antes)
    bool m_dirty = false;

public:
    /// Sube el lote actual a SU entrada del anillo (lo llama `render`; público para el banco RHI).
    void upload();
    /// Al empezar cada frame: el arena vuelve a cero y se sueltan los arenas retirados.
    void beginFrame();
    /// Lotes subidos en el último frame (diagnóstico: cuántos draws instanciados hubo).
    int ringSize() const { return m_draws; }
    /// Instancias ya sub-asignadas este frame (diagnóstico del tope duro).
    size_t arenaUsed() const { return m_arenaUsed; }
    /// Capacidad del arena (instancias).
    size_t arenaCap() const { return m_arenaCap; }
    /// Bytes reservados en buffers de instancias (host-visible: cuentan como RAM).
    size_t hostBytes() const;
};

}} // namespace Haruka::Renderer

