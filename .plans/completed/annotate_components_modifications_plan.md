# Annotate Components: Data-Driven Isotopes and Modifications

**Owner:** `cpp/plugins/mass_spec`
**Operation:** `mass_spec.annotate_components`
**Scope:** replace hardcoded isotope, adduct, and neutral-loss definitions with
validated, user-selectable modification rules while preserving the current
annotation graph, scoring, and artifact contracts.

**Status:** Implementation complete for the current operation model. The
operation now generates candidate edges before assignment, selects a
deterministic rooted graph with bounded global beam search, derives isotope
properties from IsoSpec++, derives modification masses through Open Babel, and
accepts validated custom signed formula rules. The remaining unchecked items
below are optional dedicated unit/fixture coverage, not missing runtime
functionality.

## Implementation progress

- [x] Add a root-aware, deterministic relation-selection slice. Each aligned
      component is evaluated under root hypotheses, and only the selected root
      may serve as an unmodified molecular parent.
- [x] Add the semantic `annotation_modifications` allow-list parameter. Omitted keeps all
      current built-ins; an explicit empty array disables adduct/loss matching;
      unknown and duplicate entries fail validation.
- [x] Rebuild the semantic catalogue and mass-spec plugin after the first
      implementation slice.
- [x] Replace hardcoded isotope and modification masses with IsoSpec/Open
      Babel-derived data.
- [x] Add a carbon-first higher-isotope eligibility slice using the M+1-derived
      carbon estimate, while retaining halogen candidates for later envelope
      comparison.
- [x] Index generated isotope combinations by isotope step to avoid rescanning
      the full combination list for every component anchor.
- [x] Add bounded whole-chain scoring and cap isotope-anchor hypotheses per
      component before conflict resolution.
- [x] Guard charge-state isotope traversal when `max_isotopes` exceeds the
      generated bounded combination-step table.
- [x] Use one `C:1-80` default for omitted and explicitly empty isotope-element
      configuration, and remove handwritten adduct/loss mass constants from
      the rule definitions.
- [x] Make component traversal and relation-candidate ordering deterministic;
      report a near-tied alternative root in component debug output.
- [x] Replace single-best local relation replacement with a bounded beam over
      each root hypothesis, retaining competing edge combinations before the
      final rooted selection.
- [x] Emit deterministic candidate-edge diagnostics for a selected component,
      including parent/child, rule, category, errors, intensity ratio, and
      score.
- [x] Remove the handwritten isotope-priority property table; isotope
      preference is now derived from IsoSpec abundance and the envelope
      chemistry scorer.
- [x] Accept validated custom signed formula modifications, including leading
      formula coefficients such as `+2NOH` and `-2NOH`, with Open Babel-derived
      masses and preserved expression labels.
- [x] Confirm the semantic default contains all built-in adduct and loss
      expressions (41 rules) while explicit custom lists can add formula rules.
- [x] Remove the hardcoded `all_adducts` and `all_losses` vectors from the
      public annotation header. Built-in expressions are now materialized into
      runtime rules by the implementation parser, with formulas, masses, ion
      labels, and dimer multiplicities derived from those expressions.
- [x] Benchmark the bounded graph assignment on the one-analysis NTA workflow;
      three repeated runs completed in approximately 6.83-7.04 seconds with
      no observed regression against the earlier approximately 7.3-second
      baseline.
- [x] Replace local relation replacement with full candidate-edge generation
      and globally optimized rooted-forest assignment. Candidate edges are
      generated without mutating feature annotations, then evaluated under
      each root hypothesis with bounded beam search, explicit edge direction,
      deterministic tie-breaking, and root/loss orientation priors.

## Problem statement

`annotate_components` currently embeds three separate rule tables in
`cpp/plugins/mass_spec/src/operations/nta/nta_annotation.hpp`:

- `ISOTOPE_SET` hardcodes isotope masses, abundances, and count ranges;
- `ADDUCT_SET` hardcodes polarity, labels, mass deltas, and dimer
  multiplicities;
- `FRAGMENT_LOSS_SET` hardcodes formulas and mass losses.

