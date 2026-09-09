use serde_json::{json, Value};
use std::fs;
use std::sync::{
    atomic::{AtomicUsize, Ordering},
    Arc,
};
use std::time::{SystemTime, UNIX_EPOCH};
use streamfind_rust_core::{
    api, Method, MethodRegistry, ParameterSchema, Project, ProjectOptions, Workflow, WorkflowStep,
};

fn install_mass_spec_schema(project: &mut Project) {
    for table in [
        "MASS_SPEC_ANALYSES",
        "MASS_SPEC_SPECTRA_HEADERS",
        "MASS_SPEC_CHROMATOGRAMS_HEADERS",
        "MASS_SPEC_CHROMATOGRAMS",
        "MASS_SPEC_NTA_FEATURES",
        "MASS_SPEC_NTA_SUSPECTS",
        "MASS_SPEC_NTA_INTERNAL_STANDARDS",
        "MASS_SPEC_NTA_TRANSFORMATION_PRODUCTS",
    ] {
        project
            .execute_sql(&format!("CREATE TABLE {table} (marker VARCHAR)"))
            .unwrap();
    }
}

fn temporary_database() -> std::path::PathBuf {
    let stamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    streamfind_rust_test_support::tmp_projects_dir().join(format!("streamfind-rust-{stamp}.duckdb"))
}

#[test]
fn project_round_trips_metadata_and_schema() {
    let path = temporary_database();
    let options = ProjectOptions {
        database_path: path.clone(),
        domain: "mass_spec".into(),
        create_if_missing: false,
        read_only: false,
    };
    let mut project = Project::create(options.clone()).unwrap();
    project.set_metadata(json!({"owner": "rust-test"})).unwrap();
    assert_eq!(project.get_metadata()["owner"], "rust-test");
    project
        .set_cache("test", "test cache", "hash", &json!({"value": 42}))
        .unwrap();
    assert_eq!(project.get_cache().unwrap().len(), 1);
    assert!(!project.get_audit_trail().unwrap().is_empty());
    project.delete_cache().unwrap();
    assert!(project.get_cache().unwrap().is_empty());
    install_mass_spec_schema(&mut project);
    let copy_path = temporary_database();
    let copied = project
        .copy(ProjectOptions {
            database_path: copy_path.clone(),
            domain: String::new(),
            create_if_missing: false,
            read_only: false,
        })
        .unwrap();
    assert_eq!(copied.get_metadata()["owner"], "rust-test");
    copied.close();
    fs::remove_file(copy_path).unwrap();
    drop(project);

    let reopened = Project::open(ProjectOptions {
        read_only: true,
        ..options
    })
    .unwrap();
    assert_eq!(reopened.info().domain, "mass_spec");
    assert_eq!(reopened.info().metadata["owner"], "rust-test");
    drop(reopened);
    assert_eq!(
        serde_json::from_str::<serde_json::Value>(
            api::get_metadata(&json!({
                "database_path": path.to_string_lossy()
            }))
            .unwrap()["columns"]["metadata"][0]
                .as_str()
                .unwrap(),
        )
        .unwrap()["owner"],
        "rust-test"
    );
    assert!(api::validate(&json!({
        "database_path": path.to_string_lossy()
    }))
    .unwrap()["valid"]
        .as_bool()
        .unwrap());
    assert_eq!(
        api::get_cache_size(&json!({
            "database_path": path.to_string_lossy()
        }))
        .unwrap(),
        0
    );
    fs::remove_file(path).unwrap();
}

