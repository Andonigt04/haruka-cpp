# Contributing to Haruka Engine

Thanks for your interest. This guide covers how to propose a change and what a pull request needs
before it can be merged. The code and testing standards themselves are in [QUALITY.md](QUALITY.md).

## Before you start

- **Bugs:** open an issue with the bug report template. Include the backend (OpenGL or Vulkan),
  the GPU and driver, the build type, and the smallest way to reproduce it.
- **Features or larger changes:** open an issue first and describe the problem you want to solve.
  The engine has strong invariants (one ground shared by physics and rendering, deterministic
  physics, CPU↔GPU parity). A short discussion first saves a rewrite later.
- **Security problems:** do not open a public issue. Follow [SECURITY.md](SECURITY.md).

## Setting up

Follow [Building](README.md#building) in the README, and configure with tests enabled:

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHARUKA_BUILD_TESTS=ON
cmake --build build -j6 --target HarukaEngine haruka_tests haruka_tests_rhi
```

## Making a change

1. Fork the repository and create a branch from `develop`.
2. Keep the change focused: one problem per pull request. A refactor and a behaviour change go in
   separate pull requests, so each one can be reviewed and reverted on its own.
3. Put a new test in the `tests/` folder that mirrors the code it tests (`src/Haruka/Physics/` →
   `tests/Physics/`) and register it at the end of its file with `HARUKA_REGISTER_TESTS`.
4. Follow [QUALITY.md](QUALITY.md): naming, headers, threads, floating point, and above all
   **tests with a counter-test**.
5. Run the suites that cover what you touched:

   ```bash
   ctest --test-dir build -L cpu -j8      # whole CPU suite, one folder per job
   ctest --test-dir build -R World.Terrain # just the folder you touched
   ./build/haruka_tests_rhi all            # GPU bench, both backends (needs a GPU)
   ```

6. If you changed shaders, check that the `.spv` was regenerated. A shader that fails to compile
   to SPIR-V falls back silently under OpenGL, so the tests can stay green while Vulkan is broken.

## Pull requests

Fill in the pull request template. In particular:

- **Say what you measured.** For a performance change, give numbers before and after, and the
  conditions: backend, GPU, build type, and whether the laptop was plugged in. For a fix, give the
  test that failed before and passes now.
- **List what you did not verify.** "Not tested on Vulkan" is useful information. A silent gap is
  not.
- **Keep the module masks building.** CI builds several `MODULES` masks. If your code touches a
  module from outside it, guard it with `#ifdef HARUKA_MOD_<NAME>` (see
  [docs/guides/MODULOS.md](docs/guides/MODULOS.md)).

## Sign your commits (DCO)

This project uses the [Developer Certificate of Origin 1.1](https://developercertificate.org/).
By adding a `Signed-off-by` line to a commit, you certify that you wrote the change, or otherwise
have the right to submit it under the project's license:

```bash
git commit -s -m "Fix seam between terrain nodes at LOD boundaries"
```

That adds `Signed-off-by: Your Name <you@example.com>` with the name and email from your git
configuration. Pull requests with unsigned commits cannot be merged; `git rebase --signoff`
fixes existing ones.

## License of contributions

Contributions are accepted under the [Apache License 2.0](LICENSE), the same license as the rest
of the project (section 5 of the license). Do not submit code you cannot license that way, such as
code copied from projects under incompatible licenses or generated from such code.

## Language

Issues, pull requests and these guides are in English. Much of the existing code is commented in
Spanish; when you edit a file, keep comments in the language that file already uses.
