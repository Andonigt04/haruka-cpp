#pragma once
/**
 * @file ResourceStore.h
 * @brief DATOS QUE VIVEN EN CPU Y EN GPU, LLEVADOS EN UN SOLO SITIO.
 *
 * Varios subsistemas tenían una copia en CPU y su versión en GPU, y cada uno se inventaba cómo saber
 * que había que volver a subirla: `revision` por chunk en `VoxRenderer` (con DOS mapas que tenían
 * que tener las mismas claves a mano), `m_lakeEpoch` en `Planet`, `m_flatRev` en `PropSystem`,
 * `m_lineRev` en `CollisionWire`, `m_dirty` en `GPUInstancing`. Cuatro soluciones al mismo problema,
 * cada una con su propio presupuesto de subida por frame y su propio `destroy` directo.
 *
 * Un recurso es un `ResourceId` con:
 *   · los DATOS DE CPU (de cualquier tipo, inmutables una vez publicados: se publica una versión
 *     nueva entera, nunca se toca la vieja bajo un lector);
 *   · lo que vive en GPU (buffers y texturas);
 *   · UNA revisión. Publicar la sube; la GPU sube UNA vez por revisión aunque se publique varias
 *     veces antes de que llegue su turno (solo la última importa).
 *
 * Reglas:
 *   · `publish` y `cpu` valen desde cualquier hilo. `processUploads`, `gpu`, `beginFrame` y
 *     `release` solo en el hilo de render.
 *   · Las subidas pendientes se hacen en `processUploads` con un PRESUPUESTO de ms por frame, el
 *     mismo para todos los recursos (antes cada subsistema tenía el suyo).
 *   · Lo que deja de usarse en GPU (al resubir o al soltar el recurso) NO se destruye en el acto: el
 *     frame que aún está en vuelo puede estar leyéndolo. Se destruye `framesInFlight` frames después.
 *   · `keepCpu = false` suelta la copia de CPU tras subirla (una textura de horneado). Con `true` se
 *     queda (la malla de una cueva, que la física lee por revisión).
 *
 * El almacén no sabe crear nada en GPU: cada `publish` trae su `Uploader`, y la destrucción la hace
 * el `Destroyer` que se le da al construirlo. Así se prueba sin GPU, con uno falso que solo anota.
 */
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "Haruka/RHI/RHITypes.h"

namespace Haruka::Renderer {

using ResourceId = uint64_t;

/** @brief Lo que un recurso tiene en GPU. El almacén solo lo guarda y lo destruye a tiempo. */
struct GpuSet {
    std::vector<RHI::BufferHandle>  buffers;
    std::vector<RHI::TextureHandle> textures;
    /// Lo que el que DIBUJA necesita saber de ESTA versión subida (nº de índices, origen…). Va con la
    /// GPU y no con la CPU a propósito: los datos de CPU pueden ir ya una revisión por delante, y
    /// dibujar con el recuento de la nueva sobre el buffer de la vieja lee fuera del buffer.
    std::shared_ptr<const void> meta;
    template <class T> const T* metaAs() const { return static_cast<const T*>(meta.get()); }
    bool empty() const { return buffers.empty() && textures.empty(); }
};

struct ResourceStats {
    size_t   resources = 0;          ///< vivos
    size_t   pendingUploads = 0;     ///< con una revisión aún sin subir
    size_t   graveyard = 0;          ///< GPU esperando a que acaben sus frames en vuelo
    uint64_t uploads = 0;            ///< subidas hechas en total
    uint64_t destroyed = 0;          ///< `GpuSet` destruidos en total
};

class ResourceStore {
public:
    using Uploader  = std::function<GpuSet(const void* cpu)>;
    using Destroyer = std::function<void(const GpuSet&)>;

    ResourceStore(Destroyer destroyer, uint32_t framesInFlight);
    ~ResourceStore();   ///< destruye TODO lo que quede (al cerrar ya no hay frames en vuelo)

    /** @brief Un recurso nuevo, sin datos todavía. `name` sale en el log y en las estadísticas. */
    ResourceId create(std::string name, bool keepCpu);

    /** @brief Publica una versión nueva de los datos y programa su subida. Cualquier hilo. */
    template <class T>
    uint64_t publish(ResourceId id, std::shared_ptr<const T> data, Uploader up) {
        return publishErased(id, std::static_pointer_cast<const void>(std::move(data)), typeid(T),
                             std::move(up));
    }

    /** @brief Los datos de CPU vigentes (o nullptr si no hay, si se soltaron, o si el tipo no casa).
     *  `revision` recibe la revisión de esos datos. Cualquier hilo. */
    template <class T>
    std::shared_ptr<const T> cpu(ResourceId id, uint64_t* revision = nullptr) const {
        return std::static_pointer_cast<const T>(cpuErased(id, typeid(T), revision));
    }

    /** @brief Lo que hay en GPU y de qué revisión es (0 = nunca subido). Hilo de render. */
    const GpuSet* gpu(ResourceId id, uint64_t* revision = nullptr) const;

    /** @brief Sube lo pendiente hasta gastar `budgetMs` (siempre al menos una). Hilo de render.
     *  Devuelve cuántas subió. */
    size_t processUploads(double budgetMs);

    /** @brief Avanza el contador de frames y destruye lo que ya no está en vuelo. Hilo de render. */
    void beginFrame();

    /** @brief Suelta el recurso. Su GPU se destruye cuando sea seguro. Hilo de render. */
    void release(ResourceId id);

    /** @brief Destruye YA toda la GPU y olvida todos los recursos. Para el cierre: hay que llamarlo
     *  ANTES de destruir el device (después, sus `destroy` irían a un device muerto). */
    void clear();

    ResourceStats stats() const;

private:
    struct Entry {
        std::string name;
        bool keepCpu = true;
        std::shared_ptr<const void> cpu;
        std::type_index type{typeid(void)};
        uint64_t cpuRevision = 0;
        Uploader uploader;
        GpuSet gpu;
        uint64_t gpuRevision = 0;
    };
    struct Grave { GpuSet set; uint64_t frame; };

    uint64_t publishErased(ResourceId id, std::shared_ptr<const void> data, std::type_index type,
                           Uploader up);
    std::shared_ptr<const void> cpuErased(ResourceId id, std::type_index type, uint64_t* rev) const;
    void bury(GpuSet set);

    mutable std::mutex m_mutex;   ///< protege `m_entries` (CPU, revisión, cola) frente a `publish`
    std::unordered_map<ResourceId, Entry> m_entries;
    std::vector<ResourceId> m_pending;   ///< orden de llegada; un id aparece una vez
    ResourceId m_nextId = 1;

    Destroyer m_destroyer;
    uint32_t  m_framesInFlight;
    uint64_t  m_frame = 0;
    std::vector<Grave> m_graveyard;
    uint64_t  m_uploads = 0, m_destroyed = 0;
};

/// Frames que la GPU puede tener en vuelo: los de Vulkan (`VKDevice::kMaxFrames`). En OpenGL el
/// driver sincroniza solo, pero esperar lo mismo no cuesta nada y no depende del backend.
inline constexpr uint32_t kFramesInFlight = 2;

/** @brief El almacén del motor. Destruye con `RHI::device()`; sin device (tras el cierre) no hace nada.
 *  `Application` llama a `beginFrame` y `processUploads` cada frame, y a `clear` al cerrar. */
ResourceStore& resources();

} // namespace Haruka::Renderer
