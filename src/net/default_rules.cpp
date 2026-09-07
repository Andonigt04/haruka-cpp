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
#include "physics/physics_engine.h"   // LA MISMA fisica que el cliente: Jolt tambien en el servidor
#include "core/planet/soi.h"           // kEarthMeanDensity: la masa del planeta, igual que el motor
#include "net/height_field.h"          // EL SUELO DE VERDAD: el bake que pisa el cliente, sin GL

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include "core/logger.h"

extern "C" int haruka_rules_load_height_field(const char* path);

// ⚠️ EL CATALOGO DE PIEZAS, Y POR QUE NO LO MANDA EL CLIENTE.
//
// `validatePlace` corre `Construction::validate`, que necesita saber lo GRANDE que es cada pieza: sin
// tamano no hay solape que comprobar. El ABI tiene `setPieceCatalog` para eso y NINGUN nodo lo llama
// nunca — el modulo lo implementaba y nadie le daba datos, asi que toda colocacion se rechazaba
// contra un catalogo vacio. Medido: el clúster pedia la validacion y el objeto no aparecia jamas.
//
// El reflejo facil seria que lo mandara el cliente al conectarse. Seria el MISMO agujero que se acaba
// de cerrar con `stats.speed`: quien declara el catalogo declara que su mesa mide un centimetro y la
// mete donde quiera. Es dato de DESPLIEGUE — se exporta del juego una vez, se envia con el servidor,
// y el modulo lo lee de disco como ya hace con el campo de altura.
//
// Formato deliberadamente tonto (binario, sin dependencias): "HKPC" + version + n, y n registros de
// {typeId, supports, needsFlat, half[3]}. Meter un parser de JSON aqui seria arrastrar una dependencia
// a un .so que se carga dentro de un nodo de produccion, para leer un fichero que genera una
// herramienta nuestra.
extern "C" int haruka_rules_load_piece_catalog(const char* path);

// El catalogo cargado de disco, si lo hay. Igual que el campo de altura: el host lo entrega UNA vez,
// antes de crear zonas, y a partir de ahi solo se lee.
static std::mutex g_catMx;
static std::vector<Haruka::Construction::PieceType> g_catalog;

namespace
{
    // ── LA FUENTE DE SUELO DEL MODULO ───────────────────────────────────────────────────────────
    //
    // El campo horneado si el host lo ha entregado; el analitico si no. UNA sola funcion, para que no
    // vuelva a haber dos respuestas a "¿a que cota esta el suelo aqui?" — que es de donde salio todo
    // el problema (ver `net/height_field.h`).
    //
    // ⚠️ Vive en un `shared_ptr` inmutable: el host lo entrega una vez, antes de crear zonas, y a
    // partir de ahi solo se LEE desde los hilos de validacion. Reemplazarlo publica un puntero nuevo;
    // las zonas vivas siguen con el suyo y nadie muta un vector bajo un lector.
    std::mutex g_fieldMx;
    std::shared_ptr<const Haruka::Net::HeightField> g_field;

    std::shared_ptr<const Haruka::Net::HeightField> currentField()
    {
        std::lock_guard<std::mutex> lk(g_fieldMx);
        return g_field;
    }

