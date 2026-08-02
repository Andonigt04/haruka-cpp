/**
 * @file mesh_cut.h
 * @brief Recorte REAL de geometría por un plano (para los "recortes del material" del modo camino:
 *        biselar el extremo de una pieza, ingletes en los codos). GL-free y DETERMINISTA — solo glm.
 *        Es la base geométrica; el render de la malla resultante y la integración en el camino van aparte.
 */
#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

namespace Haruka { namespace Construction {

/** @brief Malla simple en CPU (posiciones + normales + índices de triángulos). */
struct SimpleMeshData {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<uint32_t>  indices;
    bool empty() const { return indices.empty(); }
    std::size_t triangleCount() const { return indices.size() / 3; }
};

/** @brief Recorta la malla por un plano: CONSERVA el semiespacio `dot(planeN, p) <= planeD` (lado "dentro")
 *  y descarta el resto. Cada triángulo que cruza se clipa (Sutherland-Hodgman) interpolando la normal; si
 *  `cap`, se RECAPA la sección cortada (polígono en el plano → abanico) para que la pieza quede sólida.
 *  DETERMINISTA. Pensado para piezas convexas tipo caja/viga (el recapado asume sección convexa). */
SimpleMeshData clipByPlane(const SimpleMeshData& in, const glm::vec3& planeN, float planeD, bool cap = true);

}} // namespace Haruka::Construction
