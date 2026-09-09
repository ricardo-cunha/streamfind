use serde_json::Value;
use std::fs;
use std::path::PathBuf;
use streamfind_rust_core::{Project, ProjectOptions};

fn fixture() -> Value {
    serde_json::from_str(include_str!(
        "../../../../tests/fixtures/project/project_conformance.json"
    ))
    .unwrap()
}

fn domain_schema_fixture() -> Value {
    serde_json::from_str(include_str!(
        "../../../../tests/fixtures/project/domain_schema_manifest.json"
    ))
    .unwrap()
}

fn database(name: &str) -> PathBuf {
    let path = streamfind_rust_test_support::tmp_projects_dir().join(name);
    let _ = fs::remove_file(&path);
    path
}

fn install_domain_schema(project: &mut Project) {
    for table in domain_schema_fixture()["domains"]["mass_spec"]["required_tables"]
        .as_array()
        .unwrap()
    {
        project
            .execute_sql(&format!(
                "CREATE TABLE {} (marker VARCHAR)",
                table.as_str().unwrap()
            ))
            .unwrap();
    }
}

fn options(path: PathBuf, fixture: &Value, read_only: bool) -> ProjectOptions {
    ProjectOptions {
        database_path: path,
        domain: fixture["domain"].as_str().unwrap().into(),
        create_if_missing: false,
        read_only,
    }
}

#[test]
fn shared_fixture_round_trips_project_contract() {
    let fixture = fixture();
    let path = database("streamfind-rust-conformance.duckdb");
    let mut project = Project::create(options(path.clone(), &fixture, false)).unwrap();
    project.set_metadata(fixture["metadata"].clone()).unwrap();
    project
        .set_workflow(
            streamfind_rust_core::Workflow::from_json(&fixture["workflow"]).unwrap(),
            &streamfind_rust_core::MethodRegistry::default(),
        )
        .unwrap();
    project
        .set_cache(
            fixture["cache"]["name"].as_str().unwrap(),
            fixture["cache"]["description"].as_str().unwrap(),
            fixture["cache"]["hash"].as_str().unwrap(),
            &fixture["cache"]["value"],
        )
        .unwrap();
    install_domain_schema(&mut project);
    assert_eq!(project.get_domain(), fixture["domain"]);
    assert_eq!(project.get_metadata(), fixture["metadata"]);
    assert_eq!(
        project.get_workflow().unwrap().to_json(),
        fixture["workflow"]
    );
    assert_eq!(project.get_cache_size().unwrap(), 1);
    project.validate().unwrap();
    drop(project);

    let reopened = Project::open(options(path.clone(), &fixture, true)).unwrap();
    assert_eq!(
        reopened.get_cache().unwrap()[0].hash,
        fixture["cache"]["hash"]
    );
    reopened.validate().unwrap();
    drop(reopened);
    fs::remove_file(path).unwrap();
}
