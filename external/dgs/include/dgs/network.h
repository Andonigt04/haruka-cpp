#ifndef DGS_NETWORK_H
#define DGS_NETWORK_H

#include <string>
#include <vector>
#include <netinet/in.h>

#include "include/dgs/types.h"

namespace DGS
{
    class UDPSocket
    {
        public:
            UDPSocket();
            ~UDPSocket();

            bool bind(int port);

            // `size` DEBE ser ≤ MAX_PACKET_SIZE y, al recibir, el llamador debe comprobar el retorno
            // (bytes reales) para no interpretar basura si el datagrama se truncó (PLAN_DGS_VALIDADOR §4.6 bug 6).
            bool send(const std::string& address, int port, const uint8_t* data, size_t size);
            int receive(uint8_t* buffer, size_t size, std::string& outAddress, int& outPort);

            int getSocketFD() { return socketFD; }
        private:
            int socketFD;
    };

    class TCPSocket
    {
        public:
            TCPSocket();
            ~TCPSocket();

            bool listen(int port);
            int accept();

            bool connect(const std::string& address, int port);

            bool send(int fd, const uint8_t* data, size_t size);
            int receive(int fd, uint8_t* buffer, size_t size);

            void closeClient(int fd);

            int getSocketFD() { return socketFD; }
        private:
            int socketFD;
    };

    // §4.6 bug 6: un datagrama mayor que el buffer se trunca en silencio y el parseo leería bytes
    // basura. Convención: SIEMPRE llamar a `receive` con un buffer de al menos MAX_PACKET_SIZE y tratar
    // `received >= size` como truncación (descartar + contar en failedTransfers del nodo). Este helper
    // centraliza la comprobación para que los nodos no la olviden en cada punto de recv.
    inline bool receivedWasTruncated(int received, size_t bufferSize)
    {
        // received<0 = error/timeout (no hay datos); received==bufferSize = el datagrama pudo caber
        // justo, pero un UDP que llena el buffer a raja tabla es tan raro como peligroso → descartar.
        return received <= 0 || (size_t)received >= bufferSize;
    }
};

#endif