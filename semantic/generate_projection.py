import argparse
import hashlib
import json
import os
import re
import tempfile
from decimal import Decimal
from pathlib import Path

import duckdb
from rdflib import Dataset, Namespace, RDF


ROOT = Path(__file__).parent
SF = Namespace("https://streamfind.dev/vocabulary#")
SKOS = Namespace("http://www.w3.org/2004/02/skos/core#")

CATALOGUE_DB = ROOT / "generated" / "catalogue.duckdb"

CATALOGUE_SCHEMA = """
CREATE TABLE catalogue_entries (
  canonical_id      VARCHAR PRIMARY KEY,   -- mass_spec.get_features | mass_spec.find_features
  kind              VARCHAR CHECK (kind IN ('operation','method')),
  domain            VARCHAR,               -- streamfind | mass_spec | raman | sensors
  label             VARCHAR,
  definition        VARCHAR,
  category          VARCHAR,
  invocation_model  VARCHAR,
  requires_connection BOOLEAN,
  guidance          VARCHAR,
  next_operations   JSON,
  interface_guidance VARCHAR,
  executable        BOOLEAN,               -- registered in C++ AND Rust
  exposed           BOOLEAN,               -- advertised via MCP (operations) / available-methods (methods)
  mcp_name          VARCHAR,               -- operations: tool name; methods: NULL (never a tool)
  input_schema      JSON,                  -- operation/method input schema: {type:object, properties, required}
  parameters        JSON,                  -- ordered param docs incl. description field
  result_schema     JSON,                  -- JSON-Schema table description
  reads_tables      JSON,                  -- DuckDB tables read (same in C++ and Rust)
  writes_tables     JSON,                  -- DuckDB tables written
  cacheable         BOOLEAN,               -- methods only
  single_occurrence BOOLEAN,               -- methods only
  mutates_project   BOOLEAN,               -- operations + methods
  required_methods  JSON,                  -- methods only: ordered [canonical_id, ...]
  module_id         VARCHAR                -- native domain module providing the capability
);
CREATE INDEX catalogue_kind_domain ON catalogue_entries (kind, domain);
CREATE INDEX catalogue_executable ON catalogue_entries (executable);
CREATE TABLE catalogue_tables (
  table_name VARCHAR PRIMARY KEY,
  domain VARCHAR,
  module_id VARCHAR,
  columns JSON
);
"""


def wire_name(value):
    return re.sub(r"(?<!^)([A-Z])", r"_\1", value).lower()


def resource_name(value):
    value = value.rsplit("#", 1)[-1]
    return value.removesuffix("Parameter")


def json_value(value):
    return float(value) if isinstance(value, Decimal) else value


def module_id(graph, resource, kind, domain, canonical_id):
    declared = graph.value(resource, SF.providedByModule)
    if declared:
        return str(declared).rsplit("#", 1)[-1]
    if domain == "mass_spec":
        if canonical_id in {
            "mass_spec.load_chromatograms",
            "mass_spec.filter_chromatograms_retention_time",
        }:
            return "mass_spec.chromatograms"
        if kind == "method":
            return "mass_spec.nta"
        return "mass_spec.base"
    return f"{domain}.base"


