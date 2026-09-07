/**
 * @file dgs_live_test.cpp
 * @brief ANTI-CHEAT DE TERRENO CONTRA UN SERVIDOR VIVO — el tramo que faltaba.
 *
 * Lo que ya estaba cubierto, y por qué no bastaba:
 *   · `test_dgs.cpp` (`dgs_ground_matches_engine`) prueba la REGLA: carga `libharuka_rules.so` por
 *     `dlopen` y comprueba que, con el campo de altura puesto, el valle deja de dar falso positivo y
 *     el noclip dentro de la montaña deja de colarse. Pero corre EN PROCESO, llamando a `validateMove`
 *     a mano.
 *   · `dgs/tests/validator_e2e.cpp` prueba el NODO: que `validador_node` consulta al módulo que le den
 *     y devuelve el veredicto correlacionado. Pero usa un módulo de juguete, sin terreno.
 *
 * Ninguno de los dos prueba lo que de verdad protege al juego: **el módulo REAL, con el terreno REAL,
 * dentro del servidor REAL**. Entre los dos hay tres cosas que se pueden romper sin que salte nada:
 * que el nodo no encuentre el `.so`, que no le llegue el campo de altura, y que el `.hfield` que deja
 * el bake no sea el que el módulo sabe leer. Esto lo cierra.
 *
 * ── CÓMO ────────────────────────────────────────────────────────────────────────────────────────
 * El nodo exige head server y persistencia vivos y luego bloquea esperando un `Command`, así que el
 * test levanta los tres. El campo de altura le llega por `HARUKA_RULES_HEIGHT_FIELD` (la tercera vía
 * del módulo: un servidor headless sin motor en el proceso).
 *
 *     [head falso] --Command--> [validador_node + libharuka_rules.so + .hfield]
 *     [persistencia falsa] <---'        ^ PKT_VALIDATE_REQ / ValidateAck
 *                                        `-- este test, haciendo de zona
 *
 * ⚠️ LA CONTRAPRUEBA ES LA MITAD DEL TEST: se levanta el nodo DOS veces, con y sin el campo. Sin ella,
 * un nodo que rechazara todo, o un módulo que no llegara a cargar, darían el mismo verde en el caso
 * del noclip. Con ella, el veredicto tiene que CAMBIAR al quitar el campo — y ese cambio es la prueba
 * de que el terreno es lo que está decidiendo.
 */
#include "net/height_field.h"
#include "core/planet/terrain_detail.h"

#include <dgs/packet.h>
#include <dgs/network.h>
#include <dgs/types.h>

#include <sys/socket.h>
#include <sys/wait.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// Binario propio, con su propio recuento: no usa `test_common.h` para no arrastrar el runner entero
// de `haruka_tests` a un test que ademas necesita el SDK de red.
static int g_pass = 0, g_fail = 0;
static void check(bool ok, const char* msg)
{
    if (ok) { ++g_pass; std::printf("  [ok]   %s\n", msg); }
    else    { ++g_fail; std::printf("  [FAIL] %s\n", msg); }
    std::fflush(stdout);
}
#define CHECK(c, m) check((c), (m))

