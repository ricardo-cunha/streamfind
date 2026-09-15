# Native Mass-Spectrometry Reader Expansion — Status and Development Plan

> **Status:** implementation substantially complete for the currently validated fixture families; broader format coverage, exact calibration, corpus validation, and release hardening remain open.
>
> **Scope:** native C++ and Rust readers with one shared public contract for Agilent, SCIEX, Bruker, Shimadzu, mzML, and mzXML.

## Goal

Provide native, lazy, cross-language mass-spectrometry readers that expose identical public behavior in C++ and Rust for logical-analysis selection, spectrum/chromatogram metadata, decoded arrays, raw flattened rows, persistence, and MCP operations.

Production readers must parse native files directly. Vendor DLLs, Clearcore, ProteoWizard, `msconvert`, and paired mzML files are development-only diagnostic or differential oracles and must never be runtime dependencies or fallbacks.

## Canonical architecture and contract

### Analysis model

Every source is represented through the normalized concepts:

```text
analysis_index          zero-based public logical-analysis index
source_analysis_number vendor/container analysis number when applicable
name                    deterministic logical-analysis name
analysis_count          number of logical analyses in the physical source
```

Project persistence and reopening use:

```text
file_path + analysis_index
```

Single-analysis formats expose index `0` and count `1`. Container formats expose one row per logical analysis and reject duplicate logical names.

### Spectrum access

The direct reader boundary is:

```text
C++:  MASS_SPEC_FILE::get_spectrum(index)
Rust: Reader::spectrum_data(index)
```

Headers remain lightweight. Payload arrays are decoded on demand. Native profile, line, sparse, and token representations are preserved; no implicit centroiding is used.

### Public raw-spectrum selection

`mass_spec.get_raw_spectra` is a flattened, table-like peak operation. There is no separate public spectrum-object operation.

Selection precedence is:

```text
analysis_names → indices → targets
```

Behavior:

1. `analysis_names` limits logical analyses before decoding.
2. A non-empty `indices` list selects only those zero-based spectra.
3. In index mode, targets do not further filter the selected spectra; all selected peaks are returned with `target_id = spectrum:<index>`.
4. If `indices` is omitted or empty, the existing targets framework is used.
5. Public retention times are seconds.
6. Out-of-range indices fail explicitly.

C++ and Rust indexed raw-spectrum requests were compared across Agilent MassHunter, SCIEX TOF, Bruker TSF/BAF, Shimadzu LCD, and mzML paths. Flattened rows and complete selected arrays matched for the validated fixtures.

## Completed implementation

### Shared C++/Rust reader and persistence layer

Completed:

- normalized analysis catalog and selected-analysis state;
- deterministic logical names;
- public analysis-index bounds checks;
- project insertion and reopening through `file_path + analysis_index`;
- shared spectrum-header and chromatogram-header schemas;
- lazy direct spectrum access for native readers;
- flattened raw-spectrum public output;
- semantic catalogue declaration for `analysis_names`, `indices`, `levels`, and targets;
- generated semantic projection synchronization.

### Agilent MassHunter

Implemented and validated for the supplied MassHunter acquisition:

- native `.d` detection;
- `AcqData` metadata parsing;
- `MSScan.bin` metadata and scan records;
- native LZF profile decoding from `MSProfile.bin`;
- native profile m/z reconstruction;
- polarity from `AcqMethod.xml`/`ionPolarity`;
- MS level, TIC, BPC, RT, precursor m/z, precursor intensity, and collision energy;
- lazy C++ and Rust spectrum decoding;
- C++/Rust header and selected-array parity;
- indexed public raw-spectrum selection.

Validated representative MS2 result:

```text
scan                 188171
level                2
polarity             1
array length         165344
retention time       188.16299438476562 s
precursor m/z        237.05221557617188
precursor intensity  528.0
collision energy     10.0
```

The Rust path uses cached native metadata for headers and does not decode profile arrays merely to derive header fields.

### Agilent ChemStation

Implemented/validated in the current development slice:

- legacy `MSD1.MS`/`DATA.MS` detection;
- legacy big-endian header and word-based directory offsets;
- retention-time index records;
- packed abundance decoding;
- public C++ spectrum exposure;
- conditional development smoke coverage for the supplied 2DLC `MSD1.MS` fixture;
- ChemStation `.ch` version 130 chromatograms;
- ChemStation `.UV` version 131 chromatograms;
- Rust/C++ public chromatogram parity for the validated slice.

The validated 2DLC slice contains 672 spectra with 20 points in the first spectrum. This does not constitute complete ChemStation/2DLC support.

### SCIEX WIFF / WIFF.SCAN

Implemented and validated:

- OLE compound-file detection and stream access;
- `.wiff.scan` companion discovery;
- sample-block discovery;
- native `Idx` parsing;
- source analysis names;
- logical-analysis selection;
- PAC compact MRM decoding;
- selected multi-experiment compact MRM decoding;
- sparse tagged Nitrosamine decoding for the validated grammar;
- native MRM chromatogram headers and arrays for validated families;
- native TOF public spectrum dispatch;
- sparse/raw token preservation without centroiding;
- native TOF precursor m/z and precursor intensity extraction;
- source-index-preserving scan reporting;
- C++/Rust parity for validated spectra and chromatograms;
- bounded indexed payload reads in both C++ and Rust.

Validated TOF fixture:

```text
public spectra       8964
MS1 rows             3653
MS2 rows             5311
selected MS2 index   20
scan                 261
array length         190
retention time       124.18999481201172 s
precursor m/z        430.3861999511719
precursor intensity  404.0
collision energy     0.0  (metadata absent)
```

The TOF warm indexed-read benchmark improved from hundreds of milliseconds to approximately `0 ms` after both implementations stopped rereading the complete `.wiff.scan` file for each spectrum.

### Bruker TSF

Implemented and validated:

- `.d` family detection;
- `analysis.tsf` and `analysis.tsf_bin` access;
- native SQLite metadata;
- type-3 Zstandard payload decoding;
- frame metadata;
- MS/MS parent metadata;
- lazy indexed spectra;
- precursor m/z, isolation window, charge, and collision energy propagation;
- native calibration provenance fields;
- C++/Rust public spectrum and selected-array parity.

Validated MS2 example:

```text
index                97
scan                 98
level                2
polarity             1
array length         1521
retention time       49.431331634521484 s
precursor m/z        922.0137939453125
precursor charge     1
collision energy     75.32083129882812
```

The current m/z calibration is an explicitly documented approximation with approximately `1.8736 ppm` maximum observed oracle difference in the validated frame sample. It must not be described as exact proprietary calibration.

### Bruker BAF

Implemented and validated:

- BAF family detection;
- native `analysis.sqlite` metadata access;
- `Spectra` and `AcquisitionKeys` metadata;
- native `ProfileIntensityId` handling;
- `0xBFA01001` DataVectorBlock validation;
- `0xEE77` decoder header validation;
- profile count and decoder-table checks;
- MSB-first bit reading;
- signed-delta reconstruction;
- zero-run expansion;
- both observed signed-delta encodings;
- lazy bounded profile-block reads;
- bulk point-count lookup using one `analysis.baf` file open during metadata construction;
- C++/Rust public spectrum and complete-array parity.

Validated representative profile:

```text
points             513287
nonzero bins       3499
maximum intensity  2140
```

The bulk point-count and bounded-block changes reduced Rust BAF cold indexed access from approximately `93132 ms` to approximately `54 ms`, matching the C++ baseline of approximately `55 ms`. Warm access is approximately `2–3 ms` in both implementations.

### Shimadzu LCD

Implemented and validated in both C++ and Rust:

- `adc.lcd` and `karl.lcd` detection;
- native TLM parsing;
- TIC/BPC chromatograms;
- MRM transitions;
- precursor/product m/z;
- polarity and collision energy;
- RT arrays and headers in seconds;
- C++/Rust header parity;
- point-for-point C++/Rust chromatogram-array parity.

Validated fixtures:

```text
adc.lcd   8 chromatograms, 21008 total points
karl.lcd  90 chromatograms, 45862 total points, 40 MRM traces
```

### mzML and mzXML

Existing native XML support is implemented for:

