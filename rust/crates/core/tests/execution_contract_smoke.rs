use serde_json::Value;
use std::collections::BTreeSet;
use std::fs;
use streamfind_rust_core::{
    valid_execution_transition, ExecutionState, MethodRegistry, Project, ProjectOptions,
    WorkflowExecutionManager,
};

fn fixture(path: &str) -> Value {
    serde_json::from_str(path).unwrap()
}

fn transition_key(transition: &Value) -> String {
    format!(
        "{}->{}",
        transition[0].as_str().unwrap(),
        transition[1].as_str().unwrap()
    )
}

fn state(value: &str) -> ExecutionState {
    match value {
        "queued" => ExecutionState::Queued,
        "running" => ExecutionState::Running,
        "cancelling" => ExecutionState::Cancelling,
        "cancelled" => ExecutionState::Cancelled,
        "completed" => ExecutionState::Completed,
        "failed" => ExecutionState::Failed,
        _ => ExecutionState::Interrupted,
    }
}

fn string_set(values: &Value) -> BTreeSet<String> {
    values
        .as_array()
        .unwrap()
        .iter()
        .map(|value| value.as_str().unwrap().to_owned())
        .collect()
}

fn assert_request(requests: &Value, name: &str, required: &[&str], optional: &[&str]) {
    let expected_required: BTreeSet<String> =
        required.iter().map(|value| (*value).to_owned()).collect();
    let expected_optional: BTreeSet<String> =
        optional.iter().map(|value| (*value).to_owned()).collect();
    assert_eq!(string_set(&requests[name]["required"]), expected_required);
    let actual_optional = requests[name]
        .get("optional")
        .map(string_set)
        .unwrap_or_default();
    assert_eq!(actual_optional, expected_optional);
}

#[test]
fn shared_execution_contract_is_consistent() {
    let multiproject = fixture(include_str!(
        "../../../../tests/fixtures/project/multiproject.json"
    ));
    let lifecycle = fixture(include_str!(
        "../../../../tests/fixtures/execution/execution_lifecycle.json"
    ));
    let mcp = fixture(include_str!(
        "../../../../tests/fixtures/mcp/execution_contract.json"
    ));

    assert_eq!(multiproject["fixture_id"], "separate_project_files");
    assert_eq!(
        multiproject["consumers"],
        serde_json::json!(["cpp", "rust"])
    );
    assert_eq!(lifecycle["fixture_id"], "execution_lifecycle");
    assert_eq!(lifecycle["consumers"], serde_json::json!(["cpp", "rust"]));
    assert_eq!(mcp["fixture_id"], "mcp_execution_contract");
    assert_eq!(mcp["consumers"], serde_json::json!(["cpp", "rust"]));

    let projects = multiproject["databases"].as_array().unwrap();
    assert_eq!(projects.len(), 2);
    assert_eq!(projects[0]["domain_id"], "mass_spec");
    assert_eq!(projects[1]["domain_id"], "raman");
    let expected_project_assertions: BTreeSet<String> = [
        "open_separate_database_files",
        "reopen_preserves_each_domain",
        "project_domain_is_immutable",
        "database_a_cannot_read_database_b_rows",
        "database_a_cannot_write_database_b_rows",
        "cache_and_audit_rows_are_file_scoped",
    ]
    .into_iter()
    .map(str::to_owned)
    .collect();
    assert_eq!(
        string_set(&multiproject["assertions"]),
        expected_project_assertions
    );

    let expected_states: BTreeSet<String> = [
        "queued",
        "running",
        "cancelling",
        "cancelled",
        "completed",
        "failed",
        "interrupted",
    ]
    .into_iter()
    .map(str::to_owned)
    .collect();
    assert_eq!(string_set(&lifecycle["states"]), expected_states);

    let expected_required: BTreeSet<String> = ["domain_id", "workflow_revision", "status"]
        .into_iter()
        .map(str::to_owned)
        .collect();
    assert_eq!(string_set(&lifecycle["required_fields"]), expected_required);

    let expected_progress: BTreeSet<String> = [
        "completed",
        "total",
        "current_step_index",
        "current_step_id",
        "message",
    ]
    .into_iter()
    .map(str::to_owned)
    .collect();
    assert_eq!(string_set(&lifecycle["progress_fields"]), expected_progress);
    let expected_terminal: BTreeSet<String> = ["cancelled", "completed", "failed", "interrupted"]
        .into_iter()
        .map(str::to_owned)
        .collect();
    assert_eq!(string_set(&lifecycle["terminal_states"]), expected_terminal);

    let valid_transitions: BTreeSet<String> = lifecycle["transitions"]
        .as_array()
        .unwrap()
        .iter()
        .map(transition_key)
        .collect();
    let expected_valid_transitions: BTreeSet<String> = [
        "queued->running",
        "queued->cancelled",
        "running->completed",
        "running->failed",
        "running->cancelling",
        "cancelling->cancelled",
        "running->interrupted",
    ]
    .into_iter()
    .map(str::to_owned)
    .collect();
    assert_eq!(valid_transitions, expected_valid_transitions);

    let expected_invalid_transitions: BTreeSet<String> = [
        "queued->completed",
        "queued->failed",
        "running->cancelled",
        "cancelling->completed",
        "completed->running",
        "failed->running",
        "cancelled->running",
        "interrupted->running",
    ]
    .into_iter()
    .map(str::to_owned)
    .collect();
    let mut invalid_transitions = BTreeSet::new();
    for transition in lifecycle["invalid_transitions"].as_array().unwrap() {
        assert!(expected_states.contains(transition[0].as_str().unwrap()));
        assert!(expected_states.contains(transition[1].as_str().unwrap()));
        invalid_transitions.insert(transition_key(transition));
        assert!(!valid_transitions.contains(&transition_key(transition)));
    }
    assert_eq!(invalid_transitions, expected_invalid_transitions);
    for transition in lifecycle["transitions"].as_array().unwrap() {
        assert!(valid_execution_transition(
            state(transition[0].as_str().unwrap()),
            state(transition[1].as_str().unwrap())
        ));
    }
    for transition in lifecycle["invalid_transitions"].as_array().unwrap() {
        assert!(!valid_execution_transition(
            state(transition[0].as_str().unwrap()),
            state(transition[1].as_str().unwrap())
        ));
    }

    assert_eq!(lifecycle["cancellation"]["queued"], "cancelled");
    assert_eq!(lifecycle["cancellation"]["running"], "cancelling");
    assert_eq!(lifecycle["cancellation"]["cancelling"], "cancelled");
    assert_eq!(lifecycle["cancellation"]["terminal"], "rejected");

    let expected_errors: BTreeSet<String> = [
        "project_not_found",
        "project_domain_mismatch",
        "workflow_not_found",
        "workflow_invalid",
        "execution_not_found",
        "execution_not_cancellable",
        "execution_state_conflict",
        "execution_interrupted",
        "backend_busy",
        "database_error",
        "method_error",
    ]
    .into_iter()
    .map(str::to_owned)
    .collect();
    assert_eq!(string_set(&mcp["error_codes"]), expected_errors);

    for request in ["start", "list", "get", "cancel"] {
        assert!(mcp["requests"].get(request).is_some());
    }
    assert_request(
        &mcp["requests"],
        "start",
        &["database_path"],
        &["workflow_revision", "parameters"],
    );
    assert_request(&mcp["requests"], "list", &["database_path"], &["status"]);
    assert_request(&mcp["requests"], "get", &["database_path"], &[]);
    assert_request(&mcp["requests"], "cancel", &["database_path"], &[]);
    let expected_result: BTreeSet<String> = [
        "domain_id",
        "workflow_revision",
        "status",
        "progress",
        "result_reference",
        "error",
    ]
    .into_iter()
    .map(str::to_owned)
    .collect();
    assert_eq!(string_set(&mcp["result_fields"]), expected_result);
    assert_eq!(string_set(&mcp["progress_fields"]), expected_progress);
}

