#include "virtual_texturing.h"
#include <iostream>
#include <cmath>
#include <algorithm>

VirtualTexturing::VirtualTexturing() {}

VirtualTexturing::~VirtualTexturing() {
    for (auto& pair : virtualTextures) {
        auto& vt = pair.second;
        if (vt->indirectionTexture) glDeleteTextures(1, &vt->indirectionTexture);
        if (vt->physicalTexture) glDeleteTextures(1, &vt->physicalTexture);
        if (vt->feedbackTexture) glDeleteTextures(1, &vt->feedbackTexture);
    }
    if (feedbackShader) glDeleteProgram(feedbackShader);
    if (pageUploadShader) glDeleteProgram(pageUploadShader);
}

void VirtualTexturing::init(const VTConfig& cfg) {
    config = cfg;

    std::cout << "✓ Virtual Texturing initialized\n";
    std::cout << "  Page Size: " << config.pageSize << "x" << config.pageSize << "\n";
    std::cout << "  Max Cache: " << (config.maxCacheMemory / (1024*1024)) << " MB\n";
    std::cout << "  Max Resident Pages: " << config.maxResidentPages << "\n";
}

void VirtualTexturing::createVirtualTextureGPUResources(VirtualTextureData& vt) {
    // Calcular tamaño de la page table
    vt.pageTableSize = glm::uvec2(
        (vt.virtualSize.x + config.pageSize - 1) / config.pageSize,
        (vt.virtualSize.y + config.pageSize - 1) / config.pageSize
    );

    // Crear indirection texture (mapa de páginas)
    glGenTextures(1, &vt.indirectionTexture);
    glBindTexture(GL_TEXTURE_2D, vt.indirectionTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32UI, vt.pageTableSize.x, vt.pageTableSize.y,
                 0, GL_RG_INTEGER, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // Crear physical texture (atlas de páginas cargadas)
    int physicalSize = static_cast<int>(
        std::sqrt(config.maxResidentPages) * config.pageSize
    );
    glGenTextures(1, &vt.physicalTexture);
    glBindTexture(GL_TEXTURE_2D, vt.physicalTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, physicalSize, physicalSize,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Crear feedback texture
    glGenTextures(1, &vt.feedbackTexture);
    glBindTexture(GL_TEXTURE_2D, vt.feedbackTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16UI, config.feedbackBufferSize, config.feedbackBufferSize,
                 0, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    std::cout << "  ✓ Created virtual texture: " << vt.id << "\n";
    std::cout << "    Virtual: " << vt.virtualSize.x << "x" << vt.virtualSize.y << "\n";
    std::cout << "    Page Table: " << vt.pageTableSize.x << "x" << vt.pageTableSize.y << "\n";
}

void VirtualTexturing::addVirtualTexture(
    const std::string& textureId,
    const std::string& filePath,
    glm::uvec2 virtualSize) {

    auto vt = std::make_unique<VirtualTextureData>();
    vt->id = textureId;
    vt->filePath = filePath;
    vt->virtualSize = virtualSize;

    createVirtualTextureGPUResources(*vt);

    virtualTextures[textureId] = std::move(vt);
}

void VirtualTexturing::processFeedback() {
    // En una implementación real, aquí se leerían los feedback buffers
    // y se cargarían las páginas solicitadas
    
    for (auto& pair : virtualTextures) {
        auto& vt = pair.second;
        
        // Procesar cola de páginas solicitadas
        while (!vt->pageQueue.empty() && vt->residentPageCount < config.maxResidentPages) {
            PageRequest req = vt->pageQueue.front();
            vt->pageQueue.pop();
            
            uploadPageToPhysical(*vt, req);
        }
        
        updateIndirectionTexture(*vt);
    }
}

void VirtualTexturing::uploadPageToPhysical(VirtualTextureData& vt, const PageRequest& request) {
    glm::uvec2 page = request.page;
    
    // Verificar si ya está cargada
    if (vt.residentPages.find(page) != vt.residentPages.end()) {
        return;
    }
    
    // Si cache lleno, evict LRU
    if (vt.residentPageCount >= config.maxResidentPages) {
        // Buscar página menos recientemente usada (simplificado)
        auto it = vt.residentPages.begin();
        vt.residentPages.erase(it);
        vt.residentPageCount--;
    }
    
    // Marcar como residente
    vt.residentPages[page] = true;
    vt.residentPageCount++;
    vt.memoryUsage += config.pageSize * config.pageSize * 4;  // RGBA8
    totalMemoryUsage += config.pageSize * config.pageSize * 4;
}

void VirtualTexturing::updateIndirectionTexture(VirtualTextureData& vt) {
    // Actualizar mapa de indirección (qué página está dónde en physical texture)
    // En implementación real, esto sería más complejo
    std::vector<glm::uvec2> pageTable(vt.pageTableSize.x * vt.pageTableSize.y, glm::uvec2(0));
    
    int pageIndex = 0;
    for (auto& pair : vt.residentPages) {
        if (pair.second) {
            glm::uvec2 page = pair.first;
            if (page.x < vt.pageTableSize.x && page.y < vt.pageTableSize.y) {
                pageTable[page.y * vt.pageTableSize.x + page.x] = glm::uvec2(pageIndex, 0);
                pageIndex++;
            }
        }
    }
    
    glBindTexture(GL_TEXTURE_2D, vt.indirectionTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, vt.pageTableSize.x, vt.pageTableSize.y,
                    GL_RG_INTEGER, GL_UNSIGNED_INT, pageTable.data());
}

void VirtualTexturing::bindVirtualTexture(GLuint shaderProgram, const std::string& textureId) {
    auto it = virtualTextures.find(textureId);
    if (it == virtualTextures.end()) return;

    auto& vt = it->second;
    
    glUseProgram(shaderProgram);
    
    // Bindear physical texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, vt->physicalTexture);
    glUniform1i(glGetUniformLocation(shaderProgram, "physicalTexture"), 0);
    
    // Bindear indirection texture
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, vt->indirectionTexture);
    glUniform1i(glGetUniformLocation(shaderProgram, "indirectionTexture"), 1);
    
    // Bindear feedback texture
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, vt->feedbackTexture);
    glUniform1i(glGetUniformLocation(shaderProgram, "feedbackTexture"), 2);
    
    // Parámetros
    glUniform2ui(glGetUniformLocation(shaderProgram, "pageTableSize"),
                 vt->pageTableSize.x, vt->pageTableSize.y);
    glUniform1i(glGetUniformLocation(shaderProgram, "pageSize"), config.pageSize);
}

GLuint VirtualTexturing::getIndirectionTexture(const std::string& textureId) const {
    auto it = virtualTextures.find(textureId);
    if (it != virtualTextures.end()) {
        return it->second->indirectionTexture;
    }
    return 0;
}

GLuint VirtualTexturing::getPhysicalTexture(const std::string& textureId) const {
    auto it = virtualTextures.find(textureId);
    if (it != virtualTextures.end()) {
        return it->second->physicalTexture;
    }
    return 0;
}

VirtualTexturing::VTStats VirtualTexturing::getStats(const std::string& textureId) const {
    VTStats stats = {};
    
    auto it = virtualTextures.find(textureId);
    if (it != virtualTextures.end()) {
        const auto& vt = it->second;
        stats.totalVirtualPages = vt->pageTableSize.x * vt->pageTableSize.y;
        stats.residentPages = vt->residentPageCount;
        stats.pageQueueSize = vt->pageQueue.size();
        stats.cacheUtilization = static_cast<float>(vt->residentPageCount) / config.maxResidentPages;
        stats.totalMemory = vt->memoryUsage;
    }
    
    return stats;
}

void VirtualTexturing::makeRoomInCache(size_t neededBytes) {
    while (totalMemoryUsage + neededBytes > config.maxCacheMemory && !virtualTextures.empty()) {
        // Evict LRU page from any texture
        for (auto& pair : virtualTextures) {
            auto& vt = pair.second;
            if (!vt->residentPages.empty()) {
                auto it = vt->residentPages.begin();
                vt->residentPages.erase(it);
                vt->residentPageCount--;
                vt->memoryUsage -= config.pageSize * config.pageSize * 4;
                totalMemoryUsage -= config.pageSize * config.pageSize * 4;
                break;
            }
        }
    }
}
