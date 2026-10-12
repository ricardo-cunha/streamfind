<!-- badges: start -->

[![Lifecycle:
experimental](https://img.shields.io/badge/lifecycle-experimental-orange.svg)](https://lifecycle.r-lib.org/articles/stages.html#experimental)
<!-- badges: end -->

<p align="center">
  <img src="inst/app/www/streamfind.png" width="70%" />
</p>

streamfind is a DuckDB-backed workflow framework for analytical data
processing in R. The current package architecture is built around
persistent `Project` child classes that hold data, workflow metadata,
cached results, and audit state in one place.

The main project classes are:

- `ProjectMassSpec`: shared mass-spectrometry project interface
- `ProjectMassSpecSpectra`: spectra-focused raw MS access
- `ProjectMassSpecChromatograms`: chromatogram-focused raw MS access
- `ProjectNonTargetAnalysis`: non-target analysis workflows and results

Workflows are assembled from ordered `Method` objects, stored on the
project, and executed reproducibly. This gives one framework for
interactive exploration, scripted processing, and Shiny-based inspection
of the same project state.

## Installation

streamfind requires R and a working C++17 toolchain. On Windows this
generally means installing
[RTools](https://cran.r-project.org/bin/windows/Rtools/). Some optional
methods depend on external software such as Java, MetFrag, Python-based
tooling, or chemistry packages listed in `Suggests`.

Install the package from GitHub:

``` r
options(timeout = 600)
if (!requireNamespace("remotes", quietly = TRUE)) {
  install.packages("remotes")
}
remotes::install_github("ricardo-cunha/streamfind", subdir = "bindings/r")
```

The repository root is not the R package root. The `subdir` argument tells
`remotes` to install the package under `bindings/r`; this command can be run
from any working directory. For local development, run package commands from
this directory and use `devtools::load_all()`.

Supplementary example data and assets are available in the companion package
[streamfind.data](https://github.com/odea-project/streamfind.data):

``` r
if (!requireNamespace("remotes", quietly = TRUE)) {
  install.packages("remotes")
}
remotes::install_github("odea-project/streamfind.data")
```

## Framework overview

The typical workflow is:

1.  Open or create a project child class.
2.  Import domain data into that project.
3.  Define a `Workflow` from `Method` objects.
4.  Attach the workflow with `set_workflow()`.
5.  Run it with `run_workflow()` or stepwise with `run_method()`.
6.  Inspect results through the project API or `run_app()`.

Example for non-target analysis:

``` r
library(streamfind)

nta <- open_ProjectNonTargetAnalysis(
  db = "streamfind.duckdb",
  project_id = "nta_demo"
)

workflow <- Workflow(list(
  Method_NonTargetAnalysis_FindFeatures(),
  Method_NonTargetAnalysis_LoadFeaturesMS1(),
  Method_NonTargetAnalysis_LoadFeaturesMS2(),
  Method_NonTargetAnalysis_GroupFeatures(),
  Method_NonTargetAnalysis_FillFeatures(),
  Method_NonTargetAnalysis_FilterFeatures()
))

set_workflow(nta, workflow)
run_workflow(nta)

features <- get_features(nta)
plot_features_count(nta)
```

## Non-target analysis

`ProjectNonTargetAnalysis` is the most feature-rich project class
currently exposed in streamfind. It supports native workflow methods
for:

- feature finding
- MS1 and MS2 trace loading
- component creation and annotation
- feature grouping and gap filling
- blank subtraction
- matrix-suppression correction
- feature filtering
- suspect screening
- internal-standard matching
- MetFrag screening
- transformation-product assignment

The project object also exposes structured getters and plots for
features, spectra, suspects, internal standards, matrix suppression,
fold change, and transformation products.

## Shiny application

The same project objects can be inspected in the integrated application:

``` r
nta$run_app()
```

This is useful when you want to browse analyses, results, and workflow
state without leaving the package data model.

## Docker container

A Docker image is provided that bundles streamfind, its R dependencies,
code-server (VS Code in the browser), SSH access, and pre-installed
external tools (Java, MetFrag). The app listens on port `3838`,
code-server on `8080`, and SSH on `22`.

### Build the image

``` bash
docker build -t streamfind .
```

### Run with host file access

Mount host directories under `/host/<name>`. The name you choose becomes
the volume label in the shinyFiles browser. The first mounted volume is
the default root.

``` bash
# Linux/macOS — user-chosen names
docker run -d --name streamfind \
  -v /home/ricardo/projects:/host/projects:rw \
  -v /mnt/instrument-data:/host/instrument_data:rw \
  -v /mnt/archive:/host/archive:ro \
  -p 3838:3838 \
  -p 8080:8080 \
  streamfind

# Windows (PowerShell) — user-chosen names
docker run -d --name streamfind `
  -v "C:\Users\apoli\projects:/host/projects:rw" `
  -v "D:\data:/host/instrument_data:rw" `
  -p 3838:3838 -p 8080:8080 -p 2222:22 `
  -e SSH_PASSWORD=yourpassword -e CS_PASSWORD=yourpassword `
  streamfind
```

Volumes appear in the file browser as:

| Volume name           | Path                                              |
|-----------------------|---------------------------------------------------|
| `projects` (default)  | `/host/projects` → host home                      |
| `instrument_data`     | `/host/instrument_data` → external drive          |
| `archive`             | `/host/archive` → read-only archive               |
| `.streamfind_debug`   | Container home directory (last, debug only)       |

The *first user-mounted volume* (`projects` above) is the shinyFiles
default root. The internal `.streamfind_debug` entry is always last and
is not meant for regular user work — it exists as a fallback and for
diagnostics.

### Custom mount roots

By default the app scans `/host` for subdirectories. If you mount your
data at different paths, set `streamfind_HOST_ROOTS` to a
colon-separated list of directories:

``` bash
docker run -d --name streamfind \
  -v /home/data:/workspace/my_data:rw \
  -v /mnt/archive:/archive/prod:ro \
  -e streamfind_HOST_ROOTS="/workspace:/archive" \
  -p 3838:3838 \
  streamfind
```

The app scans each directory in order and creates a volume entry for
every subdirectory found. The first detected subdirectory becomes the
default root.

### Accessing network / SMB / CIFS drives

Mount the share on the Docker host first, then bind-mount it into the
container:

Linux:

``` bash
sudo mount -t cifs //10.89.11.34/share /mnt/network/project_data \
  -o username=youruser,password=yourpass,uid=$(id -u),gid=$(id -g)
docker run -d --name streamfind \
  -v /mnt/network/project_data:/host/network_data:rw ...
```

Windows: Map the drive (e.g. `Z:`), then pass it:

``` powershell
docker run -d --name streamfind -v "Z:\:/host/network_data:rw" ...
```

Once mounted, the directory is automatically detected as a file-browser
volume.

### Useful commands

``` bash
# Watch the startup log
docker logs streamfind -f

# Run a one-off R command inside the container
docker exec streamfind R -e 'library(streamfind); rcpp_openbabel_structure_svg("CCO")'

# Open a shell
docker exec -it streamfind bash

# Stop and remove
docker stop streamfind; docker rm streamfind
```

### Environment variables

| Variable       | Default      | Description                     |
|----------------|--------------|---------------------------------|
| `SSH_PASSWORD`         | `streamfind`                 | SSH password for streamfind                  |
| `CS_PASSWORD`          | `streamfind`                 | code-server web UI password                  |
| `streamfind_WORKSPACE` | `/host`                      | code-server workspace root (first subfolder is the IDE start dir) |
| `streamfind_HOST_ROOTS` | `/host`                     | Colon-separated list of directories to scan for user-mounted volumes |

## External dependencies

Some workflows depend on third-party tools that are not bundled with
streamfind. For example:

- MetFrag screening requires a MetFrag CL installation and Java
- some chemistry features depend on `rcdk` and `rJava`
- optional reporting uses Quarto

Method-level documentation describes these requirements where relevant.

## Documentation

Reference documentation and articles are published at:

- <https://odea-project.github.io/streamfind/>
- <https://odea-project.github.io/streamfind/reference/index.html>

## Project background

streamfind is developed within the project “Flexible data analysis and
workflow designer to identify chemicals in the water cycle” with
contributions centred at IUTA and partner organisations. The package is
under active development and the project-class framework is the primary
interface going forward.
