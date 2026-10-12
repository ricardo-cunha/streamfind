#include <cassert>
#include <fstream>
#include <iostream>
#include <string>

#include <duckdb.h>
#include <openbabel/obconversion.h>
#include <openbabel/mol.h>
#include <openbabel/tokenst.h>

namespace {

bool test_duckdb() {
    duckdb_database database = nullptr;
    if (duckdb_open(nullptr, &database) != DuckDBSuccess) return false;

    duckdb_connection connection = nullptr;
    if (duckdb_connect(database, &connection) != DuckDBSuccess) return false;

    duckdb_result result{};
    if (duckdb_query(connection, "SELECT 42", &result) != DuckDBSuccess) return false;
    duckdb_destroy_result(&result);
    duckdb_disconnect(&connection);
    duckdb_close(&database);
    return true;
}

bool test_openbabel() {
    std::ifstream data;
    if (OpenBabel::OpenDatafile(data, "logp.txt").empty() || !data.good()) {
        std::cerr << "OpenBabel data lookup failed\n";
        return false;
    }

    OpenBabel::OBConversion conversion;
    if (!conversion.SetInAndOutFormats("smi", "smi")) {
        std::cerr << "OpenBabel format setup failed\n";
        return false;
    }
    OpenBabel::OBMol molecule;
    if (!conversion.ReadString(&molecule, "c1ccccc1")) {
        std::cerr << "OpenBabel parse failed\n";
        return false;
    }
    if (molecule.GetFormula() != "C6H6") {
        std::cerr << "OpenBabel formula failed: " << molecule.GetFormula() << "\n";
        return false;
    }
    return true;
}

}

int main() {
    if (!test_duckdb()) { std::cerr << "DuckDB smoke test failed\n"; return 1; }
    if (!test_openbabel()) { std::cerr << "OpenBabel smoke test failed\n"; return 1; }
    return 0;
}