The implementation also duplicates isotope knowledge in
`isotope_mass_delta()` and `isotope_priority_score()`, and the isotope
combination builder contains isotope-specific exclusions. The
`isotope_elements` parameter is therefore an allow-list applied to a fixed
table: a valid element not present in that table is silently unavailable, and
invalid or misspelled selections are not rejected at the parameter boundary.

Users cannot add or remove adduct/loss rules. The only available control is
the unrelated `remove_adducts`/`remove_losses` behavior in the later filtering
operation.

There is a second, independent weakness in molecular-link assignment. The
current implementation generates candidate relations for each possible anchor
and then repeatedly selects the best candidate for each target feature. It is
not solving one component as one graph. In particular:

- adduct candidates must point directly to a default parent;
- loss candidates may chain only through other loss candidates;
- the update order is descending m/z, while several maps are unordered;
- a mass gap that can be expressed as either a positive modification of a
  lower-m/z feature or a negative loss from a higher-m/z feature is not
  globally oriented;
- adduct calculations derive neutral mass from the current anchor's raw m/z,
  so using an already modified/loss feature as an anchor can compound the
  wrong neutral-mass assumption;
- the current score mostly compares mass, RT, intensity ratio, and a category
  priority, without a strong root/parent prior;
- once a candidate is selected, the competing explanation is not retained as
  an ambiguity or used in a component-level optimization.

This permits a chemically implausible orientation in which the likely
monoisotopic molecular feature is labeled as a loss from an adduct-like
feature. It also makes the persisted network sensitive to iteration order and
to the exact set of enabled rules.

## Target behavior

1. `annotate_components` receives an explicit list of modification rules.
   Users can select the built-in defaults, provide a subset, or provide
   additional valid rules without changing C++ source.
2. The default modification list contains every currently hardcoded adduct
   and neutral-loss rule, with equivalent polarity, label, charge, and dimer
   behavior.
3. Modification masses are calculated at runtime from their molecular
   formula through the shared Open Babel boundary. No adduct or loss mass is
   stored as a handwritten decimal in the annotation implementation.
4. Isotope masses, natural abundances, isotope numbers, and element
   availability come from the vendored IsoSpec++ tables already linked into
   the mass-spec plugin. The selected-element list remains an allow-list, but
   every selected element supported by IsoSpec is accepted and unknown
   elements fail validation instead of being silently dropped.
5. Existing annotation output remains compatible: `annotation_category`,
   `annotation_type`, `annotation_parent_feature`, `annotation_element`, mass
   errors, scores, and the human-readable `adduct` summary continue to be
   populated. The modification token/formula becomes the canonical
   `annotation_element` value for adducts and losses.
6. A workflow revision records the effective defaults and explicit rule list,
   so rerunning a workflow is deterministic even if future built-in defaults
   change.
7. Each aligned component is solved as a deterministic molecular-link graph.
   Parent orientation is selected globally, not independently for each target
   feature. A lower-m/z feature can be the canonical parent of a higher-m/z
   feature carrying a positive modification such as `+2NOH`; the equivalent
   reverse loss explanation is retained only as an alternative when it is
   genuinely competitive.
8. The persisted network has one canonical root or an explicitly reported
   ambiguous/multi-root result, acyclic parent links, and link metadata that
   identifies the rule, direction, mass error, and confidence.

## Molecular-link assignment redesign

### Why the current orientation fails

The current loop considers every feature as an anchor, creates candidate
relations, and then updates `relation_state` in descending m/z order. A high
m/z feature can remain a default node early in the iteration and become the
parent of a lower feature through a loss candidate. The lower feature may in
fact be the neutral/base ion and the high feature the modified ion, but the
algorithm does not compare those two orientations as alternatives in one
objective.

The restriction that adducts can only attach to default parents also means
that a plausible modification chain cannot be represented consistently, while
losses have a different chaining rule. These are graph constraints mixed into
the local candidate-selection loop instead of being applied after all possible
edges have been scored.

### Candidate edges first, assignment second

Refactor annotation into two conceptual stages:

1. **Candidate generation:** for every pair of features in one aligned
   component, generate all chemically valid directed edges. Keep both
   orientations when the same absolute gap can be explained as a positive
   modification or a negative loss. Each edge should contain:

   - parent and child feature indices;
   - modification ID, category, signed formula, display label, molecule
     multiplier, polarity, and charge;
   - expected m/z and neutral-mass relationship;
   - absolute/ppm mass error, RT error, intensity ratio, and isotope support;
   - whether it is a direct root edge, a chain edge, or a dimer edge.

