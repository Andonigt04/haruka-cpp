# Security policy

## Reporting a vulnerability

**Please do not report security problems in public issues, pull requests or discussions.**

Report them privately through GitHub:
**[Report a vulnerability](https://github.com/Andonigt04/haruka-cpp/security/advisories/new)**
(the *Security* tab, then *Report a vulnerability*).

Include what you can of the following:

- the affected component and version (commit hash);
- the kind of issue (for example memory corruption, path traversal, denial of service);
- steps or a file that reproduces it;
- the impact you expect: what an attacker gains and what they need to control.

## What to expect

This project is maintained by one person, so responses are best effort. The intent is:

- to acknowledge your report within a week;
- to keep you informed while it is investigated and fixed;
- to credit you in the advisory once a fix is released, unless you prefer not to be named.

Please give a reasonable amount of time to fix the problem before you disclose it publicly.

## Supported versions

Only the latest commit on `main` receives security fixes. There are no maintained release
branches yet.

## Scope

The parts of the engine that handle data from outside the process are the most relevant:

| Area | Where | Why it matters |
|---|---|---|
| Networking (DGS client) | `Src/Haruka/Network/`, `external/dgs` | parses packets from a server |
| Server rules module | `haruka_rules` (loaded with `dlopen`) | runs inside the authoritative server |
| Scenes, saves and settings | `Src/Haruka/Core/Scene/`, `Src/Haruka/Settings/` | JSON from disk, possibly from other players |
| Prefabs | `Src/Haruka/Game/Prefab/` | JSON and glTF (`.glb`/`.gltf`) from disk |
| Assets | `Src/Haruka/IO/`, `Src/Haruka/Renderer/Model.*` | images, models and audio through stb, assimp and OpenAL |

Bugs in third-party components (Dear ImGui, Jolt, nlohmann/json, stb, assimp, SDL…) should be
reported upstream. Tell us as well if the engine is affected in a way that needs a change here.
