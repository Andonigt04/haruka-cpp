// ================================================================================================
// Módulo de reglas POR DEFECTO del MOTOR para el DGS (implementa el ABI dgs_game_module_v1).
//
// Arquitectura: el MOTOR trae la física/validación por defecto que el DGS carga (dlopen). Es GL-FREE
// (solo el ABI del DGS + haruka_simbase = el sampler de terreno CPU + glm) → arranca en un servidor
// headless. Un PROYECTO puede sustituirlo con reglas propias (un script del juego) vía GAME_MODULE_SO;
// si no, se usa ESTE. La MISMA física servirá al cliente para predicción.
// ================================================================================================
#include "include/dgs/game_module.h"
#include "core/terrain/terrain_sample.h"   // haruka_simbase: física por defecto (sampler de terreno)
#include "game/construction/construction.h"    // EL MISMO núcleo de colocación que usa el cliente
#include "physics/world_provider.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <cstdio>
#include <cmath>
#include <cstring>
#include "core/logger.h"

namespace
{
    // SUELO MÍNIMO del terreno, cacheado por celda. Resuelve DOS problemas medidos a la vez:
    //
    //  (1) FALSOS POSITIVOS. El cliente camina sobre la MALLA; el servidor no tiene malla y valida
    //      contra el ANALÍTICO. La malla es una interpolación (bilineal + morph CDLOD) de muestras del
    //      analítico, así que SUAVIZA: medido con `haruka_tests earthdiag`, llega a quedar 5.12 m POR
    //      DEBAJO del analítico. El margen fijo era de 5.0 m → el servidor expulsaba como noclip a
    //      jugadores LEGÍTIMOS parados en el suelo que ven. Un margen constante no puede acotar esto
    //      porque el suavizado depende del LOD del cliente, que el servidor no conoce.
    //      Lo que SÍ lo acota: al ser una combinación CONVEXA de analíticos vecinos, la malla nunca baja
    //      del MÍNIMO local del analítico. Medido: con radio 32 m el peor caso es 0.00 m — acota.
    //
    //  (2) COSTE. `sampleTerrainV2` cuesta ~68 µs (medido, 200k llamadas): tomar el mínimo con 9 muestras
    //      por validación serían ~0.6 ms POR JUGADOR Y TICK, inviable. Pero el terreno es ESTÁTICO y
    //      determinista → el mínimo de una celda se calcula UNA vez y vale para siempre. Amortizado sale
    //      más barato que el código anterior, que muestreaba en CADA validación.
    struct FloorCache
    {
        // ⚠️ El radio del vecindario es una FRACCIÓN del radio del planeta, no una distancia fija. El
        // suavizado no mide 32 m «porque sí»: lo fija el tamaño de celda del LOD, que se obtiene
        // subdividiendo el planeta → escala CON el radio. Puesto en metros absolutos, en un planeta
        // pequeño (5 km) el vecindario se come el relieve entero y el mínimo local baja tanto que un
        // noclip de 20 m pasaba desapercibido (lo cazó el test `dgs_rules_module`).
        // Valor calibrado con la medida de la Tierra: 32 m / 6371 km.
        static constexpr double kCellFrac = 5.0e-6;
        static constexpr size_t kMaxCells = 1u << 16;  // techo de memoria (~1 MB); al llenarse se vacía

        std::unordered_map<uint64_t, float> cells;
        static double cellM(double R) { return R * kCellFrac; }

        // Clave: el punto de superficie cuantizado a la rejilla 3D de la celda. Estable y sin
        // trigonometría (una rejilla 3D corta la esfera en celdas de ~lado²; la uniformidad exacta da
        // igual aquí).
        static uint64_t key(const glm::dvec3& dir, double R) {
            const glm::dvec3 p = dir * R;
            const double s = cellM(R);
            const auto q = [s](double v) { return (int64_t)std::floor(v / s); };
            const uint64_t a = (uint64_t)(q(p.x) & 0x1FFFFF), b = (uint64_t)(q(p.y) & 0x1FFFFF),
                           c = (uint64_t)(q(p.z) & 0x1FFFFF);
            return a | (b << 21) | (c << 42);
        }

