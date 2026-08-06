#include "physics_engine.h"

#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstdlib>   // std::getenv — Jolt es opt-in (HARUKA_JOLT=1) mientras se integra
#include <future>    // refresco ASÍNCRONO de la malla de colisión (no bloquear el frame)
#include <chrono>

#ifdef HARUKA_HAS_JOLT
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <unordered_map>
#include <cstdarg>
#include "core/logger.h"

// Boilerplate de Jolt (capas + filtros), aislado en este .cpp — el header NO ve Jolt.
namespace {
    void jphTrace(const char* fmt, ...) { va_list a; va_start(a, fmt); char b[512]; vsnprintf(b, sizeof(b), fmt, a); va_end(a); HARUKA_LOGI("Jolt", "%s", b); }
    namespace JLayers { static constexpr JPH::ObjectLayer NON_MOVING = 0, MOVING = 1, NUM = 2; }
    namespace JBP     { static constexpr JPH::BroadPhaseLayer NON_MOVING{0}, MOVING{1}; static constexpr JPH::uint NUM = 2; }
    class BPImpl final : public JPH::BroadPhaseLayerInterface {
    public: BPImpl() { m[JLayers::NON_MOVING] = JBP::NON_MOVING; m[JLayers::MOVING] = JBP::MOVING; }
        JPH::uint GetNumBroadPhaseLayers() const override { return JBP::NUM; }
        JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override { return m[l]; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
        const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override { return "L"; }
#endif
    private: JPH::BroadPhaseLayer m[JLayers::NUM];
    };
    class OVBImpl : public JPH::ObjectVsBroadPhaseLayerFilter {
    public: bool ShouldCollide(JPH::ObjectLayer o, JPH::BroadPhaseLayer b) const override { return o == JLayers::NON_MOVING ? (b == JBP::MOVING) : true; } };
    class OVOImpl : public JPH::ObjectLayerPairFilter {
    public: bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override { return a == JLayers::NON_MOVING ? (b == JLayers::MOVING) : true; } };
}
#endif // HARUKA_HAS_JOLT

namespace Haruka { namespace Physics {

#ifdef HARUKA_HAS_JOLT
// Fase 1: wrapper de Jolt. Simula los cuerpos DINÁMICOS (los kinemáticos los dirige el juego). El
// terreno es de momento una ESFERA (planeta+altura); la malla local llega en la Fase 2. Gravedad
// RADIAL aplicada por cuerpo como fuerza (Jolt no la sabe curva) → a = F/m = g, independiente de la masa.
struct PhysicsEngine::JoltImpl {
    static void globalInit() {
        static bool done = false; if (done) return; done = true;
        JPH::RegisterDefaultAllocator();
        JPH::Trace = jphTrace;
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
    JPH::TempAllocatorImpl        temp{16 * 1024 * 1024};
    JPH::JobSystemSingleThreaded  jobs{JPH::cMaxPhysicsJobs};   // 1 hilo → determinista (DGS)
    BPImpl  bpli; OVBImpl ovbp; OVOImpl ovo;
    JPH::PhysicsSystem            system;
    std::unordered_map<RigidBody*, JPH::BodyID> map;
    JPH::BodyID groundId; bool haveGround = false; bool groundIsMesh = false; glm::dvec3 meshCenter{0.0};
    double meshBuildElev = 0.0;   // altura del terreno (m sobre la esfera) EN meshCenter al construir la
                                  // malla → si el LOD refina bajo un jugador QUIETO y esa altura cambia,
                                  // hay que rehacer aunque no se haya movido (si no: se camina sobre la
                                  // malla vieja gruesa = "flotar" sobre el terreno fino ya dibujado).
    // MARCO LOCAL: Jolt usa float; a escala planetaria (Tierra a ~1.5e8 m en la escena) eso da ~16 m de
    // precisión → inservible. Simulamos TODO relativo a `origin` (posición del primer cuerpo) → floats
    // pequeños. El mundo exterior sigue en double. (Recentrar al alejarse mucho = refinamiento posterior.)
    glm::dvec3 origin{0.0}; bool originSet = false;
    double simTime = 0.0, lastRebuildT = -1e9;   // reloj de simulación para rate-limitar la reconstrucción

    // REFRESCO ASÍNCRONO de la malla de colisión. Construir el parche (muestrear el terreno + montar la
    // MeshShape) cuesta ms; hacerlo en el hilo de física daba un TIRÓN al avanzar 48 m. La Shape NO toca
    // el PhysicsSystem, así que se construye en un worker; solo el intercambio del cuerpo (barato) va en
    // el hilo de física. La malla VIEJA sigue colisionando hasta que la nueva está lista → nunca hay
    // hueco (no se cae). El campo de altura interpolado (world_system_provider) hace el worker barato.
    std::future<JPH::Ref<JPH::Shape>> groundJob;
    glm::dvec3 groundJobCenter{0.0};
    double     groundJobElev = 0.0;

    // Construye la MeshShape del parche alrededor de `near` (SIN tocar el PhysicsSystem → seguro fuera
    // del hilo de física). Devuelve null si no hay malla. `terrainMesh` es thread-safe (caché con mutex).
    JPH::Ref<JPH::Shape> buildGroundShape(IWorldProvider* w, const glm::dvec3& near) const {
        std::vector<glm::dvec3> verts; std::vector<uint32_t> tris;
        if (!w || !w->terrainMesh(near, 96.0, verts, tris) || tris.size() < 3 || verts.size() < 3)
            return {};
        JPH::VertexList jv; jv.reserve(verts.size());
        for (const auto& p : verts) { const glm::dvec3 lp = p - origin;
            jv.push_back(JPH::Float3((float)lp.x, (float)lp.y, (float)lp.z)); }
        JPH::IndexedTriangleList jt; jt.reserve(tris.size() / 3);
        for (size_t i = 0; i + 2 < tris.size(); i += 3)
            jt.push_back(JPH::IndexedTriangle(tris[i], tris[i + 1], tris[i + 2], 0));
        JPH::MeshShapeSettings ms(jv, jt);
        JPH::ShapeSettings::ShapeResult res = ms.Create();
        return res.IsValid() ? res.Get() : JPH::Ref<JPH::Shape>{};
    }

    // Cambia el cuerpo del suelo por `shape` (en el hilo de física). La vieja se quita justo aquí, no
    // antes: hasta este instante seguía colisionando.
    void swapGroundShape(JPH::Ref<JPH::Shape> shape, const glm::dvec3& center, double elev) {
        if (!shape) return;
        JPH::BodyInterface& bi = system.GetBodyInterface();
        if (haveGround) { bi.RemoveBody(groundId); bi.DestroyBody(groundId); haveGround = false; }
        JPH::BodyCreationSettings s(shape, JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
            JPH::EMotionType::Static, JLayers::NON_MOVING);
        groundId = bi.CreateAndAddBody(s, JPH::EActivation::DontActivate);
        haveGround = true; groundIsMesh = true; meshCenter = center; meshBuildElev = elev;
    }

    JoltImpl() {
        globalInit();
        system.Init(4096, 0, 16384, 16384, bpli, ovbp, ovo);
        system.SetGravity(JPH::Vec3::sZero());   // la gravedad radial la aplicamos nosotros
    }
    // Construye/REFRESCA el suelo alrededor de `near`. La MALLA (si el provider la da) se reconstruye al
    // moverse el jugador (sigue la región); la esfera (fallback: servidor/test) es fija → una vez basta.
    void rebuildGround(IWorldProvider* w, const glm::dvec3& near) {
        if (!w || !w->hasActivePlanet()) return;
        JPH::BodyInterface& bi = system.GetBodyInterface();
        std::vector<glm::dvec3> verts; std::vector<uint32_t> tris;
        const bool hasMesh = w->terrainMesh(near, 96.0, verts, tris) && tris.size() >= 3 && verts.size() >= 3;
        if (haveGround && !groundIsMesh && !hasMesh) return;   // sigue en modo esfera fija → nada que hacer
        if (haveGround) { bi.RemoveBody(groundId); bi.DestroyBody(groundId); haveGround = false; }

        if (hasMesh) {                                          // colisión con la MALLA REAL (float-safe: local)
            JPH::VertexList jv; jv.reserve(verts.size());
            for (const auto& p : verts) { const glm::dvec3 lp = p - origin;
                jv.push_back(JPH::Float3((float)lp.x, (float)lp.y, (float)lp.z)); }
            JPH::IndexedTriangleList jt; jt.reserve(tris.size() / 3);
            for (size_t i = 0; i + 2 < tris.size(); i += 3)
                jt.push_back(JPH::IndexedTriangle(tris[i], tris[i + 1], tris[i + 2], 0));
            JPH::MeshShapeSettings ms(jv, jt);
            JPH::ShapeSettings::ShapeResult res = ms.Create();
            if (res.IsValid()) {
                JPH::BodyCreationSettings s(res.Get(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
                    JPH::EMotionType::Static, JLayers::NON_MOVING);
                groundId = bi.CreateAndAddBody(s, JPH::EActivation::DontActivate);
                haveGround = true; groundIsMesh = true; meshCenter = near;
                meshBuildElev = w->terrainHeightAt(near);   // referencia para detectar refinamiento vertical
                return;
            }
        }
        // Fallback: esfera (planeta + altura de un punto). Fija.
        const glm::dvec3 c = w->activePlanetCenter();
        const double R = w->activePlanetRadius();
        const double h = w->terrainHeightAt(c + glm::dvec3(R, 0, 0));
        const glm::dvec3 lc = c - origin;
        JPH::BodyCreationSettings s(new JPH::SphereShape((float)(R + h)),
            JPH::RVec3((float)lc.x, (float)lc.y, (float)lc.z), JPH::Quat::sIdentity(),
            JPH::EMotionType::Static, JLayers::NON_MOVING);
        groundId = bi.CreateAndAddBody(s, JPH::EActivation::DontActivate);
        haveGround = true; groundIsMesh = false;
    }
    // --- CONTROLADOR DE PERSONAJE (Jolt CharacterVirtual) ---
    // Un personaje NO es un rígido dinámico: quiere subir escalones, no resbalar por pendientes suaves,
    // pararse en seco y saber si pisa suelo. Emular eso a mano sobre una esfera rígida sale mal pieza a
    // pieza (rodaba, se enterraba, no saltaba, deslizaba). `CharacterVirtual` lo trae de fábrica.
    JPH::Ref<JPH::CharacterVirtual> charCtrl;
    RigidBody*                      charBody = nullptr;

    void addCharacter(RigidBody* b) {
        charBody = b;
        if (!originSet) { origin = b->position; originSet = true; }
        JPH::Ref<JPH::CharacterVirtualSettings> cs = new JPH::CharacterVirtualSettings();
        // ESFERA del mismo radio que el cuerpo: así `position` sigue siendo el CENTRO y el juego puede
        // seguir sacando el pie como `centro − arriba·radio`, sin tocar su matemática.
        cs->mShape         = new JPH::SphereShape((float)b->radius);
        cs->mMaxSlopeAngle = JPH::DegreesToRadians(50.0f);   // más empinado que esto = resbalas
        const glm::dvec3 lp = b->position - origin;
        charCtrl = new JPH::CharacterVirtual(cs, JPH::RVec3((float)lp.x, (float)lp.y, (float)lp.z),
                                             JPH::Quat::sIdentity(), &system);
    }

    // Quitar un cuerpo TAMBIÉN de Jolt. Si no, su BodyID sigue vivo (colisionador fantasma que nadie ve)
    // y la clave `RigidBody*` del mapa queda COLGANDO: si un cuerpo nuevo se aloja en esa misma dirección
    // coincide con la entrada vieja y acaba movido por el cuerpo equivocado.
    void removeBody(RigidBody* b) {
        if (b == charBody) { charCtrl = nullptr; charBody = nullptr; return; }
        auto it = map.find(b);
        if (it == map.end()) return;
        JPH::BodyInterface& bi = system.GetBodyInterface();
        bi.RemoveBody(it->second);
        bi.DestroyBody(it->second);
        map.erase(it);
    }

    void addBody(RigidBody* b) {
        if (b->isCharacter) { addCharacter(b); return; }
        if (!originSet) { origin = b->position; originSet = true; }   // el 1er cuerpo fija el marco local
        JPH::Ref<JPH::Shape> shape = (b->shape == RigidBody::Shape::Box)
            ? (JPH::Shape*)new JPH::BoxShape(JPH::Vec3((float)b->halfExtents.x, (float)b->halfExtents.y, (float)b->halfExtents.z))
            : (JPH::Shape*)new JPH::SphereShape((float)b->radius);
        const glm::dvec3 lp = b->position - origin;                   // posición en el marco local
        JPH::BodyCreationSettings s(shape,
            JPH::RVec3((float)lp.x, (float)lp.y, (float)lp.z),
            JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, JLayers::MOVING);
        s.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        s.mMassPropertiesOverride.mMass = (float)b->mass;
        // Un PERSONAJE no rueda: se le permite trasladar pero no girar. Si no, la esfera acumula
        // velocidad angular y el contacto la convierte en avance → sigue deslizando aunque se le ponga
        // la velocidad lineal a cero (justo el "no tiene fricción" que se veía).
        if (b->lockRotation)
            s.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY
                           | JPH::EAllowedDOFs::TranslationZ;
        JPH::BodyInterface& bi = system.GetBodyInterface();
        JPH::BodyID id = bi.CreateAndAddBody(s, JPH::EActivation::Activate);
        bi.SetLinearVelocity(id, JPH::Vec3((float)b->velocity.x, (float)b->velocity.y, (float)b->velocity.z));
        map[b] = id;
    }
    // --- Mundo ESTÁTICO en Jolt: props colocados, piezas de construcción y muros de edificio. ---
    // Sin esto, Jolt solo conoce el terreno: un cuerpo dinámico (el jugador) ATRAVIESA todo lo demás,
    // porque esas cajas solo existían para el resolutor a mano. Se reconstruyen cuando cambia la lista
    // (`version`), no cada frame: son estáticas y cambiarlas es raro (colocar/romper algo).
    std::vector<JPH::BodyID> staticIds;
    uint64_t staticVersion = ~0ull;

    static bool finite3(const glm::dvec3& v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }
    void syncStatics(PhysicsEngine* eng) {
        const uint64_t version = eng->staticsVersion();
        if (version == staticVersion || !originSet) return;
        staticVersion = version;
        JPH::BodyInterface& bi = system.GetBodyInterface();
        for (JPH::BodyID id : staticIds) { bi.RemoveBody(id); bi.DestroyBody(id); }
        staticIds.clear();

        auto addBox = [&](const glm::dvec3& center, const glm::dvec3& he, const glm::dquat& q0) {
            if (he.x < 1e-4 || he.y < 1e-4 || he.z < 1e-4) return;   // caja degenerada → Jolt la rechaza
            const glm::dvec3 lp = center - origin;                   // al marco local (Jolt va en float)
            // Jolt ASERTA (SIGTRAP) si el cuaternión no está normalizado o la posición no es finita. Las
            // rotaciones vienen de matrices hechas con productos vectoriales que, tras pasar por float,
            // se salen de la tolerancia de Jolt → NORMALIZAR aquí y descartar lo no-finito. Es la frontera
            // con Jolt: se saneia una vez, no se confía en cada productor.
            const double qlen = glm::length(q0);
            if (!finite3(lp) || !finite3(he) || qlen < 1e-9 || !std::isfinite(qlen)) return;
            const glm::dquat q = q0 / qlen;                          // normalizado
            JPH::BodyCreationSettings s(
                new JPH::BoxShape(JPH::Vec3((float)he.x, (float)he.y, (float)he.z)),
                JPH::RVec3((float)lp.x, (float)lp.y, (float)lp.z),
                JPH::Quat((float)q.x, (float)q.y, (float)q.z, (float)q.w).Normalized(),
                JPH::EMotionType::Static, JLayers::NON_MOVING);
            staticIds.push_back(bi.CreateAndAddBody(s, JPH::EActivation::DontActivate));
        };

        // TODO el mundo estático, de una: objetos colocados + piezas/muros (placed), RECURSOS (árboles/
        // rocas, prop) y colisionadores de escena (staticBox, AABB → OBB sin girar). Si alguna faltara,
        // el jugador —que ahora es un cuerpo de Jolt— la ATRAVESARÍA (colisión "sin terminar").
        const auto& placed = eng->getPlacedOBBs();
        const auto& props  = eng->getPropOBBs();
        const auto& boxes  = eng->getStaticBoxes();
        staticIds.reserve(placed.size() + props.size() + boxes.size());
        for (const StaticOBB& o : placed) addBox(o.center, o.halfExtents, glm::quat_cast(o.rot));
        for (const StaticOBB& o : props)  addBox(o.center, o.halfExtents, glm::quat_cast(o.rot));
        for (const StaticBox& b : boxes)
            addBox((b.bmin + b.bmax) * 0.5, (b.bmax - b.bmin) * 0.5, glm::dquat(1, 0, 0, 0));
    }

    // Gravedad RADIAL del planeta activo (m/s², hacia su centro). `gravBodies()` solo trae los cuerpos
    // que EMITEN luz (estrellas) → el planeta sobre el que andas NO está ahí y habría gravedad ~0.
    glm::dvec3 planetGravity(IWorldProvider* w, const glm::dvec3& pos) const {
        if (!w || !w->hasActivePlanet()) return glm::dvec3(0.0);
        const glm::dvec3 toC = w->activePlanetCenter() - pos;
        const double r = glm::length(toC);
        if (r < 1e-6) return glm::dvec3(0.0);
        const double R = w->activePlanetRadius();
        return (toC / r) * (9.81 * (R * R) / (r * r));
    }

    void stepCharacter(double dt, PhysicsEngine* eng, IWorldProvider* w) {
        if (!charCtrl || !charBody) return;
        RigidBody* b = charBody;
        const glm::dvec3 lp = b->position - origin;

        // VOLANDO/NADANDO manda el juego: se lleva el controlador a su posición y no se simula.
        if (b->isKinematic) {
            if (finite3(b->position))
                charCtrl->SetPosition(JPH::RVec3((float)lp.x, (float)lp.y, (float)lp.z));
            if (finite3(b->velocity))
                charCtrl->SetLinearVelocity(JPH::Vec3((float)b->velocity.x, (float)b->velocity.y, (float)b->velocity.z));
            b->onGround = false;
            return;
        }

        const glm::dvec3 gvec = planetGravity(w, b->position);
        const double gl = glm::length(gvec);
        const glm::dvec3 up = (gl > 1e-9) ? -gvec / gl : glm::dvec3(0.0, 1.0, 0.0);
        charCtrl->SetUp(JPH::Vec3((float)up.x, (float)up.y, (float)up.z));   // "arriba" es RADIAL y cambia al moverte
        if (finite3(b->position))
            charCtrl->SetPosition(JPH::RVec3((float)lp.x, (float)lp.y, (float)lp.z));
        // La GRAVEDAD la integra el LLAMADOR: `ExtendedUpdate` no la aplica a la velocidad (su parámetro
        // `inGravity` solo sirve para empujar hacia abajo cuando estás encima de algo). Sin esto el
        // personaje anda y choca pero NUNCA CAE.
        if (finite3(b->velocity)) {
            const glm::dvec3 vel = b->velocity + gvec * dt;
            charCtrl->SetLinearVelocity(JPH::Vec3((float)vel.x, (float)vel.y, (float)vel.z));
        }

        // Escalones y "pegado al suelo" en el marco RADIAL (por defecto Jolt los da en ±Y del mundo,
        // que en una esfera solo vale en el polo).
        JPH::CharacterVirtual::ExtendedUpdateSettings us;
        us.mStickToFloorStepDown = JPH::Vec3((float)(-up.x * 0.5), (float)(-up.y * 0.5), (float)(-up.z * 0.5));
        us.mWalkStairsStepUp     = JPH::Vec3((float)( up.x * 0.4), (float)( up.y * 0.4), (float)( up.z * 0.4));

        charCtrl->ExtendedUpdate((float)dt,
            JPH::Vec3((float)gvec.x, (float)gvec.y, (float)gvec.z), us,
            system.GetDefaultBroadPhaseLayerFilter(JLayers::MOVING),
            system.GetDefaultLayerFilter(JLayers::MOVING),
            JPH::BodyFilter{}, JPH::ShapeFilter{}, temp);

        const JPH::RVec3 p = charCtrl->GetPosition();
        const JPH::Vec3  v = charCtrl->GetLinearVelocity();
        b->position = origin + glm::dvec3(p.GetX(), p.GetY(), p.GetZ());
        b->velocity = glm::dvec3(v.GetX(), v.GetY(), v.GetZ());
        b->onGround = (charCtrl->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround);
    }

    void step(double dt, PhysicsEngine* eng, IWorldProvider* w,
              const std::vector<std::shared_ptr<RigidBody>>& bodies) {
        // SIN cuerpos dinámicos NI personaje no hay nada que simular: no construir malla (muestrear el
        // terreno es CARO) ni pisar Jolt. Evita el build en (0,0,0) y deja el frame libre.
        if (map.empty() && !charCtrl) return;
        simTime += dt;
        // Región de la malla del terreno = alrededor del primer cuerpo dinámico. Se REFRESCA cuando el
        // cuerpo se aleja >48 m del centro con el que se construyó, pero como MUCHO ~4×/s (rate-limit):
        // rehacer la malla muestrea el terreno (caro) → un cuerpo veloz no debe dispararlo cada paso.
        // La región del suelo sigue al PERSONAJE si lo hay (es quien camina); si no, al 1er dinámico.
        glm::dvec3 near(0.0);
        if (charBody) near = charBody->position;
        else for (auto& sp : bodies) if (map.count(sp.get())) { near = sp->position; break; }
        // Primera vez (spawn): SÍNCRONO — hay que tener suelo ya, no se puede caer un frame.
        if (!haveGround) { rebuildGround(w, near); lastRebuildT = simTime; }

        // ¿Terminó un refresco en curso? Intercambia el cuerpo (barato, hilo de física). La malla vieja
        // colisionó hasta este instante → cero hueco.
        if (groundJob.valid() && groundJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            swapGroundShape(groundJob.get(), groundJobCenter, groundJobElev);
        }

        // ¿Hace falta refrescar? (movido >48 m, o el terreno bajo los pies cambió >0.5 m por refinado del
        // LOD / deform, aunque el jugador esté QUIETO). Se lanza en un WORKER: el parche (muestrear +
        // montar la MeshShape) se construye FUERA del frame; el frame solo paga el intercambio cuando
        // esté listo (arriba). Un solo job en vuelo. El campo de altura interpolado hace el worker barato.
        const bool drifted = groundIsMesh && glm::length(near - meshCenter) > 48.0;
        const bool refined = groundIsMesh && std::abs(w->terrainHeightAt(near) - meshBuildElev) > 0.5;
        // onSphere: el primer build cayó al fallback de ESFERA (terrainMesh falló ese instante, p.ej. el
        // planeta aún no estaba listo). La esfera es PLANA a la altura de un punto → el terreno aparece
        // decenas de m desplazado. Hay que SEGUIR intentando la malla (si no, se queda clavado en la
        // esfera para siempre, porque drifted/refined exigen que YA haya malla). Rate-limit lo acota.
        const bool onSphere = haveGround && !groundIsMesh;
        if (((groundIsMesh && (drifted || refined)) || onSphere)
            && simTime - lastRebuildT > 0.25 && !groundJob.valid()) {
            groundJobCenter = near;
            groundJobElev   = w->terrainHeightAt(near);
            groundJob = std::async(std::launch::async,
                                   [this, w, near]{ return buildGroundShape(w, near); });
            lastRebuildT = simTime;
        }
        syncStatics(eng);   // TODO el mundo estático (colocados + recursos + escena) → cuerpos de Jolt
        stepCharacter(dt, eng, w);
        JPH::BodyInterface& bi = system.GetBodyInterface();
        for (auto& sp : bodies) {
            auto it = map.find(sp.get()); if (it == map.end()) continue;
            // ¿Lo manda el JUEGO ahora mismo (volar, nadar)? Entonces Jolt NO debe integrarlo: pasa a
            // KINEMÁTICO y se le lleva a la posición que dicta el juego. Antes el juego ponía
            // `isKinematic=true` pero Jolt lo ignoraba y lo seguía simulando con gravedad → los dos
            // peleaban por la posición y volar no funcionaba. Kinemático en Jolt además EMPUJA a los
            // cuerpos dinámicos con los que choque, que es justo lo que quieres al volar contra algo.
            if (sp->isKinematic) {
                if (bi.GetMotionType(it->second) != JPH::EMotionType::Kinematic)
                    bi.SetMotionType(it->second, JPH::EMotionType::Kinematic, JPH::EActivation::Activate);
                if (finite3(sp->position)) {
                    const glm::dvec3 lp = sp->position - origin;
                    bi.SetPositionAndRotation(it->second,
                        JPH::RVec3((float)lp.x, (float)lp.y, (float)lp.z),
                        JPH::Quat::sIdentity(), JPH::EActivation::Activate);
                }
                if (finite3(sp->velocity))
                    bi.SetLinearVelocity(it->second, JPH::Vec3((float)sp->velocity.x, (float)sp->velocity.y, (float)sp->velocity.z));
                continue;   // sin gravedad y sin lectura de vuelta: la posición la posee el juego
            }
            if (bi.GetMotionType(it->second) != JPH::EMotionType::Dynamic)
                bi.SetMotionType(it->second, JPH::EMotionType::Dynamic, JPH::EActivation::Activate);
            // Un cuerpo LIBRE lo POSEE el motor: NO se le re-impone su velocidad cada paso. Hacerlo
            // re-aplicaba la velocidad de SEPARACIÓN que el solver genera al resolver una penetración
            // (normalmente transitoria) una y otra vez → el objeto acumulaba energía y salía volando,
            // como si no pesara. Solo se le pasa cuando el juego la cambia A PROPÓSITO (lanzar, empujar).
            if (sp->velocityDirty) {
                if (finite3(sp->velocity))   // Jolt aserta con NaN → nunca inyectar basura
                    bi.SetLinearVelocity(it->second, JPH::Vec3((float)sp->velocity.x, (float)sp->velocity.y, (float)sp->velocity.z));
                bi.ActivateBody(it->second);
                sp->velocityDirty = false;
            }
            glm::dvec3 gdir; const double g = eng->calculateGravityAtPosition(sp->position, gdir);
            glm::dvec3 F = gdir * (g * sp->mass);   // cuerpos celestes (el Sol) — insignificante
            // Gravedad del PLANETA ACTIVO (radial). `gravBodies()` solo trae los cuerpos que EMITEN luz
            // (estrellas), NO el planeta sobre el que andas → sin esto el jugador no cae y flota al saltar.
            // Superficie = 9.81 m/s² (el valor que usaba el jugador a mano), con caída por altura (R²/r²).
            if (w && w->hasActivePlanet()) {
                const glm::dvec3 toC = w->activePlanetCenter() - sp->position;
                const double r = glm::length(toC);
                if (r > 1e-6) {
                    const double R = w->activePlanetRadius();
                    const double gp = 9.81 * (R * R) / (r * r);
                    F += (toC / r) * (gp * sp->mass);
                }
            }
            if (finite3(F))
                bi.AddForce(it->second, JPH::Vec3((float)F.x, (float)F.y, (float)F.z));
        }
        system.Update((float)dt, 1, &temp, &jobs);
        for (auto& sp : bodies) {
            auto it = map.find(sp.get()); if (it == map.end()) continue;
            if (sp->isKinematic) continue;   // lo mueve el juego (vuelo/nado): no le pisemos la posición
            const JPH::RVec3 p = bi.GetPosition(it->second);
            const JPH::Vec3  v = bi.GetLinearVelocity(it->second);
            const JPH::Quat  q = bi.GetRotation(it->second);
            const JPH::Vec3  a = bi.GetAngularVelocity(it->second);
            sp->position    = origin + glm::dvec3(p.GetX(), p.GetY(), p.GetZ());   // local → mundo (double)
            sp->velocity    = glm::dvec3(v.GetX(), v.GetY(), v.GetZ());
            sp->orientation = glm::dquat(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
            sp->angularVel  = glm::dvec3(a.GetX(), a.GetY(), a.GetZ());
        }
    }
};
#endif // HARUKA_HAS_JOLT

PhysicsEngine::PhysicsEngine() {
#ifdef HARUKA_HAS_JOLT
    // Jolt es el motor de rígidos por defecto. Escape de depuración: HARUKA_JOLT=0 fuerza el solver a mano.
    const char* v = std::getenv("HARUKA_JOLT");
    if (!(v && v[0] == '0')) {
        JoltImpl::globalInit();                 // registra el allocator de Jolt ANTES de construir los
        m_jolt = std::make_unique<JoltImpl>();  // miembros de JoltImpl (el TempAllocator ya lo necesita)
    }
#endif
}

PhysicsEngine::~PhysicsEngine() {}

void PhysicsEngine::addBody(std::shared_ptr<RigidBody> body) {
    bodies.push_back(body);
    if (octree) octree->insert(body);
#ifdef HARUKA_HAS_JOLT
    // Los DINÁMICOS los simula Jolt; los kinemáticos los dirige el juego (posición directa).
    if (m_jolt && !body->isKinematic) m_jolt->addBody(body.get());
#endif
}

void PhysicsEngine::removeBody(const std::string& name) {
    auto it = std::find_if(bodies.begin(), bodies.end(),
        [&name](const std::shared_ptr<RigidBody>& b) { return b->name == name; });
    if (it != bodies.end()) {
#ifdef HARUKA_HAS_JOLT
        if (m_jolt) m_jolt->removeBody(it->get());   // si no, queda colisionador fantasma + clave colgante
#endif
        bodies.erase(it);
    }
}

std::shared_ptr<RigidBody> PhysicsEngine::getBody(const std::string& name) {
    auto it = std::find_if(bodies.begin(), bodies.end(),
        [&name](const std::shared_ptr<RigidBody>& b) { return b->name == name; });
    return (it != bodies.end()) ? *it : nullptr;
}

void PhysicsEngine::update(double deltaTime) {
#ifdef HARUKA_HAS_JOLT
    // Fase 1: con Jolt disponible, los cuerpos DINÁMICOS los simula él (gravedad radial + suelo +
    // colisión estables). Los kinemáticos, OBB estáticos y resolveSphere siguen su camino (no aquí).
    if (m_jolt) { m_jolt->step(deltaTime, this, m_world, bodies); return; }
#endif
    integrateForces(deltaTime);
    broadPhaseAABB();
    detectCollisions();
    resolveCollisions();
    resolveStaticCollisions();
}

void PhysicsEngine::advance(double frameDt) {
    // TIMESTEP FIJO con acumulador. El motor SIEMPRE avanza en pasos de kFixedDt, sea cual sea el
    // dt de reloj. Es REQUISITO de determinismo cliente↔servidor: con dt variable, el mismo input
    // da trayectorias distintas según los fps → cliente y DGS discreparían y la validación fallaría.
    // El resto (frameDt no múltiplo exacto) se acumula para el frame siguiente.
    m_accum += frameDt;
    // Cota anti-espiral: si el frame se congela (breakpoint, hitch), no intentar recuperar segundos
    // de simulación de golpe (bloquearía más) — se descarta el exceso.
    if (m_accum > kMaxAccum) m_accum = kMaxAccum;
    while (m_accum >= kFixedDt) {
        update(kFixedDt);
        m_accum -= kFixedDt;
    }
}

void PhysicsEngine::integrateForces(double dt) {
    // Nivel del mar del planeta activo (esfera de radio seaR). La TIERRA siempre está por
    // encima del nivel del mar → un cuerpo por debajo de esta esfera está en una cuenca
    // oceánica = sumergido. Se usa para el empuje de Arquímedes de los cuerpos dinámicos.
    bool       haveSea = false;
    glm::dvec3 seaCenter(0.0);
    double     seaR = 0.0;
    if (m_world && m_world->hasActivePlanet()) {
        haveSea   = true;
        seaCenter = m_world->activePlanetCenter();
        seaR      = m_world->activePlanetRadius();
    }

    for (auto& body : bodies) {
        if (body->isKinematic) continue;

        // Gravedad RADIAL hacia el planeta (antes era plana -Y, que solo valía cerca del polo). La
        // suma newtoniana de los cuerpos vía calculateGravityAtPosition da la dirección y magnitud
        // reales; sin m_world cae a (0,-1,0)·9.81. Es lo que permite que un cuerpo dinámico "caiga
        // hacia el suelo" en cualquier punto del planeta.
        glm::dvec3 gDir;
        double gMag = calculateGravityAtPosition(body->position, gDir);
        body->acceleration = gDir * gMag;

        // Velocity Verlet
        body->velocity += body->acceleration * dt;

        // Arrastre AERODINÁMICO (solo cuerpos dinámicos = conjunto acotado cerca del
        // juego → O(cuerpos), sin coste global). Resistencia del aire (amortigua hacia
        // 0) + empuje del viento (cuadrático con la velocidad relativa). Suave.
        body->velocity *= (1.0 - std::min(m_airDamp * dt, 0.5));
        glm::dvec3 vRel = m_wind - body->velocity;
        double rel = glm::length(vRel);
        if (rel > 1e-6)
            body->velocity += vRel * std::min(m_windCoef * rel * dt, 0.20);

        // --- BUOYANCY (empuje de Arquímedes) + arrastre de agua para cuerpos SUMERGIDOS ---
        // f = fracción sumergida: 0 cuando el cuerpo apenas toca la superficie por arriba
        // (dist = seaR+radio), 1 cuando está totalmente bajo el agua (dist ≤ seaR−radio).
        // Empuje hacia el radial +up escala con f y con la razón de densidades (buoyRatio>1 →
        // flota); arrastre fuerte amortigua la velocidad → el objeto se ASIENTA flotando en la
        // superficie (equilibrio a f≈1/buoyRatio) en vez de oscilar. up radial ≈ world-up cerca
        // del jugador (donde la gravedad plana -Y ya apunta hacia el planeta).
        if (haveSea && body->radius > 1e-4) {
            glm::dvec3 rel2 = body->position - seaCenter;
            double dist = glm::length(rel2);
            double r    = body->radius;
            if (dist > 1e-6 && dist < seaR + r) {
                glm::dvec3 up = rel2 / dist;
                double f = glm::clamp((seaR + r - dist) / (2.0 * r), 0.0, 1.0);
                // SPLASH: al CRUZAR la superficie hacia dentro (aire→agua) con velocidad de entrada
                // apreciable → dispara el callback UNA vez (no cada frame, por el flag inWater). El
                // juego emite partículas PBF ahí → el impacto salpica.
                if (f > 0.05 && !body->inWater) {
                    double vDown = -glm::dot(body->velocity, up); // componente hacia el agua (m/s)
                    if (vDown > 1.5 && m_onWaterEntry)
                        m_onWaterEntry(seaCenter + up * seaR, body->velocity, r);
                    body->inWater = true;
                }
                if (f > 0.0) {
                    double g = glm::length(gravity);
                    const double buoyRatio = 1.1;                 // agua/objeto (>1 = flota)
                    body->velocity += up * (f * g * buoyRatio * dt);   // Arquímedes
                    body->velocity -= body->velocity * (1.0 - std::exp(-3.0 * f * dt)); // arrastre
                }
            } else {
                body->inWater = false; // fuera del agua → rearma el splash para la próxima entrada
            }
        }

        body->position += body->velocity * dt;

        // --- GROUND-SNAP contra el TERRENO ------------------------------------------------------
        // El suelo está en radio = planetRadius + altura del terreno + radio del cuerpo. Si el cuerpo
        // cayó por debajo, se sube a la superficie y se le quita la componente de velocidad que
        // apunta HACIA DENTRO (la tangencial se conserva → puede deslizar/andar). El terreno lo da
        // m_world (cliente: la malla F10; servidor: el sampler analítico) → misma superficie que la
        // que se ve/valida. Sustituye al "surface constraint" que el juego hacía a mano.
        if (haveSea && body->radius > 1e-4) {
            const glm::dvec3 rel = body->position - seaCenter;
            const double dist = glm::length(rel);
            if (dist > 1e-6) {
                const glm::dvec3 up = rel / dist;
                if (body->shape == RigidBody::Shape::Box && m_world) {
                    // COLISIÓN CONTRA EL TERRENO POR LOS VÉRTICES DE LA FORMA REAL. Si el cuerpo define
                    // sus `points` (casco convexo del objeto), se prueba cada uno; si no, la caja usa sus
                    // 8 esquinas. Así la colisión sigue la FORMA del objeto (no una esfera/caja fija). El
                    // cuerpo se apoya en su cara/vértices y VUELCA sobre la arista si el centro de masa se
                    // sale del apoyo. ⚠️ ESTABILIDAD (antes "flotaba/giraba"): posición corregida por la
                    // penetración MÁXIMA una vez (no sumada → no se autolanza), impulsos REPARTIDOS entre
                    // contactos, y amortiguación angular fuerte + asentamiento al descansar lento.
                    const glm::dvec3 comW = body->orientation * body->comOffset;
                    const double invM = 1.0 / body->mass;
                    const double havg = (body->halfExtents.x + body->halfExtents.y + body->halfExtents.z) / 3.0;
                    const double invI = 1.0 / ((1.0 / 6.0) * body->mass * (2.0 * havg) * (2.0 * havg) + 1e-9);
                    glm::dvec3 hit[32]; int nh = 0; double maxPen = 0.0;
                    auto test = [&](const glm::dvec3& local) {
                        const glm::dvec3 corner = body->position + body->orientation * local;
                        const double cdist = glm::length(corner - seaCenter);
                        if (cdist < 1e-6) return;
                        const double pen = (seaR + m_world->terrainHeightAt(corner)) - cdist;
                        if (pen > 0.0 && nh < 32) { hit[nh++] = corner; maxPen = std::max(maxPen, pen); }
                    };
                    if (!body->points.empty()) {
                        for (const glm::dvec3& p : body->points) test(p);
                    } else {
                        for (int sx = -1; sx <= 1; sx += 2)
                        for (int sy = -1; sy <= 1; sy += 2)
                        for (int sz = -1; sz <= 1; sz += 2)
                            test(glm::dvec3(sx * body->halfExtents.x, sy * body->halfExtents.y, sz * body->halfExtents.z));
                    }
                    if (nh > 0) {
                        body->position += up * (maxPen * 0.8);                    // sacar del suelo UNA vez
                        const double relax = 1.0 / (double)nh;                    // repartir entre esquinas
                        for (int k = 0; k < nh; ++k) {
                            const glm::dvec3 rc = hit[k] - (body->position + comW);
                            const glm::dvec3 vc = body->velocity + glm::cross(body->angularVel, rc);
                            const double vn = glm::dot(vc, up);
                            if (vn >= 0.0) continue;
                            const glm::dvec3 rcxn = glm::cross(rc, up);
                            const double kn = invM + glm::dot(up, glm::cross(rcxn * invI, rc));
                            const double e = (vn < -1.5) ? 0.3 : 0.0;              // BOTE solo en impactos fuertes
                            const double jn = (-(1.0 + e) * vn / std::max(kn, 1e-9)) * relax;   // relajado
                            const glm::dvec3 P = up * jn;
                            body->velocity   += P * invM;
                            body->angularVel += glm::cross(rc, P) * invI;
                            const glm::dvec3 vt = vc - up * vn;                     // rozamiento tangencial
                            const double vtl = glm::length(vt);
                            if (vtl > 1e-6) {
                                const glm::dvec3 t = vt / vtl;
                                const glm::dvec3 rcxt = glm::cross(rc, t);
                                const double kt = invM + glm::dot(t, glm::cross(rcxt * invI, rc));
                                const double jt = glm::clamp((-vtl / std::max(kt, 1e-9)) * relax, -0.7 * jn, 0.7 * jn);
                                const glm::dvec3 Pt = t * jt;
                                body->velocity   += Pt * invM;
                                body->angularVel += glm::cross(rc, Pt) * invI;
                            }
                        }
                        // Amortiguación angular: fuerte, y AÚN más cuando descansa girando lento →
                        // ASIENTA en vez de rotar sobre sí mismo eternamente (lo que veías).
                        const double slow = glm::length(body->angularVel) < 0.6 ? 8.0 : 3.0;
                        body->angularVel *= std::max(0.0, 1.0 - slow * dt);
                    }
                } else {
                const double terrainM = m_world ? m_world->terrainHeightAt(body->position) : 0.0;
                const double floorDist = seaR + terrainM + body->radius;
                if (dist < floorDist) {
                    body->position = seaCenter + up * floorDist;          // sube al suelo
                    const double vIn = glm::dot(body->velocity, up);      // <0 = cayendo hacia dentro
                    if (vIn < 0.0) {
                        const double e = (vIn < -1.5) ? 0.3 : 0.0;        // BOTE solo en impactos fuertes (no en reposo)
                        body->velocity -= up * vIn * (1.0 + e);           // anula lo radial + rebota una fracción
                    }

                    // DESLIZAMIENTO POR PENDIENTE. La componente de la gravedad TANGENTE al terreno
                    // empuja cuesta abajo, pero solo en cuestas más empinadas que el ÁNGULO DE REPOSO
                    // (kReposeCos): así los objetos sueltos ruedan por las laderas fuertes y se quedan
                    // en lo llano — y el jugador NO resbala en pendientes suaves. La normal sale de
                    // muestrear el MISMO campo del suelo (terrainHeightAt) en dos tangentes → coincide
                    // con la superficie que se ve/colisiona. + rozamiento tangencial (los sueltos frenan).
                    constexpr double kReposeCos      = 0.82;   // cos(~35°): más empinado → desliza
                    constexpr double kGroundFriction = 2.5;    // 1/s de rozamiento tangencial en el suelo
                    glm::dvec3 t1 = glm::cross(up, std::abs(up.y) < 0.99 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0));
                    const double t1len = glm::length(t1);
                    if (m_world && t1len > 1e-9) {
                        t1 /= t1len;
                        const glm::dvec3 t2 = glm::cross(up, t1);
                        const double e  = std::max(0.25, body->radius);
                        const double dh1 = (m_world->terrainHeightAt(body->position + t1 * e)
                                          - m_world->terrainHeightAt(body->position - t1 * e)) / (2.0 * e);
                        const double dh2 = (m_world->terrainHeightAt(body->position + t2 * e)
                                          - m_world->terrainHeightAt(body->position - t2 * e)) / (2.0 * e);
                        const glm::dvec3 N = glm::normalize(up - t1 * dh1 - t2 * dh2);
                        if (glm::dot(N, up) < kReposeCos) {              // cuesta > reposo → deslizar
                            const glm::dvec3 gvec = gDir * gMag;         // gravedad (radial)
                            const glm::dvec3 tang = gvec - glm::dot(gvec, N) * N;   // proyección cuesta abajo
                            body->velocity += tang * dt;
                        }
                        const glm::dvec3 vTang = body->velocity - glm::dot(body->velocity, up) * up;
                        body->velocity -= vTang * std::min(kGroundFriction * dt, 1.0);
                    }
                    // VUELCO por BALANCE: si el centro de masa está DESCENTRADO (comOffset≠0), apoyado
                    // la gravedad hace PALANCA → un torque que gira el cuerpo hacia donde pesa (vuelca
                    // por un lado y no por otro). Inercia ∝ masa·radio² → los más pesados/grandes
                    // vuelcan más despacio. comOffset=0 (equilibrado) → torque nulo, no vuelca.
                    if (glm::dot(body->comOffset, body->comOffset) > 1e-12) {
                        const glm::dvec3 comW   = body->orientation * body->comOffset;   // offset → mundo
                        const glm::dvec3 torque = glm::cross(comW, gDir * (gMag * body->mass));
                        const double     I      = 0.4 * body->mass * body->radius * body->radius + 1e-6;
                        body->angularVel += (torque / I) * dt;
                        body->angularVel *= std::max(0.0, 1.0 - 1.5 * dt);              // rozamiento angular
                    }
                }
                }   // cierra el else (rama ESFERA)
            }
        }

        // Integrar la ORIENTACIÓN desde la velocidad angular (cualquier cuerpo que gire; con angularVel=0
        // no hay cambio). Cuaternión: q ← normalize(q + ½·ω·q·dt).
        if (glm::dot(body->angularVel, body->angularVel) > 1e-14) {
            const glm::dquat w(0.0, body->angularVel.x, body->angularVel.y, body->angularVel.z);
            body->orientation = glm::normalize(body->orientation + 0.5 * w * body->orientation * dt);
        }
    }
}

namespace {
    // Vértices del mundo de un cuerpo con forma: su casco `points` si lo tiene, si no las 8 esquinas.
    inline int shapeVertices(const RigidBody* b, glm::dvec3 out[32]) {
        int n = 0;
        if (!b->points.empty()) {
            for (const auto& p : b->points) if (n < 32) out[n++] = b->position + b->orientation * p;
        } else {
            for (int sx = -1; sx <= 1; sx += 2) for (int sy = -1; sy <= 1; sy += 2) for (int sz = -1; sz <= 1; sz += 2)
                out[n++] = b->position + b->orientation * glm::dvec3(sx * b->halfExtents.x, sy * b->halfExtents.y, sz * b->halfExtents.z);
        }
        return n;
    }
    // ¿Punto p (mundo) DENTRO de la caja? outN = normal de cara SALIENTE (mundo), pen = profundidad (eje mínimo).
    inline bool pointInBox(const glm::dvec3& p, const RigidBody* box, glm::dvec3& outN, double& pen) {
        const glm::dvec3 lp = glm::conjugate(box->orientation) * (p - box->position);
        const glm::dvec3 h = box->halfExtents;
        const double dx = h.x - std::abs(lp.x), dy = h.y - std::abs(lp.y), dz = h.z - std::abs(lp.z);
        if (dx < 0.0 || dy < 0.0 || dz < 0.0) return false;               // fuera
        glm::dvec3 ln;
        if (dx <= dy && dx <= dz)      { ln = glm::dvec3(lp.x < 0 ? -1 : 1, 0, 0); pen = dx; }
        else if (dy <= dz)             { ln = glm::dvec3(0, lp.y < 0 ? -1 : 1, 0); pen = dy; }
        else                           { ln = glm::dvec3(0, 0, lp.z < 0 ? -1 : 1); pen = dz; }
        outN = box->orientation * ln;                                     // normal saliente en el mundo
        return true;
    }
}

void PhysicsEngine::detectCollisions() {
    collisions.clear();

    // NARROW-PHASE POR FORMA (nada de radio envolvente para la colisión, salvo esfera↔esfera, que ES
    // su forma real). El radio solo lo usa el BROAD-PHASE para descartar pares.
    //  · esfera↔esfera: 1 contacto por radios.
    //  · caja↔caja: los VÉRTICES de cada una dentro de la otra → un contacto por vértice (apila real).
    //  · esfera↔caja: el punto más cercano de la caja al centro de la esfera.
    auto narrow = [&](RigidBody* a, RigidBody* b) {
        const bool aBox = (a->shape == RigidBody::Shape::Box);
        const bool bBox = (b->shape == RigidBody::Shape::Box);
        if (!aBox && !bBox) {                                   // esfera ↔ esfera
            const glm::dvec3 d = b->position - a->position;
            const double dist = glm::length(d), minD = a->radius + b->radius;
            if (dist < minD && dist > 1e-9) {
                CollisionInfo c; c.bodyA = a; c.bodyB = b; c.penetration = minD - dist;
                c.normal = d / dist; c.point = a->position + c.normal * a->radius;
                collisions.push_back(c);
            }
            return;
        }
        if (aBox && bBox) {                                    // caja ↔ caja (por vértices, ambos sentidos)
            glm::dvec3 v[32], nOut; double pen;
            const int na = shapeVertices(a, v);
            for (int i = 0; i < na; ++i) if (pointInBox(v[i], b, nOut, pen)) {
                CollisionInfo c; c.bodyA = a; c.bodyB = b; c.penetration = pen;
                c.normal = -nOut; c.point = v[i]; collisions.push_back(c);   // saca el vértice de A fuera de B
            }
            const int nb = shapeVertices(b, v);
            for (int i = 0; i < nb; ++i) if (pointInBox(v[i], a, nOut, pen)) {
                CollisionInfo c; c.bodyA = a; c.bodyB = b; c.penetration = pen;
                c.normal = nOut; c.point = v[i]; collisions.push_back(c);    // saca el vértice de B fuera de A
            }
            return;
        }
        // esfera ↔ caja
        RigidBody* s = aBox ? b : a;                            // la esfera
        RigidBody* x = aBox ? a : b;                            // la caja
        const glm::dvec3 lc = glm::conjugate(x->orientation) * (s->position - x->position);
        const glm::dvec3 cl = glm::clamp(lc, -x->halfExtents, x->halfExtents);
        glm::dvec3 nOut; double pen;
        if (cl == lc) {                                        // centro DENTRO de la caja
            if (!pointInBox(s->position, x, nOut, pen)) return;
            pen += s->radius;
        } else {
            const glm::dvec3 wc = x->position + x->orientation * cl;
            const glm::dvec3 delta = s->position - wc;
            const double dist = glm::length(delta);
            if (dist >= s->radius || dist < 1e-9) return;
            nOut = delta / dist; pen = s->radius - dist;
        }
        CollisionInfo c; c.bodyA = a; c.bodyB = b; c.penetration = pen;
        c.point = x->position + x->orientation * cl;
        c.normal = (s == a) ? -nOut : nOut;                    // nOut va de la caja hacia la esfera
        collisions.push_back(c);
    };

    if (!octree) {
        for (size_t i = 0; i < bodies.size(); ++i)
            for (size_t j = i + 1; j < bodies.size(); ++j)
                narrow(bodies[i].get(), bodies[j].get());
    } else {
        for (auto& body : bodies) {
            std::vector<std::shared_ptr<RigidBody>> nearby;
            octree->getNearbodies(body, nearby);
            for (auto& other : nearby)
                if (body.get() < other.get())                  // cada par UNA vez (orden estable por puntero)
                    narrow(body.get(), other.get());
        }
    }
}

void PhysicsEngine::resolveCollisions() {
    // Resolución por IMPULSOS en el PUNTO DE CONTACTO real (col.point): separa el solape, intercambia
    // MOMENTO (lineal) y aplica impulso ANGULAR con el brazo al centro de masa → los cuerpos se empujan
    // y GIRAN al chocar según DÓNDE se tocan (una caja golpeada en una esquina vuelca). Inercia POR
    // FORMA (caja vs esfera). Rozamiento tangencial en el cono de Coulomb.
    constexpr double kFriction = 0.5;
    auto invInertia = [](const RigidBody* b) -> double {
        if (b->isKinematic) return 0.0;
        double I;
        if (b->shape == RigidBody::Shape::Box) {
            const double havg = (b->halfExtents.x + b->halfExtents.y + b->halfExtents.z) / 3.0;
            I = (1.0 / 6.0) * b->mass * (2.0 * havg) * (2.0 * havg);
        } else {
            I = 0.4 * b->mass * b->radius * b->radius;
        }
        return 1.0 / (I + 1e-9);
    };
    for (auto& col : collisions) {
        RigidBody* A = col.bodyA; RigidBody* B = col.bodyB;
        const bool aKin = A->isKinematic, bKin = B->isKinematic;
        if (aKin && bKin) continue;
        const glm::dvec3 n = col.normal;                        // de A hacia B
        const double invMa = aKin ? 0.0 : 1.0 / A->mass;
        const double invMb = bKin ? 0.0 : 1.0 / B->mass;
        const double invSum = invMa + invMb;
        if (invSum <= 0.0) continue;

        // (1) Separación del solape (Baumgarte 0.8: suave, para no explotar con varios contactos/par).
        const double corr = (col.penetration * 0.8) / invSum;
        A->position -= n * (corr * invMa);
        B->position += n * (corr * invMb);

        // (2) Impulso en el punto de contacto REAL, con inercia por forma.
        const double invIa = invInertia(A), invIb = invInertia(B);
        const glm::dvec3 rA = col.point - (A->position + A->orientation * A->comOffset);
        const glm::dvec3 rB = col.point - (B->position + B->orientation * B->comOffset);
        const glm::dvec3 vRel = (B->velocity + glm::cross(B->angularVel, rB))
                              - (A->velocity + glm::cross(A->angularVel, rA));
        const double vn = glm::dot(vRel, n);
        if (vn < 0.0) {                                         // se acercan → responder
            const double e = (vn < -1.5) ? 0.2 : 0.0;          // bote solo en impactos fuertes
            const glm::dvec3 rAxn = glm::cross(rA, n), rBxn = glm::cross(rB, n);
            const double kn = invSum + glm::dot(n, glm::cross(rAxn * invIa, rA))
                                     + glm::dot(n, glm::cross(rBxn * invIb, rB));
            const double jn = -(1.0 + e) * vn / std::max(kn, 1e-9);
            const glm::dvec3 P = n * jn;
            A->velocity   -= P * invMa;                B->velocity   += P * invMb;
            A->angularVel -= glm::cross(rA, P) * invIa; B->angularVel += glm::cross(rB, P) * invIb;

            // (3) Rozamiento tangencial (cono de Coulomb) → frena el deslizamiento y da GIRO al chocar.
            const glm::dvec3 vt = vRel - n * vn;
            const double vtl = glm::length(vt);
            if (vtl > 1e-6) {
                const glm::dvec3 t = vt / vtl;
                const glm::dvec3 rAxt = glm::cross(rA, t), rBxt = glm::cross(rB, t);
                const double kt = invSum + glm::dot(t, glm::cross(rAxt * invIa, rA))
                                         + glm::dot(t, glm::cross(rBxt * invIb, rB));
                const double jt = glm::clamp(-vtl / std::max(kt, 1e-9), -kFriction * jn, kFriction * jn);
                const glm::dvec3 Pt = t * jt;
                A->velocity   -= Pt * invMa;                B->velocity   += Pt * invMb;
                A->angularVel -= glm::cross(rA, Pt) * invIa; B->angularVel += glm::cross(rB, Pt) * invIb;
            }
        }
    }
}

void PhysicsEngine::addStaticBox(const glm::dvec3& center, const glm::dvec3& halfExtents) {
    staticBoxes.push_back({ center - halfExtents, center + halfExtents });
    ++m_staticsVersion;
}

void PhysicsEngine::clearStaticBoxes() {
    staticBoxes.clear();
    ++m_staticsVersion;
}

void PhysicsEngine::addPlacedOBB(const glm::dvec3& center, const glm::dvec3& halfExtents, const glm::dmat3& rot) {
    placedOBBs.push_back({ center, halfExtents, rot });
    ++m_staticsVersion;   // Jolt reconstruye sus cuerpos estáticos en el siguiente paso
}

void PhysicsEngine::clearPlacedOBBs() {
    placedOBBs.clear();
    ++m_staticsVersion;
}

void PhysicsEngine::addPropOBB(const glm::dvec3& center, const glm::dvec3& halfExtents, const glm::dmat3& rot) {
    propOBBs.push_back({ center, halfExtents, rot });
    ++m_staticsVersion;   // los recursos se regeneran al moverse → Jolt refresca sus estáticos
}

void PhysicsEngine::clearPropOBBs() {
    propOBBs.clear();
    ++m_staticsVersion;
}

glm::dvec3 PhysicsEngine::resolveSphere(const glm::dvec3& center0, double radius,
                                        const glm::dvec3& up, bool& grounded) const {
    glm::dvec3 center = center0;
    auto process = [&](const std::vector<StaticOBB>& list) {
        for (const auto& b : list) {
            // Broad-phase: salta cajas lejanas (clave con cientos de props).
            glm::dvec3 dd = center - b.center;
            double maxR = radius + glm::length(b.halfExtents) + 0.5;
            if (glm::dot(dd, dd) > maxR * maxR) continue;

            // Centro de la esfera al espacio LOCAL del OBB (rot ortonormal → inv = transpose).
            glm::dvec3 lp = glm::transpose(b.rot) * (center - b.center);
            const glm::dvec3& he = b.halfExtents;
            glm::dvec3 cp = glm::clamp(lp, -he, he);
            glm::dvec3 d  = lp - cp;
            double dist2  = glm::dot(d, d);

            glm::dvec3 newlp = lp;
            if (dist2 <= 1e-12) {                       // centro DENTRO → empuja por la cara más cercana
                double px = he.x - std::abs(lp.x);
                double py = he.y - std::abs(lp.y);
                double pz = he.z - std::abs(lp.z);
                double m  = std::min({ px, py, pz });
                if      (m == px) newlp.x = (lp.x >= 0.0 ? he.x + radius : -he.x - radius);
                else if (m == py) newlp.y = (lp.y >= 0.0 ? he.y + radius : -he.y - radius);
                else              newlp.z = (lp.z >= 0.0 ? he.z + radius : -he.z - radius);
            } else if (dist2 < radius * radius) {       // la esfera roza la caja → empuja por la normal
                double dist = std::sqrt(dist2);
                newlp = cp + (dist > 1e-9 ? d / dist : glm::dvec3(0, 1, 0)) * radius;
            } else {
                continue;                               // no toca
            }

            glm::dvec3 newCenter = b.center + b.rot * newlp;
            if (glm::dot(newCenter - center, up) > 0.3 * radius) grounded = true; // apoyado encima
            center = newCenter;
        }
    };
    process(placedOBBs);
    process(propOBBs);
    return center;
}

void PhysicsEngine::broadPhaseAABB() {
    if (!octree || bodies.empty()) return;
    // Rebuild the octree each step so moved bodies are found correctly.
    // Each body is re-inserted using its updated position from integrateForces().
    for (auto& body : bodies) {
        octree->remove(body);
        octree->insert(body);
    }
}

void PhysicsEngine::resolveStaticCollisions() {
    for (auto& body : bodies) {
        if (body->isKinematic) continue;

        for (const auto& box : staticBoxes) {
            // Sphere center (body->position IS the sphere center)
            const glm::dvec3& c = body->position;
            const double      r = body->radius;

            // Closest point on AABB to sphere center
            glm::dvec3 closest = glm::clamp(c, box.bmin, box.bmax);
            glm::dvec3 diff    = c - closest;
            double dist = glm::length(diff);

            double penetration = r - dist;
            if (penetration <= 0.0) continue;

            glm::dvec3 normal;
            if (dist < 1e-9) {
                // Center is inside the box — push out on the minimum-penetration axis
                double px = std::min(c.x - box.bmin.x, box.bmax.x - c.x);
                double py = std::min(c.y - box.bmin.y, box.bmax.y - c.y);
                double pz = std::min(c.z - box.bmin.z, box.bmax.z - c.z);
                if (px <= py && px <= pz)
                    normal = (c.x < (box.bmin.x + box.bmax.x) * 0.5) ? glm::dvec3(-1,0,0) : glm::dvec3(1,0,0);
                else if (py <= px && py <= pz)
                    normal = (c.y < (box.bmin.y + box.bmax.y) * 0.5) ? glm::dvec3(0,-1,0) : glm::dvec3(0,1,0);
                else
                    normal = (c.z < (box.bmin.z + box.bmax.z) * 0.5) ? glm::dvec3(0,0,-1) : glm::dvec3(0,0,1);
                penetration = r + std::min({px, py, pz});
            } else {
                normal = diff / dist;
            }

            // Push body out of the box
            body->position += normal * penetration;

            // Cancel velocity component directed into the surface
            double vn = glm::dot(body->velocity, normal);
            if (vn < 0.0) body->velocity -= normal * vn;
        }
    }
}

double PhysicsEngine::calculateGravityAtPosition(const glm::dvec3& worldPos, glm::dvec3& outGravityDir) {
    if (!m_world) {
        outGravityDir = glm::dvec3(0.0, -1.0, 0.0);
        return 9.81;  // Default Earth gravity
    }

    glm::dvec3 totalGravity = glm::dvec3(0.0);

    // Sum gravity from all celestial bodies
    const auto& bodies = m_world->gravBodies();
    for (const auto& body : bodies) {
        totalGravity += calculateGravityContribution(body, worldPos);
    }
    
    double magnitude = glm::length(totalGravity);
    if (magnitude > 1e-6) {
        outGravityDir = glm::normalize(totalGravity);
    } else {
        outGravityDir = glm::dvec3(0.0, -1.0, 0.0);
        magnitude = 0.0;
    }
    
    return magnitude;
}

glm::dvec3 PhysicsEngine::calculateGravityContribution(const GravBody& body, const glm::dvec3& worldPos) {
    // Calculate vector from test position to body
    glm::dvec3 delta = body.worldPos - worldPos;
    double distance = glm::length(delta);
    
    // Avoid division by zero
    if (distance < 1e-6) {
        return glm::dvec3(0.0);
    }
    
    // Newton's universal law of gravitation: F = G * m1 * m2 / r²
    // Acceleration: a = F / m1 = G * m2 / r²
    double acceleration = gravitationalConstant * body.mass / (distance * distance);
    glm::dvec3 direction = glm::normalize(delta);
    
    return direction * acceleration;
}

bool PhysicsEngine::checkTerrainCollision(
    const glm::dvec3& worldPos,
    glm::dvec3* outCollisionPoint,
    glm::dvec3* outCollisionNormal) {
    // (Fase 1) El ground-snap real irá aquí, usando m_world->terrainHeightAt. Hoy es un stub
    // (nunca se usaba: el ground se hacía analíticamente en el juego). Desacoplado ya del engine.
    (void)worldPos; (void)outCollisionPoint; (void)outCollisionNormal;
    return false;
}

void PhysicsEngine::applyGravity(const glm::dvec3& worldPos, double deltaTime, glm::dvec3& inOutVelocity) {
    glm::dvec3 gravityDir;
    double gravityMagnitude = calculateGravityAtPosition(worldPos, gravityDir);
    
    // Apply gravitational acceleration: v += a * dt
    glm::dvec3 gravityAcceleration = gravityDir * gravityMagnitude;
    inOutVelocity += gravityAcceleration * deltaTime;
}

}} // namespace Haruka::Physics
