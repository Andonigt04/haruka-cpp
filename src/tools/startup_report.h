#pragma once
#include <string>
#include <vector>
#include <cstdio>

/**
 * Collects per-system init results during Application::init(), then prints
 * a clean summary (clear screen + table) showing failures and skips.
 * Usage:
 *   StartupReport::get().record("GBuffer", StartupReport::OK);
 *   StartupReport::get().record("IBL",     StartupReport::FAIL, "shader missing");
 *   StartupReport::get().printSummary();
 */
class StartupReport {
public:
    enum Status { OK, FAIL, SKIP };

    struct Entry {
        std::string system;
        Status      status;
        std::string detail;
    };

    static StartupReport& get() {
        static StartupReport inst;
        return inst;
    }

    void record(const std::string& system, Status status, const std::string& detail = "") {
        entries.push_back({system, status, detail});
        // Live progress line
        const char* icon = (status == OK) ? "\033[32m✓\033[0m" :
                           (status == FAIL) ? "\033[31m✗\033[0m" : "\033[33m-\033[0m";
        if (detail.empty())
            printf("  %s %s\n", icon, system.c_str());
        else
            printf("  %s %s: %s\n", icon, system.c_str(), detail.c_str());
        fflush(stdout);
    }

    void printSummary() {
        int ok = 0, fail = 0, skip = 0;
        for (const auto& e : entries) {
            if (e.status == OK)   ok++;
            else if (e.status == FAIL) fail++;
            else skip++;
        }

        // Clear console
        printf("\033[2J\033[H");

        printf("\033[1m══════════════════════════════════════════\033[0m\n");
        printf("\033[1m  Haruka Engine — Startup Report\033[0m\n");
        printf("\033[1m══════════════════════════════════════════\033[0m\n\n");

        if (fail == 0 && skip == 0) {
            printf("  \033[32m✓ All %d systems initialized OK\033[0m\n\n", ok);
        } else {
            printf("  \033[32m✓ OK:   %d\033[0m   "
                   "\033[31m✗ FAIL: %d\033[0m   "
                   "\033[33m- SKIP: %d\033[0m\n\n", ok, fail, skip);

            if (fail > 0) {
                printf("  \033[1;31mFailed systems:\033[0m\n");
                for (const auto& e : entries) {
                    if (e.status != FAIL) continue;
                    printf("    \033[31m✗ %-28s\033[0m %s\n",
                           e.system.c_str(), e.detail.c_str());
                }
                printf("\n");
            }
            if (skip > 0) {
                printf("  \033[1;33mSkipped systems:\033[0m\n");
                for (const auto& e : entries) {
                    if (e.status != SKIP) continue;
                    printf("    \033[33m- %-28s\033[0m %s\n",
                           e.system.c_str(), e.detail.c_str());
                }
                printf("\n");
            }
        }

        printf("\033[1m══════════════════════════════════════════\033[0m\n");
        fflush(stdout);
        entries.clear();
    }

private:
    StartupReport() = default;
    std::vector<Entry> entries;
};
