#include "ui/settings_panel.h"
#include <vector>
#include <algorithm>
#include "settings/settings_manager.h"
#include "input/gamepad.h"
#include <cmath>
#include <variant>
#include "rhi/rhi_device.h"   // enumerar GPUs para el combo de tarjeta gráfica
#include "renderer/motor_instance.h"
#include "core/application.h"
#include "core/locale.h"
#include "core/modules.h"
#ifdef HARUKA_MOD_AUDIO
#include "audio/audio_manager.h"
#endif

#include <imgui.h>
#include <SDL3/SDL.h>
#include <cstring>
#include <filesystem>
#include <string>

namespace Haruka::UI {

void SettingsPanel::beginRebind(const std::string& action, int slotIndex, bool pad, bool padAxis,
                                bool padGroup, bool chain) {
    m_rebindingAction = action;
    m_rebindIndex     = slotIndex;
    m_rebindPad       = pad;
    m_rebindPadAxis   = padAxis;
    m_rebindPadGroup  = padGroup;
    m_rebindChain     = chain;
    m_waitingForKey   = true;
    SettingsManager::get().setCapturing(true); // freeze all actions until a key lands
}

void SettingsPanel::cancelRebind() {
    m_rebindingAction.clear();
    m_rebindIndex   = -1;
    m_rebindPad = m_rebindPadAxis = m_rebindPadGroup = m_rebindChain = false;
    m_waitingForKey = false;
    SettingsManager::get().setCapturing(false);
}

static const char* texQualityNames[]   = { "Low", "Medium", "High", "Ultra" };
static const char* shadowQualNames[]   = { "Off",  "Low",   "Medium", "High" };
static const char* aaNames[]           = { "None", "FXAA",  "TAA" };
static const char* waterQualityNames[] = { "Low", "Medium", "High", "Ultra" };
static const char* windowModeNames[]   = { "Windowed", "Borderless", "Fullscreen" };
static const char* renderBackendNames[] = { "OpenGL", "Vulkan" };

bool SettingsPanel::render() {
    auto& sm = SettingsManager::get();
    bool close = false;

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(580, 480), ImGuiCond_Always);

    if (!ImGui::Begin((TR("settings.title") + "##panel").c_str(), nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus))
    {
        ImGui::End();
        return false;
    }

    if (ImGui::BeginTabBar("##settingsTabs")) {
        if (ImGui::BeginTabItem(TR("settings.graphics").c_str())) { tabGraphics(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(TR("settings.audio").c_str()))    { tabAudio();    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(TR("settings.language").c_str())) { tabLanguage(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(TR("settings.controls").c_str())) { tabControls(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button(TR("ui.saveClose").c_str(), ImVec2(140, 0))) {
        sm.save();
        if (auto* app = MotorInstance::getInstance().getApplication())
            app->applyGraphicsSettings();
        close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("ui.cancel").c_str(), ImVec2(90, 0)))
        close = true;

    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !m_waitingForKey)
        close = true;

    if (close) cancelRebind(); // also clears the capture flag

    ImGui::End();
    return close;
}

// ── Graphics tab ─────────────────────────────────────────────────────────────

void SettingsPanel::tabGraphics() {
    auto& g = SettingsManager::get().graphics();

    ImGui::SeparatorText(TR("gfx.performance").c_str());
    if (ImGui::Button(TR("gfx.presetLow").c_str())) {
        Settings::applyLowPreset(g);
        if (auto* app = MotorInstance::getInstance().getApplication())
            app->applyGraphicsSettings();
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("gfx.presetHigh").c_str())) {
        Settings::applyHighPreset(g);
        if (auto* app = MotorInstance::getInstance().getApplication())
            app->applyGraphicsSettings();
    }
    {
        int fps = g.maxFps;
        if (ImGui::SliderInt(TR("gfx.fpsCap").c_str(), &fps, 0, 240,
                             fps == 0 ? TR("gfx.uncapped").c_str() : "%d"))
            g.maxFps = fps;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("gfx.fpsCap.tip").c_str());
    }

    ImGui::SeparatorText(TR("gfx.rendering").c_str());

    // Modo de ventana: se aplica al instante (sin esperar a Guardar) para verlo en vivo.
    int wm = (int)g.windowMode;
    if (ImGui::Combo(TR("gfx.windowMode").c_str(), &wm, windowModeNames, 3)) {
        g.windowMode = (Settings::WindowMode)wm;
        if (auto* app = MotorInstance::getInstance().getApplication())
            app->applyGraphicsSettings();
    }

