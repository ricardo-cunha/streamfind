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

Add `-RunPipeline` to `test-nta.ps1` for the expensive NTA workflow. External data
is resolved from the sibling `streamfind.data` repository and can be overridden with
`STREAMFIND_EXAMPLE_DATA_ROOT`. Vendor fixtures use `STREAMFIND_VENDOR_DATA_ROOT`.

The C++ scripts use the native catalogue generated at:

```text
tmp\build\mingw-ucrt64\semantic_catalogue\catalogue.duckdb
```
