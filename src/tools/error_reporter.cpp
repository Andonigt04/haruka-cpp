#include "error_reporter.h"

std::string ErrorInfo::getComponentName() const {
    switch (component) {
        case ErrorComponent::MOTOR: return "MOTOR";
        case ErrorComponent::EDITOR: return "EDITOR";
        case ErrorComponent::GAMEPLAY: return "GAMEPLAY";
        case ErrorComponent::RENDERER: return "RENDERER";
        case ErrorComponent::NETWORK: return "NETWORK";
        case ErrorComponent::PHYSICS: return "PHYSICS";
        case ErrorComponent::AUDIO: return "AUDIO";
        case ErrorComponent::IO: return "I/O";
        case ErrorComponent::SCENE: return "SCENE";
        case ErrorComponent::ASSET_LOADER: return "ASSET_LOADER";
        default: return "UNKNOWN";
    }
}

std::string ErrorInfo::getErrorName() const {
    switch (code) {
        // Motor
        case ErrorCode::MOTOR_INIT_FAILED: return "MOTOR_INIT_FAILED";
        case ErrorCode::WINDOW_CREATION_FAILED: return "WINDOW_CREATION_FAILED";
        case ErrorCode::OPENGL_INIT_FAILED: return "OPENGL_INIT_FAILED";
        case ErrorCode::SHADER_COMPILATION_FAILED: return "SHADER_COMPILATION_FAILED";

        // Editor
        case ErrorCode::EDITOR_INIT_FAILED: return "EDITOR_INIT_FAILED";
        case ErrorCode::PROJECT_LOAD_FAILED: return "PROJECT_LOAD_FAILED";
        case ErrorCode::SCENE_SAVE_FAILED: return "SCENE_SAVE_FAILED";
        case ErrorCode::INVALID_PROJECT: return "INVALID_PROJECT";

        // Gameplay
        case ErrorCode::GAME_LOGIC_ERROR: return "GAME_LOGIC_ERROR";
        case ErrorCode::INVALID_SCENE: return "INVALID_SCENE";
        case ErrorCode::OBJECT_CREATION_FAILED: return "OBJECT_CREATION_FAILED";
        case ErrorCode::COMPONENT_INIT_FAILED: return "COMPONENT_INIT_FAILED";

        // Renderer
        case ErrorCode::RENDER_TARGET_FAILED: return "RENDER_TARGET_FAILED";
        case ErrorCode::TEXTURE_LOAD_FAILED: return "TEXTURE_LOAD_FAILED";
        case ErrorCode::MODEL_LOAD_FAILED: return "MODEL_LOAD_FAILED";
        case ErrorCode::SHADER_UNIFORM_MISSING: return "SHADER_UNIFORM_MISSING";

        // Network
        case ErrorCode::SOCKET_CREATION_FAILED: return "SOCKET_CREATION_FAILED";
        case ErrorCode::CONNECTION_TIMEOUT: return "CONNECTION_TIMEOUT";
        case ErrorCode::INVALID_MESSAGE: return "INVALID_MESSAGE";
        case ErrorCode::SEND_FAILED: return "SEND_FAILED";

        // Physics
        case ErrorCode::PHYSICS_INIT_FAILED: return "PHYSICS_INIT_FAILED";
        case ErrorCode::COLLISION_CALC_FAILED: return "COLLISION_CALC_FAILED";

        // Audio
        case ErrorCode::AUDIO_INIT_FAILED: return "AUDIO_INIT_FAILED";
        case ErrorCode::AUDIO_LOAD_FAILED: return "AUDIO_LOAD_FAILED";

        // IO
        case ErrorCode::FILE_NOT_FOUND: return "FILE_NOT_FOUND";
        case ErrorCode::FILE_READ_ERROR: return "FILE_READ_ERROR";
        case ErrorCode::FILE_WRITE_ERROR: return "FILE_WRITE_ERROR";
        case ErrorCode::PERMISSION_DENIED: return "PERMISSION_DENIED";
        case ErrorCode::THREAD_ERROR: return "THREAD_ERROR";
        case ErrorCode::OUT_OF_MEMORY: return "OUT_OF_MEMORY";

        // Scene
        case ErrorCode::SCENE_PARSE_ERROR: return "SCENE_PARSE_ERROR";
        case ErrorCode::INVALID_OBJECT: return "INVALID_OBJECT";

        // Asset Loader
        case ErrorCode::ASSET_NOT_FOUND: return "ASSET_NOT_FOUND";
        case ErrorCode::ASSET_FORMAT_INVALID: return "ASSET_FORMAT_INVALID";
        case ErrorCode::ASSET_CORRUPTED: return "ASSET_CORRUPTED";
        case ErrorCode::ASSET_STREAMER_INIT_FAILED: return "ASSET_STREAMER_INIT_FAILED";
        case ErrorCode::ASSET_LOAD_TIMEOUT: return "ASSET_LOAD_TIMEOUT";
        case ErrorCode::ASSET_CACHE_FULL: return "ASSET_CACHE_FULL";

        default: return "UNKNOWN_ERROR";
    }
}

std::string ErrorInfo::toString() const {
    std::stringstream ss;
    ss << "\nComponent:  " << getComponentName() << "\n";
    ss << "Error Code: " << static_cast<int>(code) << " (" << getErrorName() << ")\n";
    ss << "Message:    " << message << "\n";
    
    if (!file.empty()) {
        ss << "Location:   " << file << ":" << line;
        if (!function.empty()) {
            ss << " in " << function;
        }
        ss << "\n";
    }
    
    ss << "Timestamp:  " << timestamp;
    
    if (!stackTrace.empty()) {
        ss << "Stack Trace:\n" << stackTrace;
    }
    
    return ss.str();
}
