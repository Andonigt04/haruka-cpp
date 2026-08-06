/**
 * @file prop_assembler.h
 * @brief ENSAMBLADOR final de la capa de props: de capa colocada a SceneObject.
 *
 * Une las tres piezas del pipeline:
 *   1. `placeProps`      — pase ORDENADO de las capas (la casa reclama, el árbol cede).
 *   2. `TerrainPropField`— convierte el parche plano (px,pz) en el campo esférico real.
 *   3. `bakeTreeMesh` + `bakeTreeTextures` → `MeshRendererComponent` + `MaterialComponent`.
 *
 * Se divide en DOS fases a propósito:
 *   · `bakeProps`  (CPU-only, TESTEABLE sin GPU): hornea malla + texturas de cada prop.
 *   · `assembleSceneObjects` (GPU): sube las mallas y monta los SceneObject en el mundo.
 *     No es testeable en el suite CPU — `MeshRendererComponent::setMesh` necesita device RHI
 *     (sube los buffers). Es el runtime del PlanetarySystem.
 *
 * La transformada del objeto sale del MISMO ancla que TerrainPropField: dirección del centro
 * del parche + base tangente + radio del planeta. Un prop en (px,pz) del parche con cota
 * `heightM` vive en el mundo en `planetCenter + dir*(radius + heightM)`, con `dir` la dirección
 * correspondiente — así el objeto se asienta EXACTAMENTE sobre el suelo que muestreó el campo.
 */
#pragma once
// glm/gtx/* (glm::rotation) exige la macro en GLM moderno; se define ANTES de cualquier include GLM.
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>   // glm::rotation (up → dir de la superficie, para el ancla de props)

#include "core/planet/prop_layer.h"             // PropLayerTable, PlacedProp
#include "core/planet/prop_layer_spawn.h"       // PropsPlaced, placeProps
#include "game/terrain_prop_field.h"            // TerrainPropField
#include "tools/procgraph/tree_mesh.h"          // bakeTreeMesh, TreeMeshData
#include "tools/procgraph/tree_textures.h"      // bakeTreeTextures, TreeBakeResult
#include "core/components/mesh_renderer_component.h"
#include "core/components/material_component.h"
#include "core/scene/scene_manager.h"           // SceneObject, SceneManager