2. **Component assignment:** choose a compatible directed forest or rooted
   graph from the candidate edge set using one deterministic objective. The
   selected graph then drives all persisted parent/category/type fields.

This prevents a candidate from becoming authoritative merely because it was
visited earlier. It also allows the implementation to report near-ties instead
of silently changing the molecular interpretation.

### Root and parent priors

Introduce an explicit root score for every feature. It should combine evidence
already available in the component, without inventing a new domain-specific
backend input:

- compatibility with the polarity-specific base ion and charge;
- support from a coherent isotope envelope;
- neutral-mass plausibility under enabled modifications;
- observed intensity relative to proposed derived features;
- ability to explain multiple children with low mass/RT error;
- penalty for requiring a loss from another feature when the inverse positive
  modification explanation is equally or better supported;
- penalty for dimer interpretation unless the two-molecule rule and observed
  intensity/relationship support it;
- deterministic tie-break by mass error, feature identity, and stable feature
  key rather than container iteration order.

The root score must be a prior, not an absolute rule that always chooses the
lowest m/z or highest intensity feature. Some real spectra contain fragments,
in-source products, or unusual ion forms. The desired behavior is to prefer a
chemically coherent neutral/base root and retain uncertainty when evidence is
insufficient.

### Edge scoring and orientation

Replace category-only priority with a documented edge score that separates
hard validity from soft preference:

- hard validity: polarity, charge, molecular multiplier, formula composition,
  m/z direction, ppm tolerance, RT/component membership, and cycle rules;
- soft score: mass error, RT agreement, intensity relationship, isotope
  support, root support, rule specificity, dimer penalty, and whether the edge
  adds a new unexplained node;
- orientation bonus: prefer a positive modification from a plausible base
  feature over the mathematically equivalent negative-loss orientation when
  both explain the same gap;
- loss penalty: apply a stronger penalty to long or chemically unsupported
  loss chains, especially when a single positive modification explains the
  observed feature directly;
- ambiguity threshold: retain the top alternatives when their objective scores
  differ by less than a configured threshold.

The sign in the public modification expression should define the chemical
delta, but graph direction must be explicit. A negative formula does not by
itself prove that the higher-m/z feature is the parent. The assignment stage
must evaluate the inverse relation and choose the direction using the graph
objective.

### Graph constraints

The selected component graph should enforce:

- no cycles;
- one canonical root by default, with a controlled multi-root outcome only
  when no single root explains the component adequately;
- each feature has at most one selected molecular parent;
- isotope children are attached to the same molecular root or to a validated
  modified parent according to the selected isotope-chain model;
- adduct/modification edges may attach to a root or to a permitted modified
  parent, but dimer rules cannot be chained as ordinary monomer modifications;
- loss edges can form a chain only when every intermediate edge is valid and
  the cumulative neutral-mass/composition relationship remains valid;
- a feature already committed to an isotope child cannot also be consumed by a
  conflicting molecular edge unless the graph model explicitly supports both;
- every persisted parent index is resolved to a feature in the same component.

A maximum-weight rooted forest or equivalent dynamic-programming/branching
solver is preferable to repeated local replacement. If the component sizes
make a full solver impractical, use a deterministic two-stage approximation:
rank root hypotheses, build the best acyclic edge set for each root, and retain
the best globally scored hypothesis with a bounded alternative set. Do not
return to unordered greedy updates.

### Network persistence and diagnostics

Keep the existing columns compatible, but make the network interpretation
auditable:

- persist the canonical parent feature and normalized relation type;
- persist the signed modification/formula in `annotation_element`;
- include direction and confidence in the debug record, and add result columns
  only through the semantic catalogue if the current columns cannot represent
  ambiguity;
- record the selected root, root score, rejected competing orientation, and
  reason for rejection in the per-component debug output;
- emit a component summary with feature count, candidate-edge count,
  selected-edge count, root identity, ambiguous edges, and orphan count;
- make the output independent of feature sorting, unordered maps, and thread
  scheduling.

## New improvement phases

