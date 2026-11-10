# Raman and IR plugin development plan

Date: 2026-10-13.
Worktree / branch: `.worktrees/raman_development` / `raman_development`.
Owner: `cpp/plugins/raman`.
Status: partial implementation in progress; ASC, Andor SIF, WiTec WIP, Shimadzu SMF, and WiTec semicolon text ingestion are implemented behind the shared reader interface. WIP/export evidence is currently established only for the reference export in the supplied corpus.

The current common `read_raman_files` operation accepts ASC, SIF, WIP, SMF,
and the observed multi-channel WiTec semicolon text export. WIP project graphs
are emitted as separate spectra with caption and processing-state metadata;
SMF evidence remains limited to the observed supplied layout.

## 1. Objective and scope

Implement native C++ ingestion and processing of Raman and infrared spectra,
including spectra acquired over time and spectra with spatial dimensions.
The first executable entry point will be `raman.read_raman_files`, replacing
`raman.add_analyses` and its semantic resource `sfram:ramanAddAnalyses` atomically.
Use `sfram:readRamanFiles` for the new declaration. Do not retain an alias,
forwarder, old execution path, or automatic workflow migration.

Keep the plugin ID and operation prefix `raman`; represent Raman versus IR as
an explicit measurement technique within its spectroscopy model. The display
label and description of the reader must make its IR support visible. There
is no need to create a second plugin or rename the existing domain for this work.

Implement readers that consume ASC, SIF, WIP, and SMF bytes directly. Text
exports are useful independent reader inputs and validation references, but
must not become a required conversion step for the binary formats. Production
code must not invoke R, reticulate, Python, MATLAB, or vendor GUI converters.

Preserve `bindings/r` as required by `AGENTS.md`. Its existing code is evidence
of intended scientific behavior, not an implementation dependency or an
unconditional numerical oracle. This plan does not modify the R package.

## 2. What was inspected

### 2.1 Existing implementation boundaries

The current C++ Raman plugin contains two registered handlers in
`cpp/plugins/raman/src/operations/operations.cpp`. Both return placeholder
status JSON; they do not read files, remove data, or publish spectra. The
entrypoint in `src/plugin_entrypoint.cpp` passes only `capability_id` to the
handler, discarding operation parameters and connected inputs. Fixing this
dispatch is part of the first operation, not a separate compatibility layer.

The relevant working mass-spec pattern is:

- `cpp/plugins/mass_spec/src/readers/reader.hpp`: `MASS_SPEC_FILE`, a reader
  interface, logical-analysis enumeration, selection, summaries, headers,
  and indexed spectrum retrieval.
- `cpp/plugins/mass_spec/src/operations/base.cpp`: `read_mass_spec_files`
  accepts `source_paths`, enumerates analyses, and emits typed artifacts.
- `cpp/plugins/mass_spec/src/plugin_entrypoint.cpp`: forwards `parameters`
  and connected `inputs` as `_inputs` through the SDK handler boundary.
- `cpp/plugins/mass_spec/semantic/{operations,parameters,artifacts,columns}.ttl`:
  discoverable entry operation, explicit table schemas, and typed ports.
- `cpp/sdk/include/streamfind/sdk/plugin_project_access.hpp`: host-owned
  table reads, bounded batches, output emission, cancellation, and progress.

Use those patterns without importing MS-specific columns, linking the Raman
plugin to mass-spec internals, or copying existing anonymous namespaces.
Core owns transactions, persistence, workflow execution, and artifact caching;
the Raman plugin owns spectroscopy semantics, readers, and algorithms.

### 2.2 R reader and representation findings

The Raman implementation inspected in this checkout resides primarily under
`bindings/r/dev/legacy_code/`, rather than the active `bindings/r/R/` API:

| Source | Observed behavior and implications |
| --- | --- |
| `class_Analyses_RamanAnalyses.R`, `.get_RamanAnalysis_from_files` | Builds analysis objects with name, replicate, blank, concentration, reference, file, metadata, and a spectra data table. Caches parsed objects in SQLite. Calls any spectrum with `rt` an `LC-Raman` analysis. |
| `rcpp_raman.cpp`, `rcpp_parse_asc_file` | Reads colon-delimited metadata and semicolon-delimited numbers. Two columns become `shift,intensity`; three become `rt,shift,intensity`. No explicit units, axis-kind validation, or reliable empty/ragged-row handling. The whitespace helper returns the original string. |
| SIF branch in `class_Analyses_RamanAnalyses.R` | Uses `orpl$file_io$load_sif` through reticulate. Evaluates calibration using only positive higher-order coefficients, substitutes excitation wavelength for a zero constant, rounds shifts to integers, reverses intensity, and sets `rt = frame_number * exposure_time`. These assumptions must not be copied. |
| Same format dispatch | Advertises `sif,json,wdf,sdf,csv,txt`, but sends all of them to `load_sif`; that list is not evidence of working parsers. No WIP or SMF reader was found in the Raman code inspected. |
| `class_RamanResults_Spectra.R` | Separates spectra, spectral peaks, chromatographic peaks, and an averaged-state flag. New C++ results should preserve those distinctions through immutable typed artifacts and lineage. |
| `class_RamanMethod_MergeSpectraTimeSeries.R` | Groups by replicate, cumulatively sums `Accumulate Cycle Time (secs)`, drops the first `preCut` spectra (default 2), and writes a merged ASC beside the sources. New sequence assembly must be explicit, preserve discarded-frame decisions in provenance, and publish artifacts without rewriting source data. |
| `bindings/r/R/utils_fct_assets.R` | Contains baseline package dispatch, Whittaker/airPLS code, and chromatography helpers referenced by the older methods. |

