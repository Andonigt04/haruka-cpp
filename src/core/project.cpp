#include "project.h"
#include "tools/error_reporter.h"

#include <fstream>
#include <filesystem>
#include <iostream>
#include <algorithm>

namespace fs = std::filesystem;

namespace Haruka
{
    Project::Project() {}
    Project::~Project() {}

    bool Project::create(const std::string& path, const std::string& name) {
        projectPath = path;
        config.name = name;
        config.version = "0.1";
        config.engineVersion = "0.1";
        config.startScene = "main";
        config.assetsPath = "assets/";
        config.outputPath = "build/";
        // Detectar rutas del motor y editor
        char absPath[4096];
        std::string engineBin, editorBin;
        if (realpath((path + "/../../build/HarukaEngine").c_str(), absPath)) {
            engineBin = absPath;
        } else {
            engineBin = "HarukaEngine"; // Solo nombre, buscar en PATH
        }
        if (realpath((path + "/../../build/HarukaEditor").c_str(), absPath)) {
            editorBin = absPath;
        } else {
            editorBin = "HarukaEditor";
        }
        config.engineBinary = engineBin;
        config.editorBinary = editorBin;
        try {
            // Crear estructura de carpetas
            namespace fs = std::filesystem;
            fs::create_directories(path);
            fs::create_directories(path + "/scenes");
            fs::create_directories(path + "/assets/models");
            fs::create_directories(path + "/assets/textures");
            fs::create_directories(path + "/assets/scripts");
            fs::create_directories(path + "/build");
            config.scenes.push_back("main.scene");
            bool ok = saveToJSON(path + "/project.hrk");
            std::cout << "Project created at: " << path << std::endl;
            return ok;
        } catch (const std::exception& e) {
            HARUKA_MOTOR_ERROR(ErrorCode::GAME_LOGIC_ERROR, std::string("Error creating project: ") + e.what());
            return false;
        }
    }

    bool Project::load(const std::string& path) {
        projectPath = path;
        return loadFromJSON(path + "/project.hrk");
    }

    bool Project::save() {
        return saveToJSON(projectPath + "/project.hrk");
    }

    bool Project::addScene(const std::string& sceneName) {
        std::string scenePath = sceneName;
        if (scenePath.find(".scene") == std::string::npos) {
            scenePath += ".scene";
        }
        
        config.scenes.push_back(scenePath);
        return save();
    }

    bool Project::removeScene(const std::string& sceneName) {
        auto it = std::find(config.scenes.begin(), config.scenes.end(), sceneName);
        if (it != config.scenes.end()) {
            config.scenes.erase(it);
            return save();
        }
        return false;
    }

    bool Project::loadFromJSON(const std::string& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            HARUKA_MOTOR_ERROR(ErrorCode::FILE_NOT_FOUND, std::string("Cannot open project file: ") + filepath);
            return false;
        }
        
