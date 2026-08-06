#ifndef DGS_ORCHESTRATOR_H
#define DGS_ORCHESTRATOR_H

#include "include/dgs/types.h"
#include "include/dgs/network.h"

#include <httplib.h>

#include <vector>
#include <map>
#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <chrono>

namespace DGS
{
    // Backend de spawn (PLAN_DGS_VALIDADOR §3.8): cómo se crean/destruyen zonas. El comportamiento del DGS
    // no depende del modo; solo cambia la infra. LOCAL = standalone (procesos/hilos), K8S = cluster,
    // TERRAFORM = provisionado con Terraform (mismo camino k8s tras el apply).
    enum class SpawnBackend : uint8_t
    {
        LOCAL,
        K8S,
        TERRAFORM
    };

    // Umbrales/constantes configurables de evaluación (§4.2), leídos de env con default. Se resuelven una
    // vez (no por llamada) para no pagar getenv/atof en el hot path de métricas.
    static float evalCfg(const char* name, float def)
    {
        const char* v = std::getenv(name);
        return v ? (float)std::atof(v) : def;
    }

    // Estado EWMA por nodo: tasas de ancho de banda derivadas de contadores acumulados (Δ libre de ventana,
    // inmune a reinicios porque el nodo reporta startTimeS y el orquestador descarta baselines viejos).
    struct MetricsRate
    {
        uint64_t lastBytesRx = 0;
        uint64_t lastBytesTx = 0;
        uint64_t lastStartTimeS = 0;
        bool     haveBaseline = false;
        bool     haveEwma = false;
        double   rxEWMA = 0.0;   // bytes/s
        double   txEWMA = 0.0;
    };

    class Orchestrator
    {
        public:
            Orchestrator(DGS::TCPSocket& s) : socket(&s), currentReplicas(1) {}
            // Constructor de TEST (sin red): `socket` queda nulo y `sendResizeCommand` se inutiliza
            // (dryRun). Solo para unit-testing de evicción/escalado (§4.3) — NUNCA en producción.
            Orchestrator() : socket(nullptr), currentReplicas(1) {}
            std::vector<ZoneInfo> activeZones;

            // Modo de simulación de decisión: `evaluateServer` decide y cuenta, pero NO contacta k8s ni
            // el socket de control (para los tests de §4.3 "decisión de escalado" sin clúster).
            bool dryRun = false;
            // Nº de decisiones de escalado tomadas (todas las señales convergen aquí; cuenta también en
            // dryRun, para poder ASSERT sin spawn real). §4.2/§4.3.
            uint64_t scaleEvents = 0;

            // lastSeen por fd: último ServerMetrics que marca la zona como viva (§4.6 bug 1 / lease §2.3).
            std::map<int, std::chrono::steady_clock::time_point> lastSeen;
            // Estado de vida por fd (§3.9).
            std::map<int, DGS::ZoneState> zoneState;

            // Identidad de nodo para el cooldown de escalado: "addr:port" (NO el fd, que se reutiliza). §4.6 bug 3.
            static std::string nodeKey(const ZoneInfo& z)
            {
                return std::string(z.addr) + ":" + std::to_string(z.port);
            }

            void updateNodeTopology(int fd, const ServerMetrics& m)
            {
                for (auto& zone : activeZones)
                {
                    if (zone.fd == fd)
                    {
                        zone.chunkXMin = m.node.chunkXMin; zone.chunkXMax = m.node.chunkXMax;
                        zone.chunkYMin = m.node.chunkYMin; zone.chunkYMax = m.node.chunkYMax;
                        zone.chunkZMin = m.node.chunkZMin; zone.chunkZMax = m.node.chunkZMax;
                        copyAddr(zone.addr, m.node.addr);
                        zone.port = m.node.port;
                        lastSeen[fd]    = std::chrono::steady_clock::now();
                        zoneState[fd]   = DGS::ZoneState::READY;
                        return;
                    }
                }

                ZoneInfo info{};
                info.fd        = fd;
                info.chunkXMin = m.node.chunkXMin; info.chunkXMax = m.node.chunkXMax;
                info.chunkYMin = m.node.chunkYMin; info.chunkYMax = m.node.chunkYMax;
                info.chunkZMin = m.node.chunkZMin; info.chunkZMax = m.node.chunkZMax;
                copyAddr(info.addr, m.node.addr);
                info.port = m.node.port;
                activeZones.push_back(info);
                lastSeen[fd]  = std::chrono::steady_clock::now();
                zoneState[fd] = DGS::ZoneState::READY;
            }

