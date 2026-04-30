#pragma once

#include <string>
#include <iostream>
#include <sstream>
#include <chrono>

/**
 * @brief Structured error reporting utility.
 *
 * Captures subsystem, code, message, source location, and optional trace data.
 */

/** @brief Engine subsystem that produced an error. */
enum class ErrorComponent {
    MOTOR = 0,           // Core engine
    EDITOR = 1,          // Editor IDE
    GAMEPLAY = 2,        // Game logic
    RENDERER = 3,        // Graphics rendering
    NETWORK = 4,         // Networking
    PHYSICS = 5,         // Physics engine
    AUDIO = 6,           // Audio system
    IO = 7,              // Input/Output
    SCENE = 8,           // Scene management
    ASSET_LOADER = 9,    // Asset loading
    UNKNOWN = 255
};

/** @brief Stable numeric error code range grouped by subsystem. */
enum class ErrorCode {
    // Motor/Core (000-099)
    MOTOR_INIT_FAILED = 0,
    WINDOW_CREATION_FAILED = 1,
    OPENGL_INIT_FAILED = 2,
    SHADER_COMPILATION_FAILED = 3,
    MOTOR_LIBRARY = 4,

    // Editor (100-199)
    EDITOR_INIT_FAILED = 100,
    PROJECT_LOAD_FAILED = 101,
    SCENE_SAVE_FAILED = 102,
    INVALID_PROJECT = 103,
    INVALID_OBJECT_SELECTED = 104,
    FAILED_TO_LOAD_FILE = 105,
    FAILED_TO_READ_FILE = 106,
    FAILED_TO_SAVE_FILE = 107,
    FAILED_TO_DELETE_FILE = 108,
    FAILED_TO_MODIFY_FILE = 109,
    FAILED_TO_LOAD_MATERIAL = 110,
    
    // Gameplay (200-299)
    GAME_LOGIC_ERROR = 200,
    INVALID_SCENE = 201,
    OBJECT_CREATION_FAILED = 202,
    COMPONENT_INIT_FAILED = 203,
    PROJECT_COMPILATION_FAIL = 204,
    
    // Renderer (300-399)
    RENDER_TARGET_FAILED = 300,
    TEXTURE_LOAD_FAILED = 301,
    MODEL_LOAD_FAILED = 302,
    SHADER_UNIFORM_MISSING = 303,
    
    // Network (400-499)
    SOCKET_CREATION_FAILED = 400,
    CONNECTION_TIMEOUT = 401,
    INVALID_MESSAGE = 402,
    SEND_FAILED = 403,
    
    // Physics (500-599)
    PHYSICS_INIT_FAILED = 500,
    COLLISION_CALC_FAILED = 501,
    
    // Audio (600-699)
    AUDIO_INIT_FAILED = 600,
    AUDIO_LOAD_FAILED = 601,
    
    // IO (700-799)
    FILE_NOT_FOUND = 700,
    FILE_READ_ERROR = 701,
    FILE_WRITE_ERROR = 702,
    PERMISSION_DENIED = 703,
    THREAD_ERROR = 704,
    OUT_OF_MEMORY = 705,
    
    // Scene (800-899)
    SCENE_PARSE_ERROR = 800,
    INVALID_OBJECT = 801,
    
    // Asset Loader (900-999)
    ASSET_NOT_FOUND = 900,
    ASSET_FORMAT_INVALID = 901,
    ASSET_CORRUPTED = 902,
    ASSET_STREAMER_INIT_FAILED = 903,
    ASSET_LOAD_TIMEOUT = 904,
    ASSET_CACHE_FULL = 905,
    
    // Unknown
    UNKNOWN_ERROR = 9999
};

/** @brief Serialized error payload used by `ErrorReporter`. */
struct ErrorInfo {
    ErrorComponent component;
    ErrorCode code;
    std::string message;
    std::string file;
    int line;
    std::string function;
    std::string timestamp;
    std::string stackTrace;

