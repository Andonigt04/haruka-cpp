#pragma once
/**
 * @file core/build_info.h
 * @brief Identidad del BINARIO que se esta ejecutando: version, build y commit.
 *
 * Existe para que un reporte de fallo identifique el build exacto. Sin esto, "no se ven los iconos"
 * no dice contra que codigo se probo, y un arreglo no se puede confirmar ni descartar.
 *
 * ⚠️ La declaracion vive aqui y NO en el `version.h` generado, a proposito: si cada fichero que
 * quiere pintar la version incluyera el generado, un commit cambiaria el hash y recompilaria todo lo
 * que lo incluye. Asi solo se recompila `build_info.cpp` y se reenlaza.
 */
#include <string>

namespace Haruka {

/** @brief Version semantica del motor, p.ej. "1.0.0". */
const char* versionString();

/** @brief Identidad completa del build: `1.0.0+42.3dd70dd` (version + build + commit).
 *
 *  El build es el numero de ejecucion de CI, o `local` si se compilo a mano. El commit lleva el
 *  sufijo `-dirty` cuando el arbol tenia cambios sin confirmar — en ese caso el binario NO es ese
 *  commit y decir que lo es seria mentir sobre lo que se esta probando. Es la cadena que va al log,
 *  a la esquina del menu y a cualquier reporte. */
const std::string& buildString();

} // namespace Haruka
