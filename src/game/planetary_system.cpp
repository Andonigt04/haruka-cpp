#include "planetary_system.h"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <limits>
#include <unordered_map>

namespace Haruka {

namespace {
constexpr double kPlayerCollisionRadiusKm = 0.00095; // ~0.95 m
constexpr double kGroundEpsilonKm = 0.00150;         // ~1.5 m de separación visual/colisión
constexpr double kTerminalFallSpeedKmS = 0.05;   // 50 m/s
}

namespace {
bool rayTriangleIntersect(
    const glm::dvec3& origin,
    const glm::dvec3& dir,
    const glm::dvec3& v0,
    const glm::dvec3& v1,
    const glm::dvec3& v2,
    double& t
) {
    const double EPS = 1e-9;
    glm::dvec3 edge1 = v1 - v0;
    glm::dvec3 edge2 = v2 - v0;
    glm::dvec3 h = glm::cross(dir, edge2);
    double a = glm::dot(edge1, h);
    if (a > -EPS && a < EPS) return false;

    double f = 1.0 / a;
    glm::dvec3 s = origin - v0;
    double u = f * glm::dot(s, h);
    if (u < 0.0 || u > 1.0) return false;

    glm::dvec3 q = glm::cross(s, edge1);
    double v = f * glm::dot(dir, q);
    if (v < 0.0 || u + v > 1.0) return false;

    double tmpT = f * glm::dot(edge2, q);
    if (tmpT > EPS) {
        t = tmpT;
        return true;
    }
    return false;
}
}

PlanetarySystem::PlanetarySystem() {}

PlanetarySystem::~PlanetarySystem() {}

void PlanetarySystem::init(Scene* scene, WorldSystem* worldSystem) {
    this->scene = scene;
    this->worldSystem = worldSystem;
}

void PlanetarySystem::initTerrain(int size, float heightScale, int seed) {
    terrain = std::make_unique<Terrain>(size, heightScale);
    terrain->setPosition(glm::vec3(-size * 0.5f, -10.0f, -size * 0.5f));
    terrain->setScale(glm::vec3(10.0f, 1.0f, 10.0f));
    terrain->generatePerlin(seed);
    std::cout << "[PlanetarySystem] Terrain initialized: size=" << size
              << " heightScale=" << heightScale << " seed=" << seed << std::endl;
}

void PlanetarySystem::renderTerrain(Shader& shader, const Camera* camera) {
    if (!terrain) return;

    // Asegurar texturas válidas para shaders deferred que esperan samplers
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, 0);

    shader.setInt("texture_diffuse1", 0);
    shader.setInt("texture_specular1", 1);
    shader.setInt("texture_emissive1", 2);

    // Si el WorldSystem indica solo mesh base, renderizar el mesh base del planeta
    if (worldSystem && worldSystem->shouldRenderBaseMesh()) {
        if (scene) {
            auto* earthObj = scene->getObject("Earth");
            if (earthObj && earthObj->meshRenderer && earthObj->meshRenderer->isResident()) {
                glm::mat4 model = earthObj->getWorldTransform(scene);
                shader.setMat4("model", model);
                earthObj->meshRenderer->render(shader);
            }
        }
        return;
    }

    terrain->render(shader, camera);
}

void PlanetarySystem::setDetailedSurfaceData(const std::string& bodyName, const PlanetData& data) {
    detailedSurfaceData[bodyName] = data;
}

