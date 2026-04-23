#include "core/application.h"
#include "core/json.hpp"
#include <iostream>
#include <fstream>
#include <stdexcept>

int main() {
    Application app;

    std::string startScenePath = "";
    // Leer project.hrk si existe
    std::ifstream f("project.hrk");
    if (f.is_open()) {
        nlohmann::json j;
        f >> j;
        if (j.contains("startScene")) {
            startScenePath = j["startScene"].get<std::string>();
        }
    }

    try {
        app.run(startScenePath);
    } catch (const std::exception& e) {
        std::cerr << "Excepcion capturada: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}