The legacy field `shift` can even hold detector pixel numbers. The target
model must represent axis quantity separately from its numeric values.
Similarly, a timed acquisition is not sufficient evidence of chromatography.

### 2.3 Local sample inventory and observations

Inspected `E:/example_files/raman` read-only. Counts below are a recursive
inventory; structural observations come from representative text records,
bounded binary headers, SMF embedded strings, and a WIP tag-tree traversal.
They do not constitute completed reader validation.

| Family | Files | Observed content |
| --- | ---: | --- |
| ASC | 16,061 | Andor metadata followed by two- or three-column numerical data; trailing semicolons occur. Includes individual spectra, detector-pixel spectra, calibrated copies, and merged time series. |
| SIF | 12 | `Andor Technology Multi-Channel File` signature. Example headers include version markers `65538` and `65567`, calibration coefficients, excitation wavelength, ROI/binning, and frame timestamp arrays. |
| WIP | 1 | `Kazamer-251008-Referenz.wip`, 17,867,655 bytes, signature `WIT_PR06`, Control FIVE 5.3.12.104 metadata. Traversed 1,447 bounded tags; found images, spectral graphs, transformations, interpretation records, and processed graph copies. |
| SMF / IR | 2 | `Polystyrene1.smf` and `Polystyrene2.smf`, each 360,448 bytes. Both decode as Berkeley DB hash v5. Object 2 (`Polystyrene`) has a `1/cm` axis record (4,523 points; 339.4482432–4700.2009584 cm⁻¹; 0.9643416 cm⁻¹ increment) and a matching 4,523-value float64 record. Objects 3 and 4 each have 16,384-value records on a `cm` axis, consistent with interferogram/background data and deliberately not emitted as spectra. The C++ reader imports the two sample spectra; signal quantity/unit and auxiliary-record roles remain unverified. |
| WITec TXT exports | 3 | Semicolon tables with a name row and unit row (`rel. 1/cm`, `CCD cts`); 4 or 5 signal columns share an X axis. Independent labeled spectra, not implicit time points. |
| Other files | 3 PNG, 1 XLSX | Supporting material. Not raw-reader targets in this plan. |

ASC subcollections:

- `asc_format/`: 22 individual spectra, including sample and blank.
- `asc_format/Messung_1_utpLV_Raman/`: 4,000 individual files,
  `220512_Proteine_Batch3_10001.asc` through `..._14000.asc`.
- `sif_format/Jana_cuvette_serum/`: 36 ASC files, including detector coordinates
  `1..1024` and `shiftadded_...` copies. Both live under a folder named SIF;
  folder names must never determine file format or axis kind.
- `sif_format/Jana_SEC_serum/`: 12,003 ASC files, including three merged files
  of about 91 MB each and the individual scans. Avoid ingesting both forms as
  one sequence or accidentally duplicating measurements through recursion.

Concrete calibration/time evidence:

- The inspected `raman_Bevacizumab_11731.asc` ranges approximately from
  -33.113487 to 2404.5544 in its first numeric column. Its metadata reports
  0.1 s exposure and 0.23967 s kinetic cycle. The header's 4,000-series count
  describes the acquisition; it does not mean this individual file contains
  4,000 spectra.
- `220524_Blutserum_Batch5_1.asc` begins at time 0.71901 and ends at 958.68
  in its explicit first column. Preserve these values; do not re-zero the
  file or recreate the two omitted initial frames. Its unit needs an explicit
  supported ASC-dialect rule or user-supplied declaration because the numeric
  column itself is unlabeled.
- `BVCZ_Avastin_1.sif` declares 4,000 frames and 1,024 points per frame;
  timestamp entries start `0,239670,479340,...`. Its calibration includes
  negative quadratic and cubic coefficients. `BVCZ_Zirabev_1.sif` declares
  9,400 frames, 1,024 points, and entries `0,100000,200000,...`.
  Verify timestamp units against the format implementation before converting.
- The WIP contains 1 x 1 spectral graphs with `SizeGraph = 1650`, 6,600-byte
  payloads, a native wavelength transformation in nm, and `CCD cts` signal
  interpretation. Captions include `(CRR)` and `(Sub BG)` derivatives. In the
  supplied file, `(CRR)` curves closely track the unannotated count curves,
  while `(CRR) (Sub BG)` curves are baseline-subtracted and differ strongly in
  scale. The four `Spektren-Referenz.txt` columns match WIP graph indices 7,
  10, 13, and 16, all captioned `(CRR) (Sub BG)`, with maximum absolute axis
  and signal differences of 0.004999 cm⁻¹ and 0.000363 CCD counts respectively.
  The five Mitte and four Rand export columns do not match any of the 20
  currently decoded WIP graphs within those tolerances; their source pairing
  is unresolved and must not be inferred from filenames alone. A repeatable
  data-backed comparison is available at
  `scripts/dev/cpp/raman_wip_export_check/`; one supplied WIP project is not
  enough evidence to generalize this classification across WITec versions.

## 3. Target native reader interface

Create one owning reader layer under `cpp/plugins/raman/src/readers/`:

```text
reader.hpp / reader.cpp       shared spectroscopy records and file facade
asc_reader.cpp                documented Andor ASC dialects
sif_reader.cpp                native Andor SIF
wip_reader.cpp                native WITec project and record graph
smf_reader.cpp                native IRsolution container and records
witec_text_reader.cpp          observed multi-spectrum text export dialect
```