    /** @brief Formats the error as a human-readable string. */
    std::string toString() const;
    /** @brief Returns the component name string. */
    std::string getComponentName() const;
    /** @brief Returns the error code name string. */
    std::string getErrorName() const;
};

/**
 * @brief Singleton error reporter with stderr logging.
 */
class ErrorReporter {
public:
    static ErrorReporter& getInstance() {
        static ErrorReporter instance;
        return instance;
    }

    /** @brief Reports an error and stores it as the last error. */
    static void report(
        ErrorComponent component,
        ErrorCode code,
        const std::string& message,
        const std::string& file = "",
        int line = 0,
        const std::string& function = ""
    ) {
        ErrorInfo error;
        error.component = component;
        error.code = code;
        error.message = message;
        error.file = file;
        error.line = line;
        error.function = function;
        error.timestamp = getCurrentTimestamp();
        
        getInstance().logError(error);
    }

    /** @brief Returns the last reported error snapshot. */
    ErrorInfo getLastError() const { return lastError; }

    /** @brief Clears the last error snapshot. */
    void clearError() {
        lastError = {};
    }

    /** @brief Returns true when an error is currently stored. */
    bool hasError() const { return lastError.code != ErrorCode::UNKNOWN_ERROR; }

private:
    ErrorReporter() = default;

    ErrorInfo lastError;

    void logError(const ErrorInfo& error) {
        lastError = error;

        // Log a stderr en formato estructurado
        std::cerr << "\n" << std::string(70, '=') << "\n";
        std::cerr << "❌ ERROR REPORT\n";
        std::cerr << std::string(70, '=') << "\n";
        std::cerr << error.toString();
        std::cerr << std::string(70, '=') << "\n\n";
    }

    static std::string getCurrentTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::ctime(&time);
        return ss.str();
    }
};

    // Convenience macros for source-location capture
#define HARUKA_ERROR(component, code, message) \
    ErrorReporter::report(component, code, message, __FILE__, __LINE__, __FUNCTION__)

#define HARUKA_MOTOR_ERROR(code, message) \
    ErrorReporter::report(ErrorComponent::MOTOR, code, message, __FILE__, __LINE__, __FUNCTION__)

#define HARUKA_EDITOR_ERROR(code, message) \
    ErrorReporter::report(ErrorComponent::EDITOR, code, message, __FILE__, __LINE__, __FUNCTION__)

#define HARUKA_GAMEPLAY_ERROR(code, message) \
    ErrorReporter::report(ErrorComponent::GAMEPLAY, code, message, __FILE__, __LINE__, __FUNCTION__)

#define HARUKA_RENDERER_ERROR(code, message) \
    ErrorReporter::report(ErrorComponent::RENDERER, code, message, __FILE__, __LINE__, __FUNCTION__)

#define HARUKA_NETWORK_ERROR(code, message) \
    ErrorReporter::report(ErrorComponent::NETWORK, code, message, __FILE__, __LINE__, __FUNCTION__)

#define HARUKA_PHYSICS_ERROR(code, message) \
    ErrorReporter::report(ErrorComponent::PHYSICS, code, message, __FILE__, __LINE__, __FUNCTION__)

#define HARUKA_AUDIO_ERROR(code, message) \
    ErrorReporter::report(ErrorComponent::AUDIO, code, message, __FILE__, __LINE__, __FUNCTION__)

#define HARUKA_IO_ERROR(code, message) \
    ErrorReporter::report(ErrorComponent::IO, code, message, __FILE__, __LINE__, __FUNCTION__)

#define HARUKA_SCENE_ERROR(code, message) \
    ErrorReporter::report(ErrorComponent::SCENE, code, message, __FILE__, __LINE__, __FUNCTION__)

#define HARUKA_ASSET_ERROR(code, message) \
    ErrorReporter::report(ErrorComponent::ASSET_LOADER, code, message, __FILE__, __LINE__, __FUNCTION__)
