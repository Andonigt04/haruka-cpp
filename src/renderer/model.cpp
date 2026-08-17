#include "model.h"

#include <iostream>
#include "stb_image.h"
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"
#include "renderer/gpu_instancing.h"

namespace Haruka { namespace Renderer {

Model::~Model() {
    RHI::Device* dev = RHI::device();
    if (!dev) return;
    for (auto& t : textures_loaded) {
        if (RHI::valid(t.rhiHandle))
            dev->destroy(t.rhiHandle);
    }
}

static glm::mat4 aiToGlm(const aiMatrix4x4& m) {
    return glm::mat4(
        m.a1, m.b1, m.c1, m.d1,
        m.a2, m.b2, m.c2, m.d2,
        m.a3, m.b3, m.c3, m.d3,
        m.a4, m.b4, m.c4, m.d4);
}

void Model::drawRHI(Haruka::RHI::Context& ctx) {
    for (auto& m : meshes) m.drawRHI(ctx);
}

void Model::drawInstancedRHI(Haruka::RHI::Context& ctx, Haruka::Renderer::GPUInstancing& inst,
                             uint32_t instanceBinding) {
    for (const auto& m : meshes) {
        if (m.getIndexCount() == 0) continue;
        if (!Haruka::RHI::valid(m.vertexBuffer()) || !Haruka::RHI::valid(m.indexBuffer())) continue;
        ctx.bindVertexBuffer(m.vertexBuffer(), 0);
        ctx.bindIndexBuffer(m.indexBuffer());
        inst.render(&ctx, (uint32_t)m.getIndexCount(), instanceBinding);
    }
}

void Model::loadModel(std::string const &path) {
    this->scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace);

    if(!this->scene || this->scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !this->scene->mRootNode) {
        HARUKA_RENDERER_ERROR(ErrorCode::MODEL_LOAD_FAILED,
            std::string("Assimp error: ") + importer.GetErrorString());
        return;
    }
    directory = path.substr(0, path.find_last_of('/'));

    m_min = glm::vec3( 1e30f);
    m_max = glm::vec3(-1e30f);
    m_hasBounds = false;

    processNode(this->scene->mRootNode, this->scene, glm::mat4(1.0f));
}

void Model::processNode(aiNode *node, const aiScene *scene, const glm::mat4& parentTransform) {
    glm::mat4 nodeTransform = parentTransform * aiToGlm(node->mTransformation);
    for(unsigned int i = 0; i < node->mNumMeshes; i++) {
        aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
        this->meshes.push_back(processMesh(mesh, scene, nodeTransform));
    }
    for(unsigned int i = 0; i < node->mNumChildren; i++) {
        processNode(node->mChildren[i], scene, nodeTransform);
    }
}

Mesh Model::processMesh(aiMesh *mesh, const aiScene *scene, const glm::mat4& transform) {
    const glm::mat3 normalMat = glm::transpose(glm::inverse(glm::mat3(transform)));
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    std::vector<MeshTexture> textures;

    for(unsigned int i = 0; i < mesh->mNumVertices; i++) {
        Vertex vertex;
        vertex.Position = glm::vec3(transform * glm::vec4(mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z, 1.0f));
        m_min = glm::min(m_min, vertex.Position);
        m_max = glm::max(m_max, vertex.Position);
        m_hasBounds = true;

        if (mesh->HasNormals()) {
            vertex.Normal = glm::normalize(normalMat * glm::vec3(mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z));
        }
        if(mesh->mTextureCoords[0]) {
            vertex.TexCoords = glm::vec2(mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y);
        } else {
            vertex.TexCoords = glm::vec2(0.0f, 0.0f);
        }
        if (mesh->mTangents) {
            vertex.Tangent   = glm::normalize(glm::mat3(transform) * glm::vec3(mesh->mTangents[i].x, mesh->mTangents[i].y, mesh->mTangents[i].z));
            vertex.Bitangent = glm::normalize(glm::mat3(transform) * glm::vec3(mesh->mBitangents[i].x, mesh->mBitangents[i].y, mesh->mBitangents[i].z));
        }
        vertices.push_back(vertex);
    }

    for(unsigned int i = 0; i < mesh->mNumFaces; i++) {
        aiFace face = mesh->mFaces[i];
        for(unsigned int j = 0; j < face.mNumIndices; j++)
            indices.push_back(face.mIndices[j]);
    }

    aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];

    auto tryLoadTexture = [&](const std::vector<aiTextureType>& types, const std::string& uniformName) {
        for (auto type : types) {
            auto loaded = loadMaterialTextures(material, type, uniformName);
            if (!loaded.empty()) {
                textures.insert(textures.end(), loaded.begin(), loaded.end());
                return true;
            }
        }
        return false;
    };

