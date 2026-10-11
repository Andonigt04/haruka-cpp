/**
 * @file TestPrefab.cpp
 * @brief PREFABRICADOS: lo que se desvía de un montaje en UN conjunto concreto.
 *
 * La regla que se prueba aquí es la que hace que un prefabricado sirva de algo: la escena guarda
 * "aquí va el barco" más **lo que le ha pasado a ESTE barco**, y nunca sus 870 tablas. Si esa regla
 * se interpreta distinto en el juego y en el editor, el mismo barco roto se ve de dos formas — el
 * error que este motor ya pagó con las aguas y con las miniaturas. Por eso vive una sola vez, en
 * `applyPrefabEdits`, y por eso tiene test.
 *
 * Lo que hay que demostrar, con su CONTRAPRUEBA (un test que solo mira que "no pete" no distingue
 * una implementación buena de una que no toca nada):
 *   1. Un cambio de pose mueve SU pieza — y deja las demás como estaban.
 *   2. Una pieza `removed` desaparece — y las que quedan son las otras, no "una menos" cualquiera.
 *   3. Los borrados NO descolocan los cambios posteriores. Es el fallo clásico de aplicar deltas
 *      por índice borrando sobre la marcha, y es MUDO: sale una pieza movida que no toca.
 *   4. Un índice fuera de rango (montaje reordenado) se ignora en vez de corromper.
 *   5. Sin cambios, el montaje sale idéntico: la lista vacía no puede "casi no hacer nada".
 */
#include "Common/TestCommon.h"
#include "Haruka/Game/Prefab/Prefab.h"
#include "Haruka/Core/Scene/SceneManager.h"

#include <filesystem>
#include <fstream>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/constants.hpp>

#include <cstdio>

using namespace Haruka;

namespace {

/// Cuatro piezas en fila, cada una con su id, para poder decir CUÁL sobrevive.
Game::Prefab::Prefab mkPrefab() {
    Game::Prefab::Prefab p;
    p.name = "banco";
    for (int i = 0; i < 4; ++i) {
        Game::Prefab::PrefabPiece pc;
        pc.id  = "tabla_" + std::to_string(i);
        pc.pos = glm::dvec3(i, 0.0, 0.0);
        p.pieces.push_back(pc);
    }
    return p;
}

} // namespace

