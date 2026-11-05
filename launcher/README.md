# launcher

The X4 VR Mod launcher: a Qt-free core (`core/`) with the `x4vrmod` command line
(`cli/`) over it; the window comes later, over the same core. The design is in
[docs/packaging-plan.md](../docs/packaging-plan.md), Phases 2 and 3.

    x4vrmod doctor            every condition for playing, one line each
    x4vrmod probe x4 [PATH]   X4's version and which of the mod's patches fit
    x4vrmod probe xr          ask the VR runtime
    x4vrmod probe vulkan      what the GPUs offer

**Is this X4 compatible?** There is no checksum. `probe x4` and `doctor` run the
same checks the injector makes in X4's memory at load, over the file, feature by
feature. Those checks live in `injector/` and `common/`, as templates over a reader.

**Everything here only reads,** and VR and Vulkan probes run in a child process with
a timeout, so a broken runtime can't hang or crash the launcher. The layer and the
injector are never loaded: their version is read from the file
(`cmake/x4vr_version.cmake`'s marker), because loading them would run their code.

Tests: `build/tests/x4vr_test_launcher` and `tests/run-launcher-x4-bytes.sh`.
