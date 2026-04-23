#pragma once
#include "../component.h"
#include <string>

namespace Haruka {

/**
 * @brief Componente que referencia un script asociado al objeto.
 */
class ScriptComponent : public Component {
public:
    /** @brief Ruta del script a ejecutar/cargar. */
    std::string scriptPath;

    /** @brief Identificador estático del componente. */
    static std::string staticType() { return "Script"; }
    /** @brief Identificador dinámico del componente. */
    std::string getType() const override { return staticType(); }
    /** @brief Inspector ImGui del path de script. */
    void renderInspector() override;
};

}