            // Purga las zonas cuyo lease venció (no reportan ServerMetrics). Devuelve los fd evictados para
            // que el llamador borre el pod / reasigne (§3.9, §4.6 bug 1).
            std::vector<int> evictStaleZones()
            {
                std::vector<int> evicted;
                const auto now = std::chrono::steady_clock::now();
                for (size_t i = 0; i < activeZones.size();)
                {
                    auto it = lastSeen.find(activeZones[i].fd);
                    if (it == lastSeen.end() || (now - it->second) > zoneLease)
                    {
                        evicted.push_back(activeZones[i].fd);
                        lastScaleTime.erase(nodeKey(activeZones[i]));
                        zoneState.erase(activeZones[i].fd);
                        lastSeen.erase(activeZones[i].fd);
                        activeZones.erase(activeZones.begin() + i);
                    }
                    else ++i;
                }
                return evicted;
            }

            int findTargetNode(int32_t chunkX, int32_t chunkY, int32_t chunkZ)
            {
                for (const auto& zone : activeZones)
                {
                    if (chunkX >= zone.chunkXMin && chunkX <= zone.chunkXMax &&
                        chunkY >= zone.chunkYMin && chunkY <= zone.chunkYMax &&
                        chunkZ >= zone.chunkZMin && chunkZ <= zone.chunkZMax) return zone.fd;
                }

                return -1;
            }

            ZoneResponse findZoneResponse(int32_t chunkX, int32_t chunkY, int32_t chunkZ)
            {
                for (const auto& zone : activeZones)
                {
                    if (chunkX >= zone.chunkXMin && chunkX <= zone.chunkXMax &&
                        chunkY >= zone.chunkYMin && chunkY <= zone.chunkYMax &&
                        chunkZ >= zone.chunkZMin && chunkZ <= zone.chunkZMax)
                    {
                        ZoneResponse r{};
                        copyAddr(r.addr, zone.addr);
                        r.port = zone.port;
                        return r;
                    }
                }
                return ZoneResponse{};
            }

            std::vector<int> findNeighbors(int fd, NeighborMode mode = NeighborMode::FACE)
            {
                const ZoneInfo* origin = nullptr;
                for (const auto& z : activeZones)
                    if (z.fd == fd) { origin = &z; break; }

                std::vector<int> neighbors;
                if (!origin) return neighbors;

                for (const auto& z : activeZones)
                {
                    if (z.fd == fd) continue;

                    bool adjX = origin->chunkXMin <= z.chunkXMax + 1 && z.chunkXMin <= origin->chunkXMax + 1;
                    bool adjY = origin->chunkYMin <= z.chunkYMax + 1 && z.chunkYMin <= origin->chunkYMax + 1;
                    bool adjZ = origin->chunkZMin <= z.chunkZMax + 1 && z.chunkZMin <= origin->chunkZMax + 1;

                    if (!adjX || !adjY || !adjZ) continue;

                    int touching = 0;
                    if (origin->chunkXMax + 1 == z.chunkXMin || z.chunkXMax + 1 == origin->chunkXMin) touching++;
                    if (origin->chunkYMax + 1 == z.chunkYMin || z.chunkYMax + 1 == origin->chunkYMin) touching++;
                    if (origin->chunkZMax + 1 == z.chunkZMin || z.chunkZMax + 1 == origin->chunkZMin) touching++;

                    bool include = false;
                    switch (mode)
                    {
                        case NeighborMode::FACE:             include = touching == 1; break;
                        case NeighborMode::FACE_EDGE:        include = touching <= 2; break;
                        case NeighborMode::FACE_EDGE_CORNER: include = touching <= 3; break;
                    }

                    if (include) neighbors.push_back(z.fd);
                }
                return neighbors;
            }