double PlanetarySystem::getSurfaceRadiusAtDirection(const CelestialBody* body, const glm::dvec3& planetToPoint) const {
    if (!body) return 0.0;

    if (glm::length(planetToPoint) < 1e-9) {
        return static_cast<double>(body->radius);
    }

    auto it = detailedSurfaceData.find(body->name);
    if (it == detailedSurfaceData.end() || it->second.vertices.empty()) {
        return static_cast<double>(body->radius);
    }

    glm::dvec3 dir = glm::normalize(planetToPoint);
    const auto& data = it->second;

    // Precisión alta: intersección rayo (centro->dir) contra triángulos de superficie
    if (!data.indices.empty() && data.indices.size() % 3 == 0) {
        const glm::dvec3 origin(0.0);
        double closestT = std::numeric_limits<double>::infinity();

        for (size_t i = 0; i + 2 < data.indices.size(); i += 3) {
            const glm::dvec3 v0 = glm::dvec3(data.vertices[data.indices[i]]);
            const glm::dvec3 v1 = glm::dvec3(data.vertices[data.indices[i + 1]]);
            const glm::dvec3 v2 = glm::dvec3(data.vertices[data.indices[i + 2]]);

            double t = 0.0;
            if (rayTriangleIntersect(origin, dir, v0, v1, v2, t)) {
                if (t < closestT) {
                    closestT = t;
                }
            }
        }

        if (std::isfinite(closestT)) {
            return static_cast<double>(body->radius) * closestT;
        }
    }

    // Fallback: aproximación por vértice más alineado
    double bestDot = -std::numeric_limits<double>::infinity();
    double localRadius = 1.0;
    for (const auto& v : data.vertices) {
        glm::dvec3 vn = glm::normalize(glm::dvec3(v));
        double d = glm::dot(vn, dir);
        if (d > bestDot) {
            bestDot = d;
            localRadius = glm::length(glm::dvec3(v));
        }
    }

    return static_cast<double>(body->radius) * localRadius;
}

CelestialBody* PlanetarySystem::addStar(const std::string& name, double mass, double radius) {
    CelestialBody sun;
    sun.name = name;
    sun.worldPos = {0.0, 0.0, 0.0};
    sun.localPos = {0.0f, 0.0f, 0.0f};
    sun.mass = mass;
    sun.radius = static_cast<float>(radius / Units::KM);
    sun.color = glm::vec3(1.0f, 0.9f, 0.0f);
    sun.emissionStrength = 1.0f;
    sun.visible = 1;
    sun.lodLevel = 0;
    sun.velocity = glm::vec3(0.0f);
    
    worldSystem->addBody(sun);
    star = worldSystem->findBody(name);
    bodyNames.push_back(name);
    
    std::cout << "⭐ Star added: " << name << " (radius: " << radius / Units::MEGAMETER << " Mm)" << std::endl;
    return star;
}

CelestialBody* PlanetarySystem::addPlanet(const std::string& name, double orbitalDistance, double mass, double radius, glm::vec3 color) {
    CelestialBody planet;
    planet.name = name;
    planet.worldPos = {orbitalDistance, 0.0, 0.0};
    planet.localPos = {static_cast<float>(orbitalDistance / Units::MEGAMETER), 0.0f, 0.0f};
    planet.mass = mass;
    planet.radius = static_cast<float>(radius / Units::KM);
    planet.color = color;
    planet.emissionStrength = 0.0f;
    planet.visible = 1;
    planet.lodLevel = 0;
    
    // Velocidad orbital: v = sqrt(G * M / r)
    double orbitalVelocity = std::sqrt(G * star->mass / orbitalDistance);
    planet.velocity = glm::vec3(0.0f, static_cast<float>(orbitalVelocity), 0.0f);
    
    worldSystem->addBody(planet);
    bodyNames.push_back(name);
    
    std::cout << "🪨 Planet added: " << name 
              << " (distance: " << orbitalDistance / Units::MEGAMETER << " Mm"
              << ", radius: " << radius / Units::KM << " km)" << std::endl;
    return worldSystem->findBody(name);
}

CelestialBody* PlanetarySystem::addBody(const CelestialBody& body) {
    worldSystem->addBody(body);
    bodyNames.push_back(body.name);
    
    std::cout << "✓ Body added: " << body.name << std::endl;
    return worldSystem->findBody(body.name);
}

void PlanetarySystem::update(double dt) {
    if (!worldSystem || !scene) return;
    
    simulationTime += dt * timeScale;
    
    // Modo seguro: no mutar la Scene del editor por frame desde gameplay.
    // Esto evita invalidar punteros internos de panels/inspector y corrupción de heap.
    // integrateOrbits(dt);
    // updateWorldOrigin();
    // syncSceneWithOrbits();
    applyPlanetaryPhysics(dt);
    updatePlayerOnPlanet();
    
    if (player) {
        player->update(static_cast<float>(dt));
    }
}