void test_prefab_edits() {
    beginTest("prefabricado: los cambios son del CONJUNTO, no del montaje");

    // ── 5. Sin cambios no cambia nada ───────────────────────────────────────────────────────────
    {
        Game::Prefab::Prefab p = mkPrefab();
        Game::Prefab::applyPrefabEdits(p, {});
        CHECK(p.pieces.size() == 4, "sin cambios, las cuatro piezas siguen ahí");
        CHECK(p.pieces[2].id == "tabla_2" && p.pieces[2].pos.x == 2.0,
              "sin cambios, la pieza 2 está intacta");
    }

    // ── 1. Mover una pieza mueve ESA ────────────────────────────────────────────────────────────
    {
        Game::Prefab::Prefab p = mkPrefab();
        Game::Prefab::PrefabEdit e; e.index = 1; e.hasPos = true; e.pos = glm::dvec3(0.0, 9.0, 0.0);
        Game::Prefab::applyPrefabEdits(p, { e });
        CHECK(p.pieces[1].pos.y == 9.0, "la pieza tocada se mueve");
        // CONTRAPRUEBA: si `applyPrefabEdits` escribiera en todas (o en la equivocada), esto canta.
        CHECK(p.pieces[0].pos.y == 0.0 && p.pieces[2].pos.y == 0.0 && p.pieces[3].pos.y == 0.0,
              "y NINGUNA otra se mueve");
        CHECK(p.pieces.size() == 4, "mover no borra");
    }

    // ── 1b. Sustituir el item de una pieza ──────────────────────────────────────────────────────
    {
        Game::Prefab::Prefab p = mkPrefab();
        Game::Prefab::PrefabEdit e; e.index = 3; e.item = "tabla_quemada";
        Game::Prefab::applyPrefabEdits(p, { e });
        CHECK(p.pieces[3].id == "tabla_quemada", "la pieza sustituida cambia de item");
        CHECK(p.pieces[0].id == "tabla_0", "y las demás conservan el suyo");
    }

    // ── 2. Romper una pieza la quita, y quita LA SUYA ───────────────────────────────────────────
    {
        Game::Prefab::Prefab p = mkPrefab();
        Game::Prefab::PrefabEdit e; e.index = 1; e.removed = true;
        Game::Prefab::applyPrefabEdits(p, { e });
        CHECK(p.pieces.size() == 3, "la pieza rota desaparece");
        // CONTRAPRUEBA del "una menos": lo que queda tiene que ser 0, 2 y 3 — no las tres primeras.
        CHECK(p.pieces[0].id == "tabla_0" && p.pieces[1].id == "tabla_2" && p.pieces[2].id == "tabla_3",
              "y las que quedan son las OTRAS, no las tres primeras");
    }

    // ── 3. Un borrado NO descoloca los cambios de después ───────────────────────────────────────
    //
    // Es el fallo que se busca: si se borrara la pieza 0 antes de aplicar el resto, el cambio sobre
    // la 2 caería sobre la que ahora ocupa esa posición (la 3) y nadie se enteraría.
    {
        Game::Prefab::Prefab p = mkPrefab();
        Game::Prefab::PrefabEdit borra;  borra.index = 0; borra.removed = true;
        Game::Prefab::PrefabEdit mueve;  mueve.index = 2; mueve.hasPos = true; mueve.pos = glm::dvec3(0.0, 0.0, 7.0);
        Game::Prefab::applyPrefabEdits(p, { borra, mueve });
        CHECK(p.pieces.size() == 3, "queda una pieza menos");
        CHECK(p.pieces[0].id == "tabla_1" && p.pieces[1].id == "tabla_2" && p.pieces[2].id == "tabla_3",
              "y son las tres de después");
        CHECK(p.pieces[1].id == "tabla_2" && p.pieces[1].pos.z == 7.0,
              "el cambio cayó en la pieza 2 del MONTAJE, no en la que ocupa su hueco tras el borrado");
        CHECK(p.pieces[2].pos.z == 0.0, "y la 3 sigue donde estaba");
    }

    // ── 4. Un índice que ya no existe se ignora ─────────────────────────────────────────────────
    {
        Game::Prefab::Prefab p = mkPrefab();
        Game::Prefab::PrefabEdit fuera; fuera.index = 99; fuera.removed = true;
        Game::Prefab::PrefabEdit neg;   neg.index = -1;   neg.hasPos = true; neg.pos = glm::dvec3(5.0);
        Game::Prefab::applyPrefabEdits(p, { fuera, neg });
        CHECK(p.pieces.size() == 4, "un índice fuera de rango no borra nada");
        CHECK(p.pieces[0].pos.x == 0.0, "ni mueve nada");
    }

    std::printf("    cambios por conjunto: mover, sustituir y romper una pieza sin tocar el montaje;\n"
                "    los borrados no descolocan los índices y un índice muerto se ignora\n");
}

/**
 * @brief LA ESCENA GUARDA LA ENTRADA Y LOS CAMBIOS — Y NO LAS PIEZAS.
 *
 * Es la afirmación entera del diseño, y sin esto es una intención: se guarda una escena con un
 * montaje colocado y sus piezas expandidas, y se comprueba EN EL FICHERO que hay una entrada y
 * ninguna pieza. Con la contraprueba obvia: un objeto normal SÍ se guarda, así que "no aparece
 * nada" no puede pasar por verde.
 */
