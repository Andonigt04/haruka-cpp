#pragma once
#include "event_manager.h"
#include <string>
#include <nlohmann/json.hpp>

namespace Haruka {


// Event for any object change (create, update, delete, etc.)
struct ObjectEvent : public Event {
    enum class ActionType {
        Created,
        Updated,
        Deleted,
        Selected,
        Duplicated,
        Reparented,
        Custom
    };
    std::string objectName;
    std::string objectType;
    ActionType action;
    nlohmann::json data; // Extra info (properties, diffs, etc.)
    ObjectEvent(const std::string& name, const std::string& type, ActionType act, const nlohmann::json& d = {})
        : objectName(name), objectType(type), action(act), data(d) {}
};

// Event for log messages
struct LogEvent : public Event {
    enum class Level {
        Info,
        Warning,
        Error
    };
    Level level;
    std::string message;
    LogEvent(Level lvl, const std::string& msg) : level(lvl), message(msg) {}
};

} // namespace Haruka
