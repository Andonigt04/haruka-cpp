#include "core/application.h"
#include "core/json.hpp"
#include "core/error_reporter.h"
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
        HARUKA_MOTOR_ERROR(ErrorCode::MOTOR_INIT_FAILED,
            std::string("Uncaught exception: ") + e.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}