namespace {

// ⚠️ BELOW 32768, and that is not arbitrary: Linux's default ephemeral range is 32768..60999, so a
// client socket in any concurrently running test can be handed one of these as its SOURCE port and
// this test's `listen()` then fails with EADDRINUSE. Seen once in the sibling repo's suite, where
// every port lived in 47xxx. SO_REUSEADDR does not protect against it.
const int kHeadPort = 21441;
const int kPersPort = 21442;
const int kValTcp   = 21443;
const int kValUdp   = 21444;

const double kR      = 6.371e6;    // radio de referencia
// ⚠️ METROS por chunk, NO kilómetros. `WorldQuery::chunkSizeX` está documentado como "km" en
// `external/dgs/include/dgs/game_module.h`, pero el módulo real lo usa como METROS:
// `gx = chunkX * chunkSizeX + pos[0]`, sin ×1000. Enviar 1.0 creyendo "1 km" coloca al jugador
// 1000 veces más cerca del centro del planeta, y entonces TODO sale rechazado — que es exactamente
// lo que pasó al escribir esto, con los cuatro veredictos a 0 y la contraprueba en rojo.
const double kChunkM = 1000.0;

std::atomic<bool> g_fin{false};

void fakeHead(std::atomic<bool>& listo)
{
    DGS::TCPSocket s;
    if (!s.listen(kHeadPort)) { listo = true; return; }
    { timeval ta{}; ta.tv_usec = 200000;
      setsockopt(s.getSocketFD(), SOL_SOCKET, SO_RCVTIMEO, &ta, sizeof(ta)); }
    listo = true;
    while (!g_fin) {
        const int fd = s.accept();
        if (fd < 0) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
        DGS::Command cmd{};
        cmd.chunkSizeX = (float)kChunkM; cmd.chunkSizeY = (float)kChunkM; cmd.chunkSizeZ = (float)kChunkM;
        cmd.port = kValTcp;
        std::snprintf(cmd.addr, sizeof(cmd.addr), "127.0.0.1");
        DGS::Packet p; p.pack(cmd);
        s.send(fd, p.getRawData(), p.getSize());
        uint8_t buf[8192];
        timeval tv{}; tv.tv_sec = 1;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        while (!g_fin) { if (s.receive(fd, buf, sizeof(buf)) <= 0) break; }
        s.closeClient(fd);
    }
}

void fakePersistence(std::atomic<bool>& listo)
{
    DGS::TCPSocket s;
    if (!s.listen(kPersPort)) { listo = true; return; }
    { timeval ta{}; ta.tv_usec = 200000;
      setsockopt(s.getSocketFD(), SOL_SOCKET, SO_RCVTIMEO, &ta, sizeof(ta)); }
    listo = true;
    while (!g_fin) {
        const int fd = s.accept();
        if (fd < 0) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
        uint8_t buf[4096];
        timeval tv{}; tv.tv_usec = 200000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        while (!g_fin) s.receive(fd, buf, sizeof(buf));
        s.closeClient(fd);
    }
}

/// Parte una posición global en chunk entero (km) + resto en metros, que es como viaja en el ABI.
/// El resto cabe holgado en `float`; la magnitud grande la lleva el entero. Meter 6,37e6 m en un
/// float directamente costaría ~0,5 m de resolución.
void partir(const glm::dvec3& g, DGS::EntityTransfer& e)
{
    const double m = kChunkM;
    e.chunkX = (int32_t)std::floor(g.x / m);
    e.chunkY = (int32_t)std::floor(g.y / m);
    e.chunkZ = (int32_t)std::floor(g.z / m);
    e.pos[0] = (float)(g.x - e.chunkX * m);
    e.pos[1] = (float)(g.y - e.chunkY * m);
    e.pos[2] = (float)(g.z - e.chunkZ * m);
}

/// Una petición de movimiento al validador. @return 1 legal · 0 violación · -1 sin respuesta.
int veredicto(DGS::TCPSocket& zona, uint32_t reqId, const glm::dvec3& antes, const glm::dvec3& ahora,
              float maxSpeed, float dt)
{
    DGS::ValidateRequest req{};
    req.requestId  = reqId;
    req.entityUuid = 7001;
    req.ownerZone  = 1;
    req.kind       = 0;
    req.entity.uuid = 7001;
    partir(ahora, req.entity);
    req.lastGX = (float)antes.x; req.lastGY = (float)antes.y; req.lastGZ = (float)antes.z;
    req.maxSpeed  = maxSpeed;
    req.dtSeconds = dt;

    DGS::Packet p; p.pack(req);
    if (!zona.send(zona.getSocketFD(), p.getRawData(), p.getSize())) return -1;

    uint8_t buf[4096];
    const int n = zona.receive(zona.getSocketFD(), buf, sizeof(buf));
    if (n <= 0) return -1;
    DGS::Packet r; r.setBuffer(buf, n);
    return (int)r.unpackValidateAck().verdict;
}

/// Levanta el nodo, pregunta por el valle y por dentro de la montaña, y lo mata.
/// @param campo  ruta del `.hfield`, o nullptr para NO instalarlo (la contraprueba).
bool tanda(const char* nodePath, const char* soPath, const char* campo,
           const glm::dvec3& dirValle, double hValle,
           const glm::dvec3& dirMonte, double hMonte,
           int& outValle, int& outMonte)
{
    outValle = outMonte = -1;

    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        if (!std::getenv("DGS_LIVE_VERBOSE")) std::freopen("/dev/null", "w", stdout);
        setenv("VALIDADOR_TCP_PORT", std::to_string(kValTcp).c_str(), 1);
        setenv("VALIDADOR_UDP_PORT", std::to_string(kValUdp).c_str(), 1);
        setenv("HEAD_SERVER_HOST", "127.0.0.1", 1);
        setenv("HEAD_SERVER_PORT", std::to_string(kHeadPort).c_str(), 1);
        setenv("PERSISTENCE_HOST", "127.0.0.1", 1);
        setenv("PERSISTENCE_PORT", std::to_string(kPersPort).c_str(), 1);
        setenv("GAME_MODULE_SO", soPath, 1);
        setenv("GAME_PLANET_RADIUS", std::to_string(kR).c_str(), 1);
        setenv("GAME_SEED", "0", 1);
        if (campo) setenv("HARUKA_RULES_HEIGHT_FIELD", campo, 1);
        else       unsetenv("HARUKA_RULES_HEIGHT_FIELD");
        execl(nodePath, nodePath, (char*)nullptr);
        _exit(127);
    }

