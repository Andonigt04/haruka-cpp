#ifndef DGS_PACKET_H
#define DGS_PACKET_H

#include <vector>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <stdexcept>
#include <limits>

#include "include/dgs/types.h"

namespace DGS
{

    class Packet
    {

    public:

        Packet() : readPos(0) {}

        template<typename T>
        void write(T data)
        {
            const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&data);
            buffer.insert(buffer.end(), ptr, ptr + sizeof(T));
        }

        template<typename T>
        T read()
        {
            if (readPos + sizeof(T) > buffer.size()) throw std::runtime_error("Packet read overflow");

            T data;
            std::memcpy(&data, &buffer[readPos], sizeof(T));
            readPos += sizeof(T);

            return data;
        }

        void writeRaw(const uint8_t* data, size_t size)
        {
            buffer.insert(buffer.end(), data, data + size);
        }

        void readRaw(uint8_t* dest, size_t size)
        {
            if (readPos + size > buffer.size()) throw std::runtime_error("Packet read overflow (Raw)");
            std::memcpy(dest, &buffer[readPos], size);
            readPos += size;
        }

        void writeString(const std::string& str)
        {
            write<uint16_t>(static_cast<uint16_t>(str.size()));
            buffer.insert(buffer.end(), str.begin(), str.end());
        }

        std::string readString()
        {
            uint16_t size = read<uint16_t>();
            if (readPos + size > buffer.size()) throw std::runtime_error("String read overflow");

            std::string str(buffer.begin() + readPos, buffer.begin() + readPos + size);
            readPos += size;

            return str;
        }

        void clear() { buffer.clear(); readPos = 0; }

        void pack(const EntityTransfer& data);
        void pack(const Command& data);
        void pack(const ServerMetrics& data);
        void pack(const ZoneQuery& data);
        void pack(const ZoneResponse& data);
        void pack(const ZoneListResponse& data);
        void pack(const GhostDelta& data);
        void pack(const ChatMessage& data);
        // Validación (PLAN_DGS_VALIDADOR §2.2): request/ack + telemetría del validador. Declarados
        // aquí para completar el API; la DEFINICIÓN va en el lib regenerado (repo real, D-D).
        void pack(const ValidateRequest& data);
        void pack(const ValidateAck& data);
        void pack(const ValidatorStatus& data);
        void pack(const EntityReassign& data);
        void pack(const ZoneLifecycle& data);
        void packDelete(const ZoneLifecycle& data);   // §3.9: mismo struct, tipo PKT_DELETE_ZONE
        void pack(const ZoneRegion& data);
        void pack(const PacketType& t) { clear(); write<PacketType>(t); }

        EntityTransfer unpackEntityTransfer();
        Command unpackCommand();
        ServerMetrics unpackServerMetrics();
        ZoneQuery unpackZoneQuery();
        ZoneResponse unpackZoneResponse();
        ZoneListResponse unpackZoneListResponse();
        GhostDelta unpackGhostDelta();
        ChatMessage unpackChatMessage();
        ValidateRequest unpackValidateRequest();
        ValidateAck unpackValidateAck();
        ValidatorStatus unpackValidatorStatus();
        EntityReassign unpackEntityReassign();
        ZoneLifecycle unpackZoneLifecycle();
        ZoneRegion unpackZoneRegion();
        PacketType unpackPacketType() { PacketType data; data = read<PacketType>(); return data; };
        
        PacketType getType() const { return buffer.empty() ? static_cast<PacketType>(0) : static_cast<PacketType>(buffer[0]); }
        
        const uint8_t* getRawData() const { return buffer.data(); }
        size_t getSize() const { return buffer.size(); }
        void setBuffer(const uint8_t* data, size_t size) { buffer.assign(data, data + size); }

        // --- Framing por longitud para TCP (§4.6 bug 6) --------------------------------------------
        // TCP es un STREAM sin límites de mensaje: un `receive` puede traer medio paquete o varios
        // encadenados, y sin un delimitador explícito el `Packet::read` interpreta bytes ajenos como
        // payload (→ `runtime_error`). Para robustecer el transporte, cada paquete viaja PREFIJADO con
        // su longitud: [len:4][payload]. `toFramed()` produce ese prefijo; `PacketFramer` (abajo) es el
        // acumulador del lado receptor que entrega paquetes completos a partir de recvs arbitrarios.
        std::vector<uint8_t> toFramed() const
        {
            std::vector<uint8_t> out;
            out.reserve(buffer.size() + 4);
            if (buffer.size() > DGS::MAX_PACKET_SIZE)
                throw std::runtime_error("Packet too large to frame (over MAX_PACKET_SIZE)");
            uint32_t len = (uint32_t)buffer.size();
            out.push_back((uint8_t)(len >> 0));
            out.push_back((uint8_t)(len >> 8));
            out.push_back((uint8_t)(len >> 16));
            out.push_back((uint8_t)(len >> 24));
            out.insert(out.end(), buffer.begin(), buffer.end());
            return out;
        }