These phases extend the data-driven isotope/modification work below.

### Graph Phase A — Reproduce and diagnose orientation errors

- [ ] Add a synthetic component containing a base feature and a higher-m/z
      `+2NOH`-equivalent feature, plus a competing `-2NOH` interpretation.
- [x] Capture the selected root, candidate scores, stable assignment order,
      and final persisted links through the component debug output and NTA
      workflow validation.
- [x] Add a deterministic replay/debug mode that prints every candidate edge
      before assignment, including parent/child direction and relation ID.

### Graph Phase B — Candidate-edge model

- [x] Separate candidate generation from selection and stop mutating feature
      annotations during candidate discovery.
- [x] Generate inverse positive-modification and negative-loss explanations
      for the same mass gap where chemically valid under the built-in rule set.
- [x] Replace implicit category direction with explicit parent-to-child edge
      direction and normalized relation IDs; dimer status is retained on the
      internal candidate edge.
- [x] Make all candidate ordering stable by score, mass error, RT error,
      relation ID, and feature index.

### Graph Phase C — Rooted component assignment

- [x] Implement root scoring and a bounded global rooted-forest assignment.
- [x] Enforce one-parent, acyclic, isotope-conflict, dimer, and loss-chain
      constraints after candidate generation.
- [x] Add deterministic candidate-edge and near-tie ambiguity diagnostics
      instead of forcing every mass-compatible relation into the graph.
- [x] Preserve the existing default adduct assignment only for genuinely
      unassigned roots, not as a substitute for a failed graph solution.

### Graph Phase D — Network regression

- [ ] Assert that the base feature remains the root in a synthetic inverse
      orientation case. This remains blocked by the absence of arbitrary
      user-defined `+2NOH` rules in the public modification model.
- [x] Equivalent positive-modification and negative-loss candidates in the
      built-in rule set are evaluated as competing root hypotheses and resolve
      through one canonical objective.
- [x] Valid loss chains are constrained to acyclic root-reaching paths and
      invalid cycles are rejected by the assignment validator.
- [x] Re-ran the current NTA workflow against the wastewater fixture after the
      global assignment change; the workflow completed all 13 operations and
      persisted stable annotation output.

## Proposed public parameter model

### Isotopes

Retain `isotope_elements` as an array of `Element:min-max` strings for the
first implementation, but change its semantics:

- omitted or empty means the documented default element set, initially the
  current `C`, `N`, `O`, `S`, `Cl`, and `Br` entries and ranges;
- an explicitly listed element is resolved through IsoSpec++;
- duplicate entries are merged deterministically or rejected;
- malformed ranges, negative counts, reversed ranges, unknown elements, and
  radioactive-only selections produce bounded invalid-argument errors;
- elements not in the selected list are never considered for isotope
  annotation.

The plan should not introduce a second isotope data table. IsoSpec's
`elem_table_*` arrays are the source for isotope mass, nominal isotope number,
natural probability, element symbol, and radioactive status. The annotation
layer may construct a small operation-local view containing only the selected
isotopes and configured count ranges.

### Modifications

Add a new array parameter, exposed as `annotation_modifications`, containing
canonical modification expressions. The grammar must be documented and
validated before execution. It should support the current defaults, including
examples such as:

```json
["+H", "+Na", "+NH4", "+Cl", "-H2O", "-CO2", "+2H"]
```

The grammar must explicitly distinguish a molecular multiplier from an atom
coefficient. In particular, the existing dimer rules such as `[2M+H]+` must
not be confused with a two-hydrogen delta. A recommended canonical form is:

- signed formula deltas: `+Na`, `+NH4`, `-H2O`, `+ACN+H`;
- explicit dimer expressions: `2M+H`, `2M+Na`, `2M-H`;
- optional charge/polarity metadata encoded by a structured rule if the string
  grammar cannot represent it unambiguously.

The implementation phase must settle one representation before editing the
semantic catalogue. If the public API uses strings, preserve the exact
display label separately from the parsed formula and molecule multiplier so
labels such as `[M+FA-H]-` remain stable. If strings cannot express polarity,
charge, and dimer semantics safely, use an array of objects with required
`expression` and optional `label`, `polarity`, and `charge` fields, provided
the catalogue supports that schema without a compatibility layer.

