#pragma once
#include "settings/game_settings.h"
#include "input/input_action.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace Haruka {

class SettingsManager {
public:
    static SettingsManager& get();

    void init(const std::string& iniPath = "game_settings.ini");

    // -- Action registration --------------------------------------------------

    // Bool button — backward-compatible, wraps keys in a DirectBinding.
    void registerAction(const std::string& name,
                        const std::string& displayName,
                        const std::string& group,
                        std::initializer_list<SDL_Scancode> defaults);

    // Typed binding — pass Axis2DBinding, Axis1DBinding, or DirectBinding.
    void registerAction(const std::string& name,
                        const std::string& displayName,
                        const std::string& group,
                        Input::BindingSource source);

    // -- Per-frame update -----------------------------------------------------

    void update(const bool* keyboardState);

    // -- Value query ----------------------------------------------------------

    // Raw variant value — use readValue<T>() for a typed result.
    const Input::ActionValue& readValue(const std::string& action) const;

    // Typed read. Returns default T{} if the action is unknown or type mismatches.
    template<typename T>
    T readValue(const std::string& action) const {
        const auto& v = readValue(action);
        if (const T* p = std::get_if<T>(&v)) return *p;
        return T{};
    }

    // -- Phase queries (Unity-style) ------------------------------------------

    bool isPerformed (const std::string& action) const; // active this frame
    bool isStarted   (const std::string& action) const; // became active this frame
    bool isCanceled  (const std::string& action) const; // became inactive this frame

    // Legacy aliases
    bool isHeld       (const std::string& action) const { return isPerformed(action); }
    bool justPressed  (const std::string& action) const { return isStarted(action);   }
    bool justReleased (const std::string& action) const { return isCanceled(action);  }

    // -- ActionMap groups -----------------------------------------------------

    // Disabled groups return inactive values until re-enabled.
    void enableGroup   (const std::string& group);
    void disableGroup  (const std::string& group);
    bool isGroupEnabled(const std::string& group) const;

    // -- Rebinding ------------------------------------------------------------

    void setBindings   (const std::string& action, std::vector<SDL_Scancode> keys);
    void addBinding    (const std::string& action, SDL_Scancode key);
    void removeBinding (const std::string& action, SDL_Scancode key);

    // -- Data access ----------------------------------------------------------

    Input::InputAction*       findAction(const std::string& name);
    const Input::InputAction* findAction(const std::string& name) const;

    const std::vector<Input::InputAction>& actions() const { return m_actions; }
    std::vector<const Input::InputAction*> actionsInGroup(const std::string& group) const;
    std::vector<std::string> groups() const;

    Settings::GraphicsSettings& graphics() { return m_graphics; }
    Settings::AudioSettings&    audio()    { return m_audio;    }
    const Settings::GraphicsSettings& graphics() const { return m_graphics; }
    const Settings::AudioSettings&    audio()    const { return m_audio;    }

    void save();

private:
    SettingsManager() = default;

    void registerImGuiHandlers();

    std::vector<Input::InputAction>  m_actions;
    std::unordered_set<std::string>  m_disabledGroups;

    static const Input::ActionValue  s_nullValue;

    Settings::GraphicsSettings  m_graphics;
    Settings::AudioSettings     m_audio;
    std::string                 m_iniPath;
};

} // namespace Haruka
