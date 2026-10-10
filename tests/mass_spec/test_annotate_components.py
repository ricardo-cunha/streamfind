import json
import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
ANNOTATION_CPP = REPO_ROOT / "cpp/plugins/mass_spec/src/operations/nta/nta_annotation.cpp"
ANNOTATION_HPP = REPO_ROOT / "cpp/plugins/mass_spec/src/operations/nta/nta_annotation.hpp"
PARAMETERS_TTL = REPO_ROOT / "cpp/plugins/mass_spec/semantic/parameters.ttl"
MERCK_PROJECT = REPO_ROOT / "tmp/projects/merck_workflow_annotate_verification.duckdb"
METFRAG_PROJECT = REPO_ROOT / "tmp/projects/metfrag_verify_annotate_verification.duckdb"


ADDUCTS = {
    "+H", "+Na", "+K", "+NH4", "+ACN+H", "+CH3OH+H",
    "2M+H", "2M+Na", "2M+K", "2M+NH4", "-H", "+Cl", "+Br",
    "+CHO2", "+CH3COO", "+FA-H", "2M-H", "2M+Cl", "2M+FA-H",
}
LOSSES = {
    "-H2O", "-CO2", "-NH3", "-CO", "-CH3", "-CH2O2", "-HCl",
    "-HF", "-SO2", "-SO3", "-H2SO4", "-CH3OH", "-C2H4", "-C2H2",
    "-NO", "-NO2", "-HNO2", "-HNO3", "-CH2", "-C2H6O", "-HPO3",
    "-H3PO4",
}


def _quoted_strings(block: str):
    return set(re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"', block))


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
        self.assertEqual(len(expressions), 41)
        self.assertEqual(set(expressions), ADDUCTS | LOSSES)
        self.assertEqual(len(expressions), len(set(expressions)))

    def test_header_contains_runtime_containers_not_hardcoded_rule_initializers(self):
        self.assertNotRegex(self.hpp, r"all_adducts\s*\{")
        self.assertNotRegex(self.hpp, r"all_losses\s*\{")


@unittest.skipUnless(MERCK_PROJECT.exists(), "disposable Merck validation project is not available")
class MerckAnnotationRegressionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import duckdb
        except ImportError as error:  # pragma: no cover - environment dependent
            raise unittest.SkipTest(f"duckdb is unavailable: {error}")

        connection = duckdb.connect(str(MERCK_PROJECT), read_only=True)
        table = connection.execute(
            """
            SELECT physical_table
            FROM ARTIFACT_INVENTORY
            WHERE producer_operation = 'mass_spec.annotate_components'
              AND contract_id = 'featuresTable'
              AND status = 'published'
            ORDER BY created_at DESC
            LIMIT 1
            """
        ).fetchone()
        if table is None:
            connection.close()
            raise unittest.SkipTest("Merck annotate_components artifact is not available")
        cls.connection = connection
        cls.table = table[0]

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "connection"):
            cls.connection.close()

    def test_merck_annotation_has_expected_category_counts(self):
        counts = dict(self.connection.execute(
            f"SELECT annotation_category, COUNT(*) FROM {self.table} GROUP BY annotation_category"
        ).fetchall())
        self.assertGreaterEqual(counts.get("isotope", 0), 100)
        self.assertGreaterEqual(counts.get("loss", 0), 100)
        self.assertGreaterEqual(counts.get("adduct", 0), 1)

    def test_most_intense_feature_isotope_root(self):
        main = self.connection.execute(
            f"SELECT feature FROM {self.table} ORDER BY TRY_CAST(intensity AS DOUBLE) DESC NULLS LAST LIMIT 1"
        ).fetchone()[0]
        children = self.connection.execute(
            f"""
            SELECT annotation_type, annotation_category, annotation_element
            FROM {self.table}
            WHERE annotation_parent_feature = ?
            ORDER BY annotation_type
            """,
            [main],
        ).fetchall()
        child_types = {row[0] for row in children}
        self.assertTrue({"[M+1]", "[M+2]", "[M+3]", "[M+4]", "[M+5]"}.issubset(child_types))
        self.assertFalse(any(row[1] in {"adduct", "loss"} for row in children))


@unittest.skipUnless(METFRAG_PROJECT.exists(), "disposable MetFrag validation project is not available")
class MetfragAnnotationRegressionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import duckdb
        except ImportError as error:  # pragma: no cover - environment dependent
            raise unittest.SkipTest(f"duckdb is unavailable: {error}")

        cls.connection = duckdb.connect(str(METFRAG_PROJECT), read_only=True)
        table = cls.connection.execute(
            """
            SELECT physical_table
            FROM ARTIFACT_INVENTORY
            WHERE producer_operation = 'mass_spec.annotate_components'
              AND contract_id = 'featuresTable'
              AND status = 'published'
            ORDER BY created_at DESC
            LIMIT 1
            """
        ).fetchone()
        if table is None:
            cls.connection.close()
            raise unittest.SkipTest("MetFrag annotate_components artifact is not available")
        cls.table = table[0]

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "connection"):
            cls.connection.close()

    def test_metfrag_annotation_covers_all_18_analyses(self):
        analysis_count = self.connection.execute(
            f"SELECT COUNT(DISTINCT analysis) FROM {self.table}"
        ).fetchone()[0]
        self.assertEqual(analysis_count, 18)

    def test_metfrag_annotation_contains_all_relation_categories(self):
        counts = dict(self.connection.execute(
            f"SELECT annotation_category, COUNT(*) FROM {self.table} GROUP BY annotation_category"
        ).fetchall())
        self.assertGreater(counts.get("isotope", 0), 1000)
        self.assertGreater(counts.get("loss", 0), 250)
        self.assertGreater(counts.get("adduct", 0), 300)


if __name__ == "__main__":
    unittest.main()
