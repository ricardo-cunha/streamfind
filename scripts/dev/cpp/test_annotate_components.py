import json
import os
import re
import subprocess
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
if os.environ.get("STREAMFIND_EXAMPLE_DATA_ROOT"):
    DATA_ROOT = Path(os.environ["STREAMFIND_EXAMPLE_DATA_ROOT"]).expanduser().resolve()
else:
    managed_home = Path(os.environ.get("STREAMFIND_HOME", Path.home() / ".streamfind")).expanduser()
    DATA_REPOSITORY = managed_home / "streamfind.data"
    DATA_ROOT = DATA_REPOSITORY / "data"
    if not DATA_ROOT.is_dir():
        if DATA_REPOSITORY.exists():
            raise RuntimeError(f"Managed streamfind.data checkout has no data directory: {DATA_ROOT}")
        DATA_REPOSITORY.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            ["git", "clone", "--depth", "1", "https://git.uni-due.de/odea-project/streamfind/streamfind.data", str(DATA_REPOSITORY)],
            check=True,
        )
ANNOTATION_CPP = REPO_ROOT / "cpp/plugins/mass_spec/src/operations/nta/nta_annotation.cpp"
ANNOTATION_HPP = REPO_ROOT / "cpp/plugins/mass_spec/src/operations/nta/nta_annotation.hpp"
PARAMETERS_TTL = REPO_ROOT / "cpp/plugins/mass_spec/semantic/parameters.ttl"
MERCK_PROJECT = REPO_ROOT / "tmp/projects/merck_workflow_annotate_verification.sf"
METFRAG_PROJECT = REPO_ROOT / "tmp/projects/metfrag_verify_annotate_verification.sf"


ADDUCTS = {
    "+Na", "+NH4", "+K", "+Cl", "+FA-H", "+CH3COO",
}
LOSSES = {
    "-H2O", "-CO2", "-NH3", "-CO",
}


