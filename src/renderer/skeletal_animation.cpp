#define GLM_ENABLE_EXPERIMENTAL
#include "renderer/skeletal_animation.h"
#include "renderer/shader.h"
#include "rhi/rhi_device.h"
#include <assimp/postprocess.h>
#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <iostream>
#include <cstddef>

namespace Haruka {

// ---------------------------------------------------------------- conversiones
glm::mat4 aiToGlm(const aiMatrix4x4& m) {
    return glm::mat4(
        m.a1, m.b1, m.c1, m.d1,
        m.a2, m.b2, m.c2, m.d2,
        m.a3, m.b3, m.c3, m.d3,
        m.a4, m.b4, m.c4, m.d4);
}
glm::vec3 aiToGlm(const aiVector3D& v) { return glm::vec3(v.x, v.y, v.z); }
glm::quat aiToGlm(const aiQuaternion& q) { return glm::quat(q.w, q.x, q.y, q.z); }

// ------------------------------------------------------------------ SkinnedMesh
SkinnedMesh::SkinnedMesh(std::vector<SkinnedVertex> verts, std::vector<unsigned int> indices) {
    m_indexCount = (unsigned int)indices.size();
    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);

    RHI::Device* dev = RHI::device();
    m_vboH = dev->createBuffer(RHI::BufferUsage::Vertex, verts.size() * sizeof(SkinnedVertex), verts.data());
    m_eboH = dev->createBuffer(RHI::BufferUsage::Index,  indices.size() * sizeof(unsigned int), indices.data());
    m_vbo = dev->nativeBuffer(m_vboH); m_ebo = dev->nativeBuffer(m_eboH);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(SkinnedVertex), (void*)offsetof(SkinnedVertex, position));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(SkinnedVertex), (void*)offsetof(SkinnedVertex, normal));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(SkinnedVertex), (void*)offsetof(SkinnedVertex, uv));
    glEnableVertexAttribArray(3); // boneIDs (ENTEROS)
    glVertexAttribIPointer(3, MAX_BONE_INFLUENCE, GL_INT, sizeof(SkinnedVertex), (void*)offsetof(SkinnedVertex, boneIDs));
    glEnableVertexAttribArray(4); // weights
    glVertexAttribPointer(4, MAX_BONE_INFLUENCE, GL_FLOAT, GL_FALSE, sizeof(SkinnedVertex), (void*)offsetof(SkinnedVertex, weights));

    glBindVertexArray(0);
}
SkinnedMesh::SkinnedMesh(SkinnedMesh&& o) noexcept
    : m_vao(o.m_vao), m_vbo(o.m_vbo), m_ebo(o.m_ebo), m_indexCount(o.m_indexCount),
      m_vboH(o.m_vboH), m_eboH(o.m_eboH) {
    o.m_vao = o.m_vbo = o.m_ebo = 0;
    o.m_vboH = o.m_eboH = {};
}
SkinnedMesh::~SkinnedMesh() {
    if (RHI::valid(m_vboH))
        if (RHI::Device* dev = RHI::device()) { dev->destroy(m_vboH); dev->destroy(m_eboH); }
    if (m_vao) glDeleteVertexArrays(1, &m_vao);   // VAO siempre GL (transitorio)
}
void SkinnedMesh::draw() const {
    glBindVertexArray(m_vao);
    glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

// ----------------------------------------------------------------- SkinnedModel
SkinnedModel::SkinnedModel(const std::string& path) {
    m_scene = m_importer.ReadFile(path,
        aiProcess_Triangulate | aiProcess_GenSmoothNormals |
        aiProcess_LimitBoneWeights | aiProcess_CalcTangentSpace);
    if (!m_scene || m_scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !m_scene->mRootNode) {
        std::cerr << "[Skeletal] no se pudo cargar " << path << ": " << m_importer.GetErrorString() << "\n";
        m_scene = nullptr;
        return;
    }
    processNode(m_scene->mRootNode, m_scene);
    std::cout << "[Skeletal] " << path << " — " << m_meshes.size() << " mallas, "
              << m_boneCounter << " huesos, " << m_scene->mNumAnimations << " clips\n";
}
void SkinnedModel::processNode(const aiNode* node, const aiScene* scene) {
    for (unsigned int i = 0; i < node->mNumMeshes; i++)
        m_meshes.push_back(processMesh(scene->mMeshes[node->mMeshes[i]]));
    for (unsigned int i = 0; i < node->mNumChildren; i++)
        processNode(node->mChildren[i], scene);
}
SkinnedMesh SkinnedModel::processMesh(const aiMesh* mesh) {
    std::vector<SkinnedVertex> verts(mesh->mNumVertices);
    for (unsigned int i = 0; i < mesh->mNumVertices; i++) {
        verts[i].position = aiToGlm(mesh->mVertices[i]);
        if (mesh->HasNormals()) verts[i].normal = aiToGlm(mesh->mNormals[i]);
        if (mesh->mTextureCoords[0])
            verts[i].uv = glm::vec2(mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y);
    }
    std::vector<unsigned int> indices;
    indices.reserve(mesh->mNumFaces * 3);
    for (unsigned int i = 0; i < mesh->mNumFaces; i++) {
        const aiFace& f = mesh->mFaces[i];
        for (unsigned int j = 0; j < f.mNumIndices; j++) indices.push_back(f.mIndices[j]);
    }
    extractBoneWeights(verts, mesh);
    return SkinnedMesh(std::move(verts), std::move(indices));
}
void SkinnedModel::extractBoneWeights(std::vector<SkinnedVertex>& verts, const aiMesh* mesh) {
    for (unsigned int i = 0; i < mesh->mNumBones; i++) {
        const aiBone* bone = mesh->mBones[i];
        std::string name = bone->mName.C_Str();
        int boneID;
        auto it = m_boneInfoMap.find(name);
        if (it == m_boneInfoMap.end()) {
            BoneInfo bi; bi.id = m_boneCounter; bi.offset = aiToGlm(bone->mOffsetMatrix);
            m_boneInfoMap[name] = bi; boneID = m_boneCounter; m_boneCounter++;
        } else boneID = it->second.id;
        for (unsigned int w = 0; w < bone->mNumWeights; w++) {
            unsigned int vId = bone->mWeights[w].mVertexId;
            float weight = bone->mWeights[w].mWeight;
            if (vId >= verts.size()) continue;
            for (int s = 0; s < MAX_BONE_INFLUENCE; s++) {
                if (verts[vId].boneIDs[s] < 0) {
                    verts[vId].boneIDs[s] = boneID;
                    verts[vId].weights[s] = weight;
                    break;
                }
            }
        }
    }
}
void SkinnedModel::draw() const { for (const auto& m : m_meshes) m.draw(); }

// ------------------------------------------------------------------------- Bone
Bone::Bone(std::string name, int id, const aiNodeAnim* channel)
    : m_name(std::move(name)), m_id(id) {
    for (unsigned int i = 0; i < channel->mNumPositionKeys; i++)
        m_pos.push_back({ aiToGlm(channel->mPositionKeys[i].mValue), (float)channel->mPositionKeys[i].mTime });
    for (unsigned int i = 0; i < channel->mNumRotationKeys; i++)
        m_rot.push_back({ aiToGlm(channel->mRotationKeys[i].mValue), (float)channel->mRotationKeys[i].mTime });
    for (unsigned int i = 0; i < channel->mNumScalingKeys; i++)
        m_scale.push_back({ aiToGlm(channel->mScalingKeys[i].mValue), (float)channel->mScalingKeys[i].mTime });
}
static float scaleFactor(float last, float next, float t) {
    float mid = t - last, diff = next - last;
    return (diff > 1e-8f) ? mid / diff : 0.0f;
}
glm::mat4 Bone::interpPos(float t) const {
    if (m_pos.size() == 1) return glm::translate(glm::mat4(1.0f), m_pos[0].pos);
    int i = 0; for (; i < (int)m_pos.size() - 1; i++) if (t < m_pos[i + 1].t) break;
    int n = std::min(i + 1, (int)m_pos.size() - 1);
    float f = scaleFactor(m_pos[i].t, m_pos[n].t, t);
    return glm::translate(glm::mat4(1.0f), glm::mix(m_pos[i].pos, m_pos[n].pos, f));
}
glm::mat4 Bone::interpRot(float t) const {
    if (m_rot.size() == 1) return glm::toMat4(glm::normalize(m_rot[0].rot));
    int i = 0; for (; i < (int)m_rot.size() - 1; i++) if (t < m_rot[i + 1].t) break;
    int n = std::min(i + 1, (int)m_rot.size() - 1);
    float f = scaleFactor(m_rot[i].t, m_rot[n].t, t);
    return glm::toMat4(glm::normalize(glm::slerp(m_rot[i].rot, m_rot[n].rot, f)));
}
glm::mat4 Bone::interpScale(float t) const {
    if (m_scale.size() == 1) return glm::scale(glm::mat4(1.0f), m_scale[0].scale);
    int i = 0; for (; i < (int)m_scale.size() - 1; i++) if (t < m_scale[i + 1].t) break;
    int n = std::min(i + 1, (int)m_scale.size() - 1);
    float f = scaleFactor(m_scale[i].t, m_scale[n].t, t);
    return glm::scale(glm::mat4(1.0f), glm::mix(m_scale[i].scale, m_scale[n].scale, f));
}
void Bone::update(float t) {
    glm::mat4 p = m_pos.empty()   ? glm::mat4(1.0f) : interpPos(t);
    glm::mat4 r = m_rot.empty()   ? glm::mat4(1.0f) : interpRot(t);
    glm::mat4 s = m_scale.empty() ? glm::mat4(1.0f) : interpScale(t);
    m_local = p * r * s;
}

// -------------------------------------------------------------------- Animation
Animation::Animation(const aiScene* scene, SkinnedModel& model, int clipIndex) {
    if (!scene || clipIndex >= (int)scene->mNumAnimations) return;
    const aiAnimation* anim = scene->mAnimations[clipIndex];
    m_duration = (float)anim->mDuration;
    m_tps = anim->mTicksPerSecond != 0.0 ? (float)anim->mTicksPerSecond : 25.0f;
    readHierarchy(m_root, scene->mRootNode);
    readMissingBones(anim, model);
}
void Animation::readHierarchy(NodeData& dst, const aiNode* src) {
    dst.name = src->mName.C_Str();
    dst.transform = aiToGlm(src->mTransformation);
    for (unsigned int i = 0; i < src->mNumChildren; i++) {
        NodeData child;
        readHierarchy(child, src->mChildren[i]);
        dst.children.push_back(std::move(child));
    }
}
void Animation::readMissingBones(const aiAnimation* anim, SkinnedModel& model) {
    auto& map = model.boneInfoMap();
    int& count = model.boneCount();
    for (unsigned int i = 0; i < anim->mNumChannels; i++) {
        const aiNodeAnim* ch = anim->mChannels[i];
        std::string name = ch->mNodeName.C_Str();
        if (map.find(name) == map.end()) { BoneInfo bi; bi.id = count; map[name] = bi; count++; }
        m_bones.emplace_back(name, map[name].id, ch);
    }
    m_boneInfoMap = map;
}
Bone* Animation::findBone(const std::string& name) {
    for (auto& b : m_bones) if (b.name() == name) return &b;
    return nullptr;
}

// --------------------------------------------------------------------- Animator
Animator::Animator(Animation* anim) {
    m_final.assign(MAX_BONES, glm::mat4(1.0f));
    if (anim) play(anim);
}
void Animator::play(Animation* anim) { m_anim = anim; m_time = 0.0f; }
void Animator::update(float dt) {
    if (!m_anim) return;
    m_time += m_anim->ticksPerSecond() * dt;
    m_time = std::fmod(m_time, m_anim->duration());
    calc(m_anim->root(), glm::mat4(1.0f));
}
void Animator::calc(const NodeData& node, const glm::mat4& parent) {
    glm::mat4 nodeT = node.transform;
    if (Bone* b = m_anim->findBone(node.name)) { b->update(m_time); nodeT = b->localTransform(); }
    glm::mat4 global = parent * nodeT;
    auto& map = m_anim->boneInfoMap();
    auto it = map.find(node.name);
    if (it != map.end() && it->second.id < MAX_BONES)
        m_final[it->second.id] = global * it->second.offset;
    for (const auto& c : node.children) calc(c, global);
}
void Animator::upload(Shader& shader) const {
    // u_finalBones[] como UBO (binding 4) → robusto con SPIR-V (sin nombres).
    static Haruka::RHI::BufferHandle uboH;
    static GLuint ubo = 0;
    const size_t bytes = MAX_BONES * sizeof(glm::mat4);
    RHI::Device* dev = RHI::device();
    if (!RHI::valid(uboH)) {
        uboH = dev->createBuffer(RHI::BufferUsage::Uniform, bytes, nullptr, RHI::BufferMemory::Dynamic);
        ubo = dev->nativeBuffer(uboH);
    }
    dev->updateBuffer(uboH, 0, bytes, m_final.data());
    glBindBufferBase(GL_UNIFORM_BUFFER, 4, ubo);   // binding sigue GL (native id)
    (void)shader;
}

} // namespace Haruka
