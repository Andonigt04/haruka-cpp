#ifndef MODEL_H
#define MODEL_H

#include <vector>
#include <string>
#include "mesh.h"

class Model {
public:
    Model(const std::string& path);
    ~Model();

    std::vector<Mesh> meshes;
    std::string directory;
    // Vulkan only: recursos y métodos para cargar y gestionar modelos

    // Vulkan draw call for all meshes
    void draw(VkCommandBuffer cmd) const;

    int getVertexCount() const;
    int getTriangleCount() const;
};
#endif