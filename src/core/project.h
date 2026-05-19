/**
 * @file project.h
 * @brief `Project` manager and `ProjectConfig` serialization contract.
 *
 * A project groups scenes, assets, and export settings into a single
 * persistent unit. The config is serialized to/from a JSON file on disk.
 * `Project::create()` generates the initial directory skeleton;
 * `Project::load()` / `Project::save()` handle round-trips.
 */
#pragma once

#include <string>
#include <vector>

namespace Haruka
{
    /**
     * @brief Persistent project configuration contract.
     */
    struct ProjectConfig
    {
        std::string name;            ///< Human-readable project name
        std::string version;         ///< Project version string
        std::string engineVersion;   ///< Engine version this project targets
        std::string startScene;      ///< Relative path to the default startup scene
        std::string engineBinary;    ///< Path to the runtime engine binary
        std::string editorBinary;    ///< Path to the editor binary
        std::vector<std::string> scenes; ///< All scene paths registered in the project
        std::string assetsPath;      ///< Root assets directory
        std::string outputPath;      ///< Build output root
        std::string shadersPath = ""; ///< Override shader directory (empty = default)
        
        // Export configuration persisted per project
        struct ExportSettings {
            std::string author = "Developer";
            std::string description = "";
            
            // Build defaults
            std::string defaultBuildType = "Release";  // Debug, Release, Shipping
            std::string defaultPlatform = "Linux";      // Linux, Windows, macOS
            
            // Compilation options
            bool includeDebugSymbols = false;
            bool optimizeAssets = true;
            bool stripUnusedContent = true;
            bool compressAssets = true;
            
            // Custom paths
            std::string customBuildOutputPath = "";
            std::string customLaunchScript = "";
        } exportSettings;
    };

    class Project
    {
    public:
        Project();
        ~Project();

        /** @brief Creates a project skeleton in target path. */
        bool create(const std::string& path, const std::string& name);
        /** @brief Loads project configuration from disk. */
        bool load(const std::string& path);
        /** @brief Saves current project configuration. */
        bool save();

        Haruka::ProjectConfig& getConfig() { return config; }
        const std::string& getPath() const { return projectPath; }

        /** @brief Adds scene entry to project config list. */
        bool addScene(const std::string& sceneName);
        /** @brief Removes scene entry from project config list. */
        bool removeScene(const std::string& sceneName);

    private:
        Haruka::ProjectConfig config;
        std::string projectPath;

        /** @brief Parses JSON project file. */
        bool loadFromJSON(const std::string& filepath);
        /** @brief Writes JSON project file. */
        bool saveToJSON(const std::string& filepath);
    };

}