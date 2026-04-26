#ifndef PROJECT_H
#define PROJECT_H

#include <string>
#include <vector>

namespace Haruka
{
    /**
     * @brief Persistent project configuration contract.
     */
    struct ProjectConfig
    {
        // Base project metadata
        std::string name;
        std::string version;
        std::string engineVersion;
        std::string startScene;
        std::string engineBinary;
        std::string editorBinary;
        std::vector<std::string> scenes;
        std::string assetsPath;
        std::string outputPath;
        std::string shadersPath = "";
        
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

#endif