def _quoted_strings(block: str):
    return set(re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"', block))


def _load_annotated_table(project_path: Path):
    try:
        import duckdb
        import pyarrow.ipc as arrow_ipc
    except ImportError as error:  # pragma: no cover - environment dependent
        raise unittest.SkipTest(f"DuckDB and PyArrow are required to read Arrow result-store data: {error}")

    database_path = project_path / "project.duckdb"
    if not database_path.is_file():
        raise unittest.SkipTest(f"disposable validation project is not available: {database_path}")
    connection = duckdb.connect(str(database_path), read_only=True)
    try:
        registration = connection.execute(
            """
            SELECT result_id
            FROM RESULT_REGISTRY
            WHERE producer_operation = 'mass_spec.annotate_components'
              AND contract_id = 'featuresTable'
              AND status = 'published'
            ORDER BY created_at DESC, result_id DESC
            LIMIT 1
            """
        ).fetchone()
    finally:
        connection.close()
    if registration is None:
        raise unittest.SkipTest("published annotate_components result is not available")

    result_id = registration[0]
    store_root = (project_path / "data").resolve()
    manifest_path = store_root / "results" / result_id / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if (
        manifest.get("state") != "published"
        or manifest.get("kind") != "table"
        or manifest.get("format") != "arrow-ipc-file"
    ):
        raise unittest.SkipTest("annotate_components result is not a published Arrow table")

    payload_path = (store_root / manifest["payload"]).resolve()
    if not payload_path.is_relative_to(store_root) or not payload_path.is_file():
        raise RuntimeError(f"Arrow result payload is missing or outside its store: {payload_path}")
    reader = arrow_ipc.open_file(payload_path)
    return reader.read_all().to_pylist()


def _numeric_intensity(row):
    value = row.get("intensity")
    try:
        return float(value) if value is not None else 0.0
    except (TypeError, ValueError):
        return 0.0


class AnnotationContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cpp = ANNOTATION_CPP.read_text(encoding="utf-8")
        cls.hpp = ANNOTATION_HPP.read_text(encoding="utf-8")
        cls.ttl = PARAMETERS_TTL.read_text(encoding="utf-8")

    def test_cpp_default_expression_sets_cover_all_builtins(self):
        adduct_block = re.search(
            r"default_adduct_expressions\(\).*?static const std::vector<std::string> expressions\{(.*?)\};",
            self.cpp,
            re.DOTALL,
        )
        loss_block = re.search(
            r"default_loss_expressions\(\).*?static const std::vector<std::string> expressions\{(.*?)\};",
            self.cpp,
            re.DOTALL,
        )
        self.assertIsNotNone(adduct_block)
        self.assertIsNotNone(loss_block)
        self.assertEqual(_quoted_strings(adduct_block.group(1)), ADDUCTS)
        self.assertEqual(_quoted_strings(loss_block.group(1)), LOSSES)

    def test_semantic_default_matches_cpp_defaults(self):
        parameter = re.search(
            r"sfms:annotationModificationsParameter(.*?)(?=\nsfms:|\Z)",
            self.ttl,
            re.DOTALL,
        )
        self.assertIsNotNone(parameter)
        default = re.search(r"sf:default\s+\"(\[.*?\])\"\s*;", parameter.group(1))
        self.assertIsNotNone(default)
        expressions = json.loads(default.group(1).replace('\\"', '"'))
        self.assertEqual(len(expressions), 10)
        self.assertEqual(set(expressions), ADDUCTS | LOSSES)
        self.assertEqual(len(expressions), len(set(expressions)))

    def test_header_contains_runtime_containers_not_hardcoded_rule_initializers(self):
        self.assertNotRegex(self.hpp, r"all_adducts\s*\{")
        self.assertNotRegex(self.hpp, r"all_losses\s*\{")

    def test_official_mass_spec_data_repository_is_available(self):
        wastewater = DATA_ROOT / "mass_spec" / "wastewater"
        self.assertTrue((wastewater / "internal_standards.csv").is_file())
        self.assertTrue(any(wastewater.glob("*.mzML")))


@unittest.skipUnless(MERCK_PROJECT.exists(), "disposable Merck validation project is not available")
class MerckAnnotationRegressionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rows = _load_annotated_table(MERCK_PROJECT)

    def test_merck_annotation_has_expected_category_counts(self):
        counts = {}
        for row in self.rows:
            category = row.get("annotation_category")
            if category is not None:
                counts[category] = counts.get(category, 0) + 1
        self.assertGreaterEqual(counts.get("isotope", 0), 100)
        self.assertGreaterEqual(counts.get("loss", 0), 100)
        self.assertGreaterEqual(counts.get("adduct", 0), 1)

    def test_most_intense_feature_isotope_root(self):
        main_row = max(self.rows, key=_numeric_intensity)
        main = main_row["feature"]
        children = [row for row in self.rows if row.get("annotation_parent_feature") == main]
        child_types = {row.get("annotation_type") for row in children}
        self.assertTrue({"[M+1]", "[M+2]", "[M+3]", "[M+4]", "[M+5]"}.issubset(child_types))
        self.assertFalse(any(row.get("annotation_category") in {"adduct", "loss"} for row in children))


@unittest.skipUnless(METFRAG_PROJECT.exists(), "disposable MetFrag validation project is not available")
class MetfragAnnotationRegressionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rows = _load_annotated_table(METFRAG_PROJECT)

    def test_metfrag_annotation_covers_all_18_analyses(self):
        self.assertEqual(len({row.get("analysis") for row in self.rows}), 18)

    def test_metfrag_annotation_contains_all_relation_categories(self):
        counts = {}
        for row in self.rows:
            category = row.get("annotation_category")
            if category is not None:
                counts[category] = counts.get(category, 0) + 1
        self.assertGreater(counts.get("isotope", 0), 1000)
        self.assertGreater(counts.get("loss", 0), 250)
        self.assertGreater(counts.get("adduct", 0), 300)


if __name__ == "__main__":
    unittest.main()
