/**
 * @file simple_mesh.h
 * @brief Backward-compatibility `SimpleMesh` alias for `Mesh`.
 */
#ifndef SIMPLE_MESH_H
#define SIMPLE_MESH_H

/**
 * @brief Backward-compatibility alias.
 *
 * Legacy code can continue using `SimpleMesh` while implementation lives in `Mesh`.
 */
#include "mesh.h"
namespace Haruka { namespace Renderer { using SimpleMesh = Mesh; } }
using SimpleMesh = Haruka::Renderer::SimpleMesh;   // back-compat (== Mesh)
namespace Haruka { using Renderer::SimpleMesh; }

#endif