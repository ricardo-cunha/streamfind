"""Validate the bundled statistics CSV and connected workflow examples."""

from __future__ import annotations

import csv
import json
import math
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
WORKFLOW_DIR = ROOT / "cpp" / "plugins" / "statistics" / "resources" / "workflows"
CSV_PATH = WORKFLOW_DIR / "statistics-example.csv"
EXPECTED = {
    "pca-example.json": "statistics.pca",
    "pls-example.json": "statistics.pls",
    "fold-change-example.json": "statistics.fold_change",
}


def load_workflows() -> list[tuple[str, dict]]:
    loaded = []
    for file_name, operation_id in EXPECTED.items():
        path = WORKFLOW_DIR / file_name
        payload = json.loads(path.read_text(encoding="utf-8"))
        workflow = payload["workflow"]
        operations = {entry["id"]: entry for entry in workflow["operations"]}
        import_nodes = [entry for entry in operations.values() if entry["operation"] == "streamfind.read_csv"]
        analysis_nodes = [entry for entry in operations.values() if entry["operation"] == operation_id]
        if len(import_nodes) != 1 or len(analysis_nodes) != 1:
            raise ValueError(f"{file_name} must contain one CSV import and one {operation_id} operation")
        connection = {
            "source_operation": import_nodes[0]["id"],
            "source_port": "anyTable",
            "target_operation": analysis_nodes[0]["id"],
            "target_port": "tableInput",
        }
        if connection not in workflow["connections"]:
            raise ValueError(f"{file_name} does not connect read_csv to tableInput")
        if Path(import_nodes[0]["parameters"]["csv_input_path"]).name != CSV_PATH.name:
            raise ValueError(f"{file_name} does not reference the bundled statistics example CSV")
        loaded.append((operation_id, analysis_nodes[0]["parameters"]))
    return loaded


def validate_csv() -> int:
    with CSV_PATH.open(newline="", encoding="utf-8") as source:
        rows = list(csv.DictReader(source))
    if len(rows) != 6:
        raise ValueError(f"Expected six bundled samples, found {len(rows)}")
    required = {"sample_id", "group", "response", "feature_a", "feature_b"}
    if set(rows[0]) != required:
        raise ValueError(f"Unexpected example CSV columns: {sorted(rows[0])}")
    sample_ids = [row["sample_id"] for row in rows]
    if len(set(sample_ids)) != len(sample_ids):
        raise ValueError("Example CSV sample IDs are not unique")
    for row_index, row in enumerate(rows, start=2):
        for column in ("response", "feature_a", "feature_b"):
            value = float(row[column])
            if not math.isfinite(value):
                raise ValueError(f"CSV row {row_index} column {column} is not finite")
            if column.startswith("feature_") and value < 0:
                raise ValueError(f"CSV row {row_index} column {column} must be non-negative")
    counts = {label: sum(row["group"] == label for row in rows) for label in {row["group"] for row in rows}}
    if counts != {"control": 3, "treated": 3}:
        raise ValueError(f"Unexpected example group counts: {counts}")
    return len(rows)


def main() -> None:
    count = validate_csv()
    workflows = load_workflows()
    by_operation = dict(workflows)
    pca_predictors = by_operation["statistics.pca"]["variable_columns"]
    pls_predictors = by_operation["statistics.pls"]["variable_columns"]
    pls_responses = by_operation["statistics.pls"]["response_columns"]
    fold_predictors = by_operation["statistics.fold_change"]["variable_columns"]
    if not pca_predictors or pls_predictors != fold_predictors:
        raise ValueError("PCA, PLS, and fold-change examples must select explicit wide predictor columns")
    if set(pls_predictors) & set(pls_responses):
        raise ValueError("PLS response columns must be separate from the predictor columns")
    print(f"Validated {len(workflows)} statistics workflows against {count} bundled CSV samples.")


if __name__ == "__main__":
    main()
