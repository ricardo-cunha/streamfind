use serde_json::json;
use streamfind_rust_core::{MethodRegistry, OperationRegistry};
use streamfind_rust_test_support::{advertised_core_tools, catalogue_entries};

/// Tool names `tools/list` must advertise: core operations plus every
/// exposed domain operation. This is derived from the committed semantic
/// catalogue so it remains stable and adapts automatically to new domains.
fn advertised_tool_names() -> Vec<String> {
    let mut names = advertised_core_tools()
        .into_iter()
        .map(|entry| entry["mcp"]["name"].as_str().unwrap().to_owned())
        .collect::<Vec<_>>();
    names.extend(
        catalogue_entries()
            .into_iter()
            .filter(|entry| {
                entry["kind"] == "operation"
                    && entry["domain"] != "streamfind"
                    && entry["exposed"] == true
            })
            .map(|entry| entry["canonical_id"].as_str().unwrap().to_owned()),
    );
    names
}

/// Assert a `tools/list` payload advertises exactly the expected tool names.
fn assert_advertises(tools: &serde_json::Value, expected: &[String]) {
    let actual_names: Vec<String> = tools
        .as_array()
        .unwrap()
        .iter()
        .map(|tool| tool["name"].as_str().unwrap().to_owned())
        .collect();
    let mut expected = expected.to_vec();
    expected.sort();
    let mut actual = actual_names.clone();
    actual.sort();
    assert_eq!(
        actual, expected,
        "advertised tools differ from the catalogue"
    );
    assert_eq!(
        actual_names.len(),
        expected.len(),
        "duplicate or missing tool names"
    );
    assert!(tools
        .as_array()
        .unwrap()
        .iter()
        .all(|tool| tool["inputSchema"]["type"] == "object"));
}

#[test]
fn supports_initialize_and_tool_listing() {
    let registry = MethodRegistry::default();
    let mut operations = OperationRegistry::default();
    streamfind_rust_mass_spec::register_operations(&mut operations).unwrap();
    let mut session = streamfind_rust_mcp::Session::new(&registry, &operations);
    assert_eq!(
        session.handle(&json!({"id": 1, "method": "initialize"}))["result"]["serverInfo"]["name"],
        "streamfind-rust"
    );
    let tools = session.handle(&json!({"id": 2, "method": "tools/list"}));
    assert_advertises(&tools["result"]["tools"], &advertised_tool_names());
    let initialize = session.handle(&json!({"id": 3, "method": "initialize"}));
    let instructions = initialize["result"]["instructions"]
        .as_str()
        .expect("initialize must provide agent usage instructions");
    for term in [
        "create",
        "describe",
        "tools/list",
        "get_available_methods",
        "connect",
    ] {
        assert!(instructions.contains(term), "instructions missing {term}");
    }
    let metadata = tools["result"]["tools"]
        .as_array()
        .unwrap()
        .iter()
        .find(|tool| tool["name"] == "set_metadata")
        .unwrap();
    assert!(metadata["inputSchema"]["required"]
        .as_array()
        .unwrap()
        .iter()
        .any(|value| value == "metadata"));
}

#[test]
fn method_discovery_returns_complete_input_schemas() {
    let mut registry = MethodRegistry::default();
    streamfind_rust_mass_spec::register_methods(&mut registry).unwrap();
    let operations = OperationRegistry::default();
    let mut session = streamfind_rust_mcp::Session::new(&registry, &operations);
    let response = session.handle(&json!({
        "id": 1,
        "method": "tools/call",
        "params": {
            "name": "get_available_methods",
            "arguments": {"domain": "mass_spec"}
        }
    }));
    assert_ne!(response["result"]["isError"], true, "{response}");
    let methods: serde_json::Value =
        serde_json::from_str(response["result"]["content"][0]["text"].as_str().unwrap()).unwrap();
    let find_features = methods
        .as_array()
        .unwrap()
        .iter()
        .find(|method| method["id"] == "mass_spec.find_features")
        .expect("find_features must be discoverable as a workflow method");
    assert_eq!(find_features["inputSchema"]["type"], "object");
    assert!(find_features["inputSchema"]["properties"].is_object());
    assert!(find_features["parameters"].is_array());
}

