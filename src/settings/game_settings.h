#pragma once

namespace Haruka::Settings {

enum class TextureQuality : int { Low = 0, Medium = 1, High = 2, Ultra = 3 };
enum class ShadowQuality  : int { Off = 0, Low = 1, Medium = 2, High = 3 };
enum class AntialiasingMode : int { None = 0, FXAA = 1, TAA = 2 };

struct GraphicsSettings {
    TextureQuality  textureQuality  = TextureQuality::High;
    ShadowQuality   shadowQuality   = ShadowQuality::Medium;
    AntialiasingMode antialiasing   = AntialiasingMode::TAA;
    float           fov             = 90.0f;
    float           renderScale     = 1.0f;
    bool            vsync           = false;
    bool            ssao            = true;
    bool            bloom           = true;
    bool            motionBlur      = false;
};

struct AudioSettings {
    float masterVolume = 1.0f;
    float musicVolume  = 0.6f;
    float sfxVolume    = 1.0f;
};

} // namespace Haruka::Settings
