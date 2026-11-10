# Development-stage scripts

These scripts are outside the official test suites. The official C++ gate is
independent and does not require external vendor data, Java, or MetFrag.

## C++ development checks

Run these against the active C++ backend:

```powershell
scripts\dev\cpp\test-data.ps1
scripts\dev\cpp\test-nta.ps1 -MaxAnalyses 3
scripts\dev\cpp\test-vendor-readers.ps1 -Vendor Shimadzu
```

Add `-RunPipeline` to `test-nta.ps1` for the expensive NTA workflow. Example
data is resolved from the shared `<user>/.streamfind/streamfind.data` checkout;
`sfdev setup --components data` provisions it on demand. Override the location
with `STREAMFIND_EXAMPLE_DATA_ROOT`. Vendor fixtures use
`STREAMFIND_VENDOR_DATA_ROOT`.

The C++ scripts use the native catalogue generated at:

```text
tmp\build\windows\core\semantic_catalogue\catalogue.duckdb
```