#[test]
fn session_lifecycle_rebinds_catalogue() {
    let registry = MethodRegistry::default();
    let mut operations = OperationRegistry::default();
    streamfind_rust_mass_spec::register_operations(&mut operations).unwrap();
    let mut session = streamfind_rust_mcp::Session::new(&registry, &operations);
    let path = streamfind_rust_test_support::tmp_projects_dir()
        .join("streamfind-rust-mcp-lifecycle.duckdb");
    let _ = std::fs::remove_file(&path);
    let call = |session: &mut streamfind_rust_mcp::Session<'_>, id, name, arguments| {
        session.handle(&json!({"id": id, "method": "tools/call", "params": {"name": name, "arguments": arguments}}))
    };
    assert!(
        call(
            &mut session,
            1,
            "create",
            json!({"database_path": path, "domain": "mass_spec"})
        )["result"]["isError"]
            != true
    );
    // Operations are always advertised; connect changes only the method
    // execution context and does not require a tools/list refresh.
    let tools = session.handle(&json!({"id": 3, "method": "tools/list"}))["result"]["tools"]
        .as_array()
        .unwrap()
        .clone();
    assert_advertises(&json!(tools), &advertised_tool_names());
    let eic = tools
        .iter()
        .find(|tool| tool["name"] == "mass_spec.get_raw_spectra_eic")
        .unwrap();
    assert_eq!(eic["inputSchema"]["properties"]["targets"]["type"], "array");
    assert_eq!(
        eic["inputSchema"]["properties"]["targets"]["items"]["type"],
        "object"
    );
    assert_eq!(
        eic["inputSchema"]["properties"]["rt_tolerance"]["type"],
        "number"
    );
    assert_eq!(
        eic["inputSchema"]["properties"]["targets"]["examples"][0][0]["id"],
        "caffeine"
    );
    // The nested target object is part of the advertised JSON Schema, not an
    // opaque generic object. This is what lets clients construct EIC inputs
    // without consulting get_available_methods (which lists methods only).
    for field in ["id", "mass", "mz", "rt", "formula", "SMILES", "InChI"] {
        assert!(
            eic["inputSchema"]["properties"]["targets"]["items"]["properties"]
                .get(field)
                .is_some(),
            "targets schema is missing field {field}"
        );
    }
    let pre_connect_info = call(
        &mut session,
        4,
        "mass_spec.get_analyses_info",
        json!({"database_path": path}),
    );
    assert_ne!(
        pre_connect_info["result"]["isError"], true,
        "{pre_connect_info}"
    );
    let info = call(
        &mut session,
        5,
        "mass_spec.get_analyses_info",
        json!({"database_path": path}),
    );
    assert_ne!(info["result"]["isError"], true, "{info}");
    assert_eq!(
        serde_json::from_str::<serde_json::Value>(
            info["result"]["content"][0]["text"].as_str().unwrap()
        )
        .unwrap(),
        json!({
            "row_count": 0,
            "columns": {
                "analysis": [],
                "replicate": [],
                "blank": [],
                "file_path": [],
                "format": [],
                "number_spectra": [],
                "number_chromatograms": []
            }
        })
    );
    assert!(
        call(&mut session, 5, "close", json!({"database_path": path}))["result"]["isError"] != true
    );
    // After close, operations remain advertised because they are stateless;
    // only the connected method context is cleared.
    let closed =
        session.handle(&json!({"id": 5, "method": "tools/list"}))["result"]["tools"].clone();
    assert_advertises(&closed, &advertised_tool_names());
    let _ = std::fs::remove_file(path);
}