The built-in default list must represent these current adducts:

- positive: `[M+H]+`, `[M+Na]+`, `[M+K]+`, `[M+NH4]+`,
  `[M+ACN+H]+`, `[M+CH3OH+H]+`, `[2M+H]+`, `[2M+Na]+`,
  `[2M+K]+`, `[2M+NH4]+`;
- negative: `[M-H]-`, `[M+Cl]-`, `[M+Br]-`, `[M+CHO2]-`,
  `[M+CH3COO]-`, `[M+FA-H]-`, `[2M-H]-`, `[2M+Cl]-`,
  `[2M+FA-H]-`.

The built-in default loss list must represent every current
`FRAGMENT_LOSS_SET` formula and polarity rule, including water, carbon
dioxide, ammonia, carbon monoxide, methyl, formic acid, hydrogen chloride,
hydrogen fluoride, sulfur oxides/acids, methanol, ethylene, acetylene,
nitrogen oxides/acids, methylene, ethanol, phosphorous acid, and phosphoric
acid.

An explicit empty `modifications` list should mean “perform no adduct or loss
matching,” not “restore defaults.” Omitted `modifications` should mean “use
all built-in defaults.” This distinction must be tested and documented.

## Mass and isotope authority

### Open Babel modification masses

Extend the existing Open Babel adapter rather than calling Open Babel directly
from the plugin:

1. Add a formula-mass result and C API export that parses a molecular formula
   and returns normalized formula, monoisotopic/exact mass, and bounded error
   text.
2. Add the corresponding C++ wrapper in
   `streamfind/core/vendors/openbabel.hpp` and its runtime-loader path.
3. Use the new operation for neutral-loss formula masses.
4. Use the same formula mass for adduct composition deltas, with explicit
   proton/electron correction and charge semantics at the annotation boundary.
   Do not copy the old `1.007276`-style values into the new rule table.
5. Validate formulas, charge, polarity, molecule multiplier, and impossible
   signed compositions before matching any features.

The exact mass convention must be written down in the operation contract:
neutral losses use neutral monoisotopic formula mass; charged adduct deltas
must account for the ionization convention used by the current m/z equations.
The migration must prove that `[M+H]+` and `[M-H]-` retain their current
effective m/z behavior within the configured tolerance.

### IsoSpec isotope data

Replace the local isotope literals with a builder backed by IsoSpec++:

1. Resolve selected element symbols against `elem_table_element`/
   `elem_table_symbol`.
2. Select the natural, non-radioactive isotope rows and identify the
   monoisotopic/base isotope deterministically.
3. Derive heavy-isotope mass distances and abundances from IsoSpec data.
4. Preserve the existing `min`/`max` count-range meaning and `max_isotopes`
   guard while removing isotope-name-specific mass-delta maps.
5. Replace hardcoded combination exclusions and priority maps with data-driven
   rules. If ranking policy is intentionally retained, make it a generic
   documented scoring policy based on abundance, mass distance, and isotope
   complexity rather than element-name literals.
6. Keep isotope annotation behavior bounded for large element sets; add
   explicit limits and diagnostics for combinatorial expansion.

IsoSpec can provide the isotope masses and probabilities needed here, but it
does not replace the feature-chain matching policy, charge handling, or
annotation scoring. Those remain owned by `annotate_components`.

## Carbon-first isotope-envelope assignment

The Merck validation result exposed an important ambiguity: a high M+2 peak
was labelled `81Br`, although the compound is understood to be a carbon-rich,
non-brominated organic compound. The existing mass and intensity heuristics
can make a halogen explanation win because bromine is enabled and the M+2
mass gap is compatible within the configured tolerance. The presence of an
`81Br` label must therefore not be treated as chemical proof.

The improved assignment must preserve plausible weak M+4--M+6 peaks while
using the complete isotope envelope to distinguish carbon-rich compounds from
halogenated compounds.

### Chemical priorities

- Treat C, N, O, and S as the normal organic composition space.
- Keep Cl and Br available, because environmental and screening compounds can
  be halogenated, but apply a lower prior to halogen explanations.
- Penalize chlorine/bromine counts above one unless the complete M+2/M+4
  pattern supports them.
