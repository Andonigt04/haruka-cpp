#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <cstdint>

namespace Haruka {

    /**
     * @brief Las 6 caras de la esfera proyectada.
     */
    enum class PlanetFace : uint8_t {
        FRONT = 0, BACK = 1, TOP = 2, BOTTOM = 3, RIGHT = 4, LEFT = 5
    };

    

} // namespace Haruka