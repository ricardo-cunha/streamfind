use std::collections::BTreeSet;
use streamfind_rust_core::MethodRegistry;

#[test]
fn nta_interface_registers_catalogue_methods_without_data_files() {
    let entries = streamfind_rust_test_support::catalogue_entries();
    let expected_ids: BTreeSet<String> = entries
        .iter()
        .filter(|entry| {
            entry["kind"] == "method"
                && entry["domain"] == "mass_spec"
                && entry["canonical_id"]
                    .as_str()
                    .is_some_and(|id| id.starts_with("mass_spec."))
        })
        .map(|entry| entry["canonical_id"].as_str().unwrap().to_owned())
        .collect();
    assert_eq!(expected_ids.len(), 19, "expected 19 catalogue Methods");

    let mut methods = MethodRegistry::default();
    streamfind_rust_mass_spec::register_methods(&mut methods).unwrap();
    let registered = methods.list("mass_spec");
    let actual_ids: BTreeSet<String> = registered
        .iter()
        .filter_map(|method| method["id"].as_str())
        .filter(|id| id.starts_with("mass_spec."))
        .map(str::to_owned)
        .collect();
    assert_eq!(
        actual_ids, expected_ids,
        "registered Method set differs from catalogue"
    );

    for id in &expected_ids {
        let entry = entries
            .iter()
            .find(|entry| entry["canonical_id"].as_str() == Some(id))
            .unwrap_or_else(|| panic!("missing catalogue entry for {id}"));
        assert_eq!(entry["kind"], "method");
        assert_eq!(entry["domain"], "mass_spec");
    }

    let find_features = methods.get("mass_spec.find_features").unwrap();
    assert!(find_features.cacheable);
    assert!(find_features.single_occurrence);
    assert!(find_features.required_methods.is_empty());
}
