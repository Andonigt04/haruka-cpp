/**
 * @file test_display.cpp
 * @brief EL SELECTOR DE MONITOR: cuando hay que re-enumerar las pantallas y que indice ocupa cada
 *        una. Los dos sitios donde el selector se equivocaba al enchufar un monitor externo.
 *
 * Sin SDL corriendo y sin monitores de verdad: los helpers de `Window` son puros (listas de IDs),
 * que es justamente por lo que se sacaron del panel — lo de dentro de ImGui no se puede probar.
 *
 * Contrapruebas, una por afirmacion: la lista IGUAL no pide re-enumerar (si no, el panel rehace los
 * nombres cada frame y el test pasaria con un `return true`); dos pantallas cambiadas por otras dos
 * tienen el MISMO numero (que es lo que miraba el codigo viejo) y aun asi hay que re-enumerar; y el
 * indice de la primera es 0 pero el de la tercera NO (el bucle roto daba 0 para todas).
 */
#include "test_common.h"
#include "core/window.h"

#include <vector>

using Haruka::Core::Window;

void test_display_selector_list() {
    beginTest("display_selector_list");

    // ── ¿Hay que re-enumerar? ───────────────────────────────────────────────────────────────────
    const SDL_DisplayID two[]   = { 11, 22 };
    const SDL_DisplayID three[] = { 11, 22, 33 };
    const std::vector<SDL_DisplayID> cached2 = { 11, 22 };

    CHECK(!Window::displayListDiffers(two, 2, cached2),
          "la MISMA lista no pide re-enumerar (si no, el panel rehace los nombres cada frame)");
    CHECK(Window::displayListDiffers(three, 3, cached2),
          "enchufar un monitor externo (2 -> 3 pantallas) SI pide re-enumerar");
    CHECK(Window::displayListDiffers(two, 2, { 11, 22, 33 }),
          "y desenchufarlo tambien (3 -> 2)");

    // El caso que un contador NO ve: mismo numero, otras pantallas (portatil que entra al dock).
    const SDL_DisplayID otherTwo[] = { 77, 88 };
    CHECK(cached2.size() == 2, "contraprueba: el dock deja el mismo NUMERO de pantallas...");
    CHECK(Window::displayListDiffers(otherTwo, 2, cached2),
          "...y aun asi hay que re-enumerar: se comparan los IDs, no cuantos son");
    // Orden distinto = reordenacion de monitores: los IDs son los mismos pero el indice de cada uno
    // cambia, y el selector guarda "Monitor N" cuando el compositor no da nombres.
    const SDL_DisplayID swapped[] = { 22, 11 };
    CHECK(Window::displayListDiffers(swapped, 2, cached2),
          "reordenar los mismos monitores tambien cuenta (el indice de cada uno cambia)");

    // Sin pantallas (headless) contra una lista vacia: nada que rehacer.
    CHECK(!Window::displayListDiffers(nullptr, 0, {}), "headless contra vacio: no hay cambio");
    CHECK(Window::displayListDiffers(nullptr, 0, cached2), "perderlas todas si es un cambio");

    // ── ¿Que indice ocupa? ──────────────────────────────────────────────────────────────────────
    CHECK(Window::displayIndexIn(three, 3, 11) == 0, "la primera pantalla es el indice 0");
    CHECK(Window::displayIndexIn(three, 3, 33) == 2,
          "la TERCERA es el indice 2 — el bucle roto salia en la primera vuelta y daba 0 para todas");
    CHECK(Window::displayIndexIn(three, 3, 22) != Window::displayIndexIn(three, 3, 33),
          "contraprueba: dos pantallas distintas NO comparten indice (dos etiquetas 'Monitor 0' eran el bug)");
    CHECK(Window::displayIndexIn(three, 3, 99) == 0,
          "una pantalla que ya no esta cae al 0 (la primaria), no a un indice fuera de la lista");
}