#[test]
fn workflow_cache_restores_written_tables() {
    let path = temporary_database();
    let mut project = Project::create(ProjectOptions {
        database_path: path.clone(),
        domain: "test".into(),
        create_if_missing: false,
        read_only: false,
    })
    .unwrap();
    let runs = Arc::new(AtomicUsize::new(0));
    let runs_for_method = Arc::clone(&runs);
    let tail_runs = Arc::new(AtomicUsize::new(0));
    let tail_runs_for_method = Arc::clone(&tail_runs);
    let mut method = Method::new(
        "test.write_table",
        "test.write_table",
        "Write a test table",
        "test",
        ParameterSchema::default(),
        Box::new(move |project, _| {
            runs_for_method.fetch_add(1, Ordering::SeqCst);
            project.execute_sql(
                "CREATE TABLE IF NOT EXISTS TEST_OUTPUT (row_key VARCHAR, value VARCHAR)",
            )?;
            project.execute_sql("DELETE FROM TEST_OUTPUT WHERE row_key = 'cache-test'")?;
            project.execute_sql("INSERT INTO TEST_OUTPUT VALUES ('cache-test', 'materialized')")?;
            Ok(json!({"status": "finished"}))
        }),
    );
    method.cacheable = true;
    method.writes = vec!["TEST_OUTPUT".into()];
    let mut registry = MethodRegistry::default();
    registry.register(method).unwrap();
    registry
        .register(Method::new(
            "test.tail",
            "test.tail",
            "Append a tail method",
            "test",
            ParameterSchema::default(),
            Box::new(move |_, _| {
                tail_runs_for_method.fetch_add(1, Ordering::SeqCst);
                Ok(json!({"status": "tail"}))
            }),
        ))
        .unwrap();
    project
        .set_workflow(
            Workflow {
                domain: "test".into(),
                steps: vec![WorkflowStep {
                    method: "test.write_table".into(),
                    parameters: json!({}),
                    metadata: None,
                }],
                ..Workflow::default()
            },
            &registry,
        )
        .unwrap();
    let workflow = project.get_workflow().unwrap();
    project
        .run_workflow(&workflow, &registry, None, None)
        .unwrap();
    let launch_snapshot: Value = serde_json::from_str(
        project
            .query_json("SELECT launch_snapshot FROM WORKFLOW_EXECUTION")
            .unwrap()[0]["launch_snapshot"]
            .as_str()
            .unwrap(),
    )
    .unwrap();
    assert_eq!(launch_snapshot["domain"], "test");
    assert_eq!(launch_snapshot["steps"].as_array().unwrap().len(), 1);
    assert_eq!(launch_snapshot["steps"][0]["id"], "test.write_table");
    project
        .execute_sql("UPDATE WORKFLOW_EXECUTION SET status = 'running'")
        .unwrap();
    drop(project);
    let mut project = Project::open(ProjectOptions {
        database_path: path.clone(),
        domain: "test".into(),
        create_if_missing: false,
        read_only: false,
    })
    .unwrap();
    assert_eq!(
        streamfind_rust_core::WorkflowExecutionManager::new(&project)
            .current()
            .unwrap()["status"],
        "interrupted"
    );
    let workflow = project.get_workflow().unwrap();
    project
        .run_workflow(&workflow, &registry, None, None)
        .unwrap();
    assert_eq!(runs.load(Ordering::SeqCst), 1);
    project
        .execute_sql("INSERT INTO WORKFLOW_EXECUTION_STEP (workflow_revision, step_index, method, parameters, parameter_hash, cache_key, status) VALUES (1, 99, 'stale', '{}', 'stale', 'stale', 'completed')")
        .unwrap();
    project
        .execute_sql("DELETE FROM TEST_OUTPUT WHERE row_key = 'cache-test'")
        .unwrap();
    let workflow = project.get_workflow().unwrap();
    project
        .run_workflow(&workflow, &registry, None, None)
        .unwrap();
    assert_eq!(runs.load(Ordering::SeqCst), 1);
    assert_eq!(
        project
            .query_json("SELECT COUNT(*) AS count FROM WORKFLOW_EXECUTION_STEP")
            .unwrap()[0]["count"],
        1
    );
    assert_eq!(
        project
            .query_json("SELECT value FROM TEST_OUTPUT WHERE row_key = 'cache-test'")
            .unwrap(),
        json!([{"value": "materialized"}])
    );
    assert_eq!(
        project
            .run_method("test.tail", &json!({}), &registry)
            .unwrap()["status"],
        "tail"
    );
    assert_eq!(runs.load(Ordering::SeqCst), 1);
    assert_eq!(tail_runs.load(Ordering::SeqCst), 1);
    project
        .execute_sql("DELETE FROM WORKFLOW_EXECUTION")
        .unwrap();
    project.delete_cache().unwrap();
    assert_eq!(
        project
            .run_method("test.tail", &json!({}), &registry)
            .unwrap()["status"],
        "tail"
    );
    assert_eq!(runs.load(Ordering::SeqCst), 2);
    assert_eq!(tail_runs.load(Ordering::SeqCst), 3);
    assert_eq!(
        project.get_workflow_execution().unwrap()[0]["status"],
        "completed"
    );
    let execution_table = api::get_workflow_execution(&json!({
        "database_path": path.to_string_lossy()
    }))
    .unwrap();
    assert_eq!(execution_table["row_count"], 3);
    assert!(execution_table["columns"]["step_index"].is_array());
    fs::remove_file(path).unwrap();
}