    private:
        std::vector<uint8_t> buffer;
        size_t readPos;
    };

    // ==============================================================================================
    // PacketFramer — acumulador de un stream TCP hacia paquetes COMPLETOS (§4.6 bug 6).
    //
    // El emisor escribe `Packet::toFramed()` (prefijo [len:4] + payload). El receptor alimenta este
    // acumulador con lo que devuelva cada `TCPSocket::receive` —que puede ser un trozo, un paquete
    // entero, o varios encadenados— y `next()` entrega SOLO paquetes íntegros. Los bytes sobrantes se
    // conservan para la siguiente llamada. Si un prefijo miente (len 0 o > MAX_PACKET_SIZE), el stream
    // está corrupto/desincronizado: se descarta un byte y se re-sincroniza, contando la pérdida en
    // `discards()` (el nodo lo suma a `failedTransfers`). Este acumulador NO aloca por paquete: reutiliza
    // el buffer interno y solo devuelve copias cuando hay un frame completo.
    // ==============================================================================================
    class PacketFramer
    {
        public:
            PacketFramer() { pending.reserve(DGS::MAX_PACKET_SIZE); }

            // Consume los bytes de un receive. Puede llamarse con recvs parciales, exactos o múltiples.
            void feed(const uint8_t* data, size_t size)
            {
                if (!data || size == 0) return;
                pending.insert(pending.end(), data, data + size);
            }

            // Extrae el siguiente paquete COMPLETO. `out` se rellena con el payload (sin prefijo).
            // Devuelve false si aún faltan bytes; el llamador debe esperar al siguiente receive.
            bool next(std::vector<uint8_t>& out)
            {
                // Búsqueda con índice: nunca borramos por byte (O(n²) con 64 KB de basura); movemos
                // `scan` hacia delante y solo compactamos cuando extraemos un frame o al terminar.
                while (scan + 4 <= pending.size())
                {
                    uint32_t len = (uint32_t)pending[scan]
                                 | ((uint32_t)pending[scan + 1] << 8)
                                 | ((uint32_t)pending[scan + 2] << 16)
                                 | ((uint32_t)pending[scan + 3] << 24);

                    if (len == 0 || len > DGS::MAX_PACKET_SIZE)
                    {
                        // Prefijo mentiroso: desincronización. Descartamos UN byte y re-sincronizamos.
                        ++scan;
                        ++discarded;
                        continue;
                    }

                    if (scan + 4u + len > pending.size())   // payload aún incompleto → esperar
                    {
                        consumeScan();   // libera lo ya descartado sin tocar el frame a medias
                        return false;
                    }

                    out.assign(pending.begin() + scan + 4, pending.begin() + scan + 4 + len);
                    scan += 4u + len;
                    consumeScan();
                    return true;
                }
                consumeScan();
                return false;
            }

            // Bytes en espera sin completar un frame (diagnóstico de fragmentación).
            size_t buffered() const { return pending.size() - scan; }
            // Frames descartados por cabecera corrupta (desincronización). Suma a failedTransfers.
            uint64_t discards() const { return discarded; }

            void clear() { pending.clear(); scan = 0; discarded = 0; }

        private:
            // Compacta el buffer: descarta los bytes ya recorridos (`scan`) quedando solo el resto.
            void consumeScan()
            {
                if (scan == 0) return;
                pending.erase(pending.begin(), pending.begin() + scan);
                scan = 0;
            }

            std::vector<uint8_t> pending;
            size_t scan = 0;
            uint64_t discarded = 0;
    };

    using PacketHandler = std::function<void(int, Packet&)>;

    class PacketDispatcher
    {
        public:
            void registerHandler(PacketType type, PacketHandler handler) {
                handlers[type] = handler;
            }

            void dispatch(int fd, Packet& p) {
                PacketType type = p.getType();
                if (handlers.count(type)) {
                    handlers[type](fd, p);
                }
            }

        private:
            std::map<PacketType, PacketHandler> handlers;
    };
};

#endif // DGS_PACKET_H