Use the namespace `streamfind::raman::reader` and a `RamanFile` facade over
format readers. A spectrum type shared by Raman and IR should be named
`Spectrum`, not encode Raman shift in its member names. The facade mirrors
the useful mass-spec responsibilities:

```cpp
// Contract sketch; exact declarations will be implemented in reader.hpp.
RamanFile(path, ReaderOptions);
get_analysis_catalog();                 // deterministic logical analysis IDs
get_source_records();                   // also identifies non-spectrum records
select_analysis(analysis_index);
get_summary();
get_spectra_headers(selection);
get_spectrum(spectrum_index);
read_spectra_batches(selection, sink);  // bounded point/spectrum batches
get_metadata();
```

The scientific reader layer must work without a Project, DuckDB, JSON
operation arguments, or Rcpp. `ReaderOptions` contains typed format options,
explicit missing-unit/calibration declarations, and record selection. The
operation layer translates JSON and emits artifacts through the SDK.

Probe signatures and validate structure before decoding. Extensions are
hints. Malformed recognized files must fail as that format; do not try a
different reader until numeric-looking output appears. Check offsets,
dimensions, multiplication overflow, lengths, text numeric conversions,
endianness, and unsupported versions. Errors identify the file, record,
and offset or line when available.

Index metadata and frame positions first; decode selected spectra in bounded
batches. ASC may need a streaming indexing pass, but must not load a 91 MB
file into one JSON array. Preserve full double precision in the scientific
model and host `DOUBLE` columns; record original numeric encoding separately.

## 4. Spectra, axes, and multidimensional representation

### 4.1 Scientific records

Represent these concepts independently:

| Concept | Required meaning |
| --- | --- |
| Physical source | File path, detected format/version, size, modification state, reader version, and source fingerprint. A sequence can reference multiple physical sources. |
| Source record | Stable vendor record ID/path, caption, parent relationship where known, kind (spectrum, interferogram, image, metadata), and processing role (raw, background, derived, unknown). |
| Logical analysis | One coherent acquisition/dataset or explicitly assembled sequence. Carries technique, source record identity, descriptive labels, and acquisition kind. |
| Spectrum | A stable spectrum ID, frame/track/channel identity, ordered pairs of spectral coordinate and signal, units, and optional additional coordinates. One analysis may contain many spectra. |
| Spectral axis | `raman_shift`, `wavenumber`, `wavelength`, `pixel`, or `unknown`, with unit, source order, and calibration provenance. Raman shift and IR wavenumber remain different quantities even when both use cm^-1. |
| Signal | Counts, counts/s, arbitrary intensity, absorbance, transmittance fraction, transmittance percent, reflectance, or an explicitly identified other/unknown quantity. Preserve negative values. |
| Extra coordinate | Dimension ID, kind, index, optional physical value/unit, and origin. Examples: elapsed time, retention time, stage X/Y/Z, detector row, or channel. Missing values are null, not zero. |

Use explicit axis arrays rather than assuming a constant step. Support
different point counts and grids between spectra. A shared-grid optimization
in memory must not change the contract or force resampling on import.

Within persisted spectra, `(analysis_id, spectrum_id, point_index)` identifies
a point; `source_point_index` preserves correspondence if a later operation
changes ordering. Neither a floating-point time nor a display name is an
identity key. Stable source-record IDs disambiguate repeated captions and
equal file stems from different directories. Sequence membership preserves
original files and local frame indices.

Do not reverse intensity independently of its paired coordinates. Preserve
source ordering on ingestion and declare it. A later ordering operation must
permute all associated arrays together. Presentation may show IR wavenumber
descending without changing stored values.

### 4.2 Raman and IR calibration

- Preserve original pixel/wavelength coordinates and all calibration
  coefficients, including zero and negative terms, with their ordering,
  pixel origin, binning, ROI, and validity range.
- For wavelength in nm and known excitation wavelength, Raman shift is
  `1e7 * (1 / excitation_nm - 1 / wavelength_nm)`. Validate positive
  wavelengths and use the calibration convention documented for the source.
- A spectrograph center wavelength is not automatically the excitation
  wavelength. The ASC field `Wavelength (nm)` alone is insufficient evidence
  for reconstructing the pixel-to-Raman-shift axis in these examples.
- Missing calibration produces a valid pixel-axis spectrum with a diagnostic,
  not an invented cm^-1 axis. Operations needing physical bands reject it
  until an explicit calibration operation has supplied the missing mapping.
- Imported IR absorbance, transmittance, reflectance, and single-beam signals
  retain their declared kind. Conversion is an explicit later operation:
  `A = -log10(T)` for a transmittance fraction, or
  `A = -log10(T_percent / 100)` for percent. Specify handling of zero/negative
  transmittance; do not silently clip or relabel values during reading.
- Interferograms are source records on an optical-path/detector axis. Reading
  stored IR spectra is the first SMF target. Fourier reconstruction,
  apodization, phase correction, and background ratioing are separate future
  operations, not undocumented steps inside the reader.

### 4.3 Time, sequences, and chromatography

Store known physical times in seconds while retaining original values, units,
and conversion provenance. Keep elapsed acquisition time and retention time
distinct, and preserve timestamp reference (start, midpoint, end, or unknown).
Calendar acquisition timestamps are separate and must not be assigned a
timezone that the source does not establish.

Determine time in this order:

1. Explicit per-frame timestamps with known source units.
2. A documented frame/kinetic period and origin, marked as derived timing.
3. Explicit user-supplied timing for a declared sequence, also marked derived.
4. Frame index with unknown physical time; retain data but disable operations
   requiring seconds. Exposure duration alone does not establish frame spacing.