        // Mínimo del analítico en la celda MÁS un anillo de holgura: se muestrea una rejilla 3×3 que
        // abarca ±lado alrededor del centro, así cualquier punto DENTRO de la celda tiene al menos un
        // lado de vecindario cubierto en todas las direcciones (que es lo que se midió que acota).
        double minElevM(const glm::dvec3& dir, const Haruka::WorldGenParams& W, double R) {
            const uint64_t k = key(dir, R);
            auto it = cells.find(k);
            if (it != cells.end()) return (double)it->second;

            glm::dvec3 t1 = glm::normalize(glm::cross(dir, std::abs(dir.y) < 0.99 ? glm::dvec3(0,1,0)
                                                                                  : glm::dvec3(1,0,0)));
            const glm::dvec3 t2 = glm::cross(dir, t1);
            const double a = kCellFrac;                        // lado de celda → radianes (= lado/R)
            double lo = 1e30;
            for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) {
                const glm::dvec3 nd = glm::normalize(dir + (t1 * (double)i + t2 * (double)j) * a);
                lo = std::min(lo, (double)Haruka::sampleTerrainV2(glm::vec3(nd), W, R).elevKm * 1000.0);
            }
            if (cells.size() >= kMaxCells) cells.clear();      // techo duro: un jugador que recorre el
            cells.emplace(k, (float)lo);                       // planeta no puede hacerla crecer sin fin
            return lo;
        }
    };

    // --- ZONA: todo el estado autoritativo cuelga de aquí -------------------------------------
    // Antes esto eran variables GLOBALES del módulo, y eso rompía tres cosas: un nodo no podía servir
    // dos zonas sin mezclarlas, no había nada identificable que reasignar a otro nodo, y el estado
    // moría al descargar la librería sin orden ni forma de liberarlo antes.
    struct Zone
    {
        Haruka::Construction::ConstructionState build;   // piezas colocadas (autoritativo)
        FloorCache floor;                                 // suelo mínimo por celda (ver FloorCache)
        std::mutex mx;                                    // el host valida desde varios hilos
    };
    inline Zone* asZone(DGS::ZoneHandle z) { return static_cast<Zone*>(z); }

    // Ventana al mundo para el núcleo de colocación: el terreno sale del sampler ANALÍTICO — el MISMO
    // en cliente y servidor, sin GL y determinista. Es lo que hace que ambos decidan igual.
    struct RulesWorld : Haruka::Physics::IWorldProvider
    {
        glm::dvec3 center{0.0};
        double     radius = 0.0;
        Haruka::WorldGenParams params{};
        std::vector<Haruka::Physics::GravBody> grav;

        explicit RulesWorld(const DGS::WorldQuery& w)
            : center(w.planetCenter[0], w.planetCenter[1], w.planetCenter[2]), radius(w.planetRadius)
        {
            params = Haruka::deriveWorldParams(w.seed, w.planetRadius);
        }
        bool       hasActivePlanet()    const override { return radius > 0.0; }
        glm::dvec3 activePlanetCenter() const override { return center; }
        double     activePlanetRadius() const override { return radius; }
        const std::vector<Haruka::Physics::GravBody>& gravBodies() const override { return grav; }
        double     terrainHeightAt(const glm::dvec3& p) const override
        {
            const glm::dvec3 d = p - center;
            const double len = glm::length(d);
            if (len < 1e-6) return 0.0;
            const Haruka::TerrainSample t =
                Haruka::sampleTerrainV2(glm::vec3(d / len), params, radius);
            return (double)t.elevKm * 1000.0;
        }
    };

    // El host envía el catálogo al cargar el mundo. Sin él no se puede validar colocación: no se
    // sabría el tamaño de cada pieza, y sin tamaño no hay solape que comprobar.
    void setPieceCatalog(DGS::ZoneHandle zh, const DGS::PieceDesc* types, uint16_t n)
    {
        Zone* z = asZone(zh);
        if (!z) return;
        std::vector<Haruka::Construction::PieceType> cat;
        cat.reserve(n);
        for (uint16_t i = 0; i < n && types; ++i) {
            Haruka::Construction::PieceType t;
            t.id          = types[i].typeId;
            t.halfExtents = glm::dvec3(types[i].half[0], types[i].half[1], types[i].half[2]);
            t.supports    = types[i].supports;
            t.needsFlat   = types[i].needsFlat != 0;
            cat.push_back(t);
        }
        std::lock_guard<std::mutex> lk(z->mx);
        z->build.setCatalog(std::move(cat));
    }

    // Valida (y si procede REGISTRA) una colocación. Devuelve 1 = aceptada.
    int validatePlace(Zone* z, const DGS::ActionHeader& h, const uint8_t* blob, uint16_t n, const DGS::WorldQuery* w)
    {
        if (!z || !w || n < sizeof(DGS::ActionHeader) + sizeof(DGS::PlaceAction)) return 0;
        DGS::PlaceAction p{};
        std::memcpy(&p, blob + sizeof(DGS::ActionHeader), sizeof(p));

        for (int i = 0; i < 3; ++i) if (!std::isfinite(p.pos[i]))  return 0;
        for (int i = 0; i < 4; ++i) if (!std::isfinite(p.quat[i])) return 0;

        const glm::dvec3 pos(p.pos[0], p.pos[1], p.pos[2]);
        glm::dquat q(p.quat[3], p.quat[0], p.quat[1], p.quat[2]);
        const double qlen = glm::length(q);
        if (qlen < 1e-9) return 0;
        q /= qlen;

        // ALCANCE: no se coloca a distancia arbitraria del punto declarado de la acción.
        const glm::dvec3 at(h.at[0], h.at[1], h.at[2]);
        if (glm::length(pos - at) > 32.0) return 0;

        RulesWorld world(*w);
        std::lock_guard<std::mutex> lk(z->mx);
        // EL MISMO `validate` que el cliente: no solapa otra pieza, está anclada, apoyo correcto…
        if (z->build.validate(p.typeId, pos, q, world) != Haruka::Construction::Placement::Ok) return 0;
        z->build.add(p.typeId, pos, q, world);   // aceptada → pasa a ser estado autoritativo
        return 1;
    }

    int validateMove(DGS::ZoneHandle zh, const DGS::MoveSample* s, const DGS::WorldQuery* w)
    {
        if (!s || !s->now || !w) return 1;
        const float dt = s->dtSeconds;
        if (dt <= 0.0f || dt > 2.0f) return 1;   // sin muestra fiable → no penalizar

        // Posición GLOBAL reportada (m). El host manda chunk + pos coherentes en metros.
        const double gx = (double)s->now->chunkX * w->chunkSizeX + s->now->pos[0];
        const double gy = (double)s->now->chunkY * w->chunkSizeY + s->now->pos[1];
        const double gz = (double)s->now->chunkZ * w->chunkSizeZ + s->now->pos[2];

        // (1) VELOCIDAD / TELEPORT: el salto desde el último punto no puede superar maxSpeed·dt.
        const double dx = gx - s->lastGX, dy = gy - s->lastGY, dz = gz - s->lastGZ;
        const double distSq = dx * dx + dy * dy + dz * dz;
        const double budget = (double)s->maxSpeed * dt + 1.0;   // +1 m (latencia/tolerancia)
        if (distSq > budget * budget) return 0;                 // speed-hack / teleport

        // (2) TERRENO: no atravesar el suelo (noclip). Se muestrea el terreno ANALÍTICO — el MISMO
        // sampler CPU que usa el cliente, sin GL, determinista → cliente y host coinciden. Antes esto
        // era un TODO; ahora el host provisiona el planeta en el WorldQuery.
        if (w->planetRadius > 1.0) {
            const glm::dvec3 center(w->planetCenter[0], w->planetCenter[1], w->planetCenter[2]);
            const glm::dvec3 rel = glm::dvec3(gx, gy, gz) - center;
            const double playerR = glm::length(rel);
            if (playerR > 1.0) {
                Haruka::WorldGenParams W = Haruka::deriveWorldParams(w->seed, w->planetRadius);
                W.reliefStrength = w->reliefStrength;
                W.profile        = w->profile;
                const glm::dvec3 dir = rel / playerR;
                // Suelo = MÍNIMO local del analítico (ver FloorCache): acota por debajo la malla sobre la
                // que camina el cliente, así que un jugador legítimo NUNCA cae por debajo. El margen
                // pequeño que queda es solo holgura numérica, no un parche para el desfase malla↔analítico
                // — eso ya lo cubre el mínimo.
                double surface;
                if (Zone* z = asZone(zh)) {
                    std::lock_guard<std::mutex> lk(z->mx);
                    surface = w->planetRadius + z->floor.minElevM(dir, W, w->planetRadius);
                } else {   // sin zona (host antiguo): comportamiento anterior, margen constante
                    surface = w->planetRadius
                            + (double)Haruka::sampleTerrainV2(glm::vec3(dir), W, w->planetRadius).elevKm * 1000.0
                            - 5.0;
                }
                if (playerR < surface - 2.0) return 0;   // por debajo del suelo mínimo = noclip/tunneling
            }
        }
        return 1;
    }

    // Validación GENÉRICA de una acción. El blob es OPACO: solo se asume que ABRE con un ActionHeader
    // (formato común del motor); el payload que sigue es del juego y NO se toca. Se comprueban SOLO
    // invariantes que no requieren estado de mundo — malformación o valores absurdos, que ningún juego
    // legítimo emite. La semántica con estado (¿posee el ítem?, ¿está en alcance?, ¿es el dueño del
    // inventario?) la resuelve el módulo del PROYECTO, que sí conoce la estructura. NULO → host acepta.
    int validateAction(DGS::ZoneHandle zh, uint32_t /*actor*/, const uint8_t* blob, uint16_t n, const DGS::WorldQuery* w)
    {
        if (!blob || n < sizeof(DGS::ActionHeader)) return 0;   // sin encabezado válido → rechazo

        DGS::ActionHeader h{};
        std::memcpy(&h, blob, sizeof(h));                       // sin alias: el blob no está alineado

        if (h.verb == DGS::ACT_NONE || h.verb >= DGS::ACT__COUNT) return 0;  // verbo desconocido
        if (h.flags != 0) return 0;                                          // bits reservados puestos
        if (!std::isfinite(h.amount) || h.amount < 0.0f) return 0;           // cantidad absurda
        // Cotas de cordura por verbo (un cliente honesto nunca las cruza; un cheat sí).
        if (h.verb == DGS::ACT_DAMAGE   && h.amount > 1.0e6f)  return 0;     // daño imposible
        if (h.verb == DGS::ACT_TRANSFER && h.amount > 1.0e5f)  return 0;     // stack imposible
        for (int i = 0; i < 3; ++i) if (!std::isfinite(h.at[i])) return 0;   // punto no finito

        // COLOCAR es el único verbo que este módulo entiende A FONDO: es geometría del MOTOR
        // (HarukaConstruction), no semántica de un juego. Se valida con el mismo núcleo que el cliente.
        if (h.verb == DGS::ACT_PLACE) return validatePlace(asZone(zh), h, blob, n, w);

        return 1;   // bien formada; el resto (estado) lo decide el módulo del proyecto
    }

    // --- CICLO DE VIDA de la zona ---------------------------------------------------------------
    DGS::ZoneHandle createZone(const DGS::WorldQuery* /*w*/) { return new Zone(); }
    void destroyZone(DGS::ZoneHandle zh) { delete asZone(zh); }

    // --- TRASPASO por REGIÓN --------------------------------------------------------------------
    // Formato: cabecera {magia, versión, nº piezas} + una entrada POD por pieza. Simple y versionado a
    // propósito: el módulo es dueño del formato y el host solo transporta bytes. Región = esfera
    // (centro + radio), que es lo natural en un mundo planetario.
    struct BlobHead { uint32_t magic; uint16_t version; uint16_t pad; uint32_t count; };
    struct BlobPiece { uint16_t typeId; uint16_t pad; double pos[3]; double quat[4]; };
    constexpr uint32_t kBlobMagic = 0x48524750u;   // 'HRGP'
    constexpr uint16_t kBlobVer   = 1;

    bool inRegion(const glm::dvec3& p, const double c[3], double r)
    {
        const glm::dvec3 cc(c[0], c[1], c[2]);
        return glm::length(p - cc) <= r;
    }

    size_t serializeRegion(DGS::ZoneHandle zh, const double center[3], double radius,
                           uint8_t* out, size_t cap)
    {
        Zone* z = asZone(zh);
        if (!z || !center || radius <= 0.0) return 0;
        std::lock_guard<std::mutex> lk(z->mx);

        std::vector<BlobPiece> sel;
        for (const auto& [id, pc] : z->build.pieces()) {
            if (!inRegion(pc.position, center, radius)) continue;
            BlobPiece b{};
            b.typeId = pc.typeId;
            b.pos[0] = pc.position.x; b.pos[1] = pc.position.y; b.pos[2] = pc.position.z;
            b.quat[0] = pc.orientation.x; b.quat[1] = pc.orientation.y;
            b.quat[2] = pc.orientation.z; b.quat[3] = pc.orientation.w;
            sel.push_back(b);
        }
        const size_t need = sizeof(BlobHead) + sel.size() * sizeof(BlobPiece);
        if (!out || cap < need) return need;          // preguntar tamaño / buffer insuficiente

        BlobHead hd{ kBlobMagic, kBlobVer, 0, (uint32_t)sel.size() };
        std::memcpy(out, &hd, sizeof(hd));
        if (!sel.empty())
            std::memcpy(out + sizeof(hd), sel.data(), sel.size() * sizeof(BlobPiece));
        return need;
    }

    int mergeRegion(DGS::ZoneHandle zh, const uint8_t* in, size_t n)
    {
        Zone* z = asZone(zh);
        if (!z || !in || n < sizeof(BlobHead)) return 0;
        BlobHead hd{};
        std::memcpy(&hd, in, sizeof(hd));
        if (hd.magic != kBlobMagic || hd.version != kBlobVer) return 0;   // formato ajeno o viejo
        if (n < sizeof(BlobHead) + (size_t)hd.count * sizeof(BlobPiece)) return 0;

        std::lock_guard<std::mutex> lk(z->mx);
        for (uint32_t i = 0; i < hd.count; ++i) {
            BlobPiece b{};
            std::memcpy(&b, in + sizeof(BlobHead) + (size_t)i * sizeof(BlobPiece), sizeof(b));
            // Se INCORPORA tal cual: ya lo validó el nodo que la aceptó. Revalidar aquí rechazaría
            // piezas legítimas cuyo apoyo se quedó en el nodo vecino (justo al borde de la región).
            z->build.addRaw(b.typeId, glm::dvec3(b.pos[0], b.pos[1], b.pos[2]),
                            glm::dquat(b.quat[3], b.quat[0], b.quat[1], b.quat[2]));
        }
        return 1;
    }

    void dropRegion(DGS::ZoneHandle zh, const double center[3], double radius)
    {
        Zone* z = asZone(zh);
        if (!z || !center || radius <= 0.0) return;
        std::lock_guard<std::mutex> lk(z->mx);
        std::vector<uint64_t> doomed;
        for (const auto& [id, pc] : z->build.pieces())
            if (inRegion(pc.position, center, radius)) doomed.push_back(id);
        for (uint64_t id : doomed) z->build.erasePiece(id);
    }

    // Self-check: al cargar el módulo, prueba que el sampler del motor está VIVO dentro del .so
    // (GL-free) — deriva params de un seed demo y muestrea una dirección. Log una vez, no valida nada.
    void selfCheck()
    {
        Haruka::WorldGenParams W = Haruka::deriveWorldParams(42u, 1.5e6);
        Haruka::TerrainSample  t = Haruka::sampleTerrainV2(glm::normalize(glm::vec3(1, 1, 1)), W, 1.5e6);
        HARUKA_LOGI("Rules", "sampler del motor OK (GL-free): elevKm=%.4f en dir(1,1,1)", t.elevKm);
    }

    const DGS::GameModule g_module = {
        DGS::GAME_MODULE_ABI, "haruka-default",
        &createZone, &destroyZone,
        &validateMove, &validateAction, &setPieceCatalog,
        &serializeRegion, &mergeRegion, &dropRegion
    };
}

extern "C" const DGS::GameModule* dgs_game_module_v1(void)
{
    static const bool once = [] { selfCheck(); return true; }();
    (void)once;
    return &g_module;
}
