#include "mesh.h"

#include <utility>
#include <glad/glad.h>
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"   // ruta PSO: drawRHI
#include <cstdio>

namespace Haruka { namespace Renderer {

// Constructor para modelos complejos (con texturas)
Mesh::Mesh(std::vector<Vertex> vertex, std::vector<unsigned int> idx, std::vector<MeshTexture> textures) {
    if (vertex.empty()) {
        HARUKA_RENDERER_ERROR(ErrorCode::MODEL_LOAD_FAILED, "Empty vertex list when creating mesh");
        return;
    }
    this->vertex = vertex;
    this->index = idx;
    this->textures = textures;
    this->isSimpleGeometry = false;
    setupMesh();
}

// Constructor simplificado (solo geometria)
Mesh::Mesh(const std::vector<glm::vec3>& vertices,
           const std::vector<glm::vec3>& normals,
           const std::vector<unsigned int>& indices) {
    this->isSimpleGeometry = true;
    this->textures.clear();
    setupSimpleMesh(vertices, normals, indices);   // interleava en `vertex` + setupMesh()
}

// Libera los buffers del mesh por el device. El VAO se libera SIEMPRE por GL (no es aún un
// recurso del RHI — llega con el refactor PSO).
static void releaseMeshBuffers(Haruka::RHI::BufferHandle vbo, Haruka::RHI::BufferHandle ebo) {
    using namespace Haruka;
    if (RHI::Device* dev = RHI::device()) {
        if (RHI::valid(vbo)) dev->destroy(vbo);
        if (RHI::valid(ebo)) dev->destroy(ebo);
    }
}

Mesh::~Mesh() {
    releaseMeshBuffers(m_vbo, m_ebo);
    if (VAO) glDeleteVertexArrays(1, &VAO);
}

Mesh::Mesh(Mesh&& o) noexcept
    : vertex(std::move(o.vertex)), index(std::move(o.index)), textures(std::move(o.textures)),
      VAO(o.VAO), VBO(o.VBO), EBO(o.EBO),
      m_vbo(o.m_vbo), m_ebo(o.m_ebo),
      isSimpleGeometry(o.isSimpleGeometry) {
    o.VAO = o.VBO = o.EBO = 0; // el origen ya no posee los handles
    o.m_vbo = o.m_ebo = {};
}

Mesh& Mesh::operator=(Mesh&& o) noexcept {
    if (this != &o) {
        releaseMeshBuffers(m_vbo, m_ebo);
        if (VAO) glDeleteVertexArrays(1, &VAO);
        vertex = std::move(o.vertex); index = std::move(o.index); textures = std::move(o.textures);
        VAO = o.VAO; VBO = o.VBO; EBO = o.EBO;
        m_vbo = o.m_vbo; m_ebo = o.m_ebo;
        isSimpleGeometry = o.isSimpleGeometry;
        o.VAO = o.VBO = o.EBO = 0;
        o.m_vbo = o.m_ebo = {};
    }
    return *this;
}

void Mesh::Draw(Shader &shader) 
{
    unsigned int diffuseNr = 1;
    unsigned int specularNr = 1;
    unsigned int normalNr = 1;
    unsigned int heightNr = 1;
    unsigned int roughnessNr = 1;
    unsigned int metallicNr = 1;
    unsigned int aoNr = 1;
    unsigned int emissiveNr = 1;
    unsigned int metallicRoughnessNr = 1;

    for(unsigned int i = 0; i < textures.size(); i++)
    {
        glActiveTexture(GL_TEXTURE0 + i); 
        
        std::string number;
        std::string name = textures[i].type;

        if(name == "texture_diffuse")
            number = std::to_string(diffuseNr++);
        else if(name == "texture_specular")
            number = std::to_string(specularNr++);
        else if(name == "texture_normal")
            number = std::to_string(normalNr++);
        else if(name == "texture_height")
            number = std::to_string(heightNr++);
        else if(name == "texture_roughness")
            number = std::to_string(roughnessNr++);
        else if(name == "texture_metallic")
            number = std::to_string(metallicNr++);
        else if(name == "texture_ao")
            number = std::to_string(aoNr++);
        else if(name == "texture_emissive")
            number = std::to_string(emissiveNr++);
        else if(name == "texture_metallic_roughness")
            number = std::to_string(metallicRoughnessNr++);

        if (!number.empty()) {
            shader.setInt(("material." + name + number).c_str(), i); 
            glBindTexture(GL_TEXTURE_2D, textures[i].id);
        }
    }
    
    // Clean not used textures 
    for(unsigned int i = textures.size(); i < 16; i++)
    {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, 0);  // Unbind
    }
    