Single ASC scans may be imported independently. Combining exported files into
one time series requires an explicit ordered group, frame numbering, and
timing policy. Validate missing/duplicate indices and acquisition boundaries.
Use natural numeric ordering only when the caller selected and declared that
ordering rule. Do not derive groups from replicate labels or recursively
combine everything in a directory. Preserve gaps and irregular timestamps.
No automatic `preCut`, dropping frames, zero-filling, or duplicate averaging.

The same representation supports `signal(time, spectral_coordinate)` and
`signal(time, x, y, spectral_coordinate)` without fabricating a rectangular
cube. Per-spectrum coordinates preserve sparse maps, multiple tracks, and
incomplete acquisitions. Map position or detector row must not be mistaken
for time. Equal timestamps across channels are valid distinct spectra.

Chromatography is a derived layer over those spectra. An explicit acquisition
declaration or justified metadata can mark a time dimension as retention
time. Generic kinetic traces remain elapsed-time traces. Trace extraction
must group by analysis, dataset, channel, and all non-time coordinates unless
the caller explicitly requests a spatial/channel reduction.

## 5. Entry operation and persisted contracts

### 5.1 `raman.read_raman_files`

The operation has `sf:projectEntry true`, no required input ports, and a
required non-empty `source_paths` array of explicit file paths, like the
mass-spec entry operation. Reject directories in the initial contract; a
development inventory helper can prepare the explicit list without hiding
file dependencies from the workflow engine.

Additional declarative parameters, added with the corresponding milestones:

- `source_options`: entries keyed by `file_path`, with format dialect,
  technique/axis/unit declarations where metadata are ambiguous, calibration
  inputs, and explicit record selection. Preserve conflicting source metadata
  and record the override; do not silently replace it.
- `sequence_groups`: group IDs and ordered members from `source_paths`, with
  frame numbering, time kind, units, origin, and timing rule. An ungrouped file
  is an independent source; a grouped file contributes once to that sequence.
  These options select acquisition semantics, not old-versus-new behavior.

Validate all inputs and requested records. Use deterministic analysis IDs and
preserve friendly captions separately. Replicate, blank, and reference labels
are initially unset and can be assigned through later operations. Unknown
quantities stay explicitly unknown; a mandatory user declaration should be
required when ambiguity would otherwise change the scientific interpretation.

Import all recognized spectral records by default, retaining their processing
roles. Catalog unsupported/non-spectral records with reasons. A requested
unsupported record, malformed selected spectrum, or file with no supported
spectra fails the operation. Successful inspection of metadata alone must not
be reported as successful spectral ingestion.

Unlike the current MS reader's metadata-only ingestion, this Raman operation
will also persist decoded spectral samples in bounded batches. This makes
raw spectral artifacts directly usable by processing nodes and readable after
the source becomes unavailable. Preserve source lineage and avoid a second
implicit source re-read in processing operations. The facade still supports
indexed access for standalone readers and future bounded inspection.

### 5.2 Proposed first-generation table contracts

Declare every column, type, nullability, and relationship in Raman-owned
`semantic/artifacts.ttl` and `semantic/columns.ttl`; the following is the schema
design to implement, not a substitute for those declarations.

| Port / contract | Grain and principal columns |
| --- | --- |
| `analysesTable` / `RAMAN_ANALYSES` | One logical analysis: `analysis_id`, display `analysis`, `analysis_index`, `technique`, `acquisition_kind`, nullable `replicate`, `blank`, `reference`, `concentration`, `number_spectra`, `has_time`, `has_spatial_coordinates`, nullable `start_time_seconds`, `end_time_seconds`, `created_at`. |
| `sourceRecordsTable` / `RAMAN_SOURCE_RECORDS` | One physical file/record association: `source_id`, `file_path`, `format`, `format_version`, `reader_version`, `source_fingerprint`, `record_id`, nullable `parent_record_id`, `record_kind`, `record_role`, `caption`, nullable `analysis_id`, `supported`, `diagnostic`. Non-spectral records can have no analysis ID. |
| `spectraHeadersTable` / `RAMAN_SPECTRA_HEADERS` | One spectrum: `analysis_id`, `spectrum_id`, `index`, `source_id`, `record_id`, `source_frame_index`, nullable `channel_id`, `array_length`, `axis_kind`, `axis_unit`, `axis_order`, `source_axis_kind`, `source_axis_unit`, `signal_kind`, `signal_unit`, nullable `excitation_wavelength_nm`, `exposure_seconds`, `time_seconds`, `time_kind`, `time_origin`, `timing_source`, `axis_min`, `axis_max`, `calibration_id`. |
| `spectraTable` / `RAMAN_SPECTRA` | One point: `analysis_id`, `spectrum_id`, `point_index`, `source_point_index`, `source_spectral_coordinate`, `spectral_coordinate`, `intensity`, `validity`. Double-valued coordinates/signals; missing measurements are null with an explicit validity reason. Preserve the source coordinate alongside any calibrated coordinate. |
| `spectrumCoordinatesTable` / `RAMAN_SPECTRUM_COORDINATES` | One additional coordinate per spectrum/dimension: `analysis_id`, `spectrum_id`, `dimension_id`, `dimension_kind`, `coordinate_index`, nullable `coordinate_value`, `coordinate_unit`, `coordinate_origin`, `value_source`. Covers spatial/detector axes without fixing a dense shape. Time is authoritative in the headers, not duplicated here. |
| `sourceMetadataTable` / `RAMAN_SOURCE_METADATA` | One metadata occurrence: `source_id`, `record_id`, nullable `spectrum_id`, `metadata_path`, `occurrence`, `value_type`, `value`, nullable `unit`, `origin`. Preserves repeated keys, raw units, calibration coefficients, source time values, and uninterpreted metadata. |

