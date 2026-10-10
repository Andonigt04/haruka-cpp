// ================================================================================================
// bench_dgs_bandwidth — test de estrés de TRANSFERENCIA entre nodos del DGS (sin sockets ni lib).
//
// Responde la pregunta: "¿cuánto puede transferir el DGS entre servidores con latencia L hasta que
// algo falle?" Modelando el límite físico real del transporte:
//
//     throughput = min(rawLink, windowBytes / RTT)          // límite por enlace y por BDP
//     enVuelo    = throughput * RTT                          // bytes in-flight = BDP
//     falla      = enVuelo > recvBuffer  → OVERFLOW (el receptor no puede retenerlo en vuelo)
//                | tiempo > deadline     → TIMEOUT (nunca termina a tiempo)
//
// Es el mismo cuello que acota el DGS de verdad: el nodo solo puede tener `windowBytes` sin ACK, y el
// receptor solo bufferiza `recvBuffer` antes de empezar a soltar (`failedTransfers`, §4.6 bug 6).
//
// Además hace un segundo pase REAL sobre el transporte: transmite el volumen completo a través del
// `PacketFramer` (toFramed→feed→next, el código del bug 6), en memoria, y mide bytes/s efectivos —
// así el número no es solo teoría, el framer aguanta ese volumen sin perder un byte.
//
// Ejecución (no enlaza libdgs_client.a ni httplib.h → compila en CI):
//     ./bench_dgs_bandwidth                # matriz completa 1/2.5/5 GiB x 10..150 ms
//     ./bench_dgs_bandwidth --fast         # volúmenes 64 MiB (verificación rápida, CI)
//     ./bench_dgs_bandwidth --link=2500 --window=128 --buf=1024 --deadline=30
//     ./bench_dgs_bandwidth --no-framer    # solo el modelo (sin el pase del framer)
//
// Claves del modelo configurables por env (mismo patrón EVAL_* del orquestador):
//     BENCH_LINK_MBPS   enlace bruto (MiB/s)   default 1250 (10 Gbit/s)
//     BENCH_WINDOW_MIB  ventana sin ACK        default  64
//     BENCH_BUF_MIB     buffer del receptor    default 256
//     BENCH_DEADLINE_S  deadline (s)           default  60
// ================================================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>

#include "include/dgs/packet.h"

namespace {

using Clock = std::chrono::steady_clock;

double envF(const char* name, double def)
{
    const char* v = std::getenv(name);
    return v ? std::atof(v) : def;
}

// Una dirección del enlace (cliente→server o server→cliente).
struct Flow
{
    const char* name;
    double      gibi;          // volumen a transferir (GiB, 1024^3)
    double      linkMBs;       // enlace bruto (MiB/s)
    double      windowBytes;   // ventana sin ACK (bytes)
    double      recvBufBytes;  // buffer del receptor (bytes)
    double      deadlineS;     // presupuesto de tiempo

    struct Result
    {
        double throughputMiB;   // min(enlace, window/RTT)
        double inFlightMiB;     // BDP
        double timeS;           // volumen/throughput
        bool   overflow;
        bool   timeout;
    };

