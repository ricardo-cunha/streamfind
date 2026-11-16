# Future plan: typed result-batch ABI optimization

## Purpose

Remove avoidable `nlohmann::json` materialization when native operations consume
Arrow-backed result tables. Keep JSON reads for small configuration/metadata
tables, while enabling high-volume operations to process typed batches directly.

## Scope

- Extend the SDK-facing `PluginProjectAccess` with a typed batch-read callback.
- Route the existing native `streamfind_plugin_batch_column` buffers through
  `PluginHostAccess` without converting them to JSON first.
- Preserve callback lifetime rules: typed column buffers are valid only during
  the callback and must not be retained by plugins.
- Migrate the high-volume mass-spec consumers first:
  - quantification response extraction;
  - chromatogram points and peaks;
  - NTA feature/array scans;
  - fragmentation table scans where numeric columns dominate.
- Leave small method, assignment, analysis, and metadata reads on the existing
  JSON API until profiling shows a benefit.

## Implementation stages

1. Define the typed callback contract and document column types, null bitmaps,
   array/string lifetime, cancellation, and error propagation.
2. Implement the SDK bridge using the existing ABI batch-column representation;
   do not add a second serialized data path or a legacy compatibility layer.
3. Add typed-batch test doubles and ABI/SDK contract coverage, including nulls,
   strings, numeric values, booleans, arrays, cancellation, and exceptions.
4. Migrate quantification and benchmark it against the current JSON-batch path
   using the SCIEX fixture. Compare peak memory, elapsed time, rows scanned, and
   response-table equivalence.
5. Migrate the remaining high-volume mass-spec consumers incrementally, with
   one focused validation per operation family.
6. Update SDK/plugin documentation and record the measured improvement.

## Follow-up workstream: selective native decoding across format parsers

The operation boundary now requests only selected chromatogram indexes, but a
format parser may still decode the complete native payload before exposing
those arrays. Audit every supported reader and optimize only where this occurs.

1. Inventory each reader’s `get_chromatograms_headers(indices)` and
   `get_chromatograms(indices)` implementation, including SCIEX, Thermo,
   Bruker, Agilent, and text-based formats. Record whether selection happens
   before payload I/O, during decoding, or only when copying the result.
2. Define a vendor-neutral reader contract for public indexes, selected header
   order, missing/empty traces, retention-time arrays, and cancellation/error
   behavior. Keep the existing index-based API unless a measured limitation
   requires a narrow extension.
3. For SCIEX, map persisted header indexes and transition metadata to native
   experiment/channel records and make compact and sparse/tagged MRM decoding
   retain only requested transition traces. Preserve metadata-only sample
   blocks, transition time windows, TIC/BPC channels, and existing errors.
4. Apply equivalent selective decoding to other parsers where the audit shows
   unnecessary payload work. Do not add format-specific behavior to the
   generic chromatogram operation; keep selection inside each owning reader.
5. Add reader-level tests with trace/decode instrumentation for every parser
   that is changed. Prove that unrequested payloads are not decoded and that
   selected headers, indexes, retention times, and intensities are unchanged.
6. Benchmark selected-channel versus all-channel loading for representative
   fixtures, measuring decoded traces/bytes, peak memory, elapsed time, and
   emitted point-row equivalence. Include the large SCIEX Nitrosamine fixture.

If a parser already performs selective I/O/decoding, record that result and add
only a regression test; do not rewrite a correct implementation for symmetry.

This workstream must not change result-table contracts or make quantification
depend on one vendor’s metadata. The generic reader API remains index-based
and vendor-neutral, while format-specific optimizations stay in their owning
parser.

## Constraints

- Keep the Arrow result-store format and table contracts unchanged.
- Do not expose backend-owned Arrow buffers beyond the callback lifetime.
- Preserve plugin cancellation and progress reporting.
- Do not change app-facing result requests; those remain result-store reads.
- Follow the current C++ ABI/package test rules and avoid changes under
  `cpp/vendor/`.

## Acceptance criteria

- Typed consumers no longer construct per-row JSON objects for their large
  result-table inputs.
- Quantification produces byte/schema-equivalent response rows to the current
  implementation on representative SCIEX fixtures.
- Native ABI, SDK, plugin, and package tests pass.
- The SCIEX quantification fixture shows materially lower peak memory and a
  measured improvement in response-extraction time.
- Every audited format is classified as selectively decoded, optimized, or
  intentionally unchanged with evidence.
- Changed parsers demonstrate that unrequested payloads are not decoded, while
  selected traces and existing special channels such as TIC/BPC remain
  identical.
- The callback contract and migration boundary are documented for future
  plugin authors.

## Provenance

Prepared as a future development stage after the quantification workflow work
in `mass-spec-expansion`. The current implementation uses bounded JSON batches;
this plan describes the subsequent ABI-level removal of that conversion.
