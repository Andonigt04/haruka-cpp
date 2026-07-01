#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace Haruka {

    /**
     * @brief Tabla de índices de terreno COMPARTIDA por nº de índices (indexCount).
     *
     * Los índices de un chunk de terreno son topología FIJA por resolución (grid + skirts):
     * idénticos en TODOS los chunks con el mismo indexCount. Guardarlos por-chunk (en la
     * ChunkData cacheada) duplica ~3456 índices × 4 B en miles/decenas de miles de chunks en RAM.
     *
     * En su lugar: la generación los registra UNA vez por indexCount aquí y LIBERA su copia;
     * la ChunkData solo lleva `indexCount`. El renderer (addToScene) los lee de aquí para crear
     * el EBO compartido. Misma idea que el EBO compartido (#1), extendida a la caché de RAM.
     *
     * Thread-safe: la generación corre en hilos worker; el render en el hilo principal.
     */
    class SharedIndexTable {
    public:
        static SharedIndexTable& get() {
            static SharedIndexTable s;
            return s;
        }

        /** Registra (COPIA) los índices de ese indexCount si aún no existen. Idempotente. */
        void registerOnce(uint32_t indexCount, const std::vector<unsigned int>& src) {
            if (indexCount == 0) return;
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_table.find(indexCount) == m_table.end())
                m_table.emplace(indexCount, src);
        }

        /** Puntero a los índices de ese indexCount, o nullptr si no se registró. */
        const std::vector<unsigned int>* find(uint32_t indexCount) const {
            std::lock_guard<std::mutex> lk(m_mtx);
            auto it = m_table.find(indexCount);
            return it != m_table.end() ? &it->second : nullptr;
        }

    private:
        SharedIndexTable() = default;
        mutable std::mutex m_mtx;
        std::unordered_map<uint32_t, std::vector<unsigned int>> m_table;
    };

} // namespace Haruka