void PlanetarySystem::integrateOrbits(double dt) {
    auto& bodies = const_cast<std::vector<CelestialBody>&>(worldSystem->getBodies());
    
    if (!star) return;
    
    for (auto& body : bodies) {
        if (body.name == star->name) continue;
        
        glm::dvec3 direction = glm::dvec3(star->worldPos.x, star->worldPos.y, star->worldPos.z) - 
                               glm::dvec3(body.worldPos.x, body.worldPos.y, body.worldPos.z);
        double distance = glm::length(direction);
        
        if (distance > body.radius * Units::KM) {
            double forceMagnitude = (G * star->mass * body.mass) / (distance * distance);
            glm::dvec3 acc = glm::normalize(direction) * forceMagnitude;
            
            body.velocity += glm::vec3(acc * dt * timeScale);
            body.worldPos.x += body.velocity.x * dt * timeScale;
            body.worldPos.y += body.velocity.y * dt * timeScale;
            body.worldPos.z += body.velocity.z * dt * timeScale;
        }
    }
}

void PlanetarySystem::updateWorldOrigin() {
    if (!player) return;
    
    auto closestPlanet = getClosestPlanet();
    if (closestPlanet) {
        worldSystem->updateOrigin(closestPlanet->worldPos);
    }
}

void PlanetarySystem::syncSceneWithOrbits() {
    if (!scene || !worldSystem) return;
    
    for (const auto& body : worldSystem->getBodies()) {
        auto sceneObj = scene->getObject(body.name);
        if (!sceneObj) {
            SceneObject obj;
            obj.name = body.name;
            obj.type = "CelestialBody";
            scene->addObject(obj);
            sceneObj = scene->getObject(obj.name);
        }
        
        if (sceneObj) {
            glm::dvec3 localKm(body.localPos.x, body.localPos.y, body.localPos.z);
            double renderRadius = Units::kmToRender(static_cast<double>(body.radius));

            sceneObj->position = Units::kmToRender(localKm);
            sceneObj->scale = glm::dvec3(renderRadius);
            sceneObj->color = body.color;
        }
    }
}

CelestialBody* PlanetarySystem::findBody(const std::string& name) {
    return worldSystem->findBody(name);
}

CelestialBody* PlanetarySystem::getClosestPlanet() {
    if (!player) return nullptr;
    
    glm::dvec3 playerPos = player->getPosition();
    CelestialBody* closest = nullptr;
    double minDist = std::numeric_limits<double>::max();
    
    for (const auto& bodyName : bodyNames) {
        auto body = findBody(bodyName);
        if (!body || body->name == (star ? star->name : "")) continue;
        
        glm::dvec3 bodyPos = body->worldPos;
        double dist = glm::length(playerPos - bodyPos);
        
        if (dist < minDist) {
            minDist = dist;
            closest = body;
        }
    }
    
    return closest;
}

void PlanetarySystem::updatePlayerOnPlanet() {
    if (!player) return;
    
    auto closestPlanet = getClosestPlanet();
    if (!closestPlanet) return;
    
    glm::dvec3 playerPos = player->getPosition();
    glm::dvec3 planetPos = closestPlanet->worldPos;
    glm::dvec3 planetToPlayer = playerPos - planetPos;
    double distToPlanet = glm::length(planetToPlayer);
    
    double localSurfaceRadius = getSurfaceRadiusAtDirection(closestPlanet, planetToPlayer);
    if (distToPlanet < localSurfaceRadius + 100.0) {
        glm::dvec3 n = (distToPlanet > 1e-9)
            ? (planetToPlayer / distToPlanet)
            : glm::dvec3(0.0, 1.0, 0.0);
        glm::dvec3 surfacePos = planetPos + n * (localSurfaceRadius + kPlayerCollisionRadiusKm + kGroundEpsilonKm);
        player->setPosition(surfacePos);
    }
}