All IDs are strings except explicitly integer indices/counts. Physical
quantities are nullable doubles when absent; flags are booleans. Enum values
and units have explicit catalogue definitions. `source_id + record_id` and
`analysis_id + spectrum_id` are scoped composite keys; artifact identity from
the host additionally scopes joins between operation runs.

When a transformation resamples or aggregates points, a single source point
may no longer exist: source-point fields become nullable and an explicit
derivation relation identifies contributing spectra/grids. Do not manufacture
one-to-one source indices for interpolated or averaged points. Calibration
IDs reference documented calibration metadata occurrences; new operations
that create calibration records must declare that linkage in their contracts.

Emit these contracts and `sfcore:operationSuccessSignal`, with a small result
summary of sources, analyses, spectra, points, and diagnostics. Empty optional
tables still have a discoverable schema. Confirm the host's empty-table
emission behavior in the initial contract test before downstream nodes rely
on an empty coordinates or metadata artifact.

Processing nodes consume explicitly connected spectra/header/coordinate
artifacts as needed, preserve identities or publish explicit derivation
relationships for aggregation, and emit new artifacts. Do not update an
implicit global Raman spectrum table. Baseline/background output tables and
aggregation lineage will be declared when their operations are added.

### 5.3 Dispatch, atomicity, and caching

Forward request parameters and `_inputs` in the Raman entrypoint, following
the existing SDK pattern. Keep only the new reader ID in the capability
registry and catalogue when the first implementation is complete. Update
discovery tests and documentation together. Implement `raman.remove_analyses`
as a real typed selection operation over connected artifacts, preserving
historical artifacts and source files; it must cease returning placeholder
success. Its outputs must consistently filter related spectra and metadata.

Preflight source manifests and publish within the host transaction. On any
selected-record failure or cancellation, no partial import becomes visible.
Check cancellation between files and batches; report progress using file/frame
work, without sending millions of individual point notifications.

The inspected `cpp/core/src/fingerprint.cpp` includes file size and modification
time for strings under path/file parameter keys. It does not recursively
fingerprint directory contents or compute content hashes. Keeping all sequence
members explicit in `source_paths` fits its current contract. Record content
digests in import provenance where practical, and do not claim they improve
cache invalidation unless they participate before cache lookup. Test ordinary
source edits and sequence-option changes. Same-size, same-mtime replacements
remain a generic host cache limitation; stronger invalidation must be added
at the host's generic dependency boundary, not as a Raman-specific cache.

## 6. Format implementation and evidence gates

### ASC and observed WITec text (first usable slice)

Implement a streaming metadata/data state machine with repeated metadata keys,
LF/CRLF, whitespace, scientific notation, blank lines, and optional trailing
delimiters. Split metadata at the first colon; section headings and duplicate
serial-number keys must survive. Validate row width and full numeric tokens.
Reject malformed input with a line number instead of truncating it silently.

Define explicit ASC dialects for two-column and time/axis/intensity data.
Pixel-looking axes and missing unit labels require metadata or declared
options; integer-valued Raman shifts alone do not prove a pixel axis. Import
three-column data as frames with stable IDs, validating frame boundaries and
point ordering even when timestamps repeat or reset. Preserve acquisition
segment boundaries rather than combining solely by equal floating-point time.

Read WITec text as a separate named dialect: names row, units row, one shared
axis, multiple labeled spectra. It must not pass through the ASC
time/axis/intensity interpretation just because a table has three columns.

Acceptance: native decoding of representative two-column counts, pixel-axis
data, a declared calibrated copy, and a merged timed ASC; WITec exports retain
every signal column/name/unit. Confirm first/last points and spectrum boundaries
against independent text inspection. Explicit sequence ingestion of the 4,000
individual ASC files must not duplicate the merged files.

### Andor SIF

Implement version-aware headers, byte-counted strings/user data, ROI and
binning, frame counts, calibration blocks, per-frame timestamps, and native
pixel payloads. Do not use fixed header line counts or infer payload offsets
from the end of the file. Preserve channel/track separation and distinguish
an accumulation already combined by the instrument from independent spectra.

Evaluate all polynomial terms in the correct detector coordinate convention.
Verify axis orientation against a demonstrably matched ASC export; filenames
alone do not prove a pair. Read embedded timestamps when present, verify their
unit, and preserve missing/nonuniform timing. Confirm dimension products and
payload byte counts for both 4,000- and 9,400-frame examples before reading.

Acceptance: all 12 local SIF files decode with consistent dimensions, selected
first/middle/last frames, calibration, timing, and bounded memory. Compare with
an independently verified reader or matched instrument export at full source
precision; record tolerances based on export rounding. Neither external reader
nor vendor software is a runtime dependency.

### WITec WIP

Implement a bounded tag-tree reader for the observed `WIT_PR06` layout. Resolve
record IDs and references, graph payload type and shape, spectral and spatial
transformations, interpretation/unit records, excitation wavelength, and
source/derived relationships. Locate graphs by structure, not ordinal tag
positions or a substring in a caption. Index images and other objects without
mistaking them for spectra. Unknown tag types retain a diagnostic and can only
be skipped when unrelated to the selected spectrum's decoding/calibration.