- mzML spectrum and chromatogram metadata;
- base64 binary arrays;
- 32-bit and 64-bit values;
- zlib-compressed arrays;
- RT unit conversion to seconds;
- precursor metadata;
- mzML/mzXML public schema parity;
- mzXML peak arrays and summary behavior.

Rust mzML now stores spectrum byte ranges and decodes only the requested `<spectrum>` slice through `spectrum_data(index)`, matching the lazy C++ design. The mzML/mzXML project parity test and reader tests pass.

## Performance work completed

### Native bounded reads

Completed for Rust BAF and both SCIEX TOF implementations:

- no full `analysis.baf` read for point-count lookup per spectrum;
- no full BAF profile-file read for selected profile decoding;
- no full `.wiff.scan` read for every selected TOF spectrum;
- selected payloads are bounded by native offsets and neighboring records.

### Measured direct-reader benchmark

The benchmark used one representative indexed spectrum per format and measured cold open plus decode and warm decode. Timings should be interpreted within each format because decoded point counts differ.

Representative point counts:

```text
Agilent MassHunter  165344
SCIEX TOF              190
Bruker TSF            1521
Bruker BAF          513287
mzML                     6
```

The benchmark reports whole milliseconds for presentation. Raw logs remain under `tmp/logs/` and are development-only.

## Remaining implementation work

The following items are still open and must not be represented as complete format support.

### Agilent

- validate all supplied MassHunter acquisitions through both public readers;
- support additional MassHunter instrument families and acquisition layouts;
- classify and decode all observed `SpectrumFormatID` variants, including centroid/MSPeak formats, or reject them explicitly;
- recover and validate `MSMassCal.bin`, `DefaultMassCal.xml`, `CalibrationID`, and `MassCalOffset` semantics;
- determine scan/segment-specific m/z calibration behavior;
- complete `MSD2.MS` and 2D correlation support;
- broaden ChemStation 1D/2D acquisition coverage;
- add confirmed DAD/UV/pump/TCC/auxiliary trace families;
- complete IMS frame metadata linkage and validated CCS/mobility calibration for the ion-mobility corpus;
- add MassHunter and ChemStation holdout fixtures.

### SCIEX

- generalize MRM transition/event assignment without marker-frequency heuristics;
- complete Mix1 tagged-cycle reconciliation;
- resolve Nitrosamine outer-fragment and sparse-tail behavior;
- parse all per-period/per-experiment `MassRangeEx` lists with boundaries;
- decode pump and auxiliary chromatogram families;
- improve unsupported-grammar diagnostics with file/sample/experiment/transition context;
- validate TOF calibration and calibrated m/z arrays across additional variants;
- validate more WIFF container layouts and malformed/truncated inputs;
- complete multi-file and multiple-logical-analysis persistence differential tests.

### Thermo

- validate pre-v64 layouts before claiming support beyond the current version-66
  subset;
- validate additional Tribrid, DIA, and multi-controller variants and reject
  unsupported controller/layout combinations explicitly;
- expose and validate UV/PDA and auxiliary channels where the public contract
  supports them;
- add arbitrary XIC/EIC support only after native trace metadata and array
  semantics are established;
- validate precursor intensity behavior across files where native metadata is
  absent or represented differently;
- broaden malformed/truncated pointer-chain, packet, centroid, profile, and
  timestamp coverage.

### Bruker TSF

- replace the validated approximate m/z calibration with vendor-exact calibration where recoverable;
- validate calibration across additional acquisition software and calibration rows;
- broaden PASEF/mobility dimension preservation and diagnostics;
- test malformed/truncated Zstandard blocks and unsupported frame variants;
- validate all persisted TSF public paths and error parity.

### Bruker BAF

- generalize object lookup through `.baf_idx`, `.baf_xtr`, and metadata rather than relying on representative object-offset assumptions;
- classify/decode additional profile, line, APCI, and later-block variants;
- decode native profile m/z arrays and exact calibration from `Transformators.Blob`, `FrameMzCalibration`, and digitizer constants;
- expose native line arrays and profile m/z arrays through both public APIs;
- validate the six known BAF acquisitions across MS1/MS2 and differing profile counts;
- add malformed-header, truncated-payload, impossible-count, overflow, and unsupported-variant tests;
- validate BAF chromatogram support separately;
- complete BAF project persistence and multi-file reopening tests.

