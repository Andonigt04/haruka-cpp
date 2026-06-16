#include "gpu_heightfield.h"
#include "renderer/compute_shader.h"

namespace Haruka {

GpuHeightfield::GpuHeightfield() = default;
GpuHeightfield::~GpuHeightfield() {
    auto del = [](Buffers& b) {
        if (b.dirs)  glDeleteBuffers(1, &b.dirs);
        if (b.elev)  glDeleteBuffers(1, &b.elev);
        if (b.norm)  glDeleteBuffers(1, &b.norm);
        if (b.water) glDeleteBuffers(1, &b.water);
    };
    del(m_sync);
    for (auto& s : m_slot) { del(s.buf); if (s.fence) glDeleteSync(s.fence); }
}

bool GpuHeightfield::ensureShader() {
    if (m_failed) return false;
    if (!m_shader) {
        m_shader = std::make_unique<Haruka::Renderer::ComputeShader>("shaders/terrain_gen.comp");
        if (m_shader->getID() == 0) { m_failed = true; return false; }
    }
    return true;
}

void GpuHeightfield::ensureBuffers(Buffers& b, std::size_t n) {
    if (n <= b.capacity && b.dirs) return;
    if (b.dirs) { glDeleteBuffers(1, &b.dirs); glDeleteBuffers(1, &b.elev); glDeleteBuffers(1, &b.norm); glDeleteBuffers(1, &b.water); }
    glGenBuffers(1, &b.dirs); glGenBuffers(1, &b.elev); glGenBuffers(1, &b.norm); glGenBuffers(1, &b.water);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, b.dirs);
    glBufferData(GL_SHADER_STORAGE_BUFFER, n * sizeof(glm::vec4), nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, b.elev);
    glBufferData(GL_SHADER_STORAGE_BUFFER, n * sizeof(float),     nullptr, GL_DYNAMIC_READ);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, b.norm);
    glBufferData(GL_SHADER_STORAGE_BUFFER, n * sizeof(glm::vec4), nullptr, GL_DYNAMIC_READ);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, b.water);
    glBufferData(GL_SHADER_STORAGE_BUFFER, n * sizeof(float),     nullptr, GL_DYNAMIC_READ);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    b.capacity = n;
}

void GpuHeightfield::uploadAndDispatch(Buffers& b, const std::vector<glm::vec3>& dirs, const Params& p) {
    const std::size_t n = dirs.size();
    ensureBuffers(b, n);
    std::vector<glm::vec4> dirs4(n);
    for (std::size_t i = 0; i < n; ++i) dirs4[i] = glm::vec4(dirs[i], 0.0f);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, b.dirs);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, n * sizeof(glm::vec4), dirs4.data());

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, b.dirs);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, b.elev);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, b.norm);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, b.water);

    m_shader->use();
    glUniform1i(0, (int)n);
    glUniform1i(1, p.seed);
    glUniform1f(2, p.continentFreqA);
    glUniform1f(3, p.reliefStrength);
    glUniform1f(4, (float)(p.radius > 1e-9 ? 1.0 / p.radius : 0.0));
    glUniform1f(5, p.seaThreshold);
    glUniform1f(6, p.voronoiDensity);
    glUniform1f(7, p.lakeDensity);
    glUniform1f(8, p.lakeMaxProb);

    glDispatchCompute((GLuint)((n + 63) / 64), 1, 1);
}

void GpuHeightfield::readback(Buffers& b, std::size_t n, std::vector<float>& elev,
                              std::vector<glm::vec3>& normal, std::vector<float>& water) {
    elev.resize(n); water.resize(n);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, b.elev);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, n * sizeof(float), elev.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, b.water);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, n * sizeof(float), water.data());
    std::vector<glm::vec4> norm4(n);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, b.norm);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, n * sizeof(glm::vec4), norm4.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    normal.resize(n);
    for (std::size_t i = 0; i < n; ++i) normal[i] = glm::vec3(norm4[i]);
}

bool GpuHeightfield::generate(const std::vector<glm::vec3>& dirs, const Params& p,
                              std::vector<float>& outElevKm, std::vector<glm::vec3>& outNormal,
                              std::vector<float>& outWaterKm) {
    if (!ensureShader()) return false;
    if (dirs.empty()) { outElevKm.clear(); outNormal.clear(); outWaterKm.clear(); return true; }
    uploadAndDispatch(m_sync, dirs, p);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    readback(m_sync, dirs.size(), outElevKm, outNormal, outWaterKm);
    return true;
}

// --------------------------- camino ASÍNCRONO ---------------------------

bool GpuHeightfield::hasFreeSlot() const {
    for (const auto& s : m_slot) if (!s.inUse) return true;
    return false;
}

int GpuHeightfield::dispatchAsync(const std::vector<glm::vec3>& dirs, const Params& p) {
    if (!ensureShader() || dirs.empty()) return -1;
    int slot = -1;
    for (int i = 0; i < kSlots; ++i) if (!m_slot[i].inUse) { slot = i; break; }
    if (slot < 0) return -1;
    Slot& s = m_slot[slot];
    uploadAndDispatch(s.buf, dirs, p);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    s.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    s.count = dirs.size();
    s.inUse = true;
    return slot;
}

bool GpuHeightfield::tryHarvest(int slot, std::vector<float>& outElevKm, std::vector<glm::vec3>& outNormal,
                                std::vector<float>& outWaterKm) {
    if (slot < 0 || slot >= kSlots) return false;
    Slot& s = m_slot[slot];
    if (!s.inUse || !s.fence) return false;
    GLenum r = glClientWaitSync(s.fence, 0, 0); // 0 timeout → no bloquea
    if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) return false; // aún computa
    readback(s.buf, s.count, outElevKm, outNormal, outWaterKm);
    glDeleteSync(s.fence); s.fence = nullptr; s.inUse = false;
    return true;
}

} // namespace Haruka