        // JSON parsing simple (manual)
        std::string line;
        while (std::getline(file, line)) {
            // Parse name
            if (line.find("\"name\"") != std::string::npos) {
                size_t start = line.find(":") + 1;
                size_t firstQuote = line.find("\"", start) + 1;
                size_t lastQuote = line.find("\"", firstQuote);
                config.name = line.substr(firstQuote, lastQuote - firstQuote);
            }
            // Parse version
            else if (line.find("\"version\"") != std::string::npos) {
                size_t start = line.find(":") + 1;
                size_t firstQuote = line.find("\"", start) + 1;
                size_t lastQuote = line.find("\"", firstQuote);
                config.version = line.substr(firstQuote, lastQuote - firstQuote);
            }
            // Parse startScene
            else if (line.find("\"startScene\"") != std::string::npos) {
                size_t start = line.find(":") + 1;
                size_t firstQuote = line.find("\"", start) + 1;
                size_t lastQuote = line.find("\"", firstQuote);
                config.startScene = line.substr(firstQuote, lastQuote - firstQuote);
            }
            else if (line.find("\"shadersPath\"") != std::string::npos) {
                size_t start = line.find(":") + 1;
                size_t firstQuote = line.find("\"", start) + 1;
                size_t lastQuote = line.find("\"", firstQuote);
                config.shadersPath = line.substr(firstQuote, lastQuote - firstQuote);
            }
            else if (line.find("\"engineBinary\"") != std::string::npos) {
                size_t start = line.find(":") + 1;
                size_t firstQuote = line.find("\"", start) + 1;
                size_t lastQuote = line.find("\"", firstQuote);
                config.engineBinary = line.substr(firstQuote, lastQuote - firstQuote);
            }
            else if (line.find("\"editorBinary\"") != std::string::npos) {
                size_t start = line.find(":") + 1;
                size_t firstQuote = line.find("\"", start) + 1;
                size_t lastQuote = line.find("\"", firstQuote);
                config.editorBinary = line.substr(firstQuote, lastQuote - firstQuote);
            }
            else if (line.find("\"assetsPath\"") != std::string::npos) {
                size_t start = line.find(":") + 1;
                size_t firstQuote = line.find("\"", start) + 1;
                size_t lastQuote = line.find("\"", firstQuote);
                config.assetsPath = line.substr(firstQuote, lastQuote - firstQuote);
            }
            // Parse outputPath
            else if (line.find("\"outputPath\"") != std::string::npos) {
                size_t start = line.find(":") + 1;
                size_t firstQuote = line.find("\"", start) + 1;
                size_t lastQuote = line.find("\"", firstQuote);
                config.outputPath = line.substr(firstQuote, lastQuote - firstQuote);
            }
            // Parse export settings
            else if (line.find("\"author\"") != std::string::npos) {
                size_t start = line.find(":") + 1;
                size_t firstQuote = line.find("\"", start) + 1;
                size_t lastQuote = line.find("\"", firstQuote);
                config.exportSettings.author = line.substr(firstQuote, lastQuote - firstQuote);
            }
            else if (line.find("\"description\"") != std::string::npos) {
                size_t start = line.find(":") + 1;
                size_t firstQuote = line.find("\"", start) + 1;
                size_t lastQuote = line.find("\"", firstQuote);
                config.exportSettings.description = line.substr(firstQuote, lastQuote - firstQuote);
            }
            else if (line.find("\"defaultBuildType\"") != std::string::npos) {
                size_t start = line.find(":") + 1;
                size_t firstQuote = line.find("\"", start) + 1;
                size_t lastQuote = line.find("\"", firstQuote);
                config.exportSettings.defaultBuildType = line.substr(firstQuote, lastQuote - firstQuote);
            }
            else if (line.find("\"defaultPlatform\"") != std::string::npos) {
                size_t start = line.find(":") + 1;
                size_t firstQuote = line.find("\"", start) + 1;
                size_t lastQuote = line.find("\"", firstQuote);
                config.exportSettings.defaultPlatform = line.substr(firstQuote, lastQuote - firstQuote);
            }
        }
        
        file.close();
        return true;
    }

    bool Project::saveToJSON(const std::string& filepath) {
        std::ofstream file(filepath);
        if (!file.is_open()) {
            HARUKA_MOTOR_ERROR(ErrorCode::FILE_WRITE_ERROR, std::string("Cannot create project file: ") + filepath);
            return false;
        }
        // Escribir JSON manual
        file << "{\n";
        file << "  \"name\": \"" << config.name << "\",\n";
        file << "  \"version\": \"" << config.version << "\",\n";
        file << "  \"engineVersion\": \"" << config.engineVersion << "\",\n";
        file << "  \"startScene\": \"" << config.startScene << "\",\n";
        file << "  \"engineBinary\": \"" << config.engineBinary << "\",\n";
        file << "  \"editorBinary\": \"" << config.editorBinary << "\",\n";
        file << "  \"scenes\": [\n";
        for (size_t i = 0; i < config.scenes.size(); i++) {
            file << "    \"" << config.scenes[i] << "\"";
            if (i < config.scenes.size() - 1) file << ",";
            file << "\n";
        }
        file << "  ],\n";
        file << "  \"assetsPath\": \"" << config.assetsPath << "\",\n";
        file << "  \"outputPath\": \"" << config.outputPath << "\",\n";
        file << "  \"shadersPath\": \"" << config.shadersPath << "\",\n";
        file << "  \"exportSettings\": {\n";
        file << "    \"author\": \"" << config.exportSettings.author << "\",\n";
        file << "    \"description\": \"" << config.exportSettings.description << "\",\n";
        file << "    \"defaultBuildType\": \"" << config.exportSettings.defaultBuildType << "\",\n";
        file << "    \"defaultPlatform\": \"" << config.exportSettings.defaultPlatform << "\",\n";
        file << "    \"includeDebugSymbols\": " << (config.exportSettings.includeDebugSymbols ? "true" : "false") << ",\n";
        file << "    \"optimizeAssets\": " << (config.exportSettings.optimizeAssets ? "true" : "false") << ",\n";
        file << "    \"stripUnusedContent\": " << (config.exportSettings.stripUnusedContent ? "true" : "false") << ",\n";
        file << "    \"compressAssets\": " << (config.exportSettings.compressAssets ? "true" : "false") << "\n";
        file << "  }\n";
        file << "}\n";
        file.close();
        return true;
    }
}