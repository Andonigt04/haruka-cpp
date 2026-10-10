/**
 * @file SimpleMesh.h
 * @brief Backward-compatibility `SimpleMesh` alias for `Mesh`.
 */
#pragma once

/**
 * @brief Backward-compatibility alias.
 *
 * Legacy code can continue using `SimpleMesh` while implementation lives in `Mesh`.
 */
#include "Mesh.h"
namespace Haruka { namespace Renderer { using SimpleMesh = Mesh; } }
using SimpleMesh = Haruka::Renderer::SimpleMesh;   // back-compat (== Mesh)
namespace Haruka { using Renderer::SimpleMesh; }