    /// Cota del suelo (m sobre el nivel del mar) en una direccion unitaria.
    double groundElevM(const std::shared_ptr<const Haruka::Net::HeightField>& f,
                       const glm::dvec3& dir, const Haruka::WorldGenParams& W, double R)
    {
        if (f && f->valid()) return f->heightAt(dir);   // el MISMO bake que pisa el cliente
        // Sin campo: el analitico, que en esta rama es un stub y devuelve 0 (esfera lisa). Se avisa
        // UNA vez, porque un servidor que valida contra el nivel del mar sin decirlo es peor que uno
        // que no valida.
        static bool warned = false;
        if (!warned) {
            warned = true;
            HARUKA_LOGW("Rules",
                "sin campo de altura: el noclip se valida contra el ANALITICO, que en esta rama es un "
                "stub (esfera lisa a nivel del mar). Entrega el bake con "
                "haruka_rules_set_height_field()/haruka_rules_load_height_field(), o "
                "HARUKA_RULES_HEIGHT_FIELD=<ruta>.");
        }
        return (double)Haruka::sampleTerrainV2(glm::vec3(dir), W, R).elevKm * 1000.0;
    }

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
    //
    // ⚠️⚠️ SIN CAMPO DE ALTURA, TODO LO DE ARRIBA DESCRIBE UN SAMPLER QUE YA NO ESTA.
    // `Haruka::sampleTerrainV2` es un STUB en esta rama (`terrain sampler removed`, devuelve 0).
    // Medido el 2026-09-01, planeta de radio terrestre, 512 direcciones por angulo aureo:
    //
    //     RELIEVE del analitico:             +0,00 m .. +0,00 m
    //     lo que acota el minimo local:      min 0,0000 · medio 0,0000 · MAX 0,0000 m
    //
    // O sea que sin campo el validador juzga el noclip contra una ESFERA LISA a nivel del mar, y esto
    // calcula el minimo de una constante. Se intento tapar dos veces con un margen (5,0 m y 2,0 m) y
    // las dos acabaron echando a jugadores honestos o dejando pasar noclips.
    //
    // CON campo entregado (`haruka_rules_set_height_field`, ver `net/height_field.h`) esto muestrea EL
    // MISMO bake que pisa el cliente y vuelve a hacer lo que dice su documentacion: acotar por debajo
    // la malla, que es una combinacion convexa de muestras del mismo campo. El aviso lo suelta
    // `groundElevM` una sola vez cuando el host no lo entrego.
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
        double minElevM(const glm::dvec3& dir, const Haruka::WorldGenParams& W, double R,
                        const std::shared_ptr<const Haruka::Net::HeightField>& f) {
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
                lo = std::min(lo, groundElevM(f, nd, W, R));
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
    struct RulesWorld;   // definida abajo: la ventana al mundo que consume la simulacion

    /**
     * @brief Estado autoritativo de una zona. Con SIMULACION propia desde 2026-09-01.
     *
     * ⚠️ ANTES LA ZONA NO SIMULABA: `GameModule::step` iba a NULO y el mundo avanzaba por lo que
     * mandaba el cliente; el servidor solo decia si o no. Y decia mal, porque juzgaba contra el
     * sampler ANALITICO mientras el cliente camina sobre la MALLA — su propia nota mide que la malla
     * llega a quedar **5,12 m por debajo**, y con el margen fijo de 5,0 m el servidor expulsaba a
     * jugadores LEGITIMOS. De ahi la `FloorCache`, que existe para tapar esa asimetria de modelo.
     *
     * Con Jolt aqui, los dos pisan la MISMA superficie y el margen deja de tener que cubrir una
     * diferencia de modelo. Lo que queda es lo que de verdad no puede coincidir: cada uno ancla su
     * malla bajo SU jugador, y eso son **8 mm** medidos (`physics_two_instances_agree`).
     *
     * ⚠️ El DGS es OTRO PROYECTO: aqui solo se implementa su ABI (`dgs_game_module_v1`), no se toca.
     * `step` ya existia en el vtable — estaba a nulo, no faltaba.
     */
    struct Zone
    {
        Haruka::Construction::ConstructionState build;   // piezas colocadas (autoritativo)
        FloorCache floor;                                 // suelo mínimo por celda (ver FloorCache)
        std::mutex mx;                                    // el host valida desde varios hilos

        std::unique_ptr<RulesWorld>              world;   // ventana al terreno, viva con la zona
        std::unique_ptr<Haruka::Physics::PhysicsEngine> sim;
        /// Un cuerpo por entidad POSEIDA. El `EntityTransfer` es transporte; el estado (y sobre todo
        /// la VELOCIDAD, que el transporte no lleva) vive aqui entre ticks.
        std::unordered_map<uint32_t, std::shared_ptr<Haruka::Physics::RigidBody>> bodies;
        double chunk[3] = { 0.0, 0.0, 0.0 };   // m por chunk, para des-cuantizar la posicion global
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
        std::shared_ptr<const Haruka::Net::HeightField> field;   // el bake, si el host lo entrego

        explicit RulesWorld(const DGS::WorldQuery& w)
            : center(w.planetCenter[0], w.planetCenter[1], w.planetCenter[2]), radius(w.planetRadius)
        {
            params = Haruka::deriveWorldParams(w.seed, w.planetRadius);
            field  = currentField();   // se fija al crear la zona y no cambia bajo ella
            // ⚠️ `grav` ESTABA VACIO, y con Jolt eso no es "sin gravedad": es que el cuerpo no cae y
            // cualquier validacion contra la fisica daria por bueno flotar. La masa sale del MISMO
            // proxy de densidad media terrestre que usa el motor (`kEarthMeanDensity`), asi que un
            // cuerpo de radio terrestre da 9,82 m/s2 en superficie — el servidor no puede tener otra
            // gravedad que el cliente.
            if (radius > 0.0) {
                const double mass = Haruka::Planet::kEarthMeanDensity * (4.0 / 3.0)
                                  * 3.14159265358979323846 * radius * radius * radius;
                grav.push_back({ center, mass, radius, 0.0, -1 });
            }
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
            // ⚠️ EL MISMO SUELO QUE LA VALIDACION. Si la simulacion del servidor cayera sobre otra
            // superficie que el control de noclip, el servidor se contradiria a si mismo: avanzaria
            // al jugador hasta una cota que su propio validador rechaza.
            return groundElevM(field, d / len, params, radius);
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

    // ── EL TECHO DE DIVERGENCIA LEGITIMA ────────────────────────────────────────────────────────
    //
    // ⚠️ AQUI HABIA DOS CONSTANTES INVENTADAS: un `+1,0 m` en el presupuesto de velocidad y un
    // `-2,0 m` en el del suelo, las dos sin procedencia. Un umbral anti-trampa mal puesto falla en las
    // dos direcciones — por debajo expulsa a jugadores honestos (ya paso: el margen de 5 m contra el
    // desfase malla/analitico echaba a gente parada en el suelo que veia), y por encima deja pasar al
    // tramposo.
    //
    // Ahora es una SUMA DE TERMINOS MEDIDOS. Cada uno tiene su cifra y su test:
    //
    //   · ANCLA (`kAnchorM`) — cliente y servidor construyen su malla de colision bajo SU jugador, y
    //     eso es inevitable. Medido: **8,1 mm** (`physics_two_instances_agree`).
    //   · UN SUB-PASO (`maxSpeed / kFixedHz`) — el acumulador de paso fijo deja un RESTO cuando el
    //     frame del cliente no cabe entero en el paso, asi que va hasta un sub-paso por delante o por
    //     detras. A 30 fps da 0 (1/30 es multiplo de 1/60); a 144 fps, **0,159 m**, que es exactamente
    //     `velocidad x 1/60`. Por eso NO es una constante en metros: escala con la velocidad.
    //   · LATENCIA — ya la cubre `maxSpeed·dt`, con el `dt` que MIDE el host.
    //
    // ⚠️ Y UN TERMINO QUE NO ESTA MEDIDO, DECLARADO COMO TAL. El cliente mueve al jugador con un
    // `CharacterVirtual` y el servidor con un rigido: no son el mismo integrador. Hasta que eso se
    // mida se reserva margen — pero CON NOMBRE, para que se vea que es deuda y no fisica.
    namespace Tolerance
    {
        constexpr double kAnchorM     = 0.010;  // 8,1 mm medidos, redondeados arriba
        constexpr double kFixedHz     = 60.0;   // el paso fijo de `PhysicsEngine::advance`
        constexpr double kUnmeasuredM = 0.50;   // DEUDA: personaje vs rigido, sin medir

        /// ⚠️ LO QUE EL PROPIO TRANSPORTE NO PUEDE REPRESENTAR. `DGS::MoveSample::lastG{X,Y,Z}` son
        /// **float en el ABI**, y un float a radio terrestre tiene 0,5 m de resolucion: el punto de
        /// referencia contra el que se mide el salto llega ya cuantizado. A 5 m/s y 1/60 s el
        /// presupuesto entero son 0,68 m, asi que no es un detalle — se come mas de la mitad, y sin
        /// contarlo el servidor expulsa por "teleport" a un jugador que NO SE HA MOVIDO.
        ///
        /// Lo caza `dgs_ground_matches_engine`, que puso a un jugador quieto en un valle a radio
        /// terrestre y lo vio expulsar. NO es una constante: es medio ulp del float EN ESA MAGNITUD,
        /// o sea una propiedad del formato, y se calcula.
        ///
        /// ⚠️ El arreglo de verdad es que `lastG*` sean `double`, o que viajen troceados como ya hace
        /// `EntityTransfer` (indice de chunk entero + resto en float). Pero el ABI es del DGS, que es
        /// OTRO PROYECTO: aqui se ACOTA el error, no se cambia el formato ajeno.
        inline double transportM(double gx, double gy, double gz)
        {
            auto halfUlp = [](double v) {
                const float f = (float)std::fabs(v);
                if (!std::isfinite(f)) return 0.0;
                return (double)(std::nextafterf(f, 3.4e38f) - f) * 0.5;
            };
            const double ex = halfUlp(gx), ey = halfUlp(gy), ez = halfUlp(gz);
            return std::sqrt(ex * ex + ey * ey + ez * ez);
        }

        inline double physicsM(double maxSpeed)
        {
            const double sp = (maxSpeed > 0.0 && std::isfinite(maxSpeed)) ? maxSpeed : 0.0;
            return kAnchorM + sp / kFixedHz + kUnmeasuredM;
        }
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
        // Lo que se puede recorrer en `dt` MAS el techo de divergencia legitima (ver arriba).
        const double budget = (double)s->maxSpeed * dt
                            + Tolerance::physicsM((double)s->maxSpeed)
                            + Tolerance::transportM(gx, gy, gz);   // `lastG*` son float en el ABI
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
                    surface = w->planetRadius
                            + z->floor.minElevM(dir, W, w->planetRadius, z->world ? z->world->field
                                                                                  : currentField());
                } else {   // sin zona (host antiguo): comportamiento anterior, margen constante
                    surface = w->planetRadius + groundElevM(currentField(), dir, W, w->planetRadius)
                            - 5.0;
                }
                // El MISMO techo que arriba: el suelo minimo ya acota el desfase malla/analitico
                // (ver `FloorCache`), asi que lo unico que queda por debajo de el es la fisica.
                // ⚠️ SIN campo de altura entregado, `surface` es el radio de referencia a pelo y este
                // umbral solo separa "bajo el nivel del mar" de "sobre el nivel del mar". Con campo,
                // es el suelo de verdad. Ver `groundElevM` y `net/height_field.h`.
                if (playerR < surface - Tolerance::physicsM((double)s->maxSpeed)) return 0;  // noclip
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

        // TIRAR algo no es COLOCAR una pieza: no hay catalogo, ni encaje, ni apoyo que comprobar. Lo
        // que habria que comprobar —que el objeto estuviera de verdad en tu inventario— es semantica
        // del juego, y este modulo no lleva inventarios. Asi que pasa si esta bien formada, y queda
        // ESCRITO que hoy cualquiera puede tirar cualquier cosa: la regla que falta es "¿lo tenias?".
        if (h.verb == DGS::ACT_DROP) return 1;

        return 1;   // bien formada; el resto (estado) lo decide el módulo del proyecto
    }

    // --- CICLO DE VIDA de la zona ---------------------------------------------------------------
    /**
     * @brief Crea la zona Y SU SIMULACION.
     *
     * ⚠️ EL `WorldQuery` SE COPIA AL MONTAR, no se guarda el puntero: el host dice que vive toda la
     * sesion, pero `RulesWorld` necesita los `WorldGenParams` derivados de la semilla y eso se hace
     * UNA vez, no por consulta. `deriveWorldParams` es el mismo camino que usa el cliente.
     */
    DGS::ZoneHandle createZone(const DGS::WorldQuery* w)
    {
        // La tercera via del campo de altura: un host que no conoce nuestros simbolos, pero un
        // servidor al que si le han dejado el fichero al lado. UNA vez, y solo si nadie lo instalo ya
        // por codigo — quien llama explicitamente manda sobre el entorno.
        static const bool envOnce = [] {
            if (currentField()) return true;
            if (const char* p = std::getenv("HARUKA_RULES_HEIGHT_FIELD"))
                if (*p) haruka_rules_load_height_field(p);
            return true;
        }();
        (void)envOnce;

        // Y el catalogo, por la misma via y por la misma razon: un servidor al que le han dejado el
        // fichero al lado. Sin el, `validatePlace` juzga contra una lista vacia y rechaza todo.
        static const bool catOnce = [] {
            if (const char* p = std::getenv("HARUKA_RULES_PIECE_CATALOG"))
                if (*p) haruka_rules_load_piece_catalog(p);
            return true;
        }();
        (void)catOnce;

        Zone* z = new Zone();
        // Si el despliegue dejo un catalogo, la zona nace con el. Sin esto `validatePlace` juzga
        // contra una lista vacia y rechaza cualquier colocacion — que es lo que pasaba.
        {
            std::lock_guard<std::mutex> lk(g_catMx);
            if (!g_catalog.empty()) z->build.setCatalog(g_catalog);
        }
        if (w) {
            z->world = std::make_unique<RulesWorld>(*w);
            z->chunk[0] = (double)w->chunkSizeX;
            z->chunk[1] = (double)w->chunkSizeY;
            z->chunk[2] = (double)w->chunkSizeZ;
            // ⚠️ LA SIMULACION SE MONTA AUNQUE NADIE LA USE TODAVIA, y es barato: `PhysicsEngine` no
            // construye suelo hasta que hay un cuerpo dinamico (ver `near` en su update). Montarla
            // aqui y no en el primer `step` evita tener que sincronizar la creacion perezosa con el
            // mutex que el host ya usa desde varios hilos.
            z->sim = std::make_unique<Haruka::Physics::PhysicsEngine>();
            z->sim->setWorldProvider(z->world.get());
        }
        return z;
    }

    /**
     * @brief Avanza UNA entidad que esta zona posee, con la MISMA fisica que corre el cliente.
     *
     * ── POR QUE ESTO CAMBIA LA VALIDACION, Y NO SOLO LA SIMULACION ──────────────────────────────
     *
     * `validateMove` juzga contra el sampler ANALITICO porque el servidor no tenia malla, y su propia
     * nota mide el precio: la malla del cliente llega a quedar **5,12 m por debajo** del analitico, el
     * margen fijo era 5,0 m, y el servidor **expulsaba a jugadores legitimos**. La `FloorCache` existe
     * para tapar esa asimetria de MODELO, no una de red.
     *
     * Con Jolt aqui los dos pisan la misma superficie. El techo de divergencia legitima deja de tener
     * que cubrir "la malla suaviza 5 m" y se queda en lo que de verdad no puede coincidir: cada uno
     * ancla su malla bajo SU jugador, y eso son **8 mm** (`physics_two_instances_agree`). Ese numero
     * es el umbral de trampa: lo que un cliente puede mentir sin que se note baja de metros a
     * milimetros.
     *
     * ⚠️ EL ESTADO VIVE EN LA ZONA, NO EN EL `EntityTransfer`. El transporte no lleva VELOCIDAD, asi
     * que si el cuerpo se reconstruyera en cada tick caeria siempre desde parado y no seria fisica:
     * seria una sucesion de saltos. Por eso `Zone::bodies` persiste entre ticks y el
     * `EntityTransfer` solo entra y sale.
     */
    void step(DGS::ZoneHandle zh, DGS::EntityTransfer* e, float dt, const DGS::WorldQuery* w)
    {
        Zone* z = asZone(zh);
        if (!z || !e || !w || !z->sim || !z->world || dt <= 0.0f) return;
        std::lock_guard<std::mutex> lk(z->mx);

        // Posicion GLOBAL, con la MISMA des-cuantizacion que `validateMove`. Escribirla dos veces
        // seria abrir la puerta a que el validador y el simulador situaran la entidad en dos sitios.
        const glm::dvec3 g((double)e->chunkX * z->chunk[0] + (double)e->pos[0],
                           (double)e->chunkY * z->chunk[1] + (double)e->pos[1],
                           (double)e->chunkZ * z->chunk[2] + (double)e->pos[2]);

        auto it = z->bodies.find(e->uuid);
        if (it == z->bodies.end()) {
            auto b = std::make_shared<Haruka::Physics::RigidBody>();
            b->position = g;
            b->radius   = 0.5;      // TODO: del catalogo de la entidad cuando el ABI lo lleve
            b->mass     = 70.0;
            b->isCharacter = false;
            b->name     = "e" + std::to_string(e->uuid);
            z->sim->addBody(b);
            it = z->bodies.emplace(e->uuid, b).first;
        }
        std::shared_ptr<Haruka::Physics::RigidBody>& body = it->second;

        // ⚠️ NO SE PISA LA POSICION DEL CUERPO CON LA DEL CLIENTE. Este `step` es el camino
        // AUTORITATIVO: si copiara lo que el cliente afirma, el servidor estaria simulando la mentira
        // del cliente y la validacion no serviria de nada. La afirmacion del cliente entra por
        // `validateMove`, que es donde se juzga; aqui solo se avanza lo que el servidor cree.
        z->sim->advance((double)dt);

        // Y de vuelta al transporte, en el MISMO marco de chunk que llego.
        const glm::dvec3 p = body->position;
        e->pos[0] = (float)(p.x - (double)e->chunkX * z->chunk[0]);
        e->pos[1] = (float)(p.y - (double)e->chunkY * z->chunk[1]);
        e->pos[2] = (float)(p.z - (double)e->chunkZ * z->chunk[2]);
    }

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
        &serializeRegion, &mergeRegion, &dropRegion,
        &step                                   // ⚠️ estaba a NULO: la zona no simulaba
    };
}

extern "C" const DGS::GameModule* dgs_game_module_v1(void)
{
    static const bool once = [] { selfCheck(); return true; }();
    (void)once;
    return &g_module;
}

// ════════════════════════════════════════════════════════════════════════════════════════════════
// EL CANAL DEL SUELO — simbolos ADICIONALES, fuera del ABI del DGS.
//
// ⚠️ POR QUE NO VAN EN `DGS::WorldQuery`. El DGS es otro proyecto y su ABI es suyo: `WorldQuery`
// lleva seed, centro, radio, `reliefStrength` y `profile`, y no hay donde meter un campo de altura.
// Y el campo NO se rederiva del seed —`heightBakeKey` incluye el mapa de elevacion de la escena, el
// de zonas y la tabla de materiales, o sea assets—, asi que tiene que VIAJAR.
//
// La salida es que este modulo, que si es nuestro, exporte dos simbolos mas. El host de Haruka los
// resuelve por `dlsym` y los llama ANTES de crear zonas. Un host del DGS que no los conozca no se
// entera de nada y se comporta exactamente como hasta ahora (analitico + aviso). Cero cambios en
// `external/dgs/`.
//
// Y como tercera via, `HARUKA_RULES_HEIGHT_FIELD=<ruta>`: un servidor lanzado con un host ajeno
// tambien puede tener el suelo bueno sin que nadie llame a nada. Se lee UNA vez, en la primera zona.
// ════════════════════════════════════════════════════════════════════════════════════════════════

/// Entrega el campo YA EN MEMORIA. Es el camino del motor: `TerrestrialPlanet::m_heightCPU` es
/// exactamente este dato, asi que no hay que decodificar nada ni tocar el disco.
/// Devuelve 1 si queda instalado. `data == nullptr` lo DESINSTALA (vuelve al analitico).
extern "C" int haruka_rules_set_height_field(int w, int h, const float* data, double baseRadiusM)
{
    if (!data || w <= 0 || h <= 0 || !(baseRadiusM > 0.0)) {
        std::lock_guard<std::mutex> lk(g_fieldMx);
        g_field.reset();
        return data ? 0 : 1;   // sin datos y sin dimensiones = desinstalar (exito)
    }
    auto f = std::make_shared<Haruka::Net::HeightField>();
    f->w = w; f->h = h; f->baseRadiusM = (float)baseRadiusM;
    f->data.assign(data, data + (size_t)w * (size_t)h);   // COPIA: el modulo sobrevive al llamante
    if (!f->valid()) return 0;
    {
        std::lock_guard<std::mutex> lk(g_fieldMx);
        g_field = std::move(f);
    }
    HARUKA_LOGI("Rules", "campo de altura instalado: %dx%d, radio base %.0f m — el noclip se valida "
                         "contra el MISMO suelo que pisa el cliente", w, h, baseRadiusM);
    return 1;
}

/// Lo mismo, desde el fichero crudo que deja el bake (`<clave>_height.hfield`). Para un servidor
/// headless que no comparte proceso con el motor.
extern "C" int haruka_rules_load_piece_catalog(const char* path)
{
    if (!path || !*path) return 0;
    std::FILE* f = std::fopen(path, "rb");
    if (!f) { HARUKA_LOGW("Rules", "no encuentro el catalogo de piezas '%s'", path); return 0; }

    char     magic[4] = {0};
    uint32_t version = 0, n = 0;
    if (std::fread(magic, 1, 4, f) != 4 ||
        std::fread(&version, sizeof(version), 1, f) != 1 ||
        std::fread(&n, sizeof(n), 1, f) != 1 ||
        std::memcmp(magic, "HKPC", 4) != 0 || version != 1u)
    { std::fclose(f); HARUKA_LOGW("Rules", "'%s' no es un catalogo de piezas v1", path); return 0; }

    // Un `n` que miente es la forma barata de hacer que un nodo reserve gigabytes. Se acota.
    if (n > 65535u) { std::fclose(f); HARUKA_LOGW("Rules", "catalogo absurdo (%u piezas)", n); return 0; }

    std::vector<Haruka::Construction::PieceType> cat;
    cat.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        uint16_t typeId = 0; uint8_t supports = 0, needsFlat = 0; double half[3] = {0,0,0};
        if (std::fread(&typeId, sizeof(typeId), 1, f) != 1 ||
            std::fread(&supports, 1, 1, f) != 1 ||
            std::fread(&needsFlat, 1, 1, f) != 1 ||
            std::fread(half, sizeof(double), 3, f) != 3)
        { std::fclose(f); HARUKA_LOGW("Rules", "catalogo truncado en la pieza %u", i); return 0; }
        Haruka::Construction::PieceType t;
        t.id = typeId;
        t.halfExtents = glm::dvec3(half[0], half[1], half[2]);
        t.supports = supports;
        t.needsFlat = needsFlat != 0;
        cat.push_back(t);
    }
    std::fclose(f);

    HARUKA_LOGI("Rules", "catalogo de piezas cargado de '%s': %u piezas", path, n);
    {
        std::lock_guard<std::mutex> lk(g_catMx);
        g_catalog = std::move(cat);
    }
    return 1;
}

extern "C" int haruka_rules_load_height_field(const char* path)
{
    if (!path || !*path) return 0;
    auto f = std::make_shared<Haruka::Net::HeightField>();
    if (!Haruka::Net::loadHeightField(path, *f) || !f->valid()) {
        HARUKA_LOGW("Rules", "no pude cargar el campo de altura '%s'", path);
        return 0;
    }
    HARUKA_LOGI("Rules", "campo de altura cargado de '%s': %dx%d, radio base %.0f m",
                path, f->w, f->h, (double)f->baseRadiusM);
    {
        std::lock_guard<std::mutex> lk(g_fieldMx);
        g_field = std::move(f);
    }
    return 1;
}