def projection():
    graph = Dataset()
    for path in sorted((ROOT / "ontology" / "core").glob("*.ttl")):
        graph.parse(path, format="turtle")
    for path in sorted((ROOT / "ontology" / "domains").rglob("*.ttl")):
        graph.parse(path, format="turtle")
    def parameter_schema(parameter):
        schema = {"type": str(graph.value(parameter, SF.type))}
        definition = graph.value(parameter, SKOS.definition)
        if definition:
            schema["description"] = str(definition)
        label = graph.value(parameter, SKOS.prefLabel)
        if label:
            schema["title"] = str(label)
        units = graph.value(parameter, SF.units)
        if units:
            schema["x-streamfind-units"] = str(units)
        nullable = graph.value(parameter, SF.nullable)
        if nullable:
            schema["x-streamfind-nullable"] = bool(nullable.toPython())
        constraint = graph.value(parameter, SF.constraints)
        if constraint:
            value = str(constraint)
            if value.startswith("{"):
                for key, item in json.loads(value).items():
                    schema[key] = item
            else:
                schema["enum"] = value.split("|")
        example = graph.value(parameter, SF.example)
        if example:
            value = json_value(example.toPython())
            if isinstance(value, str):
                try:
                    value = json.loads(value)
                except json.JSONDecodeError:
                    pass
            schema["examples"] = [value]
        item = graph.value(parameter, SF.items)
        if item:
            schema["items"] = parameter_schema(item)
        properties = {}
        required = []
        for prop in graph.objects(parameter, SF.hasProperty):
            name = str(graph.value(prop, SF.propertyName) or graph.value(prop, SF.columnName) or wire_name(str(prop).rsplit("#", 1)[-1]))
            properties[name] = parameter_schema(prop)
            required_value = graph.value(prop, SF.required)
            if required_value and required_value.toPython():
                required.append(name)
        if properties:
            schema["properties"] = properties
            if required:
                schema["required"] = required
            schema["additionalProperties"] = False
        return schema

    def mcp_schema(schema):
        """Convert semantic type names into standard JSON Schema types."""
        output = dict(schema)
        if output.get("type") == "real":
            output["type"] = "number"
        elif output.get("type") == "table":
            output["type"] = "object"
        if isinstance(output.get("items"), dict):
            output["items"] = mcp_schema(output["items"])
        if isinstance(output.get("properties"), dict):
            output["properties"] = {
                name: mcp_schema(value) for name, value in output["properties"].items()
            }
        return output

    parameters = {
        parameter: {
            "name": wire_name(resource_name(str(parameter))),
            "type": str(graph.value(parameter, SF.type)),
            "required": graph.value(parameter, SF.required).toPython(),
            "constraints": graph.value(parameter, SF.constraints).toPython() if graph.value(parameter, SF.constraints) else {},
            "items": resource_name(str(graph.value(parameter, SF.items))) if graph.value(parameter, SF.items) else None,
            "extensions": str(graph.value(parameter, SF.extensions)).split(",") if graph.value(parameter, SF.extensions) else [],
            "schema": parameter_schema(parameter),
            "example": parameter_schema(parameter).get("examples", [None])[0],
            "description": str(graph.value(parameter, SKOS.definition) or ""),
        }
        for parameter in graph.subjects(RDF.type, SF.Parameter)
    }
    def result_type(result):
        return str(graph.value(result, SF.type))

    def method_metadata(method):
            required = graph.value(method, SF.requiredMethods)
            required_methods = []
            if required:
                for value in graph.items(required):
                    canonical = graph.value(value, SF.methodId) or graph.value(value, SF.operationId)
                    required_methods.append(str(canonical) if canonical else str(value))
            return {
                "cacheable": bool(graph.value(method, SF.cacheable).toPython()),
                "required_methods": required_methods,
                "single_occurrence": bool(graph.value(method, SF.singleOccurrence).toPython()),
            }

    def result_schema(result):
        if result is None:
            return {"type": "object"}
        schema = {"type": result_type(result)}
        label = graph.value(result, SKOS.prefLabel)
        definition = graph.value(result, SKOS.definition)
        if label:
            schema["title"] = str(label)
        if definition:
            schema["description"] = str(definition)
        item = graph.value(result, SF.items)
        if item:
            schema["items"] = result_schema(item)
        properties = {}
        for prop in graph.objects(result, SF.hasProperty):
            column = column_schema(prop)
            properties[str(graph.value(prop, SF.propertyName) or graph.value(prop, SF.columnName))] = (
                {"type": "array", "items": column}
                if schema["type"] == "table" else column
            )
        if properties:
            schema["properties"] = properties
        return schema

    def column_schema(column):
        schema = {"type": result_type(column)}
        item = graph.value(column, SF.items)
        if item:
            schema["items"] = column_schema(item)
        properties = {}
        for prop in graph.objects(column, SF.hasProperty):
            properties[str(graph.value(prop, SF.propertyName) or graph.value(prop, SF.columnName))] = column_schema(prop)
        if properties:
            schema["properties"] = properties
        constraint = graph.value(column, SF.constraints)
        if constraint:
            schema["enum"] = str(constraint).split("|")
        return schema
    def table_manifest():
        tables = []
        for table in graph.subjects(RDF.type, SF.Table):
            table_name = graph.value(table, SF.tableName)
            if not table_name:
                continue
            physical_name = str(table_name)
            domain_resource = graph.value(table, SF.availableInDomain)
            domain = str(domain_resource).rsplit("#", 1)[-1] if domain_resource else "streamfind"
            columns = []
            for column in graph.objects(table, SF.hasColumn):
                column_name = graph.value(column, SF.columnName) or graph.value(column, SF.propertyName)
                column_type = graph.value(column, SF.type)
                if column_name:
                    columns.append({"name": str(column_name), "type": str(column_type or "")})
            module = "mass_spec.base" if domain == "mass_spec" else "streamfind.base"
            tables.append({
                "table_name": physical_name,
                "domain": domain,
                "module_id": module,
                "columns": columns,
            })
        return sorted(tables, key=lambda value: value["table_name"])

    entries = []
    subjects = set(graph.subjects(SF.operationId, None)) | set(graph.subjects(SF.methodId, None))
    for operation in subjects:
        kind = "operation"
        canonical_id = str(graph.value(operation, SF.operationId))
        domain = str(graph.value(operation, SF.availableInDomain)).rsplit("#", 1)[-1] if graph.value(operation, SF.availableInDomain) else "streamfind"
        if (method_id := graph.value(operation, SF.methodId)) is not None:
            kind = "method"
            canonical_id = str(method_id)
            domain = str(graph.value(operation, SF.availableInDomain)).rsplit("#", 1)[-1]
        defaults_node = graph.value(operation, SF.defaults)
        defaults = json.loads(str(defaults_node)) if defaults_node else {}
        values = []
        for parameter in graph.objects(operation, SF.hasParameter):
            value = dict(parameters[parameter])
            value["default"] = defaults.get(value["name"])
            value["schema"] = dict(value["schema"])
            if value["default"] is not None:
                value["schema"]["default"] = value["default"]
            values.append(value)
        result_node = graph.value(operation, SF.returns)
        mutates = graph.value(operation, SF.mutatesProject)
        effects = {
            "mutates_project": mutates.toPython() if mutates else False,
            "reads": [str(graph.value(table, SF.tableName)) for table in graph.objects(operation, SF.reads)],
            "writes": [str(graph.value(table, SF.tableName)) for table in graph.objects(operation, SF.writes)],
        }
        domain_resource = graph.value(operation, SF.availableInDomain)
        domain_guidance = graph.value(domain_resource, SF.guidance) if domain_resource else None
        invocation = graph.value(operation, SF.invocationModel)
        invocation_model = str(invocation) if invocation else ("workflow" if kind == "method" else "stateless")
        requires_connection = graph.value(operation, SF.requiresConnection)
        requires_connection_value = bool(requires_connection.toPython()) if requires_connection else kind == "method"
        category = graph.value(operation, SF.category)
        next_operations = [
            str(graph.value(value, SF.operationId) or value)
            for value in graph.objects(operation, SF.nextOperation)
        ]
        entry = {
            "kind": kind,
            "canonical_id": canonical_id,
            "domain": domain,
            "module_id": module_id(graph, operation, kind, domain, canonical_id),
            "label": str(graph.value(operation, SKOS.prefLabel)),
            "definition": str(graph.value(operation, SKOS.definition)),
            "interface": {
                "category": str(category) if category else ("workflow-method" if kind == "method" else "domain-operation"),
                "invocation_model": invocation_model,
                "requires_connection": requires_connection_value,
                "guidance": str(graph.value(operation, SF.guidance) or ""),
                "next_operations": next_operations,
            },
            "interface_guidance": str(domain_guidance or ""),
            "executable": True,
            "exposed": True,
            "mcp": {
                "name": str(graph.value(operation, SF.toolName)),
                "input_schema": {
                    "type": "object",
                    "title": str(graph.value(operation, SKOS.prefLabel)),
                    "description": str(graph.value(operation, SKOS.definition)),
                    "properties": {value["name"]: mcp_schema(value["schema"]) for value in values},
                    "required": [value["name"] for value in values if value["required"]],
                },
            },
            "parameters": values,
            "result": {"id": str(result_node), "schema": result_schema(result_node)},
            "effects": effects,
        }
        if kind == "method":
            entry.update(method_metadata(operation))
        entries.append(entry)
    entries.sort(key=lambda value: value["canonical_id"])
    return {"version": 2, "entries": entries, "tables": table_manifest()}