void test_prefab_scene_roundtrip() {
    beginTest("prefabricado: la escena guarda la entrada y sus cambios, no las piezas");

    const std::string path = "/tmp/haruka_prefab_roundtrip.scene";

    {
        Core::Scene::SceneManager sc;
        sc.setName("banco");

        auto entry = std::make_shared<Core::Scene::SceneObject>();
        entry->name = "banco_0";
        entry->type = "Prefab";
        entry->prefabName = "banco";
        entry->position = glm::dvec3(10.0, 0.0, -3.0);
        Game::Prefab::PrefabEdit rota;  rota.index = 2; rota.removed = true;
        Game::Prefab::PrefabEdit movida; movida.index = 1; movida.hasPos = true; movida.pos = glm::dvec3(0.0, 0.5, 0.0);
        Game::Prefab::PrefabEdit otra;  otra.index = 3; otra.item = "tabla_quemada";
        entry->prefabEdits = { rota, movida, otra };
        sc.addLoadedObject(entry);

        // Las piezas expandidas: objetos de verdad mientras viven, pero DERIVADOS.
        for (int i = 0; i < 4; ++i) {
            auto pieza = std::make_shared<Core::Scene::SceneObject>();
            pieza->name = "banco_0/" + std::to_string(i);
            pieza->type = "Mesh";
            pieza->fromPrefab = true;
            pieza->prefabName = "banco";
            pieza->prefabOwnerUid = entry->uid;
            pieza->prefabPieceIndex = i;
            sc.addLoadedObject(pieza);
        }

        // Las piezas cuelgan de su entrada: un montaje es UNA línea que se despliega, no 4 sueltas.
        for (int i = 0; i < 4; ++i) {
            sc.getAllObjects()[1 + i]->parentIndex = 0;
            sc.getAllObjects()[0]->childrenIndices.push_back(1 + i);
        }

        // CONTRAPRUEBA: un objeto normal en la misma escena. Si el guardado se comiera cosas por
        // otro motivo, este también desaparecería y el test no distinguiría una cosa de la otra.
        auto suelto = std::make_shared<Core::Scene::SceneObject>();
        suelto->name = "roca";
        suelto->type = "Mesh";
        sc.addLoadedObject(suelto);
        // Y colgando de la entrada, para probar el REMAPEO: en memoria es el índice 5, pero en el
        // fichero la entrada es el 0 y la roca el 1, porque las cuatro piezas de en medio no se
        // escriben. Sin remapear, la roca diría "mi padre es el 0" por casualidad y un caso con dos
        // montajes apuntaría a cualquier cosa.
        sc.getAllObjects()[5]->parentIndex = 0;
        sc.getAllObjects()[0]->childrenIndices.push_back(5);

        CHECK(sc.getAllObjects().size() == 6, "en memoria están la entrada, sus 4 piezas y la roca");
        CHECK(sc.save(path), "la escena se guarda");
    }

    // ── EN EL FICHERO ───────────────────────────────────────────────────────────────────────────
    {
        std::ifstream f(path);
        CHECK(f.is_open(), "el fichero existe");
        nlohmann::json j; f >> j;
        CHECK(j["objects"].size() == 2, "se escriben DOS objetos: la entrada y la roca (no las piezas)");
        const nlohmann::json* ent = nullptr;
        for (const auto& o : j["objects"]) if (o.value("name", "") == "banco_0") ent = &o;
        CHECK(ent != nullptr, "la entrada está en el fichero");
        if (ent) {
            CHECK((*ent).value("prefab", "") == "banco", "y lleva el nombre del montaje");
            CHECK((*ent)["prefabEdits"].size() == 3, "con sus tres cambios");
            // Los hijos DERIVADOS se caen de la lista (los describe la entrada, no un índice) y el
            // que sí se escribe viaja remapeado: la roca es el 1 del fichero, no el 5 de memoria.
            CHECK((*ent)["childrenIndices"].size() == 1,
                  "la entrada sólo conserva el hijo que SÍ se escribe");
            CHECK((*ent)["childrenIndices"][0].get<int>() == 1,
                  "y su índice está REMAPEADO al array del fichero (1), no al de memoria (5)");
        }
        for (const auto& o : j["objects"])
            if (o.value("name", "") == "roca")
                CHECK(o.value("parentIndex", -1) == 0, "la roca sigue colgando de la entrada");
    }

    // ── Y DE VUELTA ─────────────────────────────────────────────────────────────────────────────
    {
        Core::Scene::SceneManager sc;
        CHECK(sc.load(path), "la escena se vuelve a cargar");
        const Core::Scene::SceneObject* ent = nullptr;
        for (const auto& o : sc.getAllObjects()) if (o && o->name == "banco_0") ent = o.get();
        CHECK(ent != nullptr, "la entrada vuelve");
        if (ent) {
            CHECK(ent->prefabName == "banco", "con su montaje");
            CHECK(ent->prefabEdits.size() == 3, "y sus tres cambios");
            // Que vuelvan los cambios NO basta: tienen que volver los MISMOS, y cada uno con lo suyo.
            int rotas = 0, movidas = 0, sustituidas = 0;
            for (const Game::Prefab::PrefabEdit& e : ent->prefabEdits) {
                if (e.removed && e.index == 2) ++rotas;
                if (e.hasPos && e.index == 1 && std::abs(e.pos.y - 0.5) < 1e-12) ++movidas;
                if (e.item == "tabla_quemada" && e.index == 3) ++sustituidas;
            }
            CHECK(rotas == 1 && movidas == 1 && sustituidas == 1,
                  "la rota sigue rota, la movida conserva su pose y la sustituida su item");
        }
        CHECK(sc.getAllObjects().size() == 2, "y sin expansor no aparece ninguna pieza");
    }

    std::filesystem::remove(path);
    std::printf("    escena con un montaje colocado: 6 objetos en memoria -> 2 en el fichero\n");
}

