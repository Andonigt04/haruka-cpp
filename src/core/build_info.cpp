#include "core/build_info.h"

#include "version.h"   // GENERADO por cmake/version_gen.cmake en cada compilacion

namespace Haruka {

const char* versionString() { return HARUKA_VERSION; }

const std::string& buildString() {
    // Se arma una vez: los tres trozos son constantes de compilacion.
    static const std::string s =
        std::string(HARUKA_VERSION) + "+" + HARUKA_BUILD_NUMBER + "." + HARUKA_BUILD_HASH;
    return s;
}

} // namespace Haruka