            void evaluateServer(const ServerMetrics& m, int nodeFD)
            {
                // Constantes configurables, resueltas UNA vez (env o default) — §4.2.
                static const double alpha    = evalCfg("EVAL_EWMA_ALPHA", 0.2);
                static const double dtEst    = evalCfg("EVAL_DT_S",       0.1);
                static const float  loadTh   = evalCfg("EVAL_LOAD_RAM",   0.80f);
                static const float  perfTh   = evalCfg("EVAL_LOAD_PERF",  0.36f);
                static const double asymmTh  = evalCfg("EVAL_NET_ASYMM",   4.0);
                static const float  failTh   = evalCfg("EVAL_FAIL_THRESH", 40.0f);
                static const int    cooldown = (int)evalCfg("EVAL_COOLDOWN_S", 30.0f);

                auto& z   = m.node;
                auto  key = nodeKey(z);

                // --- Estado EWMA por nodo (tasas de banda). Contadores acumulados → Δ libre de ventana.
                MetricsRate& r = metricRates[key];

                // Nodo recién arrancado / sin historia: establecer baseline, no decidir (§4.6 bug 5).
                if (!r.haveBaseline)
                {
                    r.lastBytesRx = m.bytesRx;
                    r.lastBytesTx = m.bytesTx;
                    r.lastStartTimeS = m.startTimeS;
                    r.haveBaseline = true;
                    return;
                }

                // Nodo REINICIADO (startTimeS cambió): descartar baseline viejo de contadores.
                if (r.lastStartTimeS != 0 && m.startTimeS != 0 && m.startTimeS != r.lastStartTimeS)
                {
                    r.lastBytesRx = m.bytesRx;
                    r.lastBytesTx = m.bytesTx;
                    r.lastStartTimeS = m.startTimeS;
                    return;
                }

                // Δ sin signo (resta modular, inmune a reinicio de contador uint64).
                uint64_t dRx = m.bytesRx - r.lastBytesRx;
                uint64_t dTx = m.bytesTx - r.lastBytesTx;
                r.lastBytesRx = m.bytesRx;
                r.lastBytesTx = m.bytesTx;

                // EWMA de la tasa en bytes/s (el intervalo real entre muestras es ~100ms del tick de zona).
                double rxRate = (double)dRx / dtEst;
                double txRate = (double)dTx / dtEst;
                if (!r.haveEwma)
                {
                    r.rxEWMA = rxRate;
                    r.txEWMA = txRate;
                    r.haveEwma = true;
                }
                else
                {
                    r.rxEWMA = alpha * rxRate + (1.0 - alpha) * r.rxEWMA;
                    r.txEWMA = alpha * txRate + (1.0 - alpha) * r.txEWMA;
                }

                auto now = std::chrono::steady_clock::now();
                auto it  = lastScaleTime.find(key);
                if (it != lastScaleTime.end() &&
                    std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count() < cooldown)
                    return;

                // Señales de escalado (§4.2): carga, saturación de red (asimetría Tx>>Rx), o fallos de
                // validación/traspaso (el validador/zonas vecinas van mal). Un solo camino de acción.
                bool load          = m.ramUsage     > loadTh && m.performance < perfTh;
                bool netSaturated  = r.txEWMA > 0 && r.txEWMA / (r.rxEWMA + 1.0) > asymmTh;
                bool failureProne  = m.failedTransfers > (uint32_t)failTh;

                if (load || netSaturated || failureProne)
                {
                    int32_t width = z.chunkXMax - z.chunkXMin;
                    if (width < 1)
                    {
                        std::cout << "[Orchestrator] Zona demasiado pequena para dividir (width=" << width << ")" << std::endl;
                        return;
                    }

                    std::cout << "[Orchestrator] Umbral alcanzado (load=" << load
                              << ", net=" << netSaturated << ", failed=" << m.failedTransfers
                              << "). Escalando sistema..." << std::endl;

                    int32_t midLow  =  (z.chunkXMin + z.chunkXMax)      / 2;
                    int32_t midHigh = ((z.chunkXMin + z.chunkXMax) + 1) / 2;

                    // dryRun: registra la DECISIÓN sin spawn (para tests §4.3 y estimar cuándo escalaría
                    // el sistema sin tocar k8s). El cooldown se aplica igual → medible en unit test.
                    if (dryRun)
                    {
                        ++scaleEvents;
                        lastScaleTime[key] = now;
                        return;
                    }

                    if (spawnZoneNode(midHigh, z.chunkXMax,
                                      z.chunkYMin, z.chunkYMax,
                                      z.chunkZMin, z.chunkZMax))
                    {
                        lastScaleTime[key] = now;
                        sendResizeCommand(nodeFD, midLow);
                    }
                }
            }

