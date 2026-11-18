# Implement the bundled ASM plugin

Deliver a validated liquid-chromatography (LC) JSON file from connected `sfms:chromatogramPeaksTable` and `sfms:analysesTable` results plus any required acquisition details. Each input row becomes one peak in its own analysis and chromatogram's measurement. Also support reading and writing complete LC and mass-spectrometry (MS) documents, including their data cubes. The attachments provide example instances. The release schemas define validation, and the StreamFind producer defines the meaning of each source column.

## 1. Scaffold the plugin and pin its resources

- [ ] Follow `cpp/plugins/statistics/CMakeLists.txt`, `plugin.json.in`, and `src/plugin_entrypoint.cpp` for ABI registration, semantic catalogue generation, staging, installation and CTest. Use target `streamfind_asm_plugin` and manifest ID/domain `asm`. Use a C++ namespace such as `streamfind::asm_format` because `asm` is a C++ keyword. The parent CMake already discovers the directory.
- [ ] Put operation contracts under `semantic/`, SDK dispatch under `src/operations/`, shared I/O under `src/utils/`, and the LC projection under `src/models/`. Put plugin-owned mapping specs under `resources/specs/`, upstream assets under `resources/asm/`, and small test instances under `tests/fixtures/`. Install runtime resources alongside the plugin. Resolve them relative to the loaded plugin in a packaged worker, including when the current directory is elsewhere.
- [ ] Use upstream revision `cc271e34e145fa5b459b17c9aab7c9709971dfcb`, inspected for this plan. For each row below import the embedded schema from `json-schemas/adm/<release>/<basename>.tabular.embed.schema.json`. The sibling `.tabular.schema.json` is the readable reference edition for checking mappings. Record source path, revision, SHA-256, schema identifier, dialect and applicable upstream notices. Preserve original resource bytes. [Upstream tree and licence](https://gitlab.com/allotrope-public/asm/-/tree/cc271e34e145fa5b459b17c9aab7c9709971dfcb)

| Release / basename | Exact instance manifest (registry key) |
| --- | --- |
| `liquid-chromatography/REC/2026/09` / `liquid-chromatography` | `http://purl.allotrope.org/manifests/liquid-chromatography/REC/2026/09/liquid-chromatography.manifest` |
| `mass-spectrometry/REC/2022/06` / `mass-spectrometry` | `http://purl.allotrope.org/manifests/mass-spectrometry/REC/2022/06/mass-spectrometry.tabular.manifest` |

The LC release has a JSON manifest under `manifests/liquid-chromatography/REC/2026/09/`. The MS 2022/06 release has both schemas and a test instance in Git, but its dated manifest resolves separately to Turtle and identifies the same dated MS schema. Record that manifest's provenance in the registry. Runtime lookup only needs the exact manifest string and the local schema. The LC embedded bundle includes hierarchy, detector, fraction-collection, cube and unit schemas with their own identifiers. Verify that every referenced identifier resolves offline for each release.

**Checkpoint:** build the plugin, inspect the staged manifest/catalogue and enumerate both schema bundles from the installed resource registry.

## 2. Establish validation before implementing exports

Both domain schemas declare `https://json-schema.org/draft/2020-12/schema`. The LC schema uses `prefixItems` and references adjacent to other constraints. The validator in `cpp/vendor/json-schema-validator` documents Draft 7. The earlier plan assumed that validator could establish ASM conformance. A successful parse or validation of a positive example cannot establish dialect support.

- [ ] Keep the existing vendored nlohmann parser for the document. Use a native evaluator that supports Draft 2020-12 and embedded resources with nested `$id` values. Verify available build dependencies first. If another dependency is needed, document its version, licence, Windows UCRT64/Linux build and installation impact for the dependency decision. Continue the resource and mapping work in parallel. Keep the original dialect and schemas intact.
- [ ] In a native test, compile both embedded schemas using a local-only resolver. Validate a positive fixture for each. Mutate an LC tuple element constrained by `prefixItems` and a referenced quantity's value or unit. Assert rejection at the offending instance path. Also exercise a small Draft 2020-12 case where a sibling of `$ref` rejects an otherwise valid value. A dialect mismatch, unresolved identifier or failed negative control blocks release of validated I/O.
- [ ] Implement one shared path: parse JSON → read exact `$asm.manifest` → select registry entry → check expected root → validate full instance. The root check also ensures the selected technique is actually present when JSON Schema `properties` alone permits absence. An explicit expected model must agree with the manifest. Return model ID, instance JSON Pointer, schema location and reason on failure.
- [ ] The writer calls the same validator and serializes the same DOM. Stage output beside the requested destination and publish only after validation and successful close. Exercise existing-file behavior, cancellation and I/O failure with an existing destination. Validate that its previous bytes survive failure. Declare a document-size limit consistent with worker memory since full-document validation retains a DOM.

**Checkpoint:** native tests demonstrate offline validation, useful error paths and preservation of an existing destination on failure.

## 3. Encode the two model specifications

Keep one JSON spec per supported manifest. Include aggregate root, operation capabilities, document/collection paths, source contract and field rules. Represent paths as key segments with explicit array iteration. Use JSON Pointer with concrete indices in diagnostics. A small fixed set of transforms (copy scalar, quantity with known unit, quantity from channel units) suffices for the first writer. Keep grouping and arithmetic in typed C++. Test that every mapping rule has an implementation and an example.

### LC REC 2026/09

Follow this instance path. Each `[]` denotes an array whose elements must be visited in order.

```text
liquid chromatography aggregate document
  liquid chromatography document[]
    measurement aggregate document
      measurement document[]
        processed data aggregate document
          processed data document[]
            peak list
              peak[]
```

The attached LC instance has one technique document and two measurements, “single channel” and “UV spectrum”, each containing three peaks. The reference schema composes requirements through `allOf`. The LC technique document requires an analyst. The shared hierarchy requires its measurement collection and, per measurement, sample and device-control documents. The sample needs a sample identifier. Device controls include a device type. LC also requires a chromatography column document and selects a detector schema through `anyOf`. The absorbance cube branch can require a chromatogram cube, and the mass chromatogram detector branch requires a mass chromatogram cube. An injection document, when supplied, requires volume setting, identifier and time. Peak entries use a `oneOf` for retention time or retention volume. Select retention time for StreamFind's time-domain peaks. Validate the full envelope to apply all inherited and detector-specific conditions. [LC schema](https://gitlab.com/allotrope-public/asm/-/blob/cc271e34e145fa5b459b17c9aab7c9709971dfcb/json-schemas/adm/liquid-chromatography/REC/2026/09/liquid-chromatography.tabular.schema.json)

### MS REC 2022/06

In the example, follow `mass spectrometry aggregate document → mass spectrometry document[] → processed data aggregate document → processed data document[]`. Each of its five processed documents has a data cube and a peak list. Four lists start with mass and peak height, and one starts with retention time and peak height. Preserve cube structure, dimension and measure arrays, units, order, and other fields through read/write.

In the schema the technique item requires analyst and submitter, and the processed item requires `data format specification type`. The domain schema declares a `peak list` property at the root, while the example places the lists inside processed documents. Test a nested peak mutation against the complete compiled schema and record which constraints actually apply there. Reader traversal uses the observed instance path. Document permissive validation separately from fields the reader recognizes. Initial MS writing serializes a complete MS document obtained from the reader or supplied through its JSON contract. [MS schema](https://gitlab.com/allotrope-public/asm/-/blob/cc271e34e145fa5b459b17c9aab7c9709971dfcb/json-schemas/adm/mass-spectrometry/REC/2022/06/mass-spectrometry.tabular.schema.json)

## 4. Implement and connect three operations

| Proposed operation | Inputs | Output and acceptance condition |
| --- | --- | --- |
| `asm.read_document` | File path and optional expected model | Publish the entire validated instance as a declared JSON result through `PluginProjectAccess::emit_result`. A downstream writer can consume it directly. |
| `asm.write_document` | Connected ASM JSON result and destination path | Validate and publish one file. Reading it again yields equal JSON, including unknown permitted fields, array order and numeric values. |
| `asm.export_chromatogram_peaks` (Write ASM ChromPeaks) | Required `chromatogramPeaksTable` and `analysesTable`, destination, scoped acquisition parameters. Connect headers and full chromatograms when needed. | Build one LC technique document per represented analysis and one measurement per chromatogram. Validate, write, and return counts, metadata sources, omissions and destination. |

Declare the operations, parameters, inputs, outputs and domain in the plugin Turtle files. Follow `cpp/core/semantic/operations.ttl` for JSON/file workflow conventions and the statistics plugin for native dispatch. Reference the existing mass-spec table contract at the catalogue boundary. Verify both the standalone ASM catalogue and aggregate catalogue expose a connectable port. Read the connected result through the host's typed batch API. Keep mass-spec source headers, its DLL and direct project SQL outside the implementation dependencies.

### Build the LC context from the acquisition

1. Read the distinct `analysis` values in the peaks and join each to exactly one connected analysis row. Ignore unused analyses. Report a missing or ambiguous join before writing. The importer constructs `analysis` from the source name and, for multi-analysis files, the logical analysis name. Use `(file_path, analysis_index)` to address that logical acquisition and `(analysis, chromatogram_index)` to address its measurement. Check the channel identifier where available.
2. Read the acquisition summary already published by the mass-spec importer in `src/operations/base.cpp`. It supplies file name/path, format, logical index, source analysis number, reader timestamp, RT range and counts. Map fields only where the ASM definition matches. The analysis name identifies a StreamFind result. Use a verified sample identifier for the sample document and a verified injection timestamp for injection time. A file stem and a summary timestamp alone establish neither fact.
3. Reuse vendor extraction in the owning mass-spec plugin. For richer metadata, extend the importer and its published analysis/header contracts with nullable, documented fields where a reader can recover real values. Inspect format-specific records for sample, instrument, column, acquisition software, operator and injection metadata. Record a per-format field inventory and test each new extraction against a known file. Current `get_hardware()` and `get_software()` implementations in several readers return empty collections. Treat unavailable values as missing. Extract once per selected acquisition at import and pass the published values through the SDK to ASM. A reimport can populate newly supported fields.
4. Take detector, channel, units and wavelength from the peak/header contracts. Join an optional `chromatogramsHeadersTable` using its `(analysis, index)`. For detector branches requiring a cube, connect the full acquisition's `chromatogramsTable` from the existing mass-spec loading operation. Verify channel, coverage and units, then build its cube from the acquired RT and raw signal. A missing required trace diagnostic should name the input port and group. Parameters provide descriptive metadata. Measured arrays come from the acquisition.
5. Construct and validate the complete LC document in the exporter. The caller connects the data and supplies missing details through the operation form. The generic reader/writer continues to handle complete existing ASM documents.

### Write ASM ChromPeaks parameters

Declare typed parameters and row schemas in the ASM semantic catalogue so workflow users can see the field names, units, scope and applicable detector branch. Derive requiredness from the chosen branch and the resolved acquisition values.

| Parameter scope | Proposed fields | Resolution |
| --- | --- | --- |
| Export | Destination path, shared analyst, shared instrument/device and column details where applicable | Use shared values only for analyses or measurements to which they apply. |
| Per analysis (`analysis` key) | Analyst, sample identifier/name, injection identifier, injection time with offset, injection volume with unit | Fill missing vendor values. If an injection document is requested, resolve all its required fields together. |
| Per measurement (`analysis`, `chromatogram_index` keys) | Detector/device type, channel signal unit, wavelength with unit, column details and applicable device settings | Check the detector branch and the meaning of the quantity before inserting each field. |

Use the published acquisition value first. Fill gaps from the most specific parameter row, then shared parameters. Flag conflicting explicit and acquisition values with both sources and the affected group. Reject duplicate parameter keys. Return all unresolved required fields together with their ASM paths and the parameters or ports that can supply them. Keep field provenance in the export summary. Optional missing fields may be omitted.

**Acceptance cases.** A vendor-rich acquisition needs only its outstanding parameters. A sparse acquisition succeeds with the appropriate scoped parameters and real trace. Two analyses from one file select different logical indices and sample identifiers. Two detector channels in one analysis retain separate units and traces. Conflicting values, a missing analysis row, and an unresolved required quantity each produce a group-specific diagnostic before output publication.

## 5. Project the peak rows

Use `cpp/plugins/mass_spec/semantic/artifacts.ttl` for the published contract, `src/utils/chromatograms.cpp::peak_columns()` within that plugin for runtime names, and its `src/operations/chromatograms/operations.cpp::find_chromatogram_peaks` for emitted values. Read projected columns by name via `PluginProjectAccess::read_typed_batches`. Copy values used after the callback. Group by analysis and chromatogram index, order peaks by retention time then peak ID, and verify the exported row count. Reject ambiguous duplicate row identities and empty inputs with a diagnostic. Check cancellation between batches.

| Source | LC field or use | Implementation rule |
| --- | --- | --- |
| `rt`, `rt_start`, `rt_end` | retention time, peak start, peak end | Write seconds. Require finite apex time and check available boundaries contain it. |
| `height`, `units` | peak height | Use primary height in the actual signal unit. The producer finds the raw apex. Use an explicit channel-unit mapping when needed. |
| `area`, `units` | peak area | Primary integration uses a linear baseline and time in seconds. For an mAU channel write mAU.s. Record missing/unsupported signal units and omit optional quantities whose units are unknown. |
| `fwhm`, `width` | peak width at half height, peak width | FWHM is measured at half height. Width is end time minus start time. Use seconds and document the integration-boundary definition. |
| `asymmetry`, `plates` | asymmetry factor measured at 10 % height, number of theoretical plates by peak width at half height | The producer calculates the former at 10% and the latter from FWHM. Check the formula and handle producer fallback values explicitly before enabling each mapping. |
| `analysis`, `chromatogram_index`, `chromatogram_id`, `peak_id` | group, cross-check channel, row identity | Peak ID is an integer allocated per run. Keep it in diagnostics. Populate ASM identifier only with a value satisfying its declared semantics and type. |
| `detector`, `channel`, `signal_type`, `chromatogram_type`, `wavelength_nm` | measurement context | Cross-check supplied context when available. Keep peaks from different channels in their bound measurements. |
| `eic_*` | per-peak trace windows | These may be padded or overlapping. Use a real measurement-level trace for any required detector cube. |
| Other fields | mapping inventory | Classify polarity, transition m/z, energy, raw/baseline values, noise, S/N, sharpness, point counts, integration fields and manual override against the pinned schema. Record every disposition. |

For example, an mAU row with `rt=12`, `height=0` and `area=17.5` produces this subset of one peak. A missing height uses the optional-field omission rule. Relative areas and heights require a defined denominator and complete measurement scope before they can be derived.

```json
{"retention time":{"value":12,"unit":"s"},"peak height":{"value":0,"unit":"mAU"},"peak area":{"value":17.5,"unit":"mAU.s"}}
```

**Checkpoint:** export two analyses sharing a chromatogram index, one with two channels, across at least two input batches. Read the file back and assert exact measurement assignment, peak counts, quantity units, preservation of zero and stable order. Run negative cases for a missing sample identifier, missing detector context, unsupported manifest, and an incompatible connected table.

## 6. Record model gaps and verify the packaged workflow

- [ ] Compare the spec inventory to all 38 producer columns. Give each a disposition (mapped, grouping/context, duplicate, deferred mapping, unavailable source metadata or candidate ASM gap). Report optional omissions with source row/group, field and reason. Store the reviewed model-wide inventory with the spec and keep per-export diagnostics outside the ASM instance.
- [ ] For each upstream candidate, record exact release, instance/schema path, desired meaning and unit, related terms checked including custom information, a reduced example and a proposed property or clarification. For example, investigate whether manual reintegration status has a standard per-peak location. For MS, investigate the scope of nested peak validation using the mutation test. Record a finding only after checking the compiled schema. Missing analyst data is a source-context issue. LC already defines the 10% asymmetry term.
- [ ] Register native tests in `cpp/plugins/asm/tests/CMakeLists.txt`. Keep positive and intentionally invalid fixtures small. Check both manifest mappings and strict DOM equality for LC/MS round trips. Use the full attached files for a local development check and record their validation outcomes, including any upstream sample discrepancies. Keep data-backed checks under `scripts/dev/cpp/`.
- [ ] Start native work with `sfdev env show` and `sfdev doctor --workflow native`. Check command help, then build `sfdev build backend --target streamfind_asm_plugin` and `sfdev build backend --target streamfind_aggregate_catalogue`. Build each new test executable explicitly. List tests with `sfdev test backend --list` and run `sfdev test backend --filter '^streamfind_asm_'` using that prefix for the registered CTest names.
- [ ] Run `sfdev package`. Start the current checkout's packaged service on its assigned port and verify executable path, operation discovery, peak-table connection, LC export and both model round trips from the installed resources. Keep projects under `tmp/projects/` and logs under `tmp/logs/`. Verify exports still validate with network access unavailable.
- [ ] Record the revision, resource checksums, tested validator/dialect, fixture results, package test and open gap findings in this plan. Move it to `.plans/completed/` when the packaged workflow passes.
