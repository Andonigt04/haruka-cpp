#pragma once
/**
 * @file skeletal_animation.h
 * @brief Animación esqueletal (GPU skinning) sobre Assimp.
 *
 * Pipeline estándar:
 *   SkinnedModel  — carga un modelo riggeado (glTF/FBX) con Assimp: mallas con
 *                   boneIDs+weights por vértice + mapa de huesos (offset = bind inv).
 *   Animation     — un clip: por hueso, keyframes (pos/rot/escala) + la jerarquía
 *                   de nodos. Construido del mismo aiScene.
 *   Animator      — muestrea el clip en el tiempo → finalBoneMatrices[MAX_BONES],
 *                   que el shader skinned.vert usa para transformar cada vértice.
 *
 * El Mesh/Model estático del motor NO se toca; esto es una vía aparte para modelos
 * animados (jugador, criaturas, items en mano...).
 */
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>
#include <map>
#include <memory>

namespace Haruka { namespace Renderer { class Shader; } } using Haruka::Renderer::Shader;

namespace Haruka {

static constexpr int MAX_BONES          = 100;
static constexpr int MAX_BONE_INFLUENCE = 4;

struct SkinnedVertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f};
    glm::vec2 uv{0.0f};
    int   boneIDs[MAX_BONE_INFLUENCE] = { -1, -1, -1, -1 };
    float weights[MAX_BONE_INFLUENCE] = { 0.0f, 0.0f, 0.0f, 0.0f };
};

struct BoneInfo {
    int       id = 0;            ///< índice en el array de matrices finales
    glm::mat4 offset{1.0f};      ///< bind-pose inversa (Assimp offsetMatrix)
};

// --- conversión Assimp → glm ---
glm::mat4 aiToGlm(const aiMatrix4x4& m);
glm::vec3 aiToGlm(const aiVector3D& v);
glm::quat aiToGlm(const aiQuaternion& q);

/** @brief Una sub-malla con skinning (VAO con boneIDs+weights). */
class SkinnedMesh {
public:
    SkinnedMesh(std::vector<SkinnedVertex> verts, std::vector<unsigned int> indices);
    ~SkinnedMesh();
    SkinnedMesh(SkinnedMesh&&) noexcept;
    SkinnedMesh(const SkinnedMesh&) = delete;
    void draw() const;
private:
    unsigned int m_vao = 0, m_vbo = 0, m_ebo = 0, m_indexCount = 0;
};

/** @brief Modelo riggeado: mallas con skinning + mapa de huesos. Conserva el aiScene. */
class SkinnedModel {
public:
    explicit SkinnedModel(const std::string& path);
    void draw() const;

    std::map<std::string, BoneInfo>& boneInfoMap() { return m_boneInfoMap; }
    int& boneCount() { return m_boneCounter; }
    const aiScene* scene() const { return m_scene; }
    bool valid() const { return m_scene != nullptr && !m_meshes.empty(); }

private:
    std::vector<SkinnedMesh>        m_meshes;
    std::map<std::string, BoneInfo> m_boneInfoMap;
    int m_boneCounter = 0;
    Assimp::Importer m_importer;
    const aiScene*   m_scene = nullptr;

    void processNode(const aiNode* node, const aiScene* scene);
    SkinnedMesh processMesh(const aiMesh* mesh);
    void extractBoneWeights(std::vector<SkinnedVertex>& verts, const aiMesh* mesh);
};

// --- canales de animación de un hueso ---
struct KeyPosition { glm::vec3 pos; float t; };
struct KeyRotation { glm::quat rot; float t; };
struct KeyScale    { glm::vec3 scale; float t; };

class Bone {
public:
    Bone(std::string name, int id, const aiNodeAnim* channel);
    void update(float time);                 ///< calcula localTransform en 'time'
    const glm::mat4& localTransform() const { return m_local; }
    const std::string& name() const { return m_name; }
private:
    std::vector<KeyPosition> m_pos;
    std::vector<KeyRotation> m_rot;
    std::vector<KeyScale>    m_scale;
    glm::mat4   m_local{1.0f};
    std::string m_name;
    int         m_id = 0;
    int   keyIndex(const std::vector<float>&, float) const;
    glm::mat4 interpPos(float) const;
    glm::mat4 interpRot(float) const;
    glm::mat4 interpScale(float) const;
};

// jerarquía de nodos copiada del aiScene (independiente del importer).
struct NodeData {
    glm::mat4 transform{1.0f};
    std::string name;
    std::vector<NodeData> children;
};

/** @brief Un clip de animación (duración, ticks, huesos, jerarquía). */
class Animation {
public:
    Animation(const aiScene* scene, SkinnedModel& model, int clipIndex = 0);
    Bone* findBone(const std::string& name);
    float ticksPerSecond() const { return m_tps; }
    float duration()       const { return m_duration; }
    const NodeData& root() const { return m_root; }
    std::map<std::string, BoneInfo>& boneInfoMap() { return m_boneInfoMap; }
private:
    float m_duration = 0.0f;
    float m_tps = 25.0f;
    std::vector<Bone> m_bones;
    NodeData m_root;
    std::map<std::string, BoneInfo> m_boneInfoMap;
    void readHierarchy(NodeData& dst, const aiNode* src);
    void readMissingBones(const aiAnimation* anim, SkinnedModel& model);
};

/** @brief Reproduce un clip → matrices finales para el shader. */
class Animator {
public:
    explicit Animator(Animation* anim = nullptr);
    void play(Animation* anim);
    void update(float dt);
    /** @brief Sube las matrices finales al shader (uniform array u_finalBones). */
    void upload(Shader& shader) const;
    const std::vector<glm::mat4>& matrices() const { return m_final; }
private:
    std::vector<glm::mat4> m_final;
    Animation* m_anim = nullptr;
    float m_time = 0.0f;
    void calc(const NodeData& node, const glm::mat4& parent);
};

} // namespace Haruka

