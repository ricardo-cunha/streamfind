use serde_json::{json, Value};
use std::collections::BTreeSet;
use streamfind_rust_core::{
    catalogue::{register_module_with_entries, DomainModuleBinding, OperationBinding},
    OperationRegistry, ParameterSchema,
};
use streamfind_rust_test_support::catalogue_entries;

fn operation_binding(id: &'static str) -> OperationBinding {
    OperationBinding {
        id,
        executor: Box::new(|_, _| Ok(json!([]))),
        validator: None,
    }
}

fn probe(entries: &[Value]) -> DomainModuleBinding {
    assert!(entries
        .iter()
        .any(|entry| entry["canonical_id"] == "mass_spec.get_analyses_info"));
    DomainModuleBinding {
        module_id: "mass_spec.base",
        domain_id: "mass_spec",
        module_version: "1",
        required_modules: vec![],
        methods: vec![],
        operations: vec![operation_binding("mass_spec.get_analyses_info")],
        tables: vec![],
        schema_binding: None,
    }
}

fn assert_rejected(entries: &[Value], module: DomainModuleBinding) {
    let mut operations = OperationRegistry::default();
    let result = register_module_with_entries(
        entries,
        module,
        &mut streamfind_rust_core::MethodRegistry::default(),
        &mut operations,
        |_| ParameterSchema {
            definitions: vec![],
        },
    );
    assert!(result.is_err());
}

#[test]
fn registration_contract_matches_catalogue_and_mcp_intersection() {
    let entries = catalogue_entries();
    let expected_methods: BTreeSet<_> = entries
        .iter()
        .filter(|entry| {
            entry["kind"] == "method"
                && entry["domain"] == "mass_spec"
                && entry["executable"] == true
        })
        .map(|entry| entry["canonical_id"].as_str().unwrap().to_owned())
        .collect();
    let expected_operations: BTreeSet<_> = entries
        .iter()
        .filter(|entry| {
            entry["kind"] == "operation"
                && entry["domain"] == "mass_spec"
                && entry["executable"] == true
        })
        .map(|entry| entry["canonical_id"].as_str().unwrap().to_owned())
        .collect();

    let mut methods = streamfind_rust_core::MethodRegistry::default();
    let mut operations = OperationRegistry::default();
    streamfind_rust_mass_spec::register_methods(&mut methods).unwrap();
    streamfind_rust_mass_spec::register_operations(&mut operations).unwrap();
    let actual_methods: BTreeSet<_> = methods
        .list("mass_spec")
        .into_iter()
        .map(|entry| entry["id"].as_str().unwrap().to_owned())
        .collect();
    let actual_operations: BTreeSet<_> = operations
        .list("mass_spec")
        .into_iter()
        .map(|entry| entry["id"].as_str().unwrap().to_owned())
        .collect();
    assert_eq!(actual_methods, expected_methods);
    assert_eq!(actual_operations, expected_operations);

    let mut session = streamfind_rust_mcp::Session::new(&methods, &operations);
    let tools = session.handle(&json!({"id": 1, "method": "tools/list"}));
    let mcp_operations: BTreeSet<_> = tools["result"]["tools"]
        .as_array()
        .unwrap()
        .iter()
        .filter_map(|tool| tool["name"].as_str())
        .filter(|name| name.starts_with("mass_spec."))
        .map(str::to_owned)
        .collect();
    let expected_mcp: BTreeSet<_> = entries
        .iter()
        .filter(|entry| {
            entry["kind"] == "operation"
                && entry["domain"] == "mass_spec"
                && entry["executable"] == true
                && entry["exposed"] == true
        })
        .map(|entry| entry["canonical_id"].as_str().unwrap().to_owned())
        .collect();
    assert_eq!(mcp_operations, expected_mcp);

    let mut unknown = probe(&entries);
    unknown.operations[0].id = "mass_spec.unknown";
    assert_rejected(&entries, unknown);

    let mut wrong_kind_entries = entries.to_vec();
    wrong_kind_entries
        .iter_mut()
        .find(|entry| entry["canonical_id"] == "mass_spec.get_analyses_info")
        .unwrap()["kind"] = json!("method");
    assert_rejected(&wrong_kind_entries, probe(&wrong_kind_entries));

    let mut wrong_domain_entries = entries.to_vec();
    wrong_domain_entries
        .iter_mut()
        .find(|entry| entry["canonical_id"] == "mass_spec.get_analyses_info")
        .unwrap()["domain"] = json!("raman");
    assert_rejected(&wrong_domain_entries, probe(&wrong_domain_entries));

    let mut wrong_module_entries = entries.to_vec();
    wrong_module_entries
        .iter_mut()
        .find(|entry| entry["canonical_id"] == "mass_spec.get_analyses_info")
        .unwrap()["module_id"] = json!("mass_spec.nta");
    assert_rejected(&wrong_module_entries, probe(&wrong_module_entries));

    let mut duplicate = probe(&entries);
    duplicate
        .operations
        .push(operation_binding("mass_spec.get_analyses_info"));
    assert_rejected(&entries, duplicate);

    let mut missing_table = probe(&entries);
    missing_table.tables = vec!["MASS_SPEC_DOES_NOT_EXIST".into()];
    assert_rejected(&entries, missing_table);

    let mut missing_dependency = probe(&entries);
    missing_dependency.required_modules = vec!["mass_spec.missing"];
    assert_rejected(&entries, missing_dependency);
}
