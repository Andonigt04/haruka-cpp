#ifndef PRIMITIVE_SHAPES_H
#define PRIMITIVE_SHAPES_H

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include "mesh.h"

class PrimitiveShapes
{
public:
    /** @brief Generates a sphere using position/normal/index buffers. */
    static void createSphere(float radius, int sectors, int stacks, std::vector<glm::vec3>& vertices, std::vector<glm::vec3>& normals, std::vector<unsigned int>& indices);
    /** @brief Generates a sphere variant intended for LOD sampling. */
    static void createSphereLOD(float radius, int sectors, int stacks, std::vector<glm::vec3>& vertices, std::vector<glm::vec3>& normals, std::vector<unsigned int>& indices);
    /** @brief Generates a cube-sphere mesh. */
    static void createCubeSphere(float radius, int subdivisions, std::vector<glm::vec3>& vertices, std::vector<glm::vec3>& normals, std::vector<unsigned int>& indices);
    /** @brief Generates a cube mesh. */
    static void createCube(float size, std::vector<glm::vec3>& vertices, std::vector<glm::vec3>& normals, std::vector<unsigned int>& indices);
    /** @brief Generates a capsule mesh. */
    static void createCapsule(float radius, float height, int sectors, int stacks, std::vector<glm::vec3>& vertices, std::vector<glm::vec3>& normals, std::vector<unsigned int>& indices);
    /** @brief Generates a plane mesh. */
    static void createPlane(float width, float height, int subdivisions, std::vector<glm::vec3>& vertices, std::vector<glm::vec3>& normals, std::vector<unsigned int>& indices);
    
    /** @name Mesh-compatible overloads */
    ///@{
    static void createSphereVertex(float radius, int sectors, int stacks, std::vector<Vertex>& vertices, std::vector<unsigned int>& indices);
    static void createCubeVertex(float size, std::vector<Vertex>& vertices, std::vector<unsigned int>& indices);
    ///@}

private:
    PrimitiveShapes() = default;
};
#endif