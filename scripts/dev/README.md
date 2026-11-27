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

The annotation regression check reads project registration metadata from
DuckDB and reads the published feature rows directly from their Arrow IPC
result-store payloads. Install its Python dependencies into the repository
`.venv`, then invoke it through `sfdev`:

```powershell
.venv\Scripts\python.exe -m pip install -r scripts\dev\cpp\requirements.txt
sfdev script run cpp/test_annotate_components.py
```

The regression check skips data-backed cases when its disposable projects are
absent from `tmp/projects/`; it does not treat DuckDB as the storage format for
published table rows.

Add `-RunPipeline` to `test-nta.ps1` for the expensive NTA workflow. Example
data is resolved from the shared `<user>/.streamfind/streamfind.data` checkout;
`sfdev setup --components data` provisions it on demand. Override the location
with `STREAMFIND_EXAMPLE_DATA_ROOT`. Vendor fixtures use
`STREAMFIND_VENDOR_DATA_ROOT`.

The C++ scripts use the native catalogue generated at:

```text
tmp\build\windows\core\semantic_catalogue\catalogue.duckdb
```

## Editable workflow projects

Use `sfdev workflows` to create a validated workflow project in the repository's
`tmp/projects/` directory through MCP. The StreamFind service must already be
running. These commands create the project and workflow but do not execute
operations, so you can open and edit the project before running it.

```powershell
sfdev workflows nta [--project-name NAME] [--url ENDPOINT] [--force]
sfdev workflows impurities [--project-name NAME] [--url ENDPOINT] [--force]
sfdev workflows quantification [--project-name NAME] [--url ENDPOINT] [--force]
```

The NTA template uses the shared wastewater mzML and target files from
`<user>/.streamfind/streamfind.data`. The impurity template uses the
`ACC1_27166_4` files in
`E:\example_files\ms_merck\More Data OLED Routine for Ricardo`. Quantification
is based on a tracked workflow snapshot extracted from the
`quantification-sciex-mix1` development project. That template retains
the original SCIEX input path, so update it if the source data is elsewhere.

Each command has a deterministic default project name. If the project already
exists, choose another `--project-name` or pass `--force` to replace it. Force
removes the existing `.sf` project folder and workflow JSON
sidecar; close the project in the app before replacing it. Use `--url` to
target a non-default service endpoint. Use
`sfdev workflows <name> --dry-run` to preview the helper invocation; dry-run
does not contact MCP or create a project.