// ========== PHYSICS SYSTEM ==========

double PlanetarySystem::calculateGravityAtPosition(const glm::dvec3& worldPos, glm::dvec3& gravityDirection) {
    double totalAcceleration = 0.0;
    gravityDirection = glm::dvec3(0.0);
    
    // Calcular gravedad de cada cuerpo celeste
    for (const auto& bodyName : bodyNames) {
        auto body = findBody(bodyName);
        if (!body) continue;
        
        glm::dvec3 bodyWorldPos = body->worldPos;
        glm::dvec3 toBody = bodyWorldPos - worldPos;
        double distance = glm::length(toBody);
        
        // No aplicar gravedad si estamos dentro del planeta
        if (distance < body->radius * Units::KM) {
            distance = body->radius * Units::KM;
        }
        
        // F = G * M / r² (distance está en km -> convertir a metros)
        double distanceMeters = distance * 1000.0;
        double accelerationMps2 = (G * body->mass) / (distanceMeters * distanceMeters);
        double acceleration = accelerationMps2 / 1000.0; // km/s²
        
        // Acumular dirección ponderada
        if (distance > 0) {
            gravityDirection += glm::normalize(toBody) * acceleration;
            totalAcceleration += acceleration;
        }
    }
    
    return glm::length(gravityDirection);
}