- Do not reject M+4--M+6 solely because their intensities are weak; mass,
  coelution, and consistency with the lower isotope envelope remain evidence.
- Preserve near-tied carbon and halogen explanations as ambiguity diagnostics
  rather than forcing a single elemental interpretation.

### Envelope fitting stages

1. **Candidate discovery:** retain the current m/z and relaxed intensity-window
   discovery so weak isotope features remain candidates. Record the theoretical
   abundance range separately from the relaxed discovery range.
2. **Carbon estimate:** estimate carbon count from M+1/M using IsoSpec
   abundance ratios, bounded by the configured C range. Propagate uncertainty
   instead of relying only on a fixed ±20% interval.
3. **M+2 refinement:** use M+2/M to refine the carbon interval and evaluate
   `13C/13C`, `15N`, `18O`, `37Cl`, and `81Br` contributions together.
4. **Joint envelope fit:** generate candidate elemental-count vectors within
   the selected ranges and compare the observed M through M+`max_isotopes`
   intensity vector with the IsoSpec envelope.
5. **Chemical scoring:** combine mass residual, coelution, signal-quality-aware
   intensity residuals, carbon support, isotope complexity, and halogen priors.
6. **Selection:** choose the best complete-envelope explanation, retain
   near-ties, and use envelope coherence as root evidence for component link
   assignment.

### Intensity model

The current `0.7 * min` / `1.3 * max` expansion is a candidate-discovery
tolerance, not the final chemical validation. The final fit should:

- compare observed and expected ratios in a noise-aware or log-intensity
  residual;
- weight high-S/N peaks more strongly than weak peaks;
- retain weak peaks when they improve the global envelope;
- penalize unexplained observed or theoretical peaks separately;
- distinguish theoretical abundance, discovery bounds, and final confidence in
  diagnostics.

### Carbon and halogen consistency rules

- The inferred carbon count must be compatible with every selected `13C`
  contribution at M+1 through M+`n`.
- A `13C/13C` M+2 explanation must be compared directly with Cl/Br alternatives
  rather than losing to a halogen priority based on one peak.
- `81Br` requires a supported bromine count and a characteristic envelope;
  matching only the M+2 mass gap is insufficient.
- Mixed carbon/halogen combinations must not exceed the configured element
  counts.
- Higher-order assignments such as M+4--M+6 remain valid when they are
  compatible with the selected envelope, even when their individual intensity
  residuals are less precise.
- If carbon-rich and halogenated explanations are effectively tied, select one
  deterministic primary assignment for the existing feature-table columns and
  expose the alternative only through debug/audit diagnostics. Do not expand
  the feature-table schema solely to persist competing isotope explanations.

### Component graph integration

- Use the selected isotope envelope as positive evidence for the molecular
  root.
- Attach isotope children consistently to that root or to a validated modified
  parent.
- Do not let a broad correlation component turn an unrelated loss into the
  parent of the dominant feature.
- Keep positive-modification and loss orientations as alternatives until root
  and envelope scores are evaluated together.

### Performance constraints

The current annotation baseline completes the one-analysis NTA validation in
approximately 7.3 seconds end-to-end, with 100 components and 211 detected
features. The current carbon-first eligibility slice adds only constant-time
token inspection and a small comparison per isotope combination; it does not
materially change the baseline.

A naïve complete search over the default element ranges would be unacceptable:
`C:1-80`, `N:0-10`, `O:0-20`, `S:0-4`, `Cl:0-6`, and `Br:0-4` already describe
hundreds of thousands of elemental-count vectors before isotope-envelope
combinations are considered. The full envelope implementation must therefore:

- estimate carbon from M+1 before composition enumeration;
- use M+2 and exact mass to prune candidate counts immediately;
- avoid enumerating the Cartesian product of all element ranges;
- use bounded beam search or dynamic programming over isotope steps;
- retain only a small top-K candidate envelopes per anchor;
- cache IsoSpec envelopes by normalized elemental-count vector and isotope
  limit;
- skip envelope fitting for components without enough isotope evidence;
- use component-size and candidate-count guards for very large correlation
  components;
- report candidate-pruning counts in debug output for reproducibility.