    Result run(double rttMs) const
    {
        const double rttS      = rttMs / 1000.0;
        const double volBytes  = gibi * 1073741824.0;
        // Ventana: la retransmisión/SACK obliga a ventana ≤ half del RTT útil en la práctica; aquí
        // usamos la ventana declarada tal cual (caso ideal sin ACK perdidos).
        const double windowRate = windowBytes / rttS;           // bytes/s limitados por BDP
        const double linkRate   = linkMBs * 1048576.0;          // bytes/s limitados por enlace
        const double rate       = windowRate < linkRate ? windowRate : linkRate;

        const double inFlight   = rate * rttS;                  // BDP real
        const double timeS      = volBytes / rate;
        Result r{};
        r.throughputMiB = rate / 1048576.0;
        r.inFlightMiB   = inFlight / 1048576.0;
        r.timeS         = timeS;
        r.overflow      = inFlight > recvBufBytes;
        r.timeout       = timeS > deadlineS;
        return r;
    }
};

// Pase REAL por el framer (bug 6): transmite `gibi` GiB en paquetes de `chunkKiB` a través de
// toFramed→feed→next y comprueba integridad (cada payload sale idéntico). Devuelve MiB/s medidos.
double framerStress(double gibi, size_t chunkKiB)
{
    const size_t chunk = chunkKiB * 1024;
    const uint64_t total = (uint64_t)(gibi * 1073741824.0);
    const uint64_t frames = total / chunk + 1;

    // Payload de origen: cabecera de Packet (PKT_ZONE_REGION + ancla) + blob de región del tamaño dado.
    DGS::Packet base;
    base.pack(DGS::PKT_ZONE_REGION);
    base.write<int32_t>(1234);      // ancla chunkX
    base.write<int32_t>(-7);        // ancla chunkY
    base.write<int32_t>(99);        // ancla chunkZ
    base.write<uint32_t>(3);        // srcZone
    base.write<uint32_t>((uint32_t)chunk);

    std::vector<uint8_t> payload(chunk, 0);
    for (size_t i = 0; i < chunk; ++i) payload[i] = (uint8_t)(i * 2654435761u >> 24);  // relleno
    base.writeRaw(payload.data(), payload.size());
    auto frame = base.toFramed();

    DGS::PacketFramer rx;
    std::vector<uint8_t> out;
    const auto t0 = Clock::now();

    uint64_t delivered = 0;
    bool     integrity = true;
    // Fragmentamos la entrada en trozos de ~7 KB (como un recv TCP) para estresar el acumulador.
    const size_t recvChunk = 7000;
    while (delivered < frames)
    {
        // Alimentamos el frame PARTIDO en varios recvs (TCP fragmentado) y el acumulador lo une.
        size_t fed = 0;
        while (fed < frame.size())
        {
            const size_t take = (frame.size() - fed < recvChunk) ? frame.size() - fed : recvChunk;
            rx.feed(frame.data() + fed, take);
            fed += take;
            while (rx.next(out))
            {
                // Integridad: el payload de salida debe ser exactamente el de entrada (blob de región).
                if (out.size() == base.getSize())
                {
                    const uint8_t* orig = base.getRawData();
                    if (std::memcmp(orig, out.data(), out.size()) != 0) integrity = false;
                }
                else integrity = false;
                ++delivered;
            }
        }
    }

    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    const double miB  = (double)delivered * (double)chunk / 1048576.0;

    std::printf("    framer real  : %llu frames de %zu KiB (%s)  ->  %.1f MiB/s, integridad %s\n",
                (unsigned long long)delivered, chunkKiB, integrity ? "OK" : "¡ROTA!",
                miB / (secs > 0 ? secs : 1e-9), integrity ? "OK" : "FALLO");
    return miB / (secs > 0 ? secs : 1e-9);
}

void printHeader(const std::vector<Flow>& flows, double rttMs)
{
    std::printf("%10s |", "RTT(ms)");
    for (const auto& f : flows) std::printf(" %12s |", f.name);
    std::printf("\n");
}

void printRow(double rttMs, const std::vector<Flow>& flows)
{
    std::printf("%10.0f |", rttMs);
    for (const auto& f : flows)
    {
        auto r = f.run(rttMs);
        const char* status = "ok";
        if (r.overflow && r.timeout) status = "OVERFLOW+TIMEOUT";
        else if (r.overflow)         status = "OVERFLOW";
        else if (r.timeout)          status = "TIMEOUT";
        std::printf(" %9.1f %6s |", r.throughputMiB, status);
    }
    std::printf("\n");
}

} // namespace

