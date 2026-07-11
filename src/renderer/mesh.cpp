#include "mesh.h"

#include <utility>
#include <glad/glad.h>
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"

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
    this->index = indices;
    this->vertex.clear();
    this->textures.clear();
    this->simpleVertexCount = static_cast<int>(vertices.size());
    setupSimpleMesh(vertices, normals, indices);
}

// Libera los buffers: por el device si son del RHI, o GL directo en la ruta de compatibilidad.
// El VAO se libera SIEMPRE por GL (no es aún un recurso del RHI — llega con el refactor PSO).
static void releaseMeshBuffers(Haruka::RHI::BufferHandle vbo, Haruka::RHI::BufferHandle ebo,
                               Haruka::RHI::BufferHandle nbo,
                               unsigned int glVBO, unsigned int glEBO, unsigned int glNBO) {
    using namespace Haruka;
    if (RHI::valid(vbo) || RHI::valid(ebo) || RHI::valid(nbo)) {
        if (RHI::Device* dev = RHI::device()) {
            if (RHI::valid(vbo)) dev->destroy(vbo);
            if (RHI::valid(ebo)) dev->destroy(ebo);
            if (RHI::valid(nbo)) dev->destroy(nbo);
        }
    } else {
        if (glVBO) glDeleteBuffers(1, &glVBO);
        if (glNBO) glDeleteBuffers(1, &glNBO);
        if (glEBO) glDeleteBuffers(1, &glEBO);
    }
}

Mesh::~Mesh() {
    releaseMeshBuffers(m_vbo, m_ebo, m_nbo, VBO, EBO, nbo);
    if (VAO) glDeleteVertexArrays(1, &VAO);
}

Mesh::Mesh(Mesh&& o) noexcept
    : vertex(std::move(o.vertex)), index(std::move(o.index)), textures(std::move(o.textures)),
      VAO(o.VAO), VBO(o.VBO), EBO(o.EBO), nbo(o.nbo),
      m_vbo(o.m_vbo), m_ebo(o.m_ebo), m_nbo(o.m_nbo),
      isSimpleGeometry(o.isSimpleGeometry), simpleVertexCount(o.simpleVertexCount) {
    o.VAO = o.VBO = o.EBO = o.nbo = 0; // el origen ya no posee los handles
    o.m_vbo = o.m_ebo = o.m_nbo = {};
}

Mesh& Mesh::operator=(Mesh&& o) noexcept {
    if (this != &o) {
        releaseMeshBuffers(m_vbo, m_ebo, m_nbo, VBO, EBO, nbo);
        if (VAO) glDeleteVertexArrays(1, &VAO);
        vertex = std::move(o.vertex); index = std::move(o.index); textures = std::move(o.textures);
        VAO = o.VAO; VBO = o.VBO; EBO = o.EBO; nbo = o.nbo;
        m_vbo = o.m_vbo; m_ebo = o.m_ebo; m_nbo = o.m_nbo;
        isSimpleGeometry = o.isSimpleGeometry; simpleVertexCount = o.simpleVertexCount;
        o.VAO = o.VBO = o.EBO = o.nbo = 0;
        o.m_vbo = o.m_ebo = o.m_nbo = {};
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

void Mesh::draw() const {
    glBindVertexArray(VAO);

    if (!isSimpleGeometry) {
        glDrawElements(GL_TRIANGLES, index.size(), GL_UNSIGNED_INT, 0);
    } else {
        // Defaults para geometria simple
        glDisableVertexAttribArray(2);
        glVertexAttrib2f(2, 0.0f, 0.0f);
        glDisableVertexAttribArray(3);
        glVertexAttrib3f(3, 1.0f, 0.0f, 0.0f);
        glDisableVertexAttribArray(4);
        glVertexAttrib3f(4, 0.0f, 1.0f, 0.0f);
        
        glDrawElements(GL_TRIANGLES, index.size(), GL_UNSIGNED_INT, 0);
    }
    
    glBindVertexArray(0);
}

void Mesh::setupMesh() {
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    // Buffers vía RHI (subida incluida). El VAO/attrib-pointers siguen en GL (transitorio).
    if (RHI::Device* dev = RHI::device()) {
        m_vbo = dev->createBuffer(RHI::BufferUsage::Vertex, vertex.size() * sizeof(Vertex), &vertex[0]);
        m_ebo = dev->createBuffer(RHI::BufferUsage::Index,  index.size() * sizeof(unsigned int), &index[0]);
        VBO = dev->nativeBuffer(m_vbo);
        EBO = dev->nativeBuffer(m_ebo);
        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    } else {
        glGenBuffers(1, &VBO);
        glGenBuffers(1, &EBO);
        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferData(GL_ARRAY_BUFFER, vertex.size() * sizeof(Vertex), &vertex[0], GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, index.size() * sizeof(unsigned int), &index[0], GL_STATIC_DRAW);
    }

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

void Mesh::setupSimpleMesh(const std::vector<glm::vec3>& vertices,
                           const std::vector<glm::vec3>& normals,
                           const std::vector<unsigned int>& indices) {
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    RHI::Device* dev = RHI::device();

    // Positions (attrib 0)
    if (dev) { m_vbo = dev->createBuffer(RHI::BufferUsage::Vertex, vertices.size() * sizeof(glm::vec3), vertices.data());
               VBO = dev->nativeBuffer(m_vbo); glBindBuffer(GL_ARRAY_BUFFER, VBO); }
    else     { glGenBuffers(1, &VBO); glBindBuffer(GL_ARRAY_BUFFER, VBO);
               glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(glm::vec3), vertices.data(), GL_STATIC_DRAW); }
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
    glEnableVertexAttribArray(0);

    // Normals (attrib 1, buffer separado)
    if (dev) { m_nbo = dev->createBuffer(RHI::BufferUsage::Vertex, normals.size() * sizeof(glm::vec3), normals.data());
               nbo = dev->nativeBuffer(m_nbo); glBindBuffer(GL_ARRAY_BUFFER, nbo); }
    else     { glGenBuffers(1, &nbo); glBindBuffer(GL_ARRAY_BUFFER, nbo);
               glBufferData(GL_ARRAY_BUFFER, normals.size() * sizeof(glm::vec3), normals.data(), GL_STATIC_DRAW); }
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
    glEnableVertexAttribArray(1);

    // Indices
    if (dev) { m_ebo = dev->createBuffer(RHI::BufferUsage::Index, indices.size() * sizeof(unsigned int), indices.data());
               EBO = dev->nativeBuffer(m_ebo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO); }
    else     { glGenBuffers(1, &EBO); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
               glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW); }

    glBindVertexArray(0);
}

}} // namespace Haruka::Renderer