### Shimadzu

- broaden corpus validation beyond `adc.lcd` and `karl.lcd`;
- add malformed LCD/TLM diagnostics;
- identify and implement additional native LCD stream families;
- validate persisted Shimadzu analysis reads.

### mzML/mzXML

- benchmark larger mzML files with multiple large spectra;
- validate indexed slice boundaries for namespaces, self-closing forms, multiline binary text, and unusual XML formatting;
- add malformed XML/base64/zlib/array-length tests;
- consider file-backed or memory-mapped indexed access for very large mzML files rather than retaining the complete source bytes in memory;
- validate broader mzXML variants and RT/unit edge cases.

### Cross-language and public API

- permanent indexed-selection regression coverage now exists in `rust/crates/mass-spec/tests/project_indexed_persistence.rs` and `core/domains/mass_spec/tests/project_indexed_persistence.cpp`, covering analysis filtering, one/multiple indices, target precedence, empty/omitted indices, level filtering, out-of-range indices, and persistence/reopen;
- add explicit instrumentation or counters proving unrequested payloads are not decoded;
- permanent C++/Rust MCP differential coverage now exists in `tests/mass_spec_mcp_differential.py`; it compares tool names/input schemas and complete operation responses for portable multi-file mzML, plus opt-in Agilent, SCIEX TOF, Bruker TSF/BAF, and Shimadzu fixtures via `STREAMFIND_MCP_VENDOR_FIXTURES`; current validation passed portable mzML, SCIEX TOF, and Shimadzu LCD; unavailable external fixtures are skipped explicitly;
- validate all public aggregate operations (`get_raw_spectra_eic`, MS1, MS2, chromatograms) after indexed changes;
- verify project persistence after reopen for every supported family;
- keep semantic catalogue, generated projection, C++, Rust, and MCP schemas synchronized.

## Recommended development order

Work in `mass_spec_reader_extension` in the following order. Each phase is a
gate for the next one; a parser is not promoted from “validated slice” to
“broader support” until both C++ and Rust produce the same public results and
reject malformed or unsupported input explicitly.

### Phase 1 — shared reader hardening and validation harness

1. Add lightweight, portable regression coverage for lazy decoding,
   indexed-selection precedence, logical-analysis persistence/reopen, and
   error parity. Keep fixture-dependent checks in `scripts/dev/` and do not
   add external vendor files to CTest or Cargo tests.
2. Add opt-in counters or instrumentation proving that unrequested spectrum
   and chromatogram payloads are not decoded.
3. Run the complete C++/Rust MCP differential matrix after every parser phase,
   including portable mzML plus every available vendor fixture.

### Phase 2 — SCIEX validated-slice completion

1. Replace MRM marker-frequency heuristics with acquisition metadata-driven
   transition and event assignment.
2. Complete Mix1 tagged-cycle reconciliation and Nitrosamine outer-fragment
   and sparse-tail handling.
3. Parse all per-period/per-experiment `MassRangeEx` boundaries and add pump
   and auxiliary chromatogram families where native evidence is available.
4. Expand TOF calibration and WIFF/WIFF.SCAN layout validation across more
   files, logical analyses, and malformed/truncated cases.
5. Re-run C++/Rust multi-analysis persistence and complete-array differential
   checks before starting another vendor expansion.

SCIEX is first because it is the active hardening area and already has native
TOF, MRM, sparse, multi-analysis, and malformed-input infrastructure. Do not
add filename-specific branches or retain marker heuristics as a compatibility
path.

### Phase 3 — Thermo validated-subset expansion

1. Harden version-66 pointer-chain, packet, centroid, profile, and timestamp
   parsing against malformed and truncated inputs.
2. Validate pre-v64 layouts and explicitly reject unsupported layout families
   until they have independent evidence.
3. Validate Tribrid, DIA, and multi-controller variants, preserving controller
   identity and scan metadata.
4. Add UV/PDA, auxiliary, and arbitrary XIC/EIC support only when native trace
   semantics and public schema requirements are established.
