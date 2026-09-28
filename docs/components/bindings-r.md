# R package

The StreamFind R package is a legacy interface for DuckDB-backed
mass-spectrometry and non-target-screening workflows, including the Shiny
application.

!!! warning "Legacy interface"
    The R package is separate from the current native C++ operation-graph
    framework. Use this page only for existing R and Shiny workflows. New
    integrations should use the native C++ package and its MCP interface.

## Installation

The package requires R and a suitable native build toolchain. On Windows this
generally includes [RTools](https://cran.r-project.org/bin/windows/Rtools/).

Install from GitHub:

```r
options(timeout = 600)
if (!requireNamespace("remotes", quietly = TRUE)) {
  install.packages("remotes")
}
remotes::install_github("ricardo-cunha/streamfind", subdir = "bindings/r")
```

The companion example-data package can be installed separately:

```r
remotes::install_github("odea-project/streamfind.data")
```

## Project workflow

Open or create a non-target-analysis project, define an ordered workflow, and
run it:

```r
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

Workflow methods must be ordered according to their prerequisites. The package
help pages document each method's parameters and expected project state.

## Chromatograms

Chromatogram workflows use the corresponding method constructors:

```r
load_chromatograms <- Method_MassSpecChromatograms_LoadChromatograms()
filter_chromatograms <-
  Method_MassSpecChromatograms_FilterChromatogramsRetentionTime(
    rtmin = 0,
    rtmax = 600
  )

workflow <- Workflow(list(load_chromatograms, filter_chromatograms))
set_workflow(nta, workflow)
run_workflow(nta)
```

## Shiny application

The package includes a Shiny application for browsing projects, analyses,
workflows, chromatograms, spectra, and non-target-analysis results:

```r
library(streamfind)

nta <- open_ProjectNonTargetAnalysis(
  db = "streamfind.duckdb",
  project_id = "nta_demo"
)
nta$run_app()
```

The application opens in a browser when run locally.

## Container image

A container image for the R package is available from
[Docker Hub](https://hub.docker.com/r/ricardocunha23/streamfind). Consult the
image documentation for the current image configuration and runtime options.