/**
 * @brief MONTADO EN BLENDER: el .glb es una lista de referencias con pose, igual que el JSON.
 *
 * El fichero (`tests/Data/prefab_blender.glb`, 3 KB) lo escribió BLENDER de verdad con
 * `tests/Data/gen_prefab_blender.py`: lo que se prueba es su salida, no la que uno cree que da. Lo
 * que hay que demostrar, cada cosa con su contraprueba:
 *   1. La conversión de ejes: (1, 2, 3) en Blender es (1, 3, -2) aquí — y NO (1, 2, 3).
 *   2. La jerarquía se compone: b2 está en local (1, 0, 0) de un muro girado 90 grados en (10, 0, 0),
 *      así que acaba en (10, 0, -1) — y NO en (11, 0, 0), que es lo que daría ignorar el giro.
 *   3. El item sale de la malla compartida, "brick.001" cuenta como "brick", y la propiedad `item`
 *      manda sobre la malla.
 *   4. La pieza escalada se DESCARTA (5 piezas de 6 mallas), y el anclaje no es pieza.
 *   5. No se lee la geometría: el mismo fichero CORTADO tras el trozo JSON da el mismo montaje.
 *   6. El .glb y el .json que se guarda de él son el mismo prefabricado.
 */
void test_prefab_gltf() {
    beginTest("prefabricado montado en Blender (.glb): referencias con pose, sin geometria");
    namespace fs = std::filesystem;
    const std::string src = std::string(HARUKA_TEST_DATA_DIR) + "prefab_blender.glb";   // tests/Data/ (CMake)

    Game::Prefab::Prefab pf;
    CHECK(Game::Prefab::loadPrefab(src, pf), "el .glb exportado por Blender carga");
    CHECK(pf.pieces.size() == 5, "5 piezas: 6 mallas menos la escalada");
    CHECK(pf.anchors.size() == 1, "1 anclaje (el vacio 'anclaje_1'), que no es pieza");

    auto near = [](const glm::dvec3& a, const glm::dvec3& b) { return glm::length(a - b) < 1e-4; };
    auto at = [&](const glm::dvec3& p) -> const Game::Prefab::PrefabPiece* {
        for (const auto& pc : pf.pieces) if (near(pc.pos, p)) return &pc;
        return nullptr;
    };
    const glm::dquat rotY90 = glm::angleAxis(glm::half_pi<double>(), glm::dvec3(0, 1, 0));
    auto sameRot = [](const glm::dquat& a, const glm::dquat& b) { return std::abs(glm::dot(a, b)) > 1.0 - 1e-6; };

    // 1. ejes
    const Game::Prefab::PrefabPiece* b1 = at({1, 3, -2});
    CHECK(b1 && b1->id == "brick", "b1: Blender (1,2,3) -> (1,3,-2), item de la malla compartida");
    CHECK(b1 && sameRot(b1->rot, rotY90), "b1: el giro sobre Z de Blender es sobre Y aqui");
    CHECK(!at({1, 2, 3}), "CONTRAPRUEBA: nada en (1,2,3) — los ejes no se leen tal cual de Blender");

    // 2. jerarquía
    const Game::Prefab::PrefabPiece* b2 = at({10, 0, -1});
    CHECK(b2 && b2->id == "brick", "b2: hijo del muro girado acaba en (10,0,-1)");
    CHECK(b2 && sameRot(b2->rot, rotY90), "b2: hereda el giro del muro");
    CHECK(!at({11, 0, 0}), "CONTRAPRUEBA: nada en (11,0,0), que es sumar posiciones sin girar");

    // 3. nombres y propiedades
    const Game::Prefab::PrefabPiece* b3 = at({0, 1, 0});
    CHECK(b3 && b3->id == "brick", "b3: la malla 'brick.001' (copia no enlazada) cuenta como 'brick'");
    const Game::Prefab::PrefabPiece* b4 = at({0, 2, 0});
    CHECK(b4 && b4->id == "hull_beam", "b4: la propiedad 'item' manda sobre el nombre de la malla");
    CHECK(b4 && b4->mechanism && std::abs(b4->arc - 12.5) < 1e-9, "b4: joint=mechanism y arc=12,5");
    CHECK(b3 && !b3->mechanism && b3->arc == 0.0, "CONTRAPRUEBA: sin propiedades, rigida y recta");

    // 4. escala
    CHECK(!at({0, 3, 0}), "b5 (escalada x2) se descarta: el tamano lo declara el item");
    CHECK(near(pf.anchors.empty() ? glm::dvec3(-1) : pf.anchors[0], {0, 5, 0}), "anclaje en (0,5,0)");

    // 5. sin geometría: cortar el fichero justo tras el trozo JSON no cambia nada
    const std::string cut = "/tmp/haruka_prefab_cut.glb";
    {
        std::ifstream in(src, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), {});
        std::uint32_t len = 0;
        std::memcpy(&len, bytes.data() + 12, 4);
        std::ofstream(cut, std::ios::binary).write(bytes.data(), 20 + len);
        std::printf("    %zu bytes -> %u sin la geometria\n", bytes.size(), 20 + len);
    }
    Game::Prefab::Prefab pc;
    CHECK(Game::Prefab::loadPrefab(cut, pc) && pc.pieces.size() == pf.pieces.size(),
          "el .glb SIN su trozo binario da el mismo montaje: la geometria no se lee");
    fs::remove(cut);

    // 6. el .json que se guarda del .glb es el mismo prefabricado
    const std::string js = "/tmp/haruka_prefab_from_glb.json";
    Game::Prefab::Prefab back;
    CHECK(Game::Prefab::savePrefab(js, pf) && Game::Prefab::loadPrefab(js, back), "se guarda como .json y se relee");
    bool same = back.pieces.size() == pf.pieces.size();
    for (size_t i = 0; same && i < pf.pieces.size(); ++i)
        same = back.pieces[i].id == pf.pieces[i].id && near(back.pieces[i].pos, pf.pieces[i].pos) &&
               sameRot(back.pieces[i].rot, pf.pieces[i].rot) &&
               back.pieces[i].mechanism == pf.pieces[i].mechanism;
    CHECK(same, ".glb y .json son el mismo prefabricado pieza a pieza");
    fs::remove(js);

    // findPrefabFile: el .json gana al .glb si hay los dos
    const fs::path dir = fs::temp_directory_path() / "haruka_prefab_find";
    fs::create_directories(dir);
    fs::copy_file(src, dir / "casa.glb", fs::copy_options::overwrite_existing);
    CHECK(Game::Prefab::findPrefabFile(dir.string(), "casa") == (dir / "casa.glb").string(), "solo .glb: se encuentra el .glb");
    std::ofstream(dir / "casa.json") << "{}";
    CHECK(Game::Prefab::findPrefabFile(dir.string(), "casa") == (dir / "casa.json").string(), "con .json y .glb gana el .json");
    CHECK(Game::Prefab::findPrefabFile(dir.string(), "nada").empty(), "CONTRAPRUEBA: un nombre que no existe da \"\"");
    fs::remove_all(dir);
}

// Registro: el grupo sale de la carpeta de este archivo (ver Common/TestCommon.h).
HARUKA_REGISTER_TESTS(
    { "prefab_edits", test_prefab_edits },
    { "prefab_scene_roundtrip", test_prefab_scene_roundtrip },
    { "prefab_gltf", test_prefab_gltf });
