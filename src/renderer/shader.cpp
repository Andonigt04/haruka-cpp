#include "renderer/shader.h"
#include <glad/glad.h>

namespace Haruka { namespace Renderer {

void Shader::use() const {
    if (ID) glUseProgram(ID);
}

}} // namespace Haruka::Renderer