Use the supplied TXT files as candidate reference exports. The four columns
in `Spektren-Referenz.txt` are matched to WIP graphs 7, 10, 13, and 16; their
captions identify them as `(CRR) (Sub BG)` derivatives. The Mitte and Rand
exports remain unmatched to decoded TDGraph records, so their pairing and
order are open evidence questions. WIP caption metadata now includes a
processing-state label (`Unannotated`, `CRR`, or `CRR + Sub BG`) without
discarding any graph. Re-run the focused checker after reader changes; its
axis/signal tolerances are 0.006 cm⁻¹ / 0.001 CCD counts, reflecting the
observed text-export precision.

Acceptance: direct WIP ingestion enumerates all supported spectral graphs,
keeps raw/processed/background roles and captions distinct, and matches the
identified export curves. The supplied point-graph evidence does not validate
general hyperspectral/time-map support; synthetic dimension tests cover the
model and further real map examples are needed before claiming that coverage.

### Shimadzu IRsolution SMF

`smf_reader.cpp` currently supports the observed Berkeley DB v5 legacy hash
pages and overflow chains. It groups records by the stored object prefix and
emits only an object whose `100001` axis record declares `1/cm` and whose
`100008` float64 payload length matches that record's point count. The two
supplied samples both parse as one 4,523-point spectrum. Their declared axis
endpoints and increment agree arithmetically with that count. The SMF axis
record does not establish the signal quantity/unit, so values are preserved
without labeling them absorbance or transmittance.

The 16,384-value arrays associated with `cm` axes are not emitted as spectra.
Their relationship to the embedded `Interferogram` and `Background
Interferogram` labels still needs a record-level mapping. The parser does not
yet expose those arrays or active/derived-spectrum relationships. The local
record layout is evidence from the two supplied files, not a general Berkeley
DB implementation; other page layouts and SMF versions fail explicitly.

Remaining acceptance: cross-check values, axis direction, and signal units
against authoritative IRsolution output or an independently validated
decoder; map auxiliary/background records; and add coverage for other SMF
versions/layouts before claiming broader support.

## 7. Processing roadmap from the R implementation

Add operations only after their input/output contracts and numeric behavior
are defined. The following operation IDs are proposed names, not claims of
existing capabilities. All stay under the `raman.` prefix and validate
technique, units, and dimensions.

| Existing R method family | Proposed native operation(s) | Required interpretation / correction |
| --- | --- | --- |
| AddShiftValues | `calibrate_spectral_axis` | Explicit calibration or reference grid with length/units/provenance checks; preserve full precision instead of rounding and assigning by position without validation. |
| InvertAxis | `order_spectral_axis` | The R method reverses intensity only. Native ordering permutes coordinates and all paired arrays together; reader-specific detector orientation belongs in format decoding. |
| MergeSpectraTimeSeries, ScaleTime | Explicit ingestion groups; `transform_time_axis` | Assemble known sequences; keep origin, units, acquisition boundaries, and provenance. Time scaling/offset is explicit and invalidates derived traces. |
| DeleteSpectraSection, DeleteScansSection | `filter_spectral_range`, `filter_time_range` | Include/exclude modes, explicit interval boundary semantics and units; preserve frame identity and discontinuities. |
| BinSpectra, BinScans | `bin_spectra`, `bin_time` | Separate physical interval and point/frame-count binning; define aggregation, partial bins, gaps, and grid policy. R global scan binning is unfinished, and its general spectral-bin branch contains MS-shaped output fields. |
| AverageSpectra | `average_spectra` | Group explicitly by replicate, time interval, or chromatographic peak. Verify common axes/units or require an explicit resampling step; emit counts, variability, and source membership. Never silently collapse time. |
| SubtractBlankSpectra | `subtract_blank_spectra` | Connect sample and blank artifacts explicitly and join on verified coordinates/channels/time policy. R matches largely by vector length/order. Negative-value clipping must be an explicit option. |
| SubtractScansSection, SubtractSpectraSection | `subtract_reference_interval` | Construct an averaged reference spectrum from a selected time interval; subtract on matched axes. Specify separately whether the reference interval is retained. Avoid the older mixed section-axis/time assumptions. |
| CorrectSpectraBaseline | `correct_spectra_baseline` (`als`, `airpls`) | Native solvers, validated penalty order and lambda convention, convergence diagnostics, and separate baseline output. R airPLS records an unclipped baseline but clips it before subtraction; define a consistent target contract. |
| SmoothSpectra | `smooth_spectra` (`moving_average`, `savitzky_golay`) | Operate per spectrum, never across concatenated frames/channels. Define edge treatment and nonuniform-grid policy; derivative outputs carry correct units. The moving-average R path can group by peak ID rather than time. |
| NormalizeSpectra | `normalize_spectra` | Support min-max, SNV, SD scaling, length block-weighting, and mean-centering with explicit formulas. R min-max differs for timed data; its untimed mean-centering divides by the mean. Correct these inconsistencies and define zero-range/zero-variance behavior. |
| RemoveCosmicRays | `remove_cosmic_rays` | New validated implementation: the R method contains `browser()`, debugging plots, and no completed correction. Preserve narrow real peaks; publish a correction mask and parameters. |
| Spectral similarity / HQI | `calculate_spectral_similarity` | Align spectra on verified axes; name squared correlation explicitly (`r^2` in the R code), retain signed correlation, and define constant-spectrum handling. Do not imply squared correlation distinguishes anticorrelation. |
| IR representation extension | `convert_signal_quantity` | Explicit absorbance/transmittance conversion with domain checks and provenance. This is additional functionality, not established R Raman behavior. |
| Chromatogram plotting/aggregation | `extract_chromatograms` | Derive total-signal or band-limited time traces from spectra; distinguish simple sum from spectral-axis integration and propagate their different units. |
| FindChromPeaks, FilterChromPeaks, GroupPeaks, FilterSpectra | `find_chromatogram_peaks`, `filter_chromatogram_peaks`, `group_chromatogram_peaks`, `select_peak_spectra` | Native peak boundaries/areas and explicit noise/grouping rules. Preserve peak and spectrum membership; mark filled estimates. Audit the older local-maxima and overlap grouping behavior before reimplementation. |