    glActiveTexture(GL_TEXTURE0);

    // draw mesh
    glBindVertexArray(VAO);
    glDrawElements(GL_TRIANGLES, index.size(), GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
}

// Toda la geometría (completa o simple) comparte ya el mismo buffer interleaved y el mismo layout
// → un solo camino de dibujo, sin la rama de atributos constantes (ver setupSimpleMesh).
void Mesh::draw() const {
    glBindVertexArray(VAO);
    glDrawElements(GL_TRIANGLES, (GLsizei)index.size(), GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
}

// Ruta PSO: sin VAO ni glDrawElements. El pipeline (shader + layout + estado) ya lo bindeó el
// llamador; aquí solo se atan los buffers de ESTA malla y se dibuja.
void Mesh::drawRHI(Haruka::RHI::Context& ctx) const {
    if (index.empty()) return;
    // Si un handle no resuelve, Context::bindVertexBuffer/bindIndexBuffer abortan EN SILENCIO y el
    // VAO se queda sin buffer → glDrawElements suelta un GL_INVALID_OPERATION opaco. Mejor gritar.
    if (!RHI::valid(m_vbo) || !RHI::valid(m_ebo)) {
        static bool s_warned = false;
        if (!s_warned) {
            std::fprintf(stderr, "[Mesh] drawRHI con buffers INVALIDOS (vbo=%u ebo=%u, %zu idx) "
                                 "→ el draw se salta\n", m_vbo.id, m_ebo.id, index.size());
            s_warned = true;
        }
        return;
    }
    ctx.bindVertexBuffer(m_vbo);
    ctx.bindIndexBuffer(m_ebo);
    ctx.drawIndexed(static_cast<uint32_t>(index.size()));
}

void Mesh::setupMesh() {
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    // Buffers vía RHI (subida incluida). El VAO/attrib-pointers siguen en GL (transitorio).
    RHI::Device* dev = RHI::device();
    m_vbo = dev->createBuffer(RHI::BufferUsage::Vertex, vertex.size() * sizeof(Vertex), &vertex[0]);
    m_ebo = dev->createBuffer(RHI::BufferUsage::Index,  index.size() * sizeof(unsigned int), &index[0]);
    VBO = dev->nativeBuffer(m_vbo);
    EBO = dev->nativeBuffer(m_ebo);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Normal));

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, TexCoords));

    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Tangent));
    
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Bitangent));
    
    glBindVertexArray(0);
}

// Geometría simple (primitivas: cubo/esfera/cápsula) → se INTERLEAVA en el MISMO `Vertex` que las
// mallas completas. Antes usaba dos buffers separados (pos + normales) y daba uv/tangent/bitangent
// con atributos CONSTANTES (glVertexAttrib2f/3f). Ambas cosas son intraducibles a Vulkan:
//   · los atributos de vértice constantes NO existen (no hay vkCmdSetVertexAttrib);
//   · el Context del RHI ata UN solo vertex buffer (binding 0), no dos.
// Horneando esos valores como datos reales, la geometría simple queda con el MISMO layout que el
// resto → mismo PSO, sin ramas en el draw. Coste de VRAM despreciable (son 4 mallas diminutas).
void Mesh::setupSimpleMesh(const std::vector<glm::vec3>& vertices,
                           const std::vector<glm::vec3>& normals,
                           const std::vector<unsigned int>& indices) {
    vertex.clear();
    vertex.reserve(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        Vertex v{};
        v.Position  = vertices[i];
        v.Normal    = (i < normals.size()) ? normals[i] : glm::vec3(0.0f, 1.0f, 0.0f);
        v.TexCoords = glm::vec2(0.0f, 0.0f);        // antes: glVertexAttrib2f(2, 0, 0)
        v.Tangent   = glm::vec3(1.0f, 0.0f, 0.0f);  // antes: glVertexAttrib3f(3, 1, 0, 0)
        v.Bitangent = glm::vec3(0.0f, 1.0f, 0.0f);  // antes: glVertexAttrib3f(4, 0, 1, 0)
        vertex.push_back(v);
    }
    index = indices;
    setupMesh();   // misma ruta que las mallas completas: un VBO interleaved + EBO
}

}} // namespace Haruka::Renderer