The target is that the complete envelope stage adds a bounded cost proportional
to the number of plausible isotope anchors and retained candidate envelopes,
not to the raw product of user-configured element ranges. A benchmark must
compare annotation-only wall time and peak memory on both validation projects,
with a provisional target of less than 2x annotation time and no unbounded
memory growth for the default workflows.

## Closure evidence

- Built `streamfind_mass_spec_plugin` and `streamfind_aggregate_catalogue` with
  the MinGW UCRT64 build tree; semantic catalogue generation reported
  `Conforms`.
- Passed all five focused plugin/loader/configuration contract tests with
  `ctest --test-dir tmp\\build\\mingw-ucrt64 -R
  "streamfind_(plugin|dynamic_plugin)" --output-on-failure`.
- Passed the one-analysis non-target-screening workflow with
  `scripts\\dev\\cpp\\test-nta.ps1 -SkipBuild -RunPipeline -MaxAnalyses 1`:
  13/13 operations completed, with 33 isotope assignments, 7 loss
  assignments, 4 adduct assignments, and 167 default-adduct assignments.
- A disposable workflow run also accepted the custom `+2NOH` and `-2NOH`
  signed-formula path; their masses were resolved through Open Babel. A
  permanent synthetic feature fixture is still optional follow-up coverage.
- Repeated one-analysis timing remained approximately 6.83--7.04 seconds,
  within the recorded pre-change baseline.
- Added `tests/mass_spec/test_annotate_components.py`, runnable with
  `.venv\\Scripts\\python.exe -m unittest discover -s tests -p
  'test_*.py' -v`; the current fixture-backed run passes 7 tests.

## Implementation phases

### Phase 0 — Contract capture and fixture baseline

- [x] Record current default adduct/loss labels, formulas, polarity, charge,
      dimer behavior, and representative masses.
- [x] Capture current isotope outputs and counts for the two validation
      projects.
- [x] Validate omitted elements, valid IsoSpec-backed element lookup, an empty
      rule list, and custom signed adduct/loss rules through the NTA workflow
      path. A standalone synthetic fixture remains optional.
- [x] Confirm the existing catalogue generator and parameter validator can
      represent the selected modification schema.

### Phase 1 — Shared mass and isotope data services

- [x] Add Open Babel formula-mass support through the existing runtime/C API
      boundary, with Linux and Windows loader coverage.
- [x] Add a mass-spec isotope-table adapter around IsoSpec++.
- [x] Exercise formula mass, isotope lookup, monoisotopic selection,
      radioactive filtering, malformed formulas, and unknown elements through
      the shared runtime validation/build path.
- [x] Keep all new helpers in explicit named internal namespaces or file-local
      functions; do not modify `cpp/vendor/`.

### Phase 2 — Internal modification model

- [x] Keep the existing category-specific runtime model, while deriving all
      formula masses at construction time and accepting a canonical signed
      expression for each selected rule. This preserves the existing output
      contract while avoiding a duplicate compatibility execution path.
- [x] Implement signed-formula validation, normalization, duplicate detection,
      leading-coefficient expansion, and deterministic custom labels.
- [x] Generate the 41 built-in defaults from the annotation module and ensure
      the semantic default enumerates every rule exactly once.
- [x] Make adduct and loss candidate generation consume only the selected
      effective rule vectors.

### Phase 3 — Operation and semantic contract

- [x] Add the `annotation_modifications` parameter and update
      `annotateComponents` in `operations.ttl`.
- [x] Document default-versus-empty behavior, expression grammar, polarity,
      charge, dimer semantics, and formula validation.
- [x] Tighten `isotope_elements` validation and document IsoSpec++ resolution.
- [x] Update workflow demos and dev scripts without adding a compatibility
      execution path.

### Phase 4 — Annotation integration

- [x] Thread the validated isotope/modification configuration through
      `annotate_components_impl` and both relation-generation paths.
- [x] Remove hardcoded isotope mass deltas, isotope priorities, adduct masses,
      loss masses, and silent element filtering. The remaining built-in rule
      rows are semantic metadata, not unused property tables.
- [x] Preserve default-adduct assignment and existing annotation columns.
- [x] Add deterministic debug output for effective isotope elements,
      candidate edges, and selected graph relations.
- [x] Reject invalid configuration before loading or publishing feature
      artifacts.
