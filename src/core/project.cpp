#include "project.h"
#include "tools/error_reporter.h"

#include <fstream>
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace Haruka
{
    Project::Project() {}
    Project::~Project() {}

    bool Project::create(const std::string& path, const std::string& name) {
        projectPath = path;
        config = ProjectConfig{};
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

        if (std::find(config.scenes.begin(), config.scenes.end(), scenePath) == config.scenes.end()) {
            config.scenes.push_back(scenePath);
        }
        return save();
    }

    bool Project::removeScene(const std::string& sceneName) {
        std::string scenePath = sceneName;
        if (scenePath.find(".scene") == std::string::npos) {
            scenePath += ".scene";
        }

        auto it = std::find(config.scenes.begin(), config.scenes.end(), scenePath);
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
        try {
            nlohmann::json data;
            file >> data;

            config.name = data.value("name", config.name);
            config.version = data.value("version", config.version);
            config.engineVersion = data.value("engineVersion", config.engineVersion);
            config.startScene = data.value("startScene", config.startScene);
            config.engineBinary = data.value("engineBinary", config.engineBinary);
            config.editorBinary = data.value("editorBinary", config.editorBinary);
            config.assetsPath = data.value("assetsPath", config.assetsPath);
            config.outputPath = data.value("outputPath", config.outputPath);
            config.shadersPath = data.value("shadersPath", config.shadersPath);

            if (data.contains("scenes") && data["scenes"].is_array()) {
                config.scenes.clear();
                for (const auto& sceneEntry : data["scenes"]) {
                    if (sceneEntry.is_string()) {
                        config.scenes.push_back(sceneEntry.get<std::string>());
                    }
                }
            }

            if (data.contains("exportSettings") && data["exportSettings"].is_object()) {
                const auto& exportData = data["exportSettings"];
                config.exportSettings.author = exportData.value("author", config.exportSettings.author);
                config.exportSettings.description = exportData.value("description", config.exportSettings.description);
                config.exportSettings.defaultBuildType = exportData.value("defaultBuildType", config.exportSettings.defaultBuildType);
                config.exportSettings.defaultPlatform = exportData.value("defaultPlatform", config.exportSettings.defaultPlatform);
                config.exportSettings.includeDebugSymbols = exportData.value("includeDebugSymbols", config.exportSettings.includeDebugSymbols);
                config.exportSettings.optimizeAssets = exportData.value("optimizeAssets", config.exportSettings.optimizeAssets);
                config.exportSettings.stripUnusedContent = exportData.value("stripUnusedContent", config.exportSettings.stripUnusedContent);
                config.exportSettings.compressAssets = exportData.value("compressAssets", config.exportSettings.compressAssets);
                config.exportSettings.customBuildOutputPath = exportData.value("customBuildOutputPath", config.exportSettings.customBuildOutputPath);
                config.exportSettings.customLaunchScript = exportData.value("customLaunchScript", config.exportSettings.customLaunchScript);
            }
        } catch (const std::exception& e) {
            HARUKA_MOTOR_ERROR(ErrorCode::INVALID_PROJECT, std::string("Invalid project file: ") + e.what());
            return false;
        }

        return true;
    }

    bool Project::saveToJSON(const std::string& filepath) {
        std::ofstream file(filepath);
        if (!file.is_open()) {
            HARUKA_MOTOR_ERROR(ErrorCode::FILE_WRITE_ERROR, std::string("Cannot create project file: ") + filepath);
            return false;
        }

        nlohmann::json data;
        data["name"] = config.name;
        data["version"] = config.version;
        data["engineVersion"] = config.engineVersion;
        data["startScene"] = config.startScene;
        data["engineBinary"] = config.engineBinary;
        data["editorBinary"] = config.editorBinary;
        data["scenes"] = config.scenes;
        data["assetsPath"] = config.assetsPath;
        data["outputPath"] = config.outputPath;
        data["shadersPath"] = config.shadersPath;
        data["exportSettings"] = {
            {"author", config.exportSettings.author},
            {"description", config.exportSettings.description},
            {"defaultBuildType", config.exportSettings.defaultBuildType},
            {"defaultPlatform", config.exportSettings.defaultPlatform},
            {"includeDebugSymbols", config.exportSettings.includeDebugSymbols},
            {"optimizeAssets", config.exportSettings.optimizeAssets},
            {"stripUnusedContent", config.exportSettings.stripUnusedContent},
            {"compressAssets", config.exportSettings.compressAssets},
            {"customBuildOutputPath", config.exportSettings.customBuildOutputPath},
            {"customLaunchScript", config.exportSettings.customLaunchScript}
        };

        file << data.dump(2) << '\n';
        return true;
    }
}