    tryLoadTexture({aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE}, "texture_diffuse");
    tryLoadTexture({aiTextureType_NORMALS, aiTextureType_HEIGHT}, "texture_normal");
    tryLoadTexture({aiTextureType_SPECULAR}, "texture_specular");
    tryLoadTexture({aiTextureType_UNKNOWN}, "texture_metallic_roughness");
    tryLoadTexture({aiTextureType_METALNESS}, "texture_metallic");
    tryLoadTexture({aiTextureType_DIFFUSE_ROUGHNESS, aiTextureType_SHININESS}, "texture_roughness");
    tryLoadTexture({aiTextureType_AMBIENT_OCCLUSION, aiTextureType_LIGHTMAP, aiTextureType_AMBIENT}, "texture_ao");
    tryLoadTexture({aiTextureType_EMISSIVE, aiTextureType_EMISSION_COLOR}, "texture_emissive");

    return Mesh(vertices, indices, textures);
}

std::vector<MeshTexture> Model::loadMaterialTextures(aiMaterial *mat, aiTextureType type, std::string typeName)
{
    std::vector<MeshTexture> textures;
    for(unsigned int i = 0; i < mat->GetTextureCount(type); i++)
    {
        aiString str;
        mat->GetTexture(type, i, &str);

        bool skip = false;
        for(unsigned int j = 0; j < textures_loaded.size(); j++)
        {
            if(std::strcmp(textures_loaded[j].path.data(), str.C_Str()) == 0)
            {
                textures.push_back(textures_loaded[j]);
                skip = true;
                break;
            }
        }
        if(!skip)
        {
            RHI::TextureHandle texHandle;
            unsigned int textureID = TextureFromFile(str.C_Str(), this->directory, this->scene, &texHandle);

            if (textureID != 0) {
                MeshTexture texture;
                texture.id = textureID;
                texture.rhiHandle = texHandle;
                texture.type = typeName;
                texture.path = str.C_Str();
                textures.push_back(texture);
                textures_loaded.push_back(texture);
            }
        }
    }
    return textures;
}

unsigned int TextureFromFile(const char *path, const std::string &directory, const aiScene *scene,
                              Haruka::RHI::TextureHandle* outHandle) {
    std::string filename = std::string(path);
    unsigned int textureID = 0;

    int width, height, nrComponents;
    unsigned char *data = nullptr;
    bool needsFree = false;

    if (filename[0] == '*')
    {
        if (!scene) return 0;
        int index = std::stoi(filename.substr(1));

        if (index >= scene->mNumTextures) {
            HARUKA_RENDERER_ERROR(ErrorCode::TEXTURE_LOAD_FAILED,
                "Texture index out of range: " + std::to_string(index));
            return 0;
        }

        aiTexture* tex = scene->mTextures[index];

        if (tex->mHeight == 0)
        {
            stbi_set_flip_vertically_on_load(true);
            data = stbi_load_from_memory(reinterpret_cast<unsigned char*>(tex->pcData), tex->mWidth, &width, &height, &nrComponents, 0);
            stbi_set_flip_vertically_on_load(false);
            needsFree = true;
        }
        else
        {
            data = reinterpret_cast<unsigned char*>(tex->pcData);
            width = tex->mWidth;
            height = tex->mHeight;
            nrComponents = 4;
            needsFree = false;
        }
    }
    else
    {
        stbi_set_flip_vertically_on_load(false);

        std::string fullPath = directory + "/" + filename;
        data = stbi_load(fullPath.c_str(), &width, &height, &nrComponents, 0);
        needsFree = true;

        if (!data)
        {
            std::string fallbackPath = "assets/textures/" + filename;
            data = stbi_load(fallbackPath.c_str(), &width, &height, &nrComponents, 0);
        }
    }

    if (data) {
        RHI::Device* dev = RHI::device();
        RHI::TextureDesc td;
        td.width = width; td.height = height;
        td.format = (nrComponents == 1) ? RHI::Format::R8
                  : (nrComponents == 4) ? RHI::Format::RGBA8 : RHI::Format::RGB8;
        td.filter = RHI::Filter::Linear; td.wrap = RHI::Wrap::Repeat; td.mipmaps = true;
        td.initialData = data;
        RHI::TextureHandle handle = dev->createTexture(td);
        textureID = dev->nativeTexture(handle);
        if (outHandle) *outHandle = handle;
        if (needsFree) stbi_image_free(data);
        // El ÉXITO no se anuncia: cargar una textura es lo normal, y con `*0`/`*1` como nombre (las
        // embebidas del .glb) la línea ni siquiera decía cuál era. El FALLO sí se reporta, justo abajo.
        return textureID;
    }

    HARUKA_RENDERER_ERROR(ErrorCode::TEXTURE_LOAD_FAILED,
        std::string("Failed to load texture: ") + path);
    return 0;
}

}} // namespace Haruka::Renderer