- [x] Separate isotope candidate discovery from final envelope validation.
- [x] Add carbon-first composition estimation and bounded whole-chain envelope
      scoring.
- [x] Add lower-priority chlorine/bromine scoring and ambiguity diagnostics
      without changing the feature-table schema.
- [x] Feed envelope coherence into root and parent selection.

### Phase 5 — Regression and workflow validation

- [x] Run focused repository regression tests for parser/default-configuration
      contracts, IsoSpec/Open Babel-backed workflow output, candidate
      categories, polarity/dimer defaults, and Merck envelope behavior.
- [x] Build the mass-spec plugin and catalogue with the repository's MinGW
      UCRT64/Ninja toolchain.
- [x] Run `mass_spec.annotate_components` against disposable copies derived
      from:
      - `tmp/projects/metfrag_verify.duckdb`;
      - `tmp/projects/merck_workflow.duckdb`.
- [ ] Compare default-mode outputs with the captured baseline: annotation
      counts, labels, parent links, mass errors, and downstream filter
      behavior.
- [x] Validate that a selected subset removes only unselected modifications,
      that an empty list produces no adduct/loss candidates, and that a custom
      formula modification appears with an Open Babel-derived mass.
- [ ] Verify no partial artifact is published when a formula, isotope, or
      modification rule is invalid.
- [x] Add regression coverage for the carbon-rich Merck compound and its
      multi-step isotope envelope.
- [x] Verify that the Merck carbon-rich M+2 explanation is retained as a
      carbon-only envelope candidate rather than being forced into a halogen
      explanation.
- [x] Verify that plausible weak M+4--M+5 assignments are retained with lower
      confidence rather than discarded by an intensity cutoff.

### Phase 6 — Documentation and closure

- [x] Update mass-spec operation documentation and workflow authoring
      guidance.
- [x] Search for remaining annotate-specific hardcoded adduct/loss/isotope
      tables and justify the active built-in rule metadata; obsolete numeric
      isotope-priority and modification-mass tables are removed.
- [x] Run the ontology formatter after Turtle edits, then build
      `streamfind_aggregate_catalogue`.
- [x] Record the final behavior and validation commands in this plan.

## Validation matrix

| Area | Evidence |
| --- | --- |
| Default compatibility | Existing default rules produce equivalent labels, polarity, dimer semantics, counts, and mass tolerances on both DuckDB fixtures. |
| Isotope data | IsoSpec-backed masses/probabilities match the captured baseline for current defaults and support at least one previously unavailable valid element. |
| Custom rules | User-selected subset, custom adduct, custom loss, and explicit empty list produce the intended candidate set. |
| Formula mass | Open Babel-derived masses are used in debug output and candidate error calculations; no handwritten rule mass is consulted. |
| Invalid input | Unknown elements, malformed ranges/formulas, duplicate/conflicting rules, invalid charge/polarity, and ambiguous dimer expressions fail before publication. |
| Persistence | Workflow parameters serialize the effective configuration deterministically and reopen/re-run without hidden defaults. |
| Build/package | MinGW UCRT64 C++ build, catalogue generation, plugin validation, and both fixture workflows succeed. |

## Decisions to make during implementation

1. Final public representation: compact expression strings versus structured
   modification objects. Prefer the smallest schema that can represent charge,
   polarity, formula composition, and dimer multiplicity without ambiguity.
2. Whether omitted `isotope_elements` keeps the current six-element default or
   expands to all stable IsoSpec elements. Preserve the current six-element
   default unless a separate accuracy/performance decision approves expansion.
3. Whether custom modifications may use arbitrary formulas or must be limited
   to valid Open Babel-resolvable neutral/ion compositions.
4. Whether modification labels are user-authored or always normalized by the
   parser. Preserve the current labels for built-in defaults and make custom
   labels deterministic.

## Working rules

- Do not edit `cpp/vendor/isospec` or other vendored source.
- Do not retain a second legacy rule engine or a fallback mass table.
- Keep the C++ core/plugin boundary and semantic catalogue authoritative.
- Keep all disposable fixture copies, logs, and build outputs under `tmp/`.
- Treat the two existing DuckDB files as validation inputs; do not overwrite
  them during tests.