        private:
            int currentReplicas;
            int nextNodePort { 30426 };
            // Cooldown de escalado keyed por "addr:port" (identidad de nodo), NO por fd reutilizable. §4.6 bug 3.
            std::map<std::string, std::chrono::steady_clock::time_point> lastScaleTime;
            // Estado EWMA por nodo (P3 §4.1/§4.2).
            std::map<std::string, MetricsRate> metricRates;
            // Lease de zona (evicción). §4.6 bug 1 / §2.3.
            std::chrono::steady_clock::duration zoneLease{ std::chrono::milliseconds(DGS::DEFAULT_LEASE_MS) };

            // strncpy que SIEMPRE termina en '\0' (el char[16] no puede quedar sin terminar; §4.6 bug 2).
            static void copyAddr(char* dst, const char* src) noexcept
            {
                std::strncpy(dst, src, DGS::MAX_ADDR_LEN);
                dst[DGS::MAX_ADDR_LEN - 1] = '\0';
            }

            static std::string readFile(const std::string& path)
            {
                std::ifstream f(path);
                return std::string(std::istreambuf_iterator<char>(f),
                                   std::istreambuf_iterator<char>());
            }

            // Returns the image name from the base zone-node deployment.
            static std::string fetchZoneImage(httplib::SSLClient& k8s, const std::string& ns)
            {
                auto res = k8s.Get("/apis/apps/v1/namespaces/" + ns + "/deployments/zone-node");
                if (res && res->status == 200)
                {
                    const auto& b = res->body;
                    auto pos = b.find("\"image\":\"");
                    if (pos != std::string::npos)
                    {
                        pos += 9;
                        auto end = b.find('"', pos);
                        if (end != std::string::npos)
                            return b.substr(pos, end - pos);
                    }
                }
                return "dgs-zone-node:latest";
            }