int main(int argc, char** argv)
{
    bool fast = false, noFramer = false;
    double linkM  = envF("BENCH_LINK_MBPS",  1250.0);
    double window = envF("BENCH_WINDOW_MIB",  64.0);
    double buf    = envF("BENCH_BUF_MIB",    256.0);
    double dead   = envF("BENCH_DEADLINE_S",  60.0);

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--fast")                       fast = true;
        else if (a == "--no-framer")             noFramer = true;
        else if (a.rfind("--link=", 0) == 0)     linkM  = std::atof(a.c_str() + 7);
        else if (a.rfind("--window=", 0) == 0)   window = std::atof(a.c_str() + 9);
        else if (a.rfind("--buf=", 0) == 0)      buf    = std::atof(a.c_str() + 6);
        else if (a.rfind("--deadline=", 0) == 0) dead   = std::atof(a.c_str() + 11);
        else { std::printf("uso: bench_dgs_bandwidth [--fast] [--no-framer] "
                           "[--link=MB] [--window=MiB] [--buf=MiB] [--deadline=s]\n"); return 2; }
    }

    // Matriz de escenarios (clientGiB / serverGiB) pedida por el usuario.
    const double g1 = fast ? 0.0625 : 1.0;      // --fast → 64 MiB (verificación, CI)
    const double g2 = fast ? 0.0625 : 2.5;
    const double g5 = fast ? 0.0625 : 5.0;
    const std::vector<double> rtts = { 10, 20, 50, 80, 150 };

    std::vector<Flow> flows;
    auto mk = [&](const char* n, double g) {
        flows.push_back(Flow{ n, g, linkM, window * 1048576.0, buf * 1048576.0, dead });
    };
    // "1 GiB cliente/server", "1 cliente y 2.5/5 server", "2.5 cliente/server", "5 cliente/server".
    mk("cli 1G/srv 1G",  g1);
    mk("cli 1G/srv 2.5G",g1 + g2);
    mk("cli 1G/srv 5G",  g1 + g5);
    mk("cli 2.5G/srv 2.5G", g2 + g2);
    mk("cli 5G/srv 5G",  g5 + g5);

    std::printf("== bench_dgs_bandwidth ==  enlace=%.0f MiB/s  ventana=%.0f MiB  bufRecv=%.0f MiB  "
                "deadline=%.0f s\n", linkM, window, buf, dead);
    std::printf("modelo: throughput = min(enlace, ventana/RTT);  BDP = throughput*RTT;  "
                "falla = BDP>bufRecv (OVERFLOW) o t>deadline (TIMEOUT)\n\n");

    // ------- Pase del modelo (BDP) ------
    printHeader(flows, 0);
    for (double r : rtts) printRow(r, flows);
    std::printf("\n");

    // ------- Pase real del framer (solo un escenario representativo) -------
    if (!noFramer)
    {
        const double vol = fast ? 0.0625 : 1.0;
        std::printf("== verificación del transporte real (PacketFramer, bug 6) =="
                    "  volumen %.2f GiB\n", vol);
        framerStress(vol, 4);       // 4 KiB (chunk región típico)
        framerStress(vol, 60);      // 60 KiB (chunk grande, zona amplia; ≤ MAX_PACKET_SIZE con cabecera)
    }

    // ------- "¿Hasta dónde aguanta?" por RTT: cuántos GiB caben en el presupuesto -------
    std::printf("\n== capacidad máxima por RTT (enlace %.0f MiB/s, ventana %.0f MiB, buf %.0f MiB, "
                "deadline %.0f s) ==\n", linkM, window, buf, dead);
    for (double r : rtts)
    {
        double rate = (window * 1048576.0) / (r / 1000.0);
        double link = linkM * 1048576.0;
        if (rate > link) rate = link;
        const double maxGiB = rate * dead / 1073741824.0;
        const double bdpMiB = rate * (r / 1000.0) / 1048576.0;
        const char* limit = (rate >= link) ? "enlace" : "ventana/BDP";
        std::printf("  %3.0f ms: %.0f MiB/s  (límite por %s)  →  hasta %.2f GiB en %.0f s, "
                    "BDP %.1f MiB (buf %s)\n",
                    r, rate / 1048576.0, limit, maxGiB, dead, bdpMiB,
                    bdpMiB > buf ? "¡NO BASTA!" : "ok");
    }

    return 0;
}