def entry_json(value):
    """Serialize dict/list payloads to compact JSON text for JSON-typed columns."""
    return json.dumps(value, separators=(",", ":")) if isinstance(value, (dict, list)) else value


def entry_row(entry):
    kind = entry["kind"]
    effects = entry["effects"]
    mcp_name = None
    input_schema = entry.get("mcp", {}).get("input_schema")
    if kind == "operation":
        tool_name = entry["mcp"]["name"]
        mcp_name = tool_name if tool_name != "None" else entry["canonical_id"]
    return (
        entry["canonical_id"],
        kind,
        entry["domain"],
        entry["label"],
        entry["definition"],
        entry["interface"]["category"],
        entry["interface"]["invocation_model"],
        entry["interface"]["requires_connection"],
        entry["interface"]["guidance"],
        entry_json(entry["interface"]["next_operations"]),
        entry.get("interface_guidance", ""),
        entry["executable"],
        entry["exposed"],
        mcp_name,
        entry_json(input_schema),
        entry_json(entry["parameters"]),
        entry_json(entry["result"]["schema"]),
        entry_json(effects["reads"]),
        entry_json(effects["writes"]),
        entry.get("cacheable"),
        entry.get("single_occurrence"),
        effects["mutates_project"],
        entry_json(entry.get("required_methods")),
        entry["module_id"],
    )