Target numerical definitions include standard min-max `(y-min)/(max-min)`,
SNV `(y-mean)/sample_sd`, SD scaling `y/sample_sd`, length block-weighting
`y/sqrt(n)`, and mean-centering `y-mean`. Resolve degenerate inputs explicitly
and record intended differences from R with small worked fixtures. Do not
provide a legacy-behavior switch.

Do not assume baseline lambda has the same meaning across the R package,
airPLS helper, and existing MS algorithms. Capture the penalty definition and
parameter transformation before selecting a native solver. Preserve a
baseline/corrected/raw relationship that can be checked numerically.

Define future trace contracts independently from MS tables:

- `chromatogramsHeadersTable`: trace ID, source spectral artifact, analysis,
  time kind/unit, channel/spatial selection, spectral band and unit,
  reduction method, signal quantity and resulting unit.
- `chromatogramsTable`: trace ID, point index, time in seconds, intensity,
  validity, and spectrum membership through a separate relation where needed.
- `chromatogramPeaksTable`: peak ID, trace ID, apex/start/end time, height,
  area, width, noise/SNR definition, and detected-versus-filled status.

For irregular spectral grids, use a defined trapezoidal band integral with
boundary interpolation and no integration across missing/masked gaps. A sum
over detector points is a different named reduction, useful for matching the
older cumulative-intensity trace. Time integration for peak area must likewise
use actual time differences, not point counts. Different grid coverage must
not silently produce comparable totals. Static spectra have no chromatogram.

## 8. Delivery milestones

### M0 — Record format evidence and finalize contracts

- [x] Inspect R source, native SDK/reader patterns, and local sample families.
- [x] Document observed variants, legacy assumptions, and undecoded boundaries.
- [ ] Add a development corpus inventory with relative paths, file hashes,
  expected format/record roles, and established binary/export pairings.
- [ ] Finalize table/ID/unit/nullability contracts and small synthetic fixtures.
- [ ] Document reader reference sources and licensing before reusing code.

### M1 — Native ASC ingestion and real entry operation

- [ ] Implement reader records, facade, ASC parsing, and source diagnostics.
- [ ] Add Raman semantic tables/columns/parameters and module declarations.
- [ ] Implement bounded emission of decoded points and metadata artifacts.
- [ ] Fix entrypoint parameter/input forwarding.
- [ ] Replace `raman.add_analyses` with `raman.read_raman_files` everywhere
  in the C++ runtime/catalogue/docs in the same change.
- [ ] Implement typed `raman.remove_analyses` and test referential consistency.
- [ ] Prove a workflow can read a spectrum, inspect its artifacts, filter its
  analysis selection, persist, close, and reopen the project.

### M2 — Time series, SIF, and independent text references

- [ ] Complete explicit ASC sequence groups and merged three-column handling.
- [ ] Add native SIF decoding, calibrated axes, timestamps, tracks, and batching.
- [x] Add the observed WITec text dialect for independent reference spectra.
- [ ] Validate all 12 SIF files and representative ASC families, then stress
  the 4,000-file sequence and merged files with time/memory measurements.
- [ ] Prove no smoothing, aggregation, or decoding merges neighboring frames.

### M3 — Native WIP and SMF / IR coverage

- [x] Implement WIP record-tree and referenced transformation decoding for the supplied point-graph layout.
- [ ] Establish WIP-to-export correspondences and verify values/units/roles.
- [x] Decode the supplied SMF layout and implement stored 1/cm spectrum ingestion.
- [ ] Obtain independent SMF numeric/signal-unit validation evidence.
- [ ] Map and preserve SMF interferogram/background record relationships.
- [ ] Test multidimensional coordinates and retain non-spectral record context.
- [ ] Publish an honest per-format/version support matrix; a decoder is not
  complete merely because it recognized the signature or located arrays.

### M4 — Spectral processing

- [x] Port `NormalizeSpectra` as `raman.normalize_spectra` with per-spectrum
  min-max, SNV, SD scaling, block weighting, and mean-centering algorithms;
  define dimensionless output metadata and reject undefined degenerate cases.
- [x] Add an initial native `airpls` baseline correction operation with a
  point-aligned baseline output and legacy clipping behavior; validate its
  linear-signal case and table contract. ALS and broader numerical parity remain
  open pending additional reference fixtures.
- [x] Add `filter_spectral_range` with inclusive coordinate bounds, include or
  exclude mode, and per-spectrum header point-count updates.
- [x] Add `smooth_spectra` moving-average smoothing with odd point-count
  windows, truncated edges, and spectrum isolation.
- [x] Add a separate native `smooth_spectra_savgol` operation with local
  polynomial least-squares fits, full shifted edge windows, and an evenly
  spaced-axis requirement. Derivative smoothing remains out of scope.
- [ ] Implement axis calibration/resampling, averaging, and
  blank/reference subtraction with explicit alignment contracts.
- [ ] Extend baseline correction with ALS and broader numerical parity checks;
  add binning.
- [ ] Add validated cosmic-ray removal, similarity, and IR signal conversion.
- [ ] Register only finished operations with declared semantic outputs and
  meaningful numerical, dimension-isolation, and edge-case tests.