            // Creates a new independent zone-node Deployment + NodePort Service
            // for the given chunk range. Returns true on success.
            bool spawnZoneNode(int32_t xMin, int32_t xMax,
                               int32_t yMin, int32_t yMax,
                               int32_t zMin, int32_t zMax)
            {
                const std::string tokenPath = "/var/run/secrets/kubernetes.io/serviceaccount/token";
                const std::string caPath    = "/var/run/secrets/kubernetes.io/serviceaccount/ca.crt";
                const std::string ns        = "dgs";

                std::string token = readFile(tokenPath);
                if (token.empty()) { std::cerr << "[Orchestrator] No token de ServiceAccount" << std::endl; return false; }

                httplib::SSLClient k8s("kubernetes.default.svc", 443);
                k8s.set_ca_cert_path(caPath.c_str());
                k8s.set_default_headers({{"Authorization", "Bearer " + token}});

                const std::string image    = fetchZoneImage(k8s, ns);
                const int         udpPort  = nextNodePort++;
                const std::string name     = "zone-node-" + std::to_string(currentReplicas + 1);

                // Node IP: head server passes MY_NODE_IP env var (set via kubectl set env).
                const char* nodeIP = std::getenv("MY_NODE_IP");
                const std::string podIP = nodeIP ? nodeIP : "127.0.0.1";

                auto i = [](int32_t v) { return std::to_string(v); };

                // --- Service (NodePort UDP) ---
                std::string svc = R"({"apiVersion":"v1","kind":"Service","metadata":{"name":")" + name +
                    R"(","namespace":")" + ns + R"("},"spec":{"selector":{"app":")" + name +
                    R"("},"ports":[{"protocol":"UDP","port":42425,"targetPort":42425,"nodePort":)" +
                    std::to_string(udpPort) +
                    R"(}],"type":"NodePort"}})";

                auto svcRes = k8s.Post("/api/v1/namespaces/" + ns + "/services", svc, "application/json");
                if (!svcRes || svcRes->status != 201)
                {
                    std::cerr << "[Orchestrator] Error creando Service " << name
                              << ": " << (svcRes ? svcRes->status : -1) << std::endl;
                    --nextNodePort;
                    return false;
                }

                // --- Deployment ---
                std::string dep =
                    R"({"apiVersion":"apps/v1","kind":"Deployment","metadata":{"name":")" + name +
                    R"(","namespace":")" + ns +
                    R"("},"spec":{"replicas":1,"selector":{"matchLabels":{"app":")" + name +
                    R"("}},"template":{"metadata":{"labels":{"app":")" + name +
                    R"("}},"spec":{"containers":[{"name":"zone-node","image":")" + image +
                    R"(","imagePullPolicy":"IfNotPresent","ports":[{"containerPort":42425,"protocol":"UDP"}],)"
                    R"("env":[)"
                        R"({"name":"HEAD_SERVER_HOST","value":"head-server"},)"
                        R"({"name":"HEAD_SERVER_PORT","value":"42424"},)"
                        R"({"name":"MY_POD_IP","value":")" + podIP + R"("},)"
                        R"({"name":"ZONE_UDP_PORT","value":")" + std::to_string(udpPort) + R"("},)"
                        R"({"name":"CHUNK_X_MIN","value":")" + i(xMin) + R"("},)"
                        R"({"name":"CHUNK_X_MAX","value":")" + i(xMax) + R"("},)"
                        R"({"name":"CHUNK_Y_MIN","value":")" + i(yMin) + R"("},)"
                        R"({"name":"CHUNK_Y_MAX","value":")" + i(yMax) + R"("},)"
                        R"({"name":"CHUNK_Z_MIN","value":")" + i(zMin) + R"("},)"
                        R"({"name":"CHUNK_Z_MAX","value":")" + i(zMax) + R"("})"
                    R"(]}]}}}})" ;

                auto depRes = k8s.Post("/apis/apps/v1/namespaces/" + ns + "/deployments", dep, "application/json");
                if (depRes && depRes->status == 201)
                {
                    ++currentReplicas;
                    std::cout << "[Orchestrator] ZoneNode " << name
                              << " creado  chunks X[" << xMin << "-" << xMax << "]"
                              << "  NodePort=" << udpPort << std::endl;
                    return true;
                }

                std::cerr << "[Orchestrator] Error creando Deployment " << name
                          << ": " << (depRes ? depRes->status : -1) << std::endl;
                // Rollback service
                k8s.Delete("/api/v1/namespaces/" + ns + "/services/" + name);
                --nextNodePort;
                return false;
            }

            void sendResizeCommand(int fd, int32_t newChunkMax)
            {
                DGS::Command cmd;
                cmd.purpose = DGS::CMD_TRANSFER_SERVER;
                cmd.chunkX  = newChunkMax;

                DGS::Packet p;
                p.pack(cmd);

                // §4.6 bug 4: socket.send devuelve bool; un envío fallido (fd muerto) NO debe ignorarse.
                if (!socket)
                {
                    // dryRun / sin socket de control: no hay nada que enviar (solo tests).
                    std::cerr << "[Orchestrator] sendResizeCommand sin socket (dryRun?): ignorado." << std::endl;
                    return;
                }
                if (!socket->send(fd, p.getRawData(), p.getSize()))
                    std::cerr << "[Orchestrator] Error enviando resize a " << fd
                              << " (zona muerta?): la evicción por lease lo limpiará." << std::endl;
                else
                    std::cout << "[Orchestrator] Actualizando contenedor ZoneNode... " << fd << std::endl;
            }

            DGS::TCPSocket* socket;
        };
};

#endif