    // ── RESOLUCION ──────────────────────────────────────────────────────────────────────────────
    //
    // ⚠️ SIN ENTRADA "AUTO", A PROPOSITO. El combo lista SOLO resoluciones reales del monitor y el
    // defecto es la nativa, ya resuelta por `Application` en el primer arranque. Un "Auto" en la
    // lista parece cómodo y es peor: el que lo tiene puesto no sabe a que resolucion juega, y el que
    // elige una concreta no puede volver a la nativa sin adivinar cual era.
    //
    // La lista se construye UNA vez: `SDL_GetFullscreenDisplayModes` reserva, y rehacerlo cada frame
    // seria una llamada al driver por frame para un combo que casi nunca se abre.
    {
        struct Res { int w, h; };
        static std::vector<Res>         s_modes;
        static std::vector<std::string> s_labels;
        static bool                     s_built = false;
        if (!s_built) {
            s_built = true;
            // La primaria, no la de la ventana: `Application` no expone el `SDL_Window*` y anadir
            // un accesor para esto no compensa. En multimonitor la lista puede no ser la del monitor
            // donde esta la ventana; lo cubre el respaldo de mas abajo, que mete siempre la actual.
            const SDL_DisplayID disp = SDL_GetPrimaryDisplay();
            int n = 0;
            if (SDL_DisplayMode** dm = SDL_GetFullscreenDisplayModes(disp, &n)) {
                for (int i = 0; i < n; ++i) {
                    // Se DEDUPLICA por w×h: el monitor expone el mismo tamaño a varias frecuencias
                    // y a varias escalas, y sin esto la lista sale con 1920x1080 seis veces.
                    const Res r{ dm[i]->w, dm[i]->h };
                    bool dup = false;
                    for (const Res& e : s_modes) if (e.w == r.w && e.h == r.h) { dup = true; break; }
                    if (!dup) s_modes.push_back(r);
                }
                SDL_free(dm);
            }
            // Si el monitor no expone modos (Wayland sin fullscreen exclusivo, por ejemplo), al menos
            // la actual tiene que estar: si no, el combo saldria vacio y no se podria ni ver cual es.
            const auto& gg = SettingsManager::get().graphics();
            if (gg.resolutionW > 0) {
                bool has = false;
                for (const Res& e : s_modes) if (e.w == gg.resolutionW && e.h == gg.resolutionH) { has = true; break; }
                if (!has) s_modes.push_back({ gg.resolutionW, gg.resolutionH });
            }
            std::sort(s_modes.begin(), s_modes.end(),
                      [](const Res& a, const Res& b) { return a.w * a.h > b.w * b.h; });
            for (const Res& r : s_modes)
                s_labels.push_back(std::to_string(r.w) + " x " + std::to_string(r.h));
        }
        if (!s_modes.empty()) {
            int cur = 0;
            for (size_t i = 0; i < s_modes.size(); ++i)
                if (s_modes[i].w == g.resolutionW && s_modes[i].h == g.resolutionH) { cur = (int)i; break; }
            std::vector<const char*> items;
            items.reserve(s_labels.size());
            for (const std::string& l : s_labels) items.push_back(l.c_str());
            if (ImGui::Combo(TR("gfx.resolution").c_str(), &cur, items.data(), (int)items.size())) {
                g.resolutionW = s_modes[(size_t)cur].w;
                g.resolutionH = s_modes[(size_t)cur].h;
                if (auto* app = MotorInstance::getInstance().getApplication())
                    app->applyGraphicsSettings();
            }
        }
    }

    // API gráfica (RHI). El device se crea en el arranque → NO se aplica en vivo: requiere
    // reiniciar. Se persiste en el imgui.ini (RenderBackend) y Application::run lo lee al iniciar.
    int rb = (int)g.renderBackend;
    if (ImGui::Combo("Render API", &rb, renderBackendNames, 2))
        g.renderBackend = (Settings::RenderBackend)rb;
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Requiere reiniciar el juego.\nVulkan aún no implementado → cae a OpenGL automáticamente.");