### M5 — Chromatography over spectra

- [ ] Extract total/band traces with physical time and reduction metadata.
- [ ] Add peak detection/integration, filtering, grouping, and optional
  explicitly labeled gap filling.
- [ ] Connect peaks back to selected/averaged spectra and reference matching.
- [ ] Demonstrate an LC-Raman workflow and a separate elapsed-time workflow;
  prove static spectra and spatial maps cannot be mislabeled as chromatography.

### M6 — Integration and documentation

- [ ] Update Raman/IR operation discovery guidance and executable workflow
  examples under the owning plugin's resources directory.
- [ ] Verify generic MCP/service discovery, validation, execution, cancellation,
  artifact inspection, reload, and packaged plugin loading.
- [ ] Update the documented support matrix with tested sample variants and
  remaining SMF/WIP/version limitations. Add domain viewer work only after
  stable artifact contracts exist and as a separately bounded implementation.

## 9. Validation and repository rules

Official tests belong in `cpp/plugins/raman/tests/`, are C++ tests registered
with CTest, and use small synthetic/backend-neutral fixtures. Do not put
the external vendor corpus, Python tests, or workflows into the native suite.

Required meaningful test categories:

1. Reader failures: empty/truncated files, malformed numbers, duplicate
   metadata, ragged text rows, invalid dimensions/offsets, unsupported versions,
   missing calibration, and cancellation without partial output.
2. Scientific interpretation: all polynomial terms, detector origin/binning,
   axis orientation, Raman shift versus IR wavenumber, signal units, timestamp
   conversion/origin, repeated times across channels, and unknown-time behavior.
3. Shape/identity: static spectrum, multiple independent spectra, irregular
   time series, explicit file sequence, spatial map, multiple tracks, ragged
   grids, repeated captions, and stable source-to-spectrum correspondence.
4. Operation contracts: actual parameter/input forwarding, new entry discovery,
   old ID absence, all declared output schemas, empty optional tables, rollback,
   artifact immutability, remove-selection consistency, caching, and reopening.
5. Algorithms: known signals and edge cases, constant/empty spectra, missing
   values, irregular grids, real sharp peaks versus cosmic spikes, baseline
   reconstruction identities, and isolation between time/channel/map slices.

Keep data-backed validation scripts under `scripts/dev/cpp/`. Add an explicit
`-DataRoot E:/example_files/raman` option for this user-supplied corpus; otherwise
resolve Raman fixtures in the sibling `streamfind.data` repository. Do not
hard-code the drive in C++ or official tests. The data repository was verified
at `C:/Users/cunha/Documents/GitHub/streamfind.data`; a worktree-aware resolver
must locate the main checkout through Git metadata before looking for its
sibling. Do not assume the parent of `.worktrees/raman_development` is the
parent of the main repository. Keep supplied data read-only.

Use this worktree's repository-local `.venv` for any Python development script,
verifying its interpreter before execution. Use the supported CMake/Ninja
MinGW-w64 UCRT64 GCC stack. Place build trees in `tmp/build`, projects in
`tmp/projects`, and per-run logs in `tmp/logs`, all inside this worktree.
No R compilation is needed for this plan or C++-only work because the R
package is not being changed. If it is later changed, its required
`devtools::load_all()` validation still applies.

After configuring the supported preset with Raman enabled, the intended
narrow validation sequence is:

```powershell
cmake --build tmp\build\mingw-ucrt64 --target streamfind_raman_plugin
cmake --build tmp\build\mingw-ucrt64 --target streamfind_aggregate_catalogue
# Build each newly registered Raman test executable before running CTest.
ctest --test-dir tmp\build\mingw-ucrt64 -L raman --output-on-failure
```

Register new Raman tests with the `raman` label. Verify catalogue/column
schemas through the native SDK generator; inspect Turtle formatting changes
for unrelated rewrites. Run affected generic workflow/plugin tests only when
their boundaries changed. Capture concrete test counts, numeric differences,
elapsed time, and peak memory for corpus checks; do not invent performance
thresholds before measuring the baseline.

## 10. External references and remaining evidence needs

These sources inform format research and validation, not runtime dependencies:

- [Andor SDK2 guide](https://andor.oxinst.com/downloads/uploads/Andor_Software_Development_Kit_2.pdf):
  vendor calibration/acquisition terminology.
- [sif_parser source project](https://github.com/fujiisoup/sif_parser):
  reference for version-aware SIF reading, calibration, and timestamp handling;
  audit the exact source revision and license before using it as an oracle or
  adapting any implementation.
- [WITio original software project](https://github.com/ElsevierSoftwareX/SOFTX-D-20-00088):
  research reference for WITec project/data tags and transformations; verify
  the relevant version and attribution requirements before implementation.
- [Shimadzu FTIR algorithms](https://www.shimadzu.com.tw/service-support/technical-support/analysis-basics/tips-ftir/algorithms.html):
  confirms that IRsolution SMF can carry sample/background information needed
  for atmospheric correction. This is context, not a complete binary schema.
- [Shimadzu FTIR data storage](https://www.shimadzu.com/an/service-support/technical-support/ftir/tips_and_tricks/storage.html):
  explains original and processed spectral records in IRsolution's data tree.

Implementation must resolve four evidence gaps explicitly: SMF numeric/unit
ground truth, exact WIP graph/export pairings, confirmed ASC unit/calibration
declarations for unlabeled axes, and acquisition semantics for individual-file
sequences. These do not prevent starting the native reader/model and ASC
entry-operation work. They do prevent claiming scientifically validated
coverage of an undecoded format or inventing time/calibration values.
