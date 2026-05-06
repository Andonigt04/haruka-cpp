#include "planetary_system.h"

namespace Haruka {

PlanetarySystem::PlanetarySystem() {}
PlanetarySystem::~PlanetarySystem() {}

void PlanetarySystem::init() {
    // Inicializamos los subsistemas una sola vez
    m_cache = std::make_unique<ChunkCache>(512); // 512MB de caché
    m_generator = std::make_unique<TerrainGenerator>();
    m_renderer = std::make_unique<TerrainRenderer>();
    
    // El streaming necesita a los otros tres
    m_streaming = std::make_unique<TerrainStreamingSystem>(*m_cache, *m_generator, *m_renderer);
    
    // El LOD es el que toma decisiones
    m_lod = std::make_unique<LODSystem>(1.5, 12);
}

void PlanetarySystem::update(double dt, const glm::dvec3& cameraPos) {
    m_simulationTime += dt;

    // 1. Mover los planetas en sus órbitas
    updateOrbits(dt);

    // 2. Para cada planeta, actualizar su terreno
    for (auto& planet : m_planets) {
        // Creamos un objeto ficticio para el LODSystem (o pasamos los datos directamente)
        auto planetProxy = std::make_shared<SceneObject>();
        planetProxy->name = planet.name;
        planetProxy->position = planet.position;
        planetProxy->scale = glm::vec3(planet.radius);

        // A. ¿Qué chunks deben verse?
        LODUpdate update = m_lod->updatePlanetLOD(planetProxy, cameraPos);
        
        // B. El streaming se encarga de cargar/descargar de la GPU
        m_streaming->processLODUpdate(update);
    }

    // 3. Recoger chunks terminados por los hilos y subirlos a la GPU
    auto readyChunks = m_streaming->getReadyChunks();
    for (auto& chunk : readyChunks) {
        // Buscamos el planeta al que pertenece el chunk para pasarle sus settings si fuera necesario
        m_renderer->addToScene(chunk->planetName, chunk->key, *chunk);
    }

    // 4. Renderizar todo el sistema planetario
    m_renderer->render(cameraPos);
}

void PlanetarySystem::updateOrbits(double dt) {
    // Aquí mueves las posiciones de m_planets usando Kepler o integración Euler
    // Por ahora, podrías dejarlos estáticos o con una rotación simple
}

void PlanetarySystem::addPlanet(const Planet& planet) {
    m_planets.push_back(planet);
}

}