5. Re-run C++/Rust corpus and MCP differential checks across every validated
   Thermo variant.

Thermo follows SCIEX because the current reader is native and validated but
bounded to one layout family. This phase widens a known parser rather than
introducing a speculative format interpretation.

### Phase 4 — Bruker calibration and safety

1. Replace the TSF approximate m/z calibration only after its provenance and
   equation are independently established; otherwise retain the approximation
   and document its measured tolerance.
2. Generalize BAF object lookup and classify additional profile, line, APCI,
   and later-block variants.
3. Implement and validate native BAF m/z calibration and expose line/profile
   arrays through both public APIs.
4. Add malformed-header, truncated-payload, impossible-count, overflow, and
   unsupported-variant checks for TSF and BAF.
5. Validate all persisted TSF/BAF paths and the six known BAF acquisitions.

Bruker follows SCIEX because calibration and BAF layout assumptions affect
large arrays and currently represent the largest correctness/performance risk
among the validated native readers.

### Phase 5 — Agilent corpus and calibration expansion

1. Validate all supplied MassHunter acquisitions through both public readers
   and classify every observed `SpectrumFormatID`.
2. Recover `MSMassCal.bin`, `DefaultMassCal.xml`, `CalibrationID`, and
   `MassCalOffset` semantics, including scan/segment-specific behavior.
3. Expand MassHunter instrument layouts, ChemStation 1D/2D coverage, and
   confirmed DAD/UV/pump/TCC/auxiliary traces.
4. Validate IMS frame linkage and CCS/mobility calibration only against a
   corpus that supplies independent evidence.

Agilent is intentionally after Bruker: its current slice is functional, while
the remaining work is broader acquisition-family and calibration coverage.

### Phase 6 — Shimadzu and XML edge coverage

1. Broaden LCD/TLM corpus validation, add malformed diagnostics, and validate
   persisted analysis reads.
2. Validate mzML indexed boundaries for namespace, self-closing, multiline,
   and unusual formatting cases; add malformed XML/base64/zlib/array-length
   checks.
3. Benchmark larger mzML files and decide whether file-backed or memory-mapped
   indexing is required.
4. Broaden mzXML variants and retention-time/unit edge cases.

### Phase 7 — release integration gate

1. Validate every supported family through public C++ and Rust reader/MCP
   paths, including all logical-analysis reopen cases.
2. Require exact parity or a recorded numeric tolerance for every metadata and
   array field.
3. Run semantic validation and projection checks after catalogue changes.
4. Complete provenance, dependency-license, and fixture-authorization review.
5. Clean disposable build and fixture artifacts, inspect the final diff, and
   commit only with explicit approval.

### Per-phase acceptance checklist

- both C++ and Rust implementations changed together when the public contract
  changes;
- the same logical analyses, public indices, headers, arrays, and errors are
  compared;
- retention times remain seconds at the public boundary;
- indexed selection decodes only requested payloads;
- malformed, truncated, overflow, and unsupported inputs fail explicitly;
- external fixtures and oracle outputs remain development-only under `tmp/`;
- no runtime SDK, DLL, Clearcore, ProteoWizard, or mzML fallback is added.

## Validation currently completed

The following checks passed during the current implementation:

```text
Rust reader suite
Rust Bruker BAF test
Rust SCIEX reader tests
Rust mzML/mzXML project parity test
C++ Release MCP build
Rust Release build/check
C++/Rust selected spectrum array comparisons
C++/Rust indexed flattened-row comparisons
C++/Rust Shimadzu chromatogram comparisons
semantic RDF/TriG/SHACL validation
generate_projection.py --check
rustfmt --check
git diff --check
```

R package validation remains environment-blocked because Rtools is unavailable:

```text
Error: Could not find tools necessary to compile a package
```

External vendor fixtures are conditional development inputs and are not release tests.

## Release and integration gates

Before declaring the expansion complete:

1. Reconcile all current worktree changes against the integration branch.
2. Run complete C++ and Rust public reader/MCP suites.
3. Run portable tests without external vendor fixtures.
4. Run opt-in external corpus and oracle differential tests.
5. Validate malformed/truncated/unsupported inputs for every native family.
6. Validate exact or documented-tolerance C++/Rust metadata and array parity.
7. Validate all logical-analysis persistence and reopen paths.
8. Resolve or explicitly document calibration approximations.
9. Run semantic validation and projection checks after catalogue changes.
10. Run `scripts\clean-build-temp.cmd` before commit, preserving only approved development logs/scripts as appropriate.
11. Review the final diff and create a commit only with explicit user approval.

## Non-negotiable constraints

- no runtime vendor DLL, Clearcore, ProteoWizard, or mzML fallback;
- no compatibility shim, duplicate source tree, legacy execution path, or filename-specific parser branch;
- no silent centroiding or representation substitution;
- no speculative calibration equation presented as exact;
- no secrets, credentials, API keys, passwords, tokens, or connection strings in source, plan, logs, or summaries;
- external fixtures, generated reports, build trees, and benchmark artifacts remain under `tmp/` and are not committed.

# Legal and provenance boundary

The native mass-spectrometry readers are maintained as independently authored
C++ and Rust implementations. Their documented development evidence consists
of lawfully obtained or project-authorized data files, publicly available
information, independent byte-level analysis, and observable outputs from
development-only differential oracles. This is a process record, not a legal
certification or warranty.

The production readers must not incorporate vendor source code, decompiled
implementation code, copied SDK headers, confidential vendor documentation,
vendor binaries, or translated oracle code. Vendor SDKs, ClearCore,
ProteoWizard, `msconvert`, `baf2sql`, paired mzML files, debugger traces, and
restricted sample files remain development-only material and must not become
runtime dependencies or release contents.

The implementation process does not by itself resolve contractual,
trade-secret, copyright, trademark, or technological-protection obligations.
The detailed per-vendor evidence and review checklist are maintained in
`.plans/provenance/vendor-format-provenance.md`.

SCIEX classic WIFF/WIFF.SCAN decoding is a separate boundary from WIFF2
encrypted metadata. streamfind does not decrypt or circumvent WIFF2 metadata;
no WIFF2 key recovery, decryption, or access-control bypass may be added
without dedicated German/EU technical and legal review.

## Items that deserve provenance review

### 1. Binary offsets and field layouts

Examples include:

```text
SCIEX Idx record sizes and offsets
SCIEX DDERealTimeData layouts
Agilent MSScan.bin record sizes
Bruker BAF DataVectorBlock offsets
Shimadzu TLM record layouts
```

These appear to be functional format facts rather than copied expression. However, document where each was learned:

```text
public documentation
legally obtained sample files
independent byte-level analysis
development-only oracle comparison
```

Avoid citing restricted vendor documentation as the source unless legal counsel confirms that use is permitted.

### 2. Calibration equations

The Bruker TSF and provisional BAF calibration logic deserves additional review.

The plan correctly describes the TSF equation as an approximation and records the observed ppm error. It should not be represented as a vendor-exact implementation unless independently validated.

For every calibration rule, record:

```text
equation provenance
whether it was independently derived
whether it came from public literature
whether it was inferred from observed data
validation error and fixture set
```

### 3. Decoder algorithms

The following are common technical algorithms or format mechanisms:

```text
LZF decompression
Zstandard decompression
base64 decoding
zlib decompression
OLE compound-file traversal
bit-packed delta decoding
```

I did not find copied vendor source for these in the production reader paths. The BAF bit reader and delta decoder are project-owned implementations, but their provenance should still be documented as:

```text
clean-room implementation from observed format behavior
```

rather than implying they were copied from vendor software.

### 4. Development-only oracle material

The plan and temporary development history refer to:

```text
ClearCore
ProteoWizard
vendor DLLs
baf2sql_c.dll
WinDbg/CDB
paired mzML files
```

These references are acceptable in development documentation if they are not distributed as production dependencies. The release package should not include:

- vendor DLLs;
- vendor SDK headers;
- decompiled output;
- proprietary documentation;
- external vendor files;
- oracle conversion scripts;
- confidential traces.

## Third-party code found

The repository contains third-party components that require their own licence audit:

