## What and why

<!-- What does this change, and what problem does it solve? Link the issue: "Fixes #123". -->

## How it was verified

<!-- The test that failed before and passes now, or the numbers before/after for a performance
change. Say under which conditions: backend, GPU, build type, plugged in or on battery. -->

- [ ] `haruka_tests` passes (group or full suite: ...)
- [ ] `haruka_tests_rhi` passes on: [ ] OpenGL [ ] Vulkan [ ] not run (why: ...)
- [ ] New or changed behaviour has a test **with a counter-test** (see QUALITY.md)

## Not verified

<!-- What you could not check. "Not run on Vulkan", "not tried in the game", etc. -->

## Checklist

- [ ] Commits are signed off (`git commit -s`, see CONTRIBUTING.md)
- [ ] Follows QUALITY.md (naming, own header first, no `../` includes, no new ad-hoc threads)
- [ ] Builds with the module masks it touches (`#ifdef HARUKA_MOD_<NAME>` where needed)
- [ ] Shaders changed? The `.spv` files were regenerated