    // ── TARJETA GRÁFICA ─────────────────────────────────────────────────────────────────────────
    //
    // La lista se pide UNA vez (`static`): enumerar levanta una instancia Vulkan temporal y esto
    // corre por frame mientras el panel esté abierto. Se pide para el backend SELECCIONADO, no para
    // el que está corriendo: si acabas de cambiar a Vulkan, lo que importa es qué GPUs tendrás al
    // reiniciar, no cuál usa el contexto GL actual.
    //
    // Se guarda el NOMBRE (ver `GraphicsSettings::preferredGpus`), nunca el índice de la lista.
    {
        static Settings::RenderBackend s_listedFor = (Settings::RenderBackend)-1;
        static std::vector<Haruka::RHI::Device::AdapterInfo> s_adapters;
        if (s_listedFor != g.renderBackend) {
            s_listedFor = g.renderBackend;
            s_adapters  = Haruka::RHI::Device::enumerateAdapters(
                g.renderBackend == Settings::RenderBackend::Vulkan
                    ? Haruka::RHI::Backend::Vulkan : Haruka::RHI::Backend::OpenGL,
                nullptr);
        }

        // ⚠️ AQUI HABIA UNA ENTRADA "Automática (dedicada si la hay)" Y SE QUITO A PROPOSITO.
        //
        // La automatica ya no es una opcion del desplegable: `Application` la resuelve al arrancar a
        // una tarjeta CONCRETA (la dedicada si la hay) y la escribe en el ajuste, asi que la lista
        // solo tiene tarjetas reales y una de ellas sale marcada. El comportamiento por defecto es el
        // mismo; lo que cambia es que ahora se VE cual es — que es justo lo que faltaba el dia que la
        // disparidad del terreno dependia de la GPU y el ajuste decia "Automática" en los dos casos.
        //
        // ⚠️ SE CONSULTA EN CADA USO, no se cachea antes del combo: el cuerpo del combo MUTA la lista,
        // y un puntero calculado arriba se queda colgando en cuanto el usuario elige. Esto ya reventó
        // una vez (`operator[]` sobre un vector vaciado dentro del propio combo).
        auto chosen = [&]() -> const std::string* {
            return (!g.preferredGpus.empty() && !g.preferredGpus[0].empty()) ? &g.preferredGpus[0]
                                                                             : nullptr;
        };
        // El preview se COPIA: `c_str()` de un elemento del vector colgaría si el cuerpo del combo
        // lo realoja mientras ImGui lo sigue usando.
        const std::string preview = chosen() ? *chosen() : std::string("(sin tarjetas)");
        if (ImGui::BeginCombo("Tarjeta gráfica", preview.c_str())) {
            for (size_t ai = 0; ai < s_adapters.size(); ++ai) {
                const auto& a = s_adapters[ai];
                const std::string* cur = chosen();
                const bool sel = cur && *cur == a.name;
                std::string label = a.name + (a.discrete ? "  [dedicada]" : "");
                // PushID por ÍNDICE: ImGui saca el id del texto, y dos tarjetas IGUALES (un multi-GPU
                // con dos placas del mismo modelo) darían el mismo nombre y por tanto el mismo id;
                // las dos filas se pisarían el estado y una no se podría elegir.
                ImGui::PushID((int)ai);
                if (ImGui::Selectable(label.c_str(), sel)) {
                    // Se escribe en el PUESTO 0 conservando el resto: la lista es de preferencia y
                    // un multi-GPU futuro poblará los siguientes puestos.
                    if (g.preferredGpus.empty()) g.preferredGpus.emplace_back();
                    g.preferredGpus[0] = a.name;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            // ⚠️ Decirlo, no fingirlo: en OpenGL elegir aquí NO tiene efecto. GL no expone selección
            // de adaptador — la GPU la fija el driver/SO antes de que el proceso arranque. Un
            // desplegable que parece funcionar y no hace nada es peor que uno que avisa.
            if (g.renderBackend == Settings::RenderBackend::Vulkan)
                ImGui::SetTooltip("Requiere reiniciar el juego.\n"
                                  "Se guarda el NOMBRE: si cambias de tarjeta o de equipo, vuelve a "
                                  "la automática en vez de apuntar a otra.\n"
                                  "HARUKA_VK_GPU=<nombre> tiene prioridad sobre esto.");
            else
                ImGui::SetTooltip("Requiere reiniciar el juego.\n"
                                  "OpenGL no permite elegir adaptador desde la API, así que el juego "
                                  "lo pide al arrancar por offload PRIME\n"
                                  "(__NV_PRIME_RENDER_OFFLOAD para NVIDIA, DRI_PRIME para Mesa). "
                                  "Depende del driver: puede no aplicarse.\n"
                                  "Con Vulkan la selección es directa y fiable.");
        }
    }

    int tq = (int)g.textureQuality;
    if (ImGui::Combo(TR("gfx.textureQuality").c_str(), &tq, texQualityNames, 4))
        g.textureQuality = (Settings::TextureQuality)tq;

    int sq = (int)g.shadowQuality;
    if (ImGui::Combo(TR("gfx.shadowQuality").c_str(), &sq, shadowQualNames, 4))
        g.shadowQuality = (Settings::ShadowQuality)sq;

    int tq2 = (int)g.terrainQuality;
    if (ImGui::Combo(TR("gfx.terrainQuality").c_str(), &tq2, waterQualityNames, 4))
        g.terrainQuality = (Settings::TerrainQuality)tq2;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("gfx.terrainQuality.tip").c_str());

    int wq = (int)g.waterQuality;
    if (ImGui::Combo(TR("gfx.waterQuality").c_str(), &wq, waterQualityNames, 4))
        g.waterQuality = (Settings::WaterQuality)wq;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("gfx.waterQuality.tip").c_str());

    int aa = (int)g.antialiasing;
    if (ImGui::Combo(TR("gfx.antialiasing").c_str(), &aa, aaNames, 3))
        g.antialiasing = (Settings::AntialiasingMode)aa;
    if (aa == (int)Settings::AntialiasingMode::TAA)
        ImGui::TextDisabled("  %s", TR("gfx.taaNote").c_str());