```text
core/vendor/zstd
core/vendor/zlib
core/vendor/pugixml-1.14
core/vendor/simdutf
core/vendor/openbabel/openbabel-3-2-0
core/vendor/json-schema-validator
```

The relevant licences appear to be retained in the repository:

- Zstandard: BSD/GPL dual licensing;
- pugixml: MIT;
- Open Babel: GPL;
- zlib and other dependencies: review their individual licence files.

The Open Babel resources are clearly third-party GPL material and must remain separated from the originality assessment of the vendor readers. Preserve their original copyright and licence files in every distributed package.

Also audit the Rust dependency tree, especially:

```text
cfb
quick-xml
duckdb
zstd
flate2
base64
regex
```

The fact that a crate is commonly used does not replace checking its exact licence and transitive dependencies.

## Where the disclaimer should go

### Primary location: root `README.md`

Add a section near the existing development-status section, preferably before `## Repository layout`:

```markdown
## Vendor-format compatibility and trademarks

streamfind is an independent open-source project and is not affiliated
with, sponsored by, or endorsed by Agilent, SCIEX, Bruker, Shimadzu,
Waters, or any other vendor referenced in the compatibility documentation.

Vendor names, product names, trademarks, and file-format names are used
solely to identify compatibility with files produced by those systems.
streamfind does not redistribute vendor software, vendor SDKs, vendor DLLs,
or vendor proprietary runtime components.

Compatibility is based on the native file structures and datasets validated
by the project. Support for a particular vendor format, instrument family,
acquisition mode, or calibration variant is not implied merely because a
reader exists.
```

This is the most important placement because it is visible from the repository landing page and applies to all components.

### Package-facing documentation

Also add a shorter version to:

```text
core/README.md
rust/README.md
docs/status.md
```

For `docs/status.md`, place it immediately after the “Compatibility scope” section. This matters because users may read the documentation site without first visiting the repository README.

### Distribution metadata

Add a `NOTICE.md` or equivalent distribution notice if the release archives are distributed independently from the source repository. It should list:

- third-party libraries;
- their licences;
- any required attribution;
- the independent-vendor compatibility notice.

The existing root `LICENSE.md` is the GPLv3 licence text, but it is not a substitute for a third-party attribution notice.

## Where not to put it

Do not put the vendor non-affiliation disclaimer only:

- inside individual parser source files;
- inside C++/Rust namespaces;
- in test files;
- only in the development plan;
- only in the MCP response;
- only in release notes.

Those locations are too easy to miss and do not adequately cover binary distributions.

## Recommended provenance file

Before public release, create a maintained internal or public document such as:

```text
.plans/provenance/mass-spec-format-provenance.md
```

For each format, use a table like:

```markdown
| Format | Knowledge source | Restricted material used | Production dependency |
|---|---|---|---|
| Agilent MassHunter | Public/sample-file analysis | None known | None |
| SCIEX WIFF | Public/sample-file analysis | None known | None |
| Bruker TSF/BAF | Public/sample-file analysis | None known | None |
| Shimadzu LCD | Public/sample-file analysis | None known | None |
| mzML/mzXML | Public specifications | None known | None |
```

For each row, add:

```text
- sample-file provenance;
- whether an NDA or SDK licence applied;
- whether reverse engineering was contractually restricted;
- whether an oracle was used;
- whether the production implementation was independently rewritten;
- known calibration limitations;
- unsupported variants.
```

## Overall risk assessment

Based on the code structure alone:

| Area | Assessment |
|---|---|
| Original StreamFind parser code | No obvious copying found |
| Vendor source/header inclusion | None found in production reader paths |
| Vendor runtime dependency | None found in production reader paths |
| Functional binary layout facts | Usually lower risk, jurisdiction-dependent |
| Reverse-engineering contractual restrictions | Requires human/legal review |
| Calibration provenance | Needs explicit documentation |
| Third-party library licensing | Requires release audit |
| Trademark/non-affiliation notice | Should be added to README/docs |
| Legal release clearance | Not established by this audit |

The engineering approach is consistent with a clean-room native-reader distribution, but counsel should review the provenance of the format discoveries, any vendor agreements, and the final dependency/licence inventory before commercial or public redistribution.
