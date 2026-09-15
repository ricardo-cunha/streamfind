#!/usr/bin/env python3
"""Verify persisted results from the development NTA workflow."""
from __future__ import annotations

import argparse
from pathlib import Path

import duckdb


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("database", type=Path)
    parser.add_argument("--min-similarity", type=float, default=0.7)
    parser.add_argument("--expected-analyses", type=int, default=1)
    args = parser.parse_args()

    connection = duckdb.connect(str(args.database), read_only=True)
    try:
        analyses = connection.execute("SELECT COUNT(*) FROM MASS_SPEC_ANALYSES").fetchone()[0]
        features = connection.execute("SELECT COUNT(*) FROM MASS_SPEC_NTA_FEATURES").fetchone()[0]
        ms2_features = connection.execute(
            """
            SELECT COUNT(*)
            FROM MASS_SPEC_NTA_FEATURES
            WHERE COALESCE(ms2_size, 0) > 0
              AND COALESCE(ms2_mz, '') <> ''
              AND COALESCE(ms2_intensity, '') <> ''
            """
        ).fetchone()[0]
        suspects = connection.execute("SELECT COUNT(*) FROM MASS_SPEC_NTA_SUSPECTS").fetchone()[0]
        fragment_suspects = connection.execute(
            "SELECT COUNT(*) FROM MASS_SPEC_NTA_SUSPECTS WHERE COALESCE(shared_fragments, 0) > 0"
        ).fetchone()[0]
        similar_suspects = connection.execute(
            "SELECT COUNT(*) FROM MASS_SPEC_NTA_SUSPECTS WHERE cosine_similarity >= ?",
            [args.min_similarity],
        ).fetchone()[0]
        similarity_range = connection.execute(
            "SELECT MIN(cosine_similarity), MAX(cosine_similarity) FROM MASS_SPEC_NTA_SUSPECTS WHERE cosine_similarity IS NOT NULL"
        ).fetchone()
    finally:
        connection.close()

    checks = {
        "analyses": (analyses, args.expected_analyses),
        "features": (features, 1),
        "features_with_ms2": (ms2_features, 1),
        "suspects": (suspects, 1),
        "suspects_with_shared_fragments": (fragment_suspects, 1),
        "suspects_with_similarity": (similar_suspects, 1),
    }
    failures = [f"{name}={actual}, expected >= {minimum}" for name, (actual, minimum) in checks.items() if actual < minimum]
    if failures:
        raise SystemExit("NTA verification failed: " + "; ".join(failures))

    print(
        "NTA verification passed: "
        f"analyses={analyses}; features={features}; features_with_ms2={ms2_features}; "
        f"suspects={suspects}; suspects_with_shared_fragments={fragment_suspects}; "
        f"suspects_similarity_ge_{args.min_similarity:g}={similar_suspects}; "
        f"similarity_range={similarity_range}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