namespace Haruka { namespace Planet {

/** @brief Ancla del parche en el MUNDO (dónde vive la retícula plana sobre la esfera). */
struct PropWorldAnchor {
    glm::dvec3 planetCenter = glm::dvec3(0.0);  // centro del planeta (m)
    double     radius       = 6371000.0;        // radio del planeta (m)
    glm::vec3  centerDir    = glm::vec3(1,0,0); // dirección del centro del parche (unit)
    glm::vec3  u            = glm::vec3(0,1,0); // base tangente: dirección de px (m/unidad)
    glm::vec3  v            = glm::vec3(0,0,1); // base tangente: dirección de pz (m/unidad)
};

/**
 * @brief Resultado del bake de UN prop: la malla CPU + las rutas de textura ya horneadas.
 *  Todo esto es GL-free; `assembleSceneObjects` lo sube.
 */
struct BakedProp {
    PlacedProp   placed;      // dónde/cómo (de la capa)
    Haruka::Tools::ProcGraph::TreeMeshData  mesh;
    Haruka::Tools::ProcGraph::TreeBakeResult tex;
    std::string  baseName;    // nombre base de las texturas horneadas
};

/**
 * @brief FASE CPU — hornea malla + texturas de cada prop colocado.
 *
 * @param placed  Salida de `placeProps`.
 * @param texDir  Directorio (absoluto) donde escribir las texturas.
 * @param relDir  Prefijo RELATIVO que devuelve bakeTreeTextures en las rutas del material
 *                ("assets/textures/props/generated/" si texDir es esa subcarpeta). El material
 *                carga ese path contra AssetPaths, así que debe apuntar a donde quedó el PNG.
 * @param texBase Prefijo común de los PNG ("prop"…). Solo se usa si la capa no trae mesh.
 * @return Los props horneados, listos para montar.
 */
inline std::vector<BakedProp> bakeProps(const PropsPlaced& placed,
                                        const std::string& texDir,
                                        const std::string& relDir = "assets/textures/",
                                        const std::string& texBase = "prop") {
    std::vector<BakedProp> out;
    // Las texturas se comparten POR CAPA (mesh): hornear un set por tipo de objeto y reutilizarlo
    // en todos sus props evita una tanda de PNG por instancia (con 100 árboles eran 400 archivos).
    // La VARIEDAD la da la malla (TreeMeshNode usa la semilla de la celda), no las texturas.
    std::unordered_map<std::string, Haruka::Tools::ProcGraph::TreeBakeResult> texByMesh;
    for (const auto& p : placed.props) {
        BakedProp b;
        b.placed   = p;
        // El nombre base del OBJETO es el TIPO (tree, rock…), NO el planeta: así los SceneObject se
        // llaman por lo que instalan y no contaminan con el nombre del mundo.
        const std::string base = p.mesh.empty() ? texBase : p.mesh;
        b.baseName = base + "_" + std::to_string(p.meshSeed);

        // Malla del árbol desde la seed determinista de la celda (única por prop).
        Haruka::Tools::ProcGraph::Graph g;
        int tree = g.emplaceNode<Haruka::Tools::ProcGraph::TreeMeshNode>(
            (int)p.meshSeed, 6.5f, 0.35f, p.scale, 8);
        g.compile();
        if (!Haruka::Tools::ProcGraph::bakeTreeMesh(g, tree, b.mesh)) continue;

        // Texturas de la CAPA (una sola vez por mesh; el resto reutiliza las mismas rutas).
        auto it = texByMesh.find(base);
        if (it == texByMesh.end()) {
            Haruka::Tools::ProcGraph::TreeBakeResult tex =
                Haruka::Tools::ProcGraph::bakeTreeTextures(
                    (int)p.meshSeed, false, texDir, base, 128, relDir);
            if (!tex.ok) continue;
            it = texByMesh.emplace(base, tex).first;
        }
        b.tex = it->second;

        out.push_back(std::move(b));
    }
    return out;
}

/**
 * @brief Dirección del punto del parche en el mundo (mismo cálculo que TerrainPropField).
 *  Necesaria para asentar el objeto sobre el suelo que muestreó el campo.
 */
inline glm::vec3 propPatchDir(const PropWorldAnchor& a, float px, float pz) {
    const double arc = 1.0 / a.radius;
    return glm::normalize(a.centerDir
        + a.u * (px * (float)arc)
        + a.v * (pz * (float)arc));
}

/**
 * @brief FASE GPU — sube los props horneados a SceneObject y los añade a la escena.
 *
 * Cada prop se convierte en un SceneObject:
 *   · position  = planetCenter + dir*(radius + heightM) → asentado sobre el campo.
 *   · scale     = el de la capa (variación por celda).
 *   · meshRenderer = MeshRendererComponent::setMesh(bakeTreeMesh) — sube los buffers.
 *   · material  = MaterialComponent con shaderPath "shaders/pbr.frag" y las texturas PBR.
 *
 * @param baked  Salida de `bakeProps`.
 * @param anchor Ancla del parche en el mundo.
 * @param scene  SceneManager donde registrar los objetos (nullptr = solo devolverlos).
 * @return Los SceneObject creados (el que llame decide registrarlos/borrarlos).
 */
inline std::vector<std::shared_ptr<SceneObject>> assembleSceneObjects(
    const std::vector<BakedProp>& baked,
    const PropWorldAnchor& anchor,
    SceneManager* scene = nullptr) {
    std::vector<std::shared_ptr<SceneObject>> objs;
    for (const auto& b : baked) {
        auto obj = std::make_shared<SceneObject>();
        obj->name = b.baseName;
        obj->type = "Prop";

        // Asentar sobre el suelo que muestreó el campo.
        const glm::vec3 dir = propPatchDir(anchor, b.placed.localPos.x, b.placed.localPos.z);
        obj->position = anchor.planetCenter
            + glm::dvec3(dir) * (anchor.radius + b.placed.heightM);
        obj->scale = Haruka::DScale(b.placed.scale);   // DScale = glm::dvec3 (uniform)

        // ORIENTAR el tronco hacia la superficie: la malla del árbol se hornea con el tronco en +Y
        // (tree_mesh.h), y sin rotación el objeto queda apuntando a +Y GLOBAL — en el ecuador del
        // planeta eso es TANGENTE a la superficie, así que los árboles salían tumbados. Con
        // `orientation` + `useOrientation` el render usa el cuaternión exacto: +Y del objeto →
        // dirección radial `dir` (la normal del suelo), de modo que el tronco queda erguido donde
        // esté, ecuador o polo.
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        obj->orientation = glm::dquat(glm::rotation(up, dir));
        obj->useOrientation = true;

        // Malla procedural subida a GPU (necesita device RHI).
        auto mrc = std::make_shared<MeshRendererComponent>();
        mrc->setMesh(b.mesh.positions, b.mesh.normals, b.mesh.colors, b.mesh.indices);
        obj->meshRenderer = mrc;

        // Material PBR con las texturas horneadas.
        auto mat = std::make_shared<MaterialComponent>();
        mat->name = b.baseName;
        mat->shaderPath = "shaders/pbr.frag";
        mat->textures["albedo"]    = b.tex.albedo;
        mat->textures["normal"]    = b.tex.normal;
        mat->textures["metallic"]  = b.tex.metallic;
        mat->textures["roughness"] = b.tex.roughness;
        mat->textures["ao"]        = b.tex.ao;
        obj->material = mat;

        objs.push_back(obj);
        if (scene) scene->addLoadedObject(obj);
    }
    return objs;
}

}} // namespace Haruka::Planet