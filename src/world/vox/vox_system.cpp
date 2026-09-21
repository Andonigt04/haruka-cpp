#include "world/vox/vox_system.h"

#include "core/camera.h"
#include "core/logger.h"
#include "core/modules.h"
#include "physics/physics_engine.h"
#include "renderer/vox_renderer.h"
#include "world/planet/planet.h"
#include "world/planet/planetary_system.h"
#include "world/terrain/terrain_lod.h"

#include <cstdlib>
#include <vector>

namespace Haruka::World {

// ── LAS PAREDES DE LAS CUEVAS COLISIONAN ────────────────────────────────────────────────────────
// Lo que el render remalló este frame va a Jolt con la MISMA malla (surface nets del mismo campo),
// así que lo que se ve es lo que se choca. Por revisión: un chunk quieto no cuesta nada.
void VoxSystem::syncColliders(const Frame& f) {
    // ⚠️ SIN `#ifdef HARUKA_MOD_PLANETS`: ese símbolo no lo define nadie (lo usa `application.cpp`
    // en el bloque de HARUKA_CAM_PITCH, que por eso tampoco encuentra el planeta). Con él, esto se
    // compilaba a nada y las paredes de la cueva no llegaban a Jolt: "traspaso la cueva".
    if (!f.physics || !f.planets) return;
    const auto* tp = f.planets->activeTerrestrial();
    if (!tp) return;
    const auto& meshes = tp->voxRenderer().cpuMeshes();
    { static size_t s_last = ~(size_t)0; if (meshes.size() != s_last) { s_last = meshes.size();
      HARUKA_LOGDIAG("Vox", "syncVoxColliders: %zu mallas CPU del render", meshes.size()); } }
    for (const auto& [key, cm] : meshes) {
        const uint64_t pk = Haruka::VoxRenderer::physicsKey(key);
        auto it = m_colliderRev.find(pk);
        if (it != m_colliderRev.end() && it->second == cm.revision) continue;
        m_colliderRev[pk] = cm.revision;
        if (cm.mesh.empty()) { f.physics->removeVoxMesh(pk); continue; }
        // El cuerpo va en coordenadas de MUNDO: origen del chunk + centro del planeta.
        f.physics->setVoxMesh(pk, &cm.mesh.positions[0].x, cm.mesh.positions.size(),
                                   cm.mesh.indices.data(), cm.mesh.indices.size(),
                                   tp->position() + cm.mesh.origin);
    }
    // Lo que el render soltó, la física también.
    for (auto it = m_colliderRev.begin(); it != m_colliderRev.end();) {
        bool vivo = false;
        for (const auto& [key, cm] : meshes) if (Haruka::VoxRenderer::physicsKey(key) == it->first) { vivo = true; break; }
        if (vivo) { ++it; continue; }
        f.physics->removeVoxMesh(it->first);
        it = m_colliderRev.erase(it);
    }
}

// ── EL SUELO CERCANO, DIBUJADO DESDE LA GEOMETRÍA DE LA COLISIÓN ────────────────────────────────
//
// Paso 3 del plan de paridad. El render deja de ser una SEGUNDA evaluación del terreno dentro de
// ±256 m y pasa a dibujar los mismos vértices y los mismos triángulos que Jolt colisiona.
//
// Por qué no basta con afinar el shader: los cuatro nodos de cada quad ya coinciden exactamente
// (0 de 59 785 fuera de la retícula, medido por `clipmap_vertex_lattice` antes de borrarlo con el
// clipmap), pero un quad no es plano y hay que
// partirlo en dos triángulos. Jolt parte por `(i,j)→(i+1,j+1)`; el teselador de la GPU parte por
// donde quiera, porque el spec de OpenGL NO lo fija para `layout(quads, ...)`. La diferencia en el
// centro del quad, medida sobre el terreno real, son 2-4 cm bajo los pies. Mientras el teselador
// esté en el bucle esa disparidad no se puede cerrar, solo acotar.
//
// De momento OPT-IN (`HARUKA_NEAR_RING=1`) y dibujando encima del clipmap sin tocarlo: revertir es
// quitar un draw call. Sombreado mínimo a propósito — lo que hay que juzgar aquí es si la geometría
// coincide con el alambre verde, y un material completo lo escondería.
void VoxSystem::updateNearGroundRing(const Frame& f) {
#ifdef HARUKA_MOD_PHYSICS
    // ── ENCENDIDO POR DEFECTO, con salida de emergencia ─────────────────────────────────────────
    //
    // Nació opt-in (`HARUKA_NEAR_RING=1`) mientras se validaba que la geometría de la colisión servía
    // para dibujar. Ya está validado, así que el camino por defecto es éste: dentro de ±192 m el
    // suelo que se dibuja son los MISMOS vértices y los MISMOS triángulos que se pisan, y el
    // teselador queda fuera del bucle.
    //
    // ⚠️ La variable se queda, ahora para APAGARLO (`HARUKA_NEAR_RING=0`). No es simetría cosmética:
    // si esto falla, el clipmap ya ha abierto su hueco de 192 m y el fallo se ve como un AGUJERO en
    // el planeta alrededor del jugador. Poder volver al camino de siempre sin recompilar ni revertir
    // es lo que separa un bug molesto de una partida imposible.
    static int s_env = -1;
    if (s_env < 0) {
        const char* e = std::getenv("HARUKA_NEAR_RING");
        s_env = (e && e[0] == '0') ? 0 : 1;
        if (s_env == 0) HARUKA_LOGW("NearGround", "APAGADO por HARUKA_NEAR_RING=0: el suelo cercano "
                                    "vuelve a salir del teselador y la paridad deja de ser exacta");
    }
    // ── ⚠️ Y NO SE DIBUJA SI EL PASE v5 ESTA ACTIVO ─────────────────────────────────────────────
    //
    // El anillo nacio contra el CLIPMAP, **que le abria un hueco de 192 m**. El pase de nodos v5 no
    // abre ese hueco: dibuja la superficie entera, incluida esa franja. Resultado, dos superficies
    // coplanares describiendo el mismo relieve peleando por el z-buffer — que es literalmente lo que
    // la cabecera de `terrain_node_renderer.h` dice que el v5 viene a quitar.
    //
    // Andoni lo vio como *"las lineas de triangulos son blanco-azuladas"*. Medido en el juego con la
    // hora congelada, energia de borde (|dif| con el pixel de al lado; las lineas finas la disparan):
    //
    //     OpenGL   anillo ON 5,33   ·   anillo OFF 1,42
    //     Vulkan   anillo ON 5,34   ·   anillo OFF 1,43
    //
    // Identico en los dos backends: no era Vulkan. Y el post no influye (5,41 con, 5,43 sin).
    //
    // ⚠️ LO QUE SE PIERDE, dicho con su numero: la paridad EXACTA triangulo-a-triangulo cerca del
    // jugador. Ya no hace falta — el autotest del alambre mide **0,0004 m de media** entre lo que se
    // dibuja y lo que se pisa, con un unico outlier de 0,0418 m. Como mucho 4 cm en el peor vertice.
    // `HARUKA_NEAR_RING=1` lo fuerza otra vez para poder comparar sin recompilar.
    {
        const char* force = std::getenv("HARUKA_NEAR_RING");
        const bool forced = force && force[0] == '1';
        if (!forced && Haruka::Terrain::TerrainNodeRenderer::enabled()) {
            static bool s_said = false;
            if (!s_said) { s_said = true;
                HARUKA_LOGI("NearGround", "no se dibuja: el pase v5 ya cubre esa franja y las dos "
                            "superficies peleaban por el z-buffer (borde 5,33 -> 1,42). "
                            "HARUKA_NEAR_RING=1 lo fuerza."); }
            return;
        }
    }
    if (s_env != 1 || !f.physics || !f.camera || !f.planets) return;

    Physics::PhysicsEngine::NearGroundRing ring;
    if (!f.physics->getNearGroundRing(ring)) return;

    // La GEOMETRÍA solo se re-sube cuando la física reconstruye el anillo (al saltar el anclaje del
    // clipmap): son ~16 600 vértices. El ancla relativa al ojo, en cambio, va CADA FRAME.
    static std::vector<glm::vec3>  s_pos;
    static std::vector<uint32_t>   s_tris;
    const bool fresh = (ring.revision != m_ringRev);
    if (fresh) {
        m_ringRev    = ring.revision;
        m_ringAnchor = ring.anchor;
        // ⚠️ RELATIVO AL ANCLA, en float. En coordenadas de mundo (~6,37e6 m) un float tiene ~0,5 m
        // de resolución: el suelo temblaría medio metro y este parche inventaría la disparidad que
        // viene a eliminar. La resta va en DOUBLE y solo el resultado baja a float.
        s_pos.clear(); s_pos.reserve(ring.verts.size());
        uint64_t ck = 1469598103934665603ull;
        for (const auto& v : ring.verts) {
            const glm::vec3 f(v - ring.anchor);
            s_pos.push_back(f);
            for (int c = 0; c < 3; ++c) { uint32_t b; std::memcpy(&b, &f[c], 4); ck = (ck ^ b) * 1099511628211ull; }
        }
        s_tris = ring.tris;
        // El checksum tiene que coincidir con el que imprime `Physics` al publicar. "Son los mismos
        // bytes" es comprobable, así que se comprueba en vez de suponerse.
        HARUKA_LOGI("NearGround", "anillo recibido: rev %llu · %zu vert · %zu tris · celda %.1f m · "
                    "alcance +-%.0f m · checksum %016llx",
                    (unsigned long long)ring.revision, s_pos.size(), s_tris.size() / 3,
                    ring.cell, ring.extent, (unsigned long long)ck);
    }
    if (s_pos.size() < 3) return;

    // Traslación ancla→ojo POR FRAME y en double: los vértices son relativos al ancla (fija entre
    // reconstrucciones) y esto los lleva al ojo (móvil). Sin esta mitad el parche quedaría pegado a
    // la cámara y se deslizaría sobre el terreno al caminar.
    const glm::vec3 anchorRelEye = glm::vec3(m_ringAnchor - glm::dvec3(f.camera->position));
    glm::dvec3 upD(0.0, 1.0, 0.0);
    glm::dvec3 pc; double pr = 0.0;
    if (f.planets->getActivePlanet(pc, pr) && glm::length(m_ringAnchor - pc) > 1e-9)
        upD = glm::normalize(m_ringAnchor - pc);

    // (Aquí hubo una sonda que avisaba si el ancla del clipmap y la del anillo caían en celdas
    // distintas. Cazó el fallo —hasta 4,00 m de desfase, un escalón entero— y con él se corrigió el
    // anclaje: el clipmap ahora se ancla DONDE EL ANILLO, así que coinciden por construcción y la
    // sonda medía un invariante que ya no puede romperse. Se retira en vez de dejarla avisando: un
    // instrumento que sigue dando números después de que su objeto desaparezca es peor que ninguno.)

    if (Haruka::Planet::TerrestrialPlanet* tp = f.planets->activeTerrestrialMut())
        tp->setNearGroundRing(fresh ? &s_pos : nullptr, fresh ? &s_tris : nullptr,
                              anchorRelEye, glm::vec3(upD), m_ringAnchor);
#endif
}

} // namespace Haruka::World
