# streamfind distribution notice

This notice accompanies streamfind source and native binary distributions. It
summarizes project licensing, third-party components, and the independent
vendor-format compatibility position. It is not legal advice or a legal
certification of format analysis, reverse-engineering rights, or redistribution
clearance.

## streamfind

streamfind is distributed under the GNU General Public License, version 3, as
set out in [`LICENSE.md`](LICENSE.md).

## Vendor compatibility and trademarks

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

This notice is an engineering and trademark clarification, not a legal
certification of reverse-engineering rights or compatibility with every
vendor format. Review applicable agreements, laws, and licence obligations
before redistributing vendor-format data or software.

## Third-party components

The exact C++/vendored-library licence texts are kept alongside their owning
vendor directories under `cpp/vendor/`. The Rust dependency inventory is provided under
`rust/LICENSES.md`. Component versions and inclusion can vary by backend and
platform; the release manifest is authoritative for a particular archive.

| Component | Version in the current native source/package | Licence | Source or retained notice |
| --- | --- | --- | --- |
| DuckDB C++ static package | v1.5.2 | MIT | `cpp/vendor/duckdb/LICENSE` |
| DuckDB Rust crate/bundled backend | `duckdb` crate 1.10505.0; bundled backend requires separate verification | MIT metadata plus bundled upstream components | C++: `cpp/vendor/duckdb/LICENSE`; Rust: `rust/LICENSES.md` |
| Open Babel | 3.2.0 | GPLv2 | `cpp/vendor/openbabel/openbabel-3-2-0/COPYING` |
| Zstandard | 1.5.7 | BSD or GPLv2 | `cpp/vendor/zstd/LICENSE`, `cpp/vendor/zstd/COPYING` |
| zlib | 1.3.2.1-motley | zlib licence | `cpp/vendor/zlib/zlib-develop/LICENSE` |
| pugixml | 1.14 | MIT | `cpp/vendor/pugixml-1.14/LICENSE` |
| simdutf | 7.3.4 | MIT | `cpp/vendor/simdutf/LICENSE` |
| nlohmann JSON | 3.12.0 | MIT | `cpp/vendor/nlohmann/LICENSE` |
| JSON Schema Validator | vendored version; see source README | MIT | `cpp/vendor/json-schema-validator/LICENSE` |
| Apache Jena | 6.2.0 | Apache-2.0 | `cpp/vendor/apache-jena/LICENSE`, `cpp/vendor/apache-jena/NOTICE` |
| Rust dependencies | see `rust/Cargo.lock` and release manifest | package-specific MIT/Apache-2.0 and other declared terms | `rust/LICENSES.md` |

The project must keep the original copyright and licence terms for every
component when redistributing source or binaries. The presence of a component
in this table does not by itself resolve the obligations of static linking,
combined works, source-code offers, or downstream redistribution.

## Native-reader process and format boundaries

The native vendor readers are independently implemented in C++ and Rust from
analysis of lawfully obtained data files, publicly available information, and
observable program output. This describes the project's engineering process; it
is not a legal opinion, warranty, or certification that every use is permitted
under a particular vendor agreement or jurisdiction.

Vendor SDKs, DLLs, debugger traces, paired conversion outputs, proprietary
documentation, confidential traces, and restricted vendor sample files are
development-only material. They are not runtime dependencies or release
contents, and no vendor source code is incorporated into the native readers.

SCIEX WIFF2 decryption is not implemented.

The project does not claim that a file format, product name, or interoperability
implementation is free from every contractual, trade-secret, copyright,
trademark, or technological-protection obligation. Review the applicable
licences, agreements, sample-data rights, and local law before redistributing
vendor-format data or software.

## Release review

Before distributing a native archive, verify that it contains:

- `NOTICE.md` and `LICENSE.md` at the package root;
- the C++ attribution payload assembled from the vendor-specific licence files
  kept beside their owning libraries under `cpp/vendor/`, or the Rust
  `LICENSES.md` dependency inventory;
- only the runtime files and dependencies intended for that backend and platform.

Verify the notice and attribution payload against the exact build inputs for
that archive. Legal review is required for GPL/static-linking obligations,
bundled DuckDB extension components, vendor asset rights, contractual
restrictions, and the intended redistribution model.