#[test]
fn manager_persists_lifecycle_and_recovers_on_reopen() {
    let path =
        streamfind_rust_test_support::tmp_projects_dir().join("execution-manager-rust.duckdb");
    let _ = fs::remove_file(&path);
    let options = ProjectOptions {
        database_path: path.clone(),
        domain: "mass_spec".into(),
        create_if_missing: false,
        read_only: false,
    };
    let mut project = Project::create(options.clone()).unwrap();
    {
        let manager = WorkflowExecutionManager::new(&project);
        let created = manager
            .create(&serde_json::json!({"method":"step","workflow_revision":1,"step_index":0}))
            .unwrap();
        assert_eq!(created["status"], "queued");
        assert!(created.get("execution_id").is_none());
        assert!(manager.current().unwrap().get("execution_id").is_none());
        assert_eq!(manager.list().unwrap().as_array().unwrap().len(), 1);
        assert!(manager
            .create(&serde_json::json!({"workflow_revision": 1}))
            .is_err());
        assert_eq!(
            manager.scheduler_tick("worker-a").unwrap()["status"],
            "running"
        );
        assert!(manager.scheduler_tick("worker-b").is_err());
        assert!(manager
            .release_worker("worker-b", ExecutionState::Completed)
            .is_err());
    }
    assert!(project
        .set_workflow(project.get_workflow().unwrap(), &MethodRegistry::default())
        .is_err());
    let manager = WorkflowExecutionManager::new(&project);
    assert_eq!(manager.cancel().unwrap()["status"], "cancelling");
    manager.transition(ExecutionState::Cancelled).unwrap();
    assert!(manager.transition(ExecutionState::Running).is_err());
    manager
        .create(&serde_json::json!({"workflow_revision":2}))
        .unwrap();
    assert_eq!(manager.cancel().unwrap()["status"], "cancelled");
    let generated = manager
        .create(&serde_json::json!({"workflow_revision":4}))
        .unwrap();
    assert!(generated.get("execution_id").is_none());
    assert_eq!(generated["status"], "queued");
    let other_path = streamfind_rust_test_support::tmp_projects_dir()
        .join("execution-manager-rust-other.duckdb");
    let _ = fs::remove_file(&other_path);
    let mut other = Project::create(ProjectOptions {
        database_path: other_path.clone(),
        domain: "mass_spec".into(),
        create_if_missing: false,
        read_only: false,
    })
    .unwrap();
    let other_manager = WorkflowExecutionManager::new(&other);
    assert_eq!(
        other_manager
            .create(&serde_json::json!({"workflow_revision": 1}))
            .unwrap()["status"],
        "queued"
    );
    drop(other_manager);
    let registry = MethodRegistry::default();
    assert_eq!(
        other.run_worker("other-worker", &registry).unwrap()["status"],
        "completed"
    );
    other.close();
    let _ = fs::remove_file(&other_path);
    manager.scheduler_tick("worker-a").unwrap();
    project.close();
    let reopened = Project::open(options).unwrap();
    assert_eq!(
        WorkflowExecutionManager::new(&reopened).current().unwrap()["status"],
        "interrupted"
    );
    reopened.close();
    let _ = fs::remove_file(path);
}