    ImGui::SliderFloat(TR("gfx.fov").c_str(), &g.fov, 60.0f, 120.0f, "%.0f°");
    ImGui::SliderFloat(TR("gfx.renderScale").c_str(), &g.renderScale, 0.5f, 2.0f, "%.2f");

    // 0 = Auto (presupuesto = 25% de la RAM del sistema, acotado). El format string muestra
    // "Auto" en 0 (sin %d) y "%d MB" en cualquier otro valor → el usuario puede forzarlo.
    ImGui::SliderInt(TR("gfx.chunkMemory").c_str(), &g.chunkMemoryMB, 0, 24576,
                     g.chunkMemoryMB == 0 ? "Auto" : "%d MB");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("gfx.chunkMemory.tip").c_str());

    ImGui::SeparatorText(TR("gfx.postProcessing").c_str());
    ImGui::Checkbox(TR("gfx.vsync").c_str(), &g.vsync);
    ImGui::Checkbox(TR("gfx.ssao").c_str(),  &g.ssao);
    if (g.ssao) ImGui::TextDisabled("  %s", TR("gfx.ssaoNote").c_str());
    // TONEMAPPING. Apagarlo RECORTA los valores altos a blanco: útil para diagnosticar qué se sale
    // de rango, desastroso sin querer (props quemados).
    ImGui::Checkbox(TR("gfx.hdr").c_str(), &g.hdr);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TR("gfx.hdr.tip").c_str());
    ImGui::Checkbox(TR("gfx.bloom").c_str(), &g.bloom);
    if (g.bloom) {
        ImGui::Indent();
        ImGui::SliderFloat(TR("gfx.bloomThreshold").c_str(), &g.bloomThreshold, 0.0f, 1.5f, "%.2f");
        ImGui::SliderFloat(TR("gfx.bloomStrength").c_str(),  &g.bloomStrength,  0.0f, 2.0f, "%.2f");
        ImGui::Unindent();
    }
    ImGui::Checkbox(TR("gfx.motionBlur").c_str(), &g.motionBlur);
    if (g.motionBlur) ImGui::TextDisabled("  %s", TR("gfx.motionBlurNote").c_str());
}

// ── Audio tab ────────────────────────────────────────────────────────────────
// Dispositivos = combo con los que enumera SDL3. Guarda el NOMBRE (vacío = predeterminado);
// el juego (voz/OpenAL) abre el dispositivo por nombre. Cambia en caliente al guardar.
static void deviceCombo(const char* label, bool recording, std::string& sel) {
    int count = 0;
    SDL_AudioDeviceID* ids = recording ? SDL_GetAudioRecordingDevices(&count)
                                       : SDL_GetAudioPlaybackDevices(&count);
    std::string def = TR("audio.defaultDevice");
    const char* preview = sel.empty() ? def.c_str() : sel.c_str();
    if (ImGui::BeginCombo(label, preview)) {
        if (ImGui::Selectable(def.c_str(), sel.empty())) sel.clear();
        for (int i = 0; i < count; ++i) {
            const char* name = SDL_GetAudioDeviceName(ids[i]);
            if (!name) continue;
            bool chosen = (sel == name);
            // PushID por índice: los nombres de dispositivo de audio SE REPITEN a menudo (dos placas
            // iguales, dos cascos USB del mismo modelo). Sin esto las dos filas comparten id, se pisan
            // el estado y una no se puede elegir — y ImGui avisa con "conflicting ID".
            ImGui::PushID(i);
            if (ImGui::Selectable(name, chosen)) sel = name;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (ids) SDL_free(ids);
}

void SettingsPanel::tabAudio() {
    auto& a = SettingsManager::get().audio();

    ImGui::SeparatorText(TR("audio.devices").c_str());
    // Entrada (micro): la enumera SDL (la captura de audio del motor lee de SDL).
    deviceCombo(TR("audio.input").c_str(),  /*recording*/true,  a.inputDevice);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("audio.input.tip").c_str());

    // Salida: la reproducción va por OpenAL → enumera con OpenAL (no SDL). Así sí aparecen
    // todos los dispositivos y elegir uno cambia a dónde suena (al reiniciar).
#ifdef HARUKA_MOD_AUDIO
    {
        std::string def = TR("audio.defaultDevice");
        const char* preview = a.outputDevice.empty() ? def.c_str() : a.outputDevice.c_str();
        if (ImGui::BeginCombo(TR("audio.output").c_str(), preview)) {
            if (ImGui::Selectable(def.c_str(), a.outputDevice.empty())) a.outputDevice.clear();
            const auto& devs = AudioManager::playbackDevices();
            for (size_t di = 0; di < devs.size(); ++di) {
                const std::string& d = devs[di];
                bool sel = (a.outputDevice == d);
                ImGui::PushID((int)di);      // mismo motivo que en deviceCombo: nombres repetidos
                if (ImGui::Selectable(d.c_str(), sel)) a.outputDevice = d;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }
#else
    deviceCombo(TR("audio.output").c_str(), /*recording*/false, a.outputDevice);
#endif
    ImGui::TextDisabled("%s", TR("audio.applyOnSave").c_str());

    ImGui::SeparatorText(TR("audio.volume").c_str());
    ImGui::SliderFloat(TR("audio.master").c_str(), &a.masterVolume, 0.0f, 1.0f);
    ImGui::SliderFloat(TR("audio.music").c_str(),  &a.musicVolume,  0.0f, 1.0f);
    ImGui::SliderFloat(TR("audio.sfx").c_str(),    &a.sfxVolume,    0.0f, 1.0f);
    ImGui::SliderFloat(TR("audio.ambient").c_str(), &a.ambientVolume, 0.0f, 1.0f);
}

// ── Language tab (i18n) ──────────────────────────────────────────────────────
// SOLO selección entre los idiomas INSTALADOS (los que tienen assets/lang/<c>.json). La
// gestión modular (instalar/quitar idiomas, voz y audio) se hace en el LAUNCHER, no aquí.
void SettingsPanel::tabLanguage() {
    auto& sm  = SettingsManager::get();
    auto& loc = Locale::get();

    static bool s_scanned = false;
    if (!s_scanned) { loc.scan(); s_scanned = true; }

    std::string curName = sm.language();
    for (const auto& l : loc.available()) if (l.code == sm.language()) curName = l.name;
    if (ImGui::BeginCombo(TR("language.active").c_str(), curName.c_str())) {
        for (const auto& l : loc.available()) {           // solo idiomas instalados
            bool sel = (l.code == sm.language());
            if (ImGui::Selectable((l.name + "  (" + l.code + ")").c_str(), sel)) {
                sm.language() = l.code;
                loc.load(l.code);   // aplica al instante (la UI usa TR())
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("%s", TR("language.managedInLauncher").c_str());
}

// ── Controls tab ─────────────────────────────────────────────────────────────

void SettingsPanel::tabControls() {
    auto& sm = SettingsManager::get();

    // ── EL MANDO: que el panel diga si SDL lo ve. "Los inputs no se leen" no se puede separar de
    // "no hay mando abierto" sin esto (un joystick sin mapeo de gamepad no aparece en
    // `SDL_GetGamepads`, y entonces no hay nada que leer).
    {
        Input::InputState probe;
        Input::Gamepad::get().poll(probe);
        if (Input::Gamepad::get().connected())
            ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f), "%s: %s", TR("ctrl.gamepad").c_str(),
                               Input::Gamepad::get().name().c_str());
        else
            ImGui::TextDisabled("%s", TR("ctrl.padNone").c_str());
    }

    // Esperando una tecla O un boton/eje del mando (segun el hueco pulsado). SettingsManager esta
    // en modo captura (beginRebind), asi que ninguna accion se dispara con lo que se pulse.
    if (m_waitingForKey) {
        {
            std::string what = m_rebindingAction;
            // En cadena (teclado de una accion de direccion) se dice QUE hueco se esta asignando.
            if (m_rebindChain && m_rebindIndex >= 0) {
                static const char* dir4[4] = { "ctrl.dirUp", "ctrl.dirDown", "ctrl.dirLeft", "ctrl.dirRight" };
                static const char* dir2[2] = { "ctrl.dirPlus", "ctrl.dirMinus" };
                if (auto* act = sm.findAction(m_rebindingAction)) {
                    const bool is2 = std::holds_alternative<Input::Axis2DBinding>(act->source);
                    const int  n   = is2 ? 4 : 2;
                    if (m_rebindIndex < n) what += std::string(" · ") + TR(is2 ? dir4[m_rebindIndex] : dir2[m_rebindIndex]);
                }
            }
            const char* key = m_rebindPad ? (m_rebindPadGroup ? "ctrl.pressPadGroup" : "ctrl.pressPad") : "ctrl.pressKey";
            ImGui::TextColored(ImVec4(1,1,0,1), TR(key).c_str(), what.c_str());
        }

        int numKeys = 0;
        const bool* kbd = SDL_GetKeyboardState(&numKeys);
        bool done = false;
        if (numKeys > SDL_SCANCODE_ESCAPE && kbd[SDL_SCANCODE_ESCAPE]) { cancelRebind(); done = true; }
        if (m_chainWaitRelease != 0 && numKeys > m_chainWaitRelease && !kbd[m_chainWaitRelease])
            m_chainWaitRelease = 0;
        if (!done && !m_rebindPad) {
            for (int i = 1; i < numKeys; ++i) {
                if (!kbd[i]) continue;
                if (i == (int)m_chainWaitRelease) continue;   // la tecla del hueco anterior, aun sin soltar
                SDL_Scancode sc = (SDL_Scancode)i;
                bool chainNext = false;
                if (m_rebindIndex >= 0) {
                    // Sustituye ESE hueco (vale para los ejes, cuyo numero de huecos es fijo).
                    if (auto* act = sm.findAction(m_rebindingAction)) {
                        auto keys = act->keys();
                        if (m_rebindIndex < (int)keys.size()) {
                            keys[m_rebindIndex] = sc;
                            sm.setBindings(m_rebindingAction, keys);
                            chainNext = m_rebindChain && m_rebindIndex + 1 < (int)keys.size();
                        }
                    }
                } else {
                    sm.addBinding(m_rebindingAction, sc); // anade (acciones Direct)
                }
                if (chainNext) {
                    // La misma tecla aun esta pulsada: el siguiente hueco se captura cuando se suelte
                    // y se pulse otra (ver `m_chainWaitRelease`).
                    const std::string a = m_rebindingAction; const int nx = m_rebindIndex + 1;
                    cancelRebind();
                    beginRebind(a, nx, false, false, false, true);
                    m_chainWaitRelease = (int)sc;
                } else {
                    cancelRebind();
                }
                done = true;
                break;
            }
        }
        if (!done && m_rebindPad) {
            Input::InputState in;
            Input::Gamepad::get().poll(in);
            if (auto* act = sm.findAction(m_rebindingAction); act && in.gamepad) {
                auto btns = act->padButtons();
                auto axes = act->padAxes();
                if (m_rebindPadGroup) {
                    // GRUPO: un stick entero (los dos ejes) o la cruceta entera (los 4 botones), con
                    // lo primero que se mueva o pulse.
                    for (int a = 0; a < (int)SDL_GAMEPAD_AXIS_COUNT && !done; ++a) {
                        if (std::fabs(in.axes[a]) < 0.6f) continue;
                        const bool left = (a == SDL_GAMEPAD_AXIS_LEFTX || a == SDL_GAMEPAD_AXIS_LEFTY);
                        const bool right = (a == SDL_GAMEPAD_AXIS_RIGHTX || a == SDL_GAMEPAD_AXIS_RIGHTY);
                        if (!left && !right) continue;          // un gatillo no es un stick
                        if (axes.size() >= 2) {
                            axes[0] = left ? SDL_GAMEPAD_AXIS_LEFTX : SDL_GAMEPAD_AXIS_RIGHTX;
                            axes[1] = left ? SDL_GAMEPAD_AXIS_LEFTY : SDL_GAMEPAD_AXIS_RIGHTY;
                            act->setPad(btns, axes);
                        }
                        cancelRebind(); done = true;
                    }
                    for (int b = 0; b < (int)SDL_GAMEPAD_BUTTON_COUNT && !done; ++b) {
                        if (!in.buttons[b]) continue;
                        const bool dpad = (b >= SDL_GAMEPAD_BUTTON_DPAD_UP && b <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
                        if (!dpad) continue;
                        if (btns.size() >= 4) {
                            btns[0] = SDL_GAMEPAD_BUTTON_DPAD_UP;   btns[1] = SDL_GAMEPAD_BUTTON_DPAD_DOWN;
                            btns[2] = SDL_GAMEPAD_BUTTON_DPAD_LEFT; btns[3] = SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
                            act->setPad(btns, axes);
                        }
                        cancelRebind(); done = true;
                    }
                }
                if (done) {
                } else if (m_rebindIndex < 0 && !m_rebindPadAxis) {
                    // "+" del mando: lo primero que llegue. Un boton se anade; un eje (stick a fondo
                    // o gatillo) ocupa el hueco de eje de la accion.
                    for (int a = 0; a < (int)SDL_GAMEPAD_AXIS_COUNT && !done; ++a) {
                        if (std::fabs(in.axes[a]) < 0.6f) continue;
                        if (!axes.empty()) axes[0] = (SDL_GamepadAxis)a;
                        act->setPad(btns, axes);
                        cancelRebind(); done = true;
                    }
                }
                if (done) {
                } else if (m_rebindPadAxis) {
                    // Un eje movido mas de 0,6 (stick a fondo o gatillo apretado) es el elegido.
                    for (int a = 0; a < (int)SDL_GAMEPAD_AXIS_COUNT; ++a) {
                        if (std::fabs(in.axes[a]) < 0.6f) continue;
                        if (m_rebindIndex >= 0 && m_rebindIndex < (int)axes.size()) axes[m_rebindIndex] = (SDL_GamepadAxis)a;
                        else if (!axes.empty()) axes[0] = (SDL_GamepadAxis)a;
                        act->setPad(btns, axes);
                        cancelRebind(); done = true;
                        break;
                    }
                } else {
                    for (int b = 0; b < (int)SDL_GAMEPAD_BUTTON_COUNT; ++b) {
                        if (!in.buttons[b]) continue;
                        if (m_rebindIndex >= 0 && m_rebindIndex < (int)btns.size()) btns[m_rebindIndex] = (SDL_GamepadButton)b;
                        else btns.push_back((SDL_GamepadButton)b);     // anade (acciones Direct)
                        act->setPad(btns, axes);
                        cancelRebind(); done = true;
                        break;
                    }
                }
            }
        }
        ImGui::Separator();
    }

    // Tabla: grupo → accion · teclado · mando. Las dos mitades de cada accion van en columnas
    // separadas (Andoni, 21-09: "que se dividan los controles"); antes solo habia teclas, y una
    // accion sin tecla (Mirar, solo mando) salia como cuatro botones vacios.
    for (const auto& group : sm.groups()) {
        ImGui::SeparatorText(group.c_str());

        if (!ImGui::BeginTable(("##kg_" + group).c_str(), 4,
                ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
            continue;

        ImGui::TableSetupColumn(TR("ctrl.action").c_str(),   ImGuiTableColumnFlags_WidthStretch, 0.25f);
        ImGui::TableSetupColumn(TR("ctrl.bindings").c_str(), ImGuiTableColumnFlags_WidthStretch, 0.35f);
        ImGui::TableSetupColumn(TR("ctrl.gamepad").c_str(),  ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed,   52.0f);
        ImGui::TableHeadersRow();

        for (const auto* action : sm.actionsInGroup(group)) {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(action->displayName.c_str());

            // ⚠️ Acotado por ACCION, no solo por indice. Las tablas de ImGui NO meten la fila en la
            // pila de ids, asi que dos acciones con la misma tecla en la misma posicion daban el
            // mismo id y pulsar un chip reasignaba el de la otra fila.
            ImGui::PushID(action->name.c_str());

            // ── Teclado ─────────────────────────────────────────────────────────────────────
            ImGui::TableSetColumnIndex(1);
            auto keys = action->keys();
            const bool positional = !std::holds_alternative<Input::DirectBinding>(action->source);
            bool anyKey = false; for (auto k : keys) anyKey = anyKey || (k != SDL_SCANCODE_UNKNOWN);
            ImGui::PushID("kbd");
            if (positional && !anyKey) {
                // Una accion de direccion SIN teclas: un solo chip. Al pulsarlo se asignan en cadena
                // (arriba, abajo, izquierda, derecha), no cuatro huecos vacios que parecen cuatro inputs.
                ImGui::SmallButton("-");
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) beginRebind(action->name, 0, false, false, false, true);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("ctrl.chainTip").c_str());
                keys.clear();
            }
            for (size_t i = 0; i < keys.size(); ++i) {
                // Hueco vacio = "-" (el guion largo no esta en la fuente por defecto: salia "?").
                const char* kname = (keys[i] == SDL_SCANCODE_UNKNOWN) ? "-" : SDL_GetScancodeName(keys[i]);
                ImGui::PushID((int)i);
                ImGui::SmallButton((kname && *kname) ? kname : "-");
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                    beginRebind(action->name, (int)i);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                    sm.removeBinding(action->name, keys[i]);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", TR("ctrl.chipTip").c_str());
                ImGui::PopID();
                if (i + 1 < keys.size()) ImGui::SameLine(0, 4);
            }
            ImGui::PopID();

            // ── Mando: botones (por hueco) y ejes ───────────────────────────────────────────
            ImGui::TableSetColumnIndex(2);
            const auto btns = action->padButtons();
            const auto axes = action->padAxes();
            // En las acciones de BOTON (Direct) los huecos no son posicionales: se ensenan solo los
            // asignados y un "+" para capturar otro (boton o eje, lo que llegue). En las de eje
            // (1D/2D) cada hueco significa algo (arriba/abajo/..., X/Y) y se ensena vacio con "-".
            ImGui::PushID("pad");
            bool first = true;
            // Un STICK es UN input y la CRUCETA otro: se ensenan como un chip cada uno (Andoni,
            // 21-09: "derecha izquierda solo deberia ser uno"). Solo los ejes/botones sueltos que no
            // forman grupo salen uno a uno.
            const bool is2D = std::holds_alternative<Input::Axis2DBinding>(action->source);
            bool stickShown = false, dpadShown = false;
            if (is2D && axes.size() >= 2) {
                const bool left  = axes[0] == SDL_GAMEPAD_AXIS_LEFTX  && axes[1] == SDL_GAMEPAD_AXIS_LEFTY;
                const bool right = axes[0] == SDL_GAMEPAD_AXIS_RIGHTX && axes[1] == SDL_GAMEPAD_AXIS_RIGHTY;
                const bool none  = axes[0] == SDL_GAMEPAD_AXIS_INVALID && axes[1] == SDL_GAMEPAD_AXIS_INVALID;
                if (left || right || none) {
                    ImGui::PushID("stick");
                    ImGui::SmallButton(TR(left ? "ctrl.stickL" : right ? "ctrl.stickR" : "ctrl.stickNone").c_str());
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) beginRebind(action->name, 0, true, true, true);
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !none) {
                        auto a2 = axes; a2[0] = a2[1] = SDL_GAMEPAD_AXIS_INVALID;
                        if (auto* act = sm.findAction(action->name)) act->setPad(btns, a2);
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("ctrl.stickTip").c_str());
                    ImGui::PopID();
                    first = false; stickShown = true;
                }
            }
            if (is2D && btns.size() >= 4) {
                const bool dpad = btns[0] == SDL_GAMEPAD_BUTTON_DPAD_UP && btns[1] == SDL_GAMEPAD_BUTTON_DPAD_DOWN
                               && btns[2] == SDL_GAMEPAD_BUTTON_DPAD_LEFT && btns[3] == SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
                bool noneB = true; for (auto b : btns) noneB = noneB && (b == SDL_GAMEPAD_BUTTON_INVALID);
                if (dpad || noneB) {
                    ImGui::PushID("dpad");
                    if (!first) ImGui::SameLine(0, 4);
                    ImGui::SmallButton(TR(dpad ? "ctrl.dpad" : "ctrl.dpadNone").c_str());
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) beginRebind(action->name, 0, true, false, true);
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && dpad) {
                        auto b2 = btns; for (auto& b : b2) b = SDL_GAMEPAD_BUTTON_INVALID;
                        if (auto* act = sm.findAction(action->name)) act->setPad(b2, axes);
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("ctrl.dpadTip").c_str());
                    ImGui::PopID();
                    first = false; dpadShown = true;
                }
            }
            for (size_t i = 0; i < btns.size(); ++i) {
                if (dpadShown) break;
                const char* n = Input::Gamepad::buttonName(btns[i]);
                if (!positional && !(n && *n)) continue;
                ImGui::PushID((int)i);
                if (!first) ImGui::SameLine(0, 4);
                first = false;
                ImGui::SmallButton((n && *n) ? n : "-");
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                    beginRebind(action->name, (int)i, true, false);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    auto b2 = btns; b2[i] = SDL_GAMEPAD_BUTTON_INVALID;
                    if (auto* act = sm.findAction(action->name)) act->setPad(b2, axes);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("ctrl.padChipTip").c_str());
                ImGui::PopID();
            }
            for (size_t i = 0; i < axes.size(); ++i) {
                if (stickShown) break;
                const char* n = Input::Gamepad::axisName(axes[i]);
                if (!positional && !(n && *n)) continue;
                ImGui::PushID(100 + (int)i);
                if (!first) ImGui::SameLine(0, 4);
                first = false;
                // Los ejes se distinguen de los botones por el prefijo: "~leftx".
                const std::string label = std::string("~") + ((n && *n) ? n : "-");
                ImGui::SmallButton(label.c_str());
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                    beginRebind(action->name, (int)i, true, true);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    auto a2 = axes; a2[i] = SDL_GAMEPAD_AXIS_INVALID;
                    if (auto* act = sm.findAction(action->name)) act->setPad(btns, a2);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("ctrl.padAxisTip").c_str());
                ImGui::PopID();
            }
            if (!positional) {
                if (!first) ImGui::SameLine(0, 4);
                if (ImGui::SmallButton("+")) beginRebind(action->name, -1, true, false);   // boton O eje: lo que llegue
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("ctrl.padAddTip").c_str());
            } else {
                // INVERTIR: por eje, y vale para teclas y mando (es el valor final el que cambia de
                // signo). Andoni (21-09): "poder invertir los controles".
                bool ix = false, iy = false; action->invert(ix, iy);
                ImGui::SameLine(0, 10);
                if (ImGui::Checkbox(is2D ? "X" : TR("ctrl.invert").c_str(), &ix))
                    if (auto* act = sm.findAction(action->name)) act->setInvert(ix, iy);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("ctrl.invertTip").c_str());
                if (is2D) {
                    ImGui::SameLine(0, 4);
                    if (ImGui::Checkbox("Y", &iy))
                        if (auto* act = sm.findAction(action->name)) act->setInvert(ix, iy);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("ctrl.invertTip").c_str());
                }
            }
            ImGui::PopID();

            // ── Anadir tecla / limpiar teclas: cortos, con tooltip (la tabla se salia del panel) ──
            ImGui::TableSetColumnIndex(3);
            if (ImGui::SmallButton("+")) beginRebind(action->name, -1); // anade una tecla (acciones Direct)
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("ctrl.add").c_str());
            ImGui::SameLine(0, 6);
            if (ImGui::SmallButton("x")) sm.setBindings(action->name, {});
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("ctrl.clear").c_str());

            ImGui::PopID();
        }

        ImGui::EndTable();
    }
}

} // namespace Haruka::UI
