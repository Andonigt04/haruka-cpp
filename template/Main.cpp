#include "Haruka/Core/Application.h"
#include "Haruka/Core/GameInterface.h"
#include <filesystem>

extern "C" Haruka::GameInterface* getGameInterface();

int main() {
    auto exeDir = std::filesystem::read_symlink("/proc/self/exe").parent_path();
    std::filesystem::current_path(exeDir);

    Application app;
    app.setGameInterface(getGameInterface());
    app.run("scenes/main.scene");
    return 0;
}