void PlanetarySystem::applyPlanetaryPhysics(double dt) {
    if (!player || !worldSystem) return;
    
    // Si el jugador está en modo vuelo/nave, desactivar gravedad
    if (player->isInFlightMode()) {
        return;
    }
    
    glm::dvec3 playerPos = player->getPosition();
    glm::dvec3 currentVelocity = player->getVelocity();

    // Saneado defensivo contra NaN/Inf
    auto finite3 = [](const glm::dvec3& v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
    if (!finite3(playerPos)) {
        playerPos = glm::dvec3(0.0, 6373.0, 0.0);
        player->setPosition(playerPos);
    }
    if (!finite3(currentVelocity)) {
        currentVelocity = glm::dvec3(0.0);
        player->setVelocity(currentVelocity);
    }

    glm::dvec3 gravityDir;
    double gravityMagnitude = calculateGravityAtPosition(playerPos, gravityDir);

    if (gravityMagnitude > 1e-12 && glm::length(gravityDir) > 1e-12) {
        player->setUpDirection(-glm::normalize(gravityDir));
    }
    
    // Aplicar gravedad al jugador
    
    // Velocidad terminal realista (50 m/s = 0.05 km/s)
    double currentSpeed = glm::length(currentVelocity);
    
    // La gravedad del jugador debe usar tiempo real; timeScale queda para órbitas/simulación general.
    glm::dvec3 gravityAcceleration = gravityDir * gravityMagnitude * dt;
    
    currentVelocity += gravityAcceleration;
    currentSpeed = glm::length(currentVelocity);
    if (currentSpeed > kTerminalFallSpeedKmS && currentSpeed > 1e-9) {
        currentVelocity = glm::normalize(currentVelocity) * kTerminalFallSpeedKmS;
    }
    
    player->setVelocity(currentVelocity);

    // Integración con subpasos para evitar tunneling
    int substeps = std::clamp((int)std::ceil((glm::length(currentVelocity) * dt) / 0.01), 1, 8);
    double subDt = dt / static_cast<double>(substeps);

    for (int i = 0; i < substeps; ++i) {
        playerPos += currentVelocity * subDt;

        auto closestPlanet = getClosestPlanet();
        if (closestPlanet) {
            glm::dvec3 planetPos = closestPlanet->worldPos;
            glm::dvec3 planetToPlayer = playerPos - planetPos;
            double distToPlanet = glm::length(planetToPlayer);
            double surfaceDistance = getSurfaceRadiusAtDirection(closestPlanet, planetToPlayer)
                                   + kPlayerCollisionRadiusKm
                                   + kGroundEpsilonKm;

            if (distToPlanet < surfaceDistance) {
                glm::dvec3 n = (distToPlanet > 1e-9)
                    ? (planetToPlayer / distToPlanet)
                    : glm::dvec3(0.0, 1.0, 0.0);

                playerPos = planetPos + n * surfaceDistance;

                // Quitar componente hacia adentro
                double inwardSpeed = glm::dot(currentVelocity, -n);
                if (inwardSpeed > 0.0) {
                    currentVelocity += n * inwardSpeed;
                }

                // Fricción tangencial
                glm::dvec3 tangentialVelocity = currentVelocity - n * glm::dot(currentVelocity, n);
                currentVelocity = tangentialVelocity * 0.98 + n * glm::max(0.0, glm::dot(currentVelocity, n));
            }
        }
    }

    player->setPosition(playerPos);
    player->setVelocity(currentVelocity);
}

void PlanetarySystem::setPlayerFlightMode(bool enabled) {
    if (player) {
        player->setFlightMode(enabled);
    }
}

std::unordered_map<std::string, PlanetarySystem::PlanetData> planetDataMap;
std::unordered_map<std::string, std::map<PlanetarySystem::ChunkConfig, PlanetarySystem::ChunkData>> chunkDataMap;

void PlanetarySystem::generatePlanet(const PlanetConfig& config, const std::string& bodyName) {
    auto data = generatePlanetInternal(config);
    planetDataMap[bodyName] = std::move(data);
    setDetailedSurfaceData(bodyName, planetDataMap[bodyName]);
}

PlanetarySystem::ChunkData PlanetarySystem::generateChunk(const PlanetConfig& config, const ChunkConfig& chunk, const std::string& bodyName) {
    auto data = generateChunkInternal(config, chunk);
    chunkDataMap[bodyName][chunk] = data;
    return data;
}

const PlanetarySystem::PlanetData* PlanetarySystem::getPlanetData(const std::string& bodyName) const {
    auto it = planetDataMap.find(bodyName);
    return (it != planetDataMap.end()) ? &it->second : nullptr;
}

const PlanetarySystem::ChunkData* PlanetarySystem::getChunkData(const std::string& bodyName, const ChunkConfig& chunk) const {
    auto it = chunkDataMap.find(bodyName);
    if (it != chunkDataMap.end()) {
        auto jt = it->second.find(chunk);
        if (jt != it->second.end()) return &jt->second;
    }
    return nullptr;
}

// Implementación interna de generación procedural (debes portar aquí la lógica de PlanetGenerator)
PlanetarySystem::PlanetData PlanetarySystem::generatePlanetInternal(const PlanetConfig& config) {
    // --- Generación de icosfera base ---
    struct Icosphere {
        std::vector<glm::vec3> vertices;
        std::vector<unsigned int> indices;
        std::map<std::pair<unsigned int, unsigned int>, unsigned int> midpointCache;

        unsigned int addVertex(const glm::vec3& v) {
            vertices.push_back(glm::normalize(v));
            return (unsigned int)vertices.size() - 1;
        }

        unsigned int getMidpoint(unsigned int i0, unsigned int i1) {
            auto key = std::minmax(i0, i1);
            auto it = midpointCache.find(key);
            if (it != midpointCache.end()) return it->second;
            glm::vec3 mid = glm::normalize((vertices[i0] + vertices[i1]) * 0.5f);
            unsigned int idx = addVertex(mid);
            midpointCache[key] = idx;
            return idx;
        }

        void create(int subdivisions) {
            static const float X = 0.525731112119133606f;
            static const float Z = 0.850650808352039932f;
            static const glm::vec3 vdata[12] = {
                {-X, 0, Z}, {X, 0, Z}, {-X, 0, -Z}, {X, 0, -Z},
                {0, Z, X}, {0, Z, -X}, {0, -Z, X}, {0, -Z, -X},
                {Z, X, 0}, {-Z, X, 0}, {Z, -X, 0}, {-Z, -X, 0}
            };
            static const unsigned int tindices[60] = {
                0,4,1, 0,9,4, 9,5,4, 4,5,8, 4,8,1,
                8,10,1, 8,3,10, 5,3,8, 5,2,3, 2,7,3,
                7,10,3, 7,6,10, 7,11,6, 11,0,6, 0,1,6,
                6,1,10, 9,0,11, 9,11,2, 9,2,5, 7,2,11
            };
            vertices.clear(); indices.clear(); midpointCache.clear();
            for (int i = 0; i < 12; ++i) addVertex(vdata[i]);
            for (int i = 0; i < 60; ++i) indices.push_back(tindices[i]);
            for (int s = 0; s < subdivisions; ++s) {
                std::vector<unsigned int> newIndices;
                for (size_t i = 0; i + 2 < indices.size(); i += 3) {
                    unsigned int i0 = indices[i], i1 = indices[i+1], i2 = indices[i+2];
                    unsigned int a = getMidpoint(i0, i1);
                    unsigned int b = getMidpoint(i1, i2);
                    unsigned int c = getMidpoint(i2, i0);
                    newIndices.insert(newIndices.end(), {i0, a, c, i1, b, a, i2, c, b, a, b, c});
                }
                indices = std::move(newIndices);
            }
        }
    };

    Icosphere icosphere;
    int subdiv = std::clamp(config.subdivisions, 0, 7);
    icosphere.create(subdiv);

    // --- Deformación procedural (portada del editor) ---
    struct LayeredDeformParams {
        float reliefKm;
        float baseRadiusKm;
        float plateReliefKm;
        float plateScale;
        float plateBoundarySharpness;
        float minContinentSizeKm;
        float continentFrequency;
        float detailFrequency;
        int planetSeed;
        bool enableContinents;
        bool enableMountains;
    };

    auto fBmHash = [](const glm::vec3& p, int seed, int octaves, float persistence, float lacunarity, float frequency) {
        float amp = 1.0f, total = 0.0f, norm = 0.0f;
        glm::vec3 pp = p * frequency;
        for (int i = 0; i < octaves; ++i) {
            float n = glm::dot(pp, glm::vec3(12.9898f + (seed + i*31) * 0.001f, 78.233f + (seed + i*31) * 0.002f, 37.719f + (seed + i*31) * 0.003f));
            float h = (std::sin(n) * 43758.5453f - std::floor(std::sin(n) * 43758.5453f)) * 2.0f - 1.0f;
            total += h * amp;
            norm += amp;
            amp *= persistence;
            pp *= lacunarity;
        }
        return norm > 1e-6f ? (total / norm) : 0.0f;
    };

    auto smoothstepf = [](float edge0, float edge1, float x) {
        float t = std::clamp((x - edge0) / std::max(1e-6f, edge1 - edge0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };

    auto computePlateDelta = [&](const glm::vec3& dir, int planetSeed, float plateScale, float plateBoundarySharpness, float plateReliefRatio) {
        float pA = fBmHash(dir, planetSeed + 401, 2, 0.55f, 2.0f, std::max(0.01f, plateScale));
        float pB = fBmHash(dir, planetSeed + 457, 2, 0.50f, 2.0f, std::max(0.01f, plateScale * 1.7f));
        float pSigned = pA * 0.70f + pB * 0.30f;
        float p01 = (pSigned + 1.0f) * 0.5f;
        float boundary = 1.0f - std::abs(2.0f * p01 - 1.0f);
        float bSharp = std::clamp(plateBoundarySharpness, 0.05f, 0.95f);
        float ridgeMask = smoothstepf(1.0f - bSharp, 1.0f, boundary);
        return (pSigned * 0.65f + ridgeMask * 0.35f) * plateReliefRatio;
    };

    LayeredDeformParams params{
        8.0f, // reliefKm
        config.baseRadiusKm,
        2.0f, // plateReliefKm
        1.0f, // plateScale
        0.5f, // plateBoundarySharpness
        200.0f, // minContinentSizeKm
        config.continentFrequency,
        config.detailFrequency,
        config.seedBase,
        config.enableContinents,
        config.enableMountains
    };

    // Deformación
    std::vector<glm::vec3> verts = icosphere.vertices;
    for (auto& v : verts) {
        glm::vec3 dir = glm::normalize(v);
        float baseDelta = computePlateDelta(dir, params.planetSeed, params.plateScale, params.plateBoundarySharpness, 0.1f);
        float newLen = 1.0f + baseDelta;
        v = dir * newLen;
    }

    // Escalado a radio real
    for (auto& v : verts) v *= config.radius;

    // Normales suaves
    std::vector<glm::vec3> norms(verts.size(), glm::vec3(0.0f));
    for (size_t i = 0; i + 2 < icosphere.indices.size(); i += 3) {
        unsigned int i0 = icosphere.indices[i + 0];
        unsigned int i1 = icosphere.indices[i + 1];
        unsigned int i2 = icosphere.indices[i + 2];
        const glm::vec3 e1 = verts[i1] - verts[i0];
        const glm::vec3 e2 = verts[i2] - verts[i0];
        glm::vec3 fn = glm::cross(e1, e2);
        norms[i0] += fn;
        norms[i1] += fn;
        norms[i2] += fn;
    }
    for (size_t i = 0; i < norms.size(); ++i) {
        float lenSq = glm::dot(norms[i], norms[i]);
        if (lenSq > 1e-20f) norms[i] = glm::normalize(norms[i]);
        else norms[i] = glm::normalize(verts[i]);
    }

    PlanetData out;
    out.vertices = std::move(verts);
    out.normals = std::move(norms);
    out.indices = icosphere.indices;
    out.radius = config.radius;
    return out;
}

PlanetarySystem::ChunkData PlanetarySystem::generateChunkInternal(const PlanetConfig& config, const ChunkConfig& chunk) {
    // Generación procedural mínima de un patch esférico para el chunk
    ChunkData out;
    // Costura perfecta: grid global por cara
    int tiles = std::max(1, chunk.tilesPerFace);
    int vertsPerTile = 8; // subdivisión por patch
    int vertsPerFace = tiles * vertsPerTile + 1;
    int face = chunk.face;
    int tileX = chunk.tileX;
    int tileY = chunk.tileY;
    // Coordenadas del patch en el grid global
    int startX = tileX * vertsPerTile;
    int startY = tileY * vertsPerTile;
    // Generar vértices del patch usando el grid global
    for (int y = 0; y <= vertsPerTile; ++y) {
        for (int x = 0; x <= vertsPerTile; ++x) {
            int gx = startX + x;
            int gy = startY + y;
            float fx = (float(gx) / float(vertsPerFace - 1)) * 2.0f - 1.0f;
            float fy = (float(gy) / float(vertsPerFace - 1)) * 2.0f - 1.0f;
            glm::vec3 cube;
            switch (face) {
                case 0: cube = glm::vec3( 1.0,  fy, -fx); break;
                case 1: cube = glm::vec3(-1.0,  fy,  fx); break;
                case 2: cube = glm::vec3( fx,  1.0, -fy); break;
                case 3: cube = glm::vec3( fx, -1.0,  fy); break;
                case 4: cube = glm::vec3( fx,  fy,  1.0); break;
                default:cube = glm::vec3(-fx,  fy, -1.0); break;
            }
            glm::vec3 sphere = glm::normalize(cube) * config.radius;
            out.vertices.push_back(sphere);
            out.normals.push_back(glm::normalize(sphere));
        }
    }
    // Triangulación
    for (int y = 0; y < vertsPerTile; ++y) {
        for (int x = 0; x < vertsPerTile; ++x) {
            int i0 = y * (vertsPerTile + 1) + x;
            int i1 = i0 + 1;
            int i2 = i0 + (vertsPerTile + 1);
            int i3 = i2 + 1;
            out.indices.push_back(i0);
            out.indices.push_back(i2);
            out.indices.push_back(i1);
            out.indices.push_back(i1);
            out.indices.push_back(i2);
            out.indices.push_back(i3);
        }
    }
    return out;
}

} // namespace Haruka