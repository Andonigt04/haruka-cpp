#include "model.h"

#include <iostream>
#include "stb_image.h"
#include "core/error_reporter.h"

void Model::Draw(Shader &shader) {
    for(unsigned int i = 0; i < meshes.size(); i++)
        meshes[i].Draw(shader);
}

void Model::loadModel(std::string const &path) {
    this->scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace);

    if(!this->scene || this->scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !this->scene->mRootNode) {
        HARUKA_RENDERER_ERROR(ErrorCode::MODEL_LOAD_FAILED,
            std::string("Assimp error: ") + importer.GetErrorString());
        return;
    }
    directory = path.substr(0, path.find_last_of('/'));

    processNode(this->scene->mRootNode, this->scene);
}

void Model::processNode(aiNode *node, const aiScene *scene) {
    for(unsigned int i = 0; i < node->mNumMeshes; i++) {
        aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
        this->meshes.push_back(processMesh(mesh, scene));
    }
    for(unsigned int i = 0; i < node->mNumChildren; i++) {
        processNode(node->mChildren[i], scene);
    }
}

Mesh Model::processMesh(aiMesh *mesh, const aiScene *scene) {
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    std::vector<MeshTexture> textures;

    for(unsigned int i = 0; i < mesh->mNumVertices; i++) {
        Vertex vertex;
        vertex.Position = glm::vec3(mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z);

        if (mesh->HasNormals()) {
            vertex.Normal = glm::vec3(mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z);
        }
        if(mesh->mTextureCoords[0]) {
            vertex.TexCoords = glm::vec2(mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y);
        } else {
            vertex.TexCoords = glm::vec2(0.0f, 0.0f);
        }
        if (mesh->mTangents) {
            vertex.Tangent = glm::vec3(mesh->mTangents[i].x, mesh->mTangents[i].y, mesh->mTangents[i].z);
            vertex.Bitangent = glm::vec3(mesh->mBitangents[i].x, mesh->mBitangents[i].y, mesh->mBitangents[i].z);
        }
        vertices.push_back(vertex);
    }

    for(unsigned int i = 0; i < mesh->mNumFaces; i++) {
        aiFace face = mesh->mFaces[i];
        for(unsigned int j = 0; j < face.mNumIndices; j++)
            indices.push_back(face.mIndices[j]);
    }

    aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
    
    // Mapeo de tipos de textura con prioridad para evitar duplicados
    // El orden importa: intentamos primero los tipos más específicos
    
    // 1. Diffuse/Base Color (GLTF usa BASE_COLOR, OBJ usa DIFFUSE)
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
    
    // Diffuse/Albedo
    tryLoadTexture({aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE}, "texture_diffuse");
    
    // Normal Map (OBJ usa HEIGHT para bump maps)
    tryLoadTexture({aiTextureType_NORMALS, aiTextureType_HEIGHT}, "texture_normal");
    
    // Specular (para workflows tradicionales)
    tryLoadTexture({aiTextureType_SPECULAR}, "texture_specular");
    
    // Metallic-Roughness (GLTF PBR - textura combinada en UNKNOWN)
    tryLoadTexture({aiTextureType_UNKNOWN}, "texture_metallic_roughness");
    
    // Metallic separado
    tryLoadTexture({aiTextureType_METALNESS}, "texture_metallic");
    
    // Roughness separado
    tryLoadTexture({aiTextureType_DIFFUSE_ROUGHNESS, aiTextureType_SHININESS}, "texture_roughness");
    
    // Ambient Occlusion
    tryLoadTexture({aiTextureType_AMBIENT_OCCLUSION, aiTextureType_LIGHTMAP, aiTextureType_AMBIENT}, "texture_ao");
    
    // Emissive
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
            unsigned int textureID = TextureFromFile(str.C_Str(), this->directory, this->scene);
            
            if (textureID != 0) {
                MeshTexture texture;
                texture.id = textureID;
                texture.type = typeName;
                texture.path = str.C_Str();
                textures.push_back(texture);
                textures_loaded.push_back(texture); 
            }
        }
    }
    return textures;
}

unsigned int TextureFromFile(const char *path, const std::string &directory, const aiScene *scene) {
    std::string filename = std::string(path);
    unsigned int textureID;
    glGenTextures(1, &textureID);

    int width, height, nrComponents;
    unsigned char *data = nullptr;
    bool needsFree = false;
    
    // attempt to load glb
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
        
        // Textura comprimida
        if (tex->mHeight == 0)
        {
            stbi_set_flip_vertically_on_load(true);  // Cambiar a true para GLB
            data = stbi_load_from_memory(reinterpret_cast<unsigned char*>(tex->pcData), tex->mWidth, &width, &height, &nrComponents, 0);
            stbi_set_flip_vertically_on_load(false);  // Restaurar
            needsFree = true;
        } 
        // Textura descomprimida (formato argb8888)
        else
        {
            data = reinterpret_cast<unsigned char*>(tex->pcData);
            width = tex->mWidth; 
            height = tex->mHeight;
            nrComponents = 4;  // aiTexture sin comprimir siempre es ARGB8888
            needsFree = false;
        }
    }
    // attempt to load obj/fbx
    else
    {
        stbi_set_flip_vertically_on_load(false);  // GLTF no necesita flip
    
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
        GLenum format = GL_RGB;
        if (nrComponents == 1) format = GL_RED;
        else if (nrComponents == 3) format = GL_RGB;
        else if (nrComponents == 4) format = GL_RGBA;

        glBindTexture(GL_TEXTURE_2D, textureID);
        glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        if (needsFree) {
            stbi_image_free(data);
        }
        
        std::cout << "Textura cargada correctamente: " << path << " (" << width << "x" << height << ", " << nrComponents << " canales)" << std::endl;
        return textureID;
    }

    HARUKA_RENDERER_ERROR(ErrorCode::TEXTURE_LOAD_FAILED,
        std::string("Failed to load texture: ") + path);
    return 0;
}