    DGS::TCPSocket zona;
    bool up = false;
    for (int i = 0; i < 200 && !up; ++i) {
        if (zona.connect("127.0.0.1", kValTcp)) { up = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (up) {
        timeval tv{}; tv.tv_sec = 3;
        setsockopt(zona.getSocketFD(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Velocidad holgada a propósito: aquí NO se prueba el límite de velocidad (eso ya lo cubre
        // `validator_e2e`), se prueba el TERRENO. Con 100 m/s de tope y 2 m de paso, el único
        // criterio que puede cambiar el veredicto es el suelo.
        const float vmax = 100.0f, dt = 1.0f;

        // De pie 1,5 m sobre el fondo del valle.
        const glm::dvec3 pieAntes = dirValle * (kR + hValle + 1.5);
        const glm::dvec3 pieAhora = dirValle * (kR + hValle + 1.5) + glm::normalize(glm::cross(
                                        dirValle, glm::dvec3(0, 0, 1))) * 2.0;
        outValle = veredicto(zona, 1, pieAntes, pieAhora, vmax, dt);

        // DENTRO de la montaña: 500 m bajo su cima, pero MUY por encima del radio de referencia —
        // que es justo lo que un validador que juzgue contra una esfera lisa deja pasar.
        const glm::dvec3 dentroA = dirMonte * (kR + hMonte - 500.0);
        const glm::dvec3 dentroB = dirMonte * (kR + hMonte - 500.0) + glm::normalize(glm::cross(
                                        dirMonte, glm::dvec3(0, 0, 1))) * 2.0;
        outMonte = veredicto(zona, 2, dentroA, dentroB, vmax, dt);
    }

    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);
    return up;
}

} // namespace

int main()
{
    std::printf("== dgs_live_terrain: modulo REAL + terreno REAL + nodo VIVO ==\n");
    signal(SIGPIPE, SIG_IGN);

    // ── ¿Está el nodo? ──────────────────────────────────────────────────────────────────────────
    // El nodo vive en el proyecto de red, que es OTRO repositorio a propósito. Si no está, este test
    // no se registra siquiera (lo decide CMake), así que llegar aquí sin él es un error de verdad.
    const char* nodePath = std::getenv("HARUKA_DGS_VALIDADOR");
    CHECK(nodePath && *nodePath, "HARUKA_DGS_VALIDADOR apunta al validador_node");
    if (!nodePath || !*nodePath) return 1;
    if (access(nodePath, X_OK) != 0) { CHECK(false, "el validador_node es ejecutable"); return 1; }

    char soAbs[PATH_MAX];
    const char* soRel = "./libharuka_rules.so";
    const char* soPath = realpath(soRel, soAbs) ? soAbs : nullptr;
    CHECK(soPath != nullptr, "libharuka_rules.so esta junto al test (lo construye el target haruka_rules)");
    if (!soPath) return 1;

    // ── El terreno: un valle y una montaña de cotas conocidas ───────────────────────────────────
    // Mismo procedimiento que `dgs_ground_matches_engine`: una MESETA de 5x5 texeles, no un pico
    // suelto, porque el muestreo es bilineal y un texel aislado no da la cota que uno cree.
    const int W = 256, H = 128;
    Haruka::Net::HeightField f;
    f.w = W; f.h = H; f.baseRadiusM = (float)kR;
    f.data.assign((size_t)W * H, 0.0f);

    const glm::dvec3 dirValle = glm::normalize(glm::dvec3( 0.3,  0.9, 0.2));
    const glm::dvec3 dirMonte = glm::normalize(glm::dvec3(-0.5,  0.1, 0.8));
    auto stamp = [&](const glm::dvec3& d, float metros) {
        const glm::vec2 uv = Haruka::Planet::equirectUV(glm::vec3(d));
        const int cx = (int)std::floor(uv.x * W), cy = (int)std::floor(uv.y * H);
        for (int j = -2; j <= 2; ++j) for (int i = -2; i <= 2; ++i) {
            const int x = ((cx + i) % W + W) % W;
            const int y = (cy + j < 0) ? 0 : (cy + j >= H ? H - 1 : cy + j);
            f.data[(size_t)y * W + x] = metros;
        }
    };
    stamp(dirValle, -400.0f);
    stamp(dirMonte, 2500.0f);

    const std::string ruta = "/tmp/haruka_dgs_live.hfield";
    CHECK(Haruka::Net::saveHeightField(ruta, f), "se escribe el .hfield que leera el nodo");

    // ⚠️ LA COTA QUE MANDA NO ES LA QUE SE ESTAMPA. `HeightField::heightAt` —gemelo de
    // `TerrestrialPlanet::sampleHeight`, que es lo que usa el modulo— suma DETALLE ANALITICO sobre el
    // campo horneado: donde se estampan -400 m, la superficie real esta a -332. Colocar al jugador
    // respecto al valor crudo lo dejaba 66 m BAJO TIERRA, y el validador lo expulsaba con razon: el
    // escenario estaba mal, no la regla. Se pregunta la cota de verdad y se coloca respecto a ella.
    const double cotaValle = f.heightAt(dirValle);
    const double cotaMonte = f.heightAt(dirMonte);
    std::printf("    cota REAL del suelo  ·  valle %.1f m  ·  monte %.1f m  (estampado: -400 / 2500)\n",
                cotaValle, cotaMonte);
    CHECK(cotaValle < -200.0 && cotaMonte > 2000.0,
          "el campo entregado da un valle hondo y una montaña alta");

    std::atomic<bool> h1{false}, h2{false};
    std::thread th(fakeHead, std::ref(h1));
    std::thread tp(fakePersistence, std::ref(h2));
    while (!h1 || !h2) std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // ── (1) CON el campo: el suelo de verdad ────────────────────────────────────────────────────
    int vConCampo = -1, mConCampo = -1;
    const bool ok1 = tanda(nodePath, soPath, ruta.c_str(),
                           dirValle, cotaValle, dirMonte, cotaMonte, vConCampo, mConCampo);
    CHECK(ok1, "el validador arranca con el modulo REAL y acepta a la zona");
    CHECK(vConCampo == 1,
          "CON campo: un jugador de pie en el fondo del valle es LEGAL");
    CHECK(mConCampo == 0,
          "CON campo: un jugador 500 m DENTRO de la montaña es VIOLACION (noclip cazado)");

    // ── (2) SIN el campo: la contraprueba ───────────────────────────────────────────────────────
    // Mismo nodo, mismo modulo, mismas peticiones — solo cambia que el suelo horneado no esta. Si el
    // veredicto NO cambia, es que el terreno no estaba decidiendo nada y (1) era casualidad.
    int vSinCampo = -1, mSinCampo = -1;
    const bool ok2 = tanda(nodePath, soPath, nullptr,
                           dirValle, cotaValle, dirMonte, cotaMonte, vSinCampo, mSinCampo);
    CHECK(ok2, "el validador arranca tambien SIN campo de altura");
    CHECK(mSinCampo != mConCampo,
          "CONTRAPRUEBA: quitar el campo CAMBIA el veredicto del noclip (luego decide el terreno)");
    std::printf("    veredictos  ·  valle: con campo %d / sin campo %d"
                "   ·  dentro de la montaña: con campo %d / sin campo %d\n",
                vConCampo, vSinCampo, mConCampo, mSinCampo);

    g_fin = true;
    th.join(); tp.join();
    std::remove(ruta.c_str());

    std::printf("\n== dgs_live_terrain: %d OK · %d FALLOS ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
