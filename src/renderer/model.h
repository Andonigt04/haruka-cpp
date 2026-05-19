/**
 * @file model.h
 * @brief Assimp-backed model loader — parses 3D assets into engine `Mesh` instances.
 */
#ifndef MODEL_H
#define MODEL_H

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <vector>
#include <string>

#include "mesh.h"
#include "shader.h"

/** @brief Loads texture resource from model directory context. */
unsigned int TextureFromFile(const char *path, const std::string &directory, const aiScene *scene);

/**
 * @brief Assimp-backed model loader and draw wrapper.
 *
 * Owns parsed meshes and deduplicated texture descriptors.
 */
class Model
{
public:
    /** @brief Loads model at construction time. */
    Model(const std::string &path) { loadModel(path); }
    
    /** @brief Draws all internal meshes. */
    void Draw(Shader &shader);

    /** @brief Aggregated vertex count across all sub-meshes. */
    int getVertexCount() const {
        int total = 0;
        for (const auto& mesh : meshes) total += mesh.getVertexCount();
        return total;
    }
    /** @brief Aggregated triangle count across all sub-meshes. */
    int getTriangleCount() const {
        int total = 0;
        for (const auto& mesh : meshes) total += mesh.getTriangleCount();
        return total;
    }
private:
    std::vector<Mesh> meshes;
    std::string directory;
    std::vector<MeshTexture> textures_loaded;

    Assimp::Importer importer;
    const aiScene* scene = nullptr;

    /** @brief Parses model file and initializes node traversal. */
    void loadModel(std::string const &path);
    /** @brief Recursively processes one Assimp node hierarchy branch. */
    void processNode(aiNode *node, const aiScene *scene);
    /** @brief Converts one Assimp mesh into engine `Mesh`. */
    Mesh processMesh(aiMesh *mesh, const aiScene *scene);
    /** @brief Loads material textures by semantic type with deduplication. */
    std::vector<MeshTexture> loadMaterialTextures(aiMaterial *mat, aiTextureType type, std::string typeName);
};
#endif