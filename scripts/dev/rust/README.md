# Rust development checks

Rust is a separate backend and is not part of the active C++ development lane.
Use the official Rust build/test wrappers from the repository root:

```powershell
scripts\build\rust\build-rust.cmd -Tests
scripts\build\rust\test-rust.cmd
```

Backend comparison belongs under `scripts\dev\conformance` and is explicitly
invoked only after both backends have been built. Do not add Rust execution to the
C++ scripts or C++ CTest targets.