def build_catalogue_db(value, path):
    connection = duckdb.connect(str(path))
    try:
        connection.execute(CATALOGUE_SCHEMA)
        connection.executemany(
            "INSERT INTO catalogue_entries VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            [entry_row(entry) for entry in value["entries"]],
        )
        connection.executemany(
            "INSERT INTO catalogue_tables VALUES (?,?,?,?)",
            [
                (table["table_name"], table["domain"], table["module_id"], entry_json(table["columns"]))
                for table in value["tables"]
            ],
        )
    finally:
        connection.close()


def canonical_row(value):
    """Normalize a fetched row value so deterministic hashing is stable."""
    if isinstance(value, str):
        try:
            return json.loads(value)
        except json.JSONDecodeError:
            return value
    return value


def duckdb_hash(path):
    connection = duckdb.connect(str(path), read_only=True)
    digest = hashlib.sha256()
    try:
        rows = connection.execute(
            "SELECT * FROM catalogue_entries ORDER BY canonical_id"
        ).fetchall()
        for row in rows:
            digest.update(
                json.dumps(
                    [canonical_row(value) for value in row],
                    sort_keys=True,
                    separators=(",", ":"),
                ).encode("utf-8")
            )
            digest.update(b"\n")
    finally:
        connection.close()
    return digest.hexdigest()


def catalogue_json(value):
    return json.dumps(value, indent=2) + "\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    value = projection()
    payload = catalogue_json(value)
    json_path = ROOT / "generated" / "catalogue.json"
    matrix_path = ROOT / "generated" / "capability_matrix.json"
    db_path = CATALOGUE_DB

    if args.check:
        stale = []
        if not json_path.exists() or json_path.read_text(encoding="utf-8") != payload:
            stale.append("semantic/generated/catalogue.json")
        matrix = {
            "version": 1,
            "capabilities": [
                {
                    "canonical_id": entry["canonical_id"],
                    "kind": entry["kind"],
                    "domain_id": entry["domain"],
                    "module_id": entry["module_id"],
                    "semantic_executable": entry["executable"],
                }
                for entry in value["entries"]
            ],
        }
        matrix_payload = catalogue_json(matrix)
        if not matrix_path.exists() or matrix_path.read_text(encoding="utf-8") != matrix_payload:
            stale.append("semantic/generated/capability_matrix.json")
        with tempfile.TemporaryDirectory(dir=str(ROOT / "generated")) as tmp_dir:
            tmp_db = Path(tmp_dir) / "catalogue.duckdb"
            build_catalogue_db(value, tmp_db)
            fresh_hash = duckdb_hash(tmp_db)
            existing_hash = duckdb_hash(db_path) if db_path.exists() else None
        if existing_hash != fresh_hash:
            stale.append("semantic/generated/catalogue.duckdb")
        if stale:
            raise SystemExit("stale generated semantic files: " + ", ".join(stale))
        print("semantic projection is up to date")
        return

    json_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(payload, encoding="utf-8")
    matrix = {
        "version": 1,
        "capabilities": [
            {
                "canonical_id": entry["canonical_id"],
                "kind": entry["kind"],
                "domain_id": entry["domain"],
                "module_id": entry["module_id"],
                "semantic_executable": entry["executable"],
            }
            for entry in value["entries"]
        ],
    }
    matrix_path.write_text(catalogue_json(matrix), encoding="utf-8")
    with tempfile.TemporaryDirectory(dir=str(ROOT / "generated")) as tmp_dir:
        tmp_db = Path(tmp_dir) / "catalogue.duckdb"
        build_catalogue_db(value, tmp_db)
        os.replace(tmp_db, db_path)
    print(f"generated {len(value['entries'])} semantic entries (catalogue.json + catalogue.duckdb)")


if __name__ == "__main__":
    main()