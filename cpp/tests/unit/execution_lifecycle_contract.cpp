#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>

#include "streamfind/project.hpp"
#include "../tmp_projects.hpp"

#ifndef STREAMFIND_MULTIPROJECT_FIXTURE
#error STREAMFIND_MULTIPROJECT_FIXTURE is required
#endif
#ifndef STREAMFIND_EXECUTION_LIFECYCLE_FIXTURE
#error STREAMFIND_EXECUTION_LIFECYCLE_FIXTURE is required
#endif
#ifndef STREAMFIND_MCP_EXECUTION_FIXTURE
#error STREAMFIND_MCP_EXECUTION_FIXTURE is required
#endif

namespace streamfind::execution_contract {

Json load_fixture(const char *path) {
    std::ifstream input(path);
    return Json::parse(std::string(std::istreambuf_iterator<char>(input), {}));
}

std::string transition_key(const Json &transition) {
    return transition.at(0).get<std::string>() + "->" + transition.at(1).get<std::string>();
}

std::set<std::string> string_set(const Json &values) {
    std::set<std::string> result;
    for (const auto &value : values) result.insert(value.get<std::string>());
    return result;
}

ExecutionState state(const std::string &value) {
    if (value == "queued") return ExecutionState::queued;
    if (value == "running") return ExecutionState::running;
    if (value == "cancelling") return ExecutionState::cancelling;
    if (value == "cancelled") return ExecutionState::cancelled;
    if (value == "completed") return ExecutionState::completed;
    if (value == "failed") return ExecutionState::failed;
    return ExecutionState::interrupted;
}

void assert_request(const Json &requests, const char *name,
                    const std::set<std::string> &required,
                    const std::set<std::string> &optional) {
    const auto &request = requests.at(name);
    if (string_set(request.at("required")) != required) {
        throw std::runtime_error("execution contract: MCP required fields changed");
    }
    const auto actual_optional = request.contains("optional")
                                     ? string_set(request.at("optional"))
                                     : std::set<std::string>{};
    if (actual_optional != optional) {
        throw std::runtime_error("execution contract: MCP optional fields changed");
    }
}

void run() {
    const auto multiproject = load_fixture(STREAMFIND_MULTIPROJECT_FIXTURE);
    const auto lifecycle = load_fixture(STREAMFIND_EXECUTION_LIFECYCLE_FIXTURE);
    const auto mcp = load_fixture(STREAMFIND_MCP_EXECUTION_FIXTURE);

    if (multiproject.at("fixture_id") != "separate_project_files" ||
        multiproject.at("consumers") != Json{"cpp", "rust"} ||
        lifecycle.at("fixture_id") != "execution_lifecycle" ||
        lifecycle.at("consumers") != Json{"cpp", "rust"} ||
        mcp.at("fixture_id") != "mcp_execution_contract" ||
        mcp.at("consumers") != Json{"cpp", "rust"}) {
        throw std::runtime_error("execution contract: fixture metadata changed");
    }

    const std::set<std::string> expected_states = {
        "queued", "running", "cancelling", "cancelled", "completed", "failed", "interrupted"};
    if (string_set(lifecycle.at("states")) != expected_states) {
        throw std::runtime_error("execution contract: lifecycle state names changed");
    }
    const std::set<std::string> expected_required = {
        "domain_id", "workflow_revision", "status"};
    if (string_set(lifecycle.at("required_fields")) != expected_required) {
        throw std::runtime_error("execution contract: required field names changed");
    }
    const std::set<std::string> expected_progress = {
        "completed", "total", "current_step_index", "current_step_id", "message"};
    if (string_set(lifecycle.at("progress_fields")) != expected_progress) {
        throw std::runtime_error("execution contract: progress field names changed");
    }
    if (string_set(lifecycle.at("terminal_states")) !=
        std::set<std::string>{"cancelled", "completed", "failed", "interrupted"}) {
        throw std::runtime_error("execution contract: terminal states changed");
    }

    std::set<std::string> valid_transitions;
    for (const auto &transition : lifecycle.at("transitions")) {
        if (!expected_states.contains(transition.at(0).get<std::string>()) ||
            !expected_states.contains(transition.at(1).get<std::string>())) {
            throw std::runtime_error("execution contract: transition references unknown state");
        }
        valid_transitions.insert(transition_key(transition));
    }
    const std::set<std::string> expected_valid_transitions = {
        "queued->running", "queued->cancelled", "running->completed",
        "running->failed", "running->cancelling", "cancelling->cancelled",
        "running->interrupted"};
    if (valid_transitions != expected_valid_transitions) {
        throw std::runtime_error("execution contract: valid transition set changed");
    }
    const std::set<std::string> expected_invalid_transitions = {
        "queued->completed", "queued->failed", "running->cancelled",
        "cancelling->completed", "completed->running", "failed->running",
        "cancelled->running", "interrupted->running"};
    std::set<std::string> invalid_transitions;
    for (const auto &transition : lifecycle.at("invalid_transitions")) {
        if (!expected_states.contains(transition.at(0).get<std::string>()) ||
            !expected_states.contains(transition.at(1).get<std::string>())) {
            throw std::runtime_error("execution contract: invalid transition references unknown state");
        }
        invalid_transitions.insert(transition_key(transition));
        if (valid_transitions.contains(transition_key(transition))) {
            throw std::runtime_error("execution contract: invalid transition is listed as valid");
        }
    }
    if (invalid_transitions != expected_invalid_transitions) {
        throw std::runtime_error("execution contract: invalid transition set changed");
    }
    for (const auto &transition : lifecycle.at("transitions")) {
        if (!valid_execution_transition(state(transition.at(0)), state(transition.at(1)))) {
            throw std::runtime_error("execution contract: valid transition rejected");
        }
    }
    for (const auto &transition : lifecycle.at("invalid_transitions")) {
        if (valid_execution_transition(state(transition.at(0)), state(transition.at(1)))) {
            throw std::runtime_error("execution contract: invalid transition accepted");
        }
    }
    if (lifecycle.at("cancellation").at("queued") != "cancelled" ||
        lifecycle.at("cancellation").at("running") != "cancelling" ||
        lifecycle.at("cancellation").at("cancelling") != "cancelled" ||
        lifecycle.at("cancellation").at("terminal") != "rejected") {
        throw std::runtime_error("execution contract: cancellation rules changed");
    }

    const std::set<std::string> expected_error_codes = {
        "project_not_found", "project_domain_mismatch", "workflow_not_found",
        "workflow_invalid", "execution_not_found", "execution_not_cancellable",
        "execution_state_conflict", "execution_interrupted", "backend_busy",
        "database_error", "method_error"};
    if (string_set(mcp.at("error_codes")) != expected_error_codes) {
        throw std::runtime_error("execution contract: error categories changed");
    }
    for (const auto &request : {"start", "list", "get", "cancel"}) {
        if (!mcp.at("requests").contains(request)) {
            throw std::runtime_error("execution contract: missing MCP request shape");
        }
    }
    assert_request(mcp.at("requests"), "start", {"database_path"},
                   {"workflow_revision", "parameters"});
    assert_request(mcp.at("requests"), "list", {"database_path"},
                   {"status"});
    assert_request(mcp.at("requests"), "get", {"database_path"}, {});
    assert_request(mcp.at("requests"), "cancel", {"database_path"}, {});
    if (string_set(mcp.at("result_fields")) !=
        std::set<std::string>{"domain_id", "workflow_revision",
                              "status", "progress", "result_reference", "error"}) {
        throw std::runtime_error("execution contract: result field names changed");
    }
    if (string_set(mcp.at("progress_fields")) != expected_progress) {
        throw std::runtime_error("execution contract: MCP progress field names changed");
    }

    const auto path = streamfind::test::tmp_projects_dir() / "execution-manager.duckdb";
    std::filesystem::remove(path);
    ProjectOptions options{path.string(), "mass_spec", {}};
    auto project = Project::create(options);
    WorkflowExecutionManager manager(project);
    const auto created = manager.create({{"method", "step"}, {"workflow_revision", 1}, {"step_index", 0}});
    if (created.at("status") != "queued" || created.contains("execution_id") || manager.current().contains("execution_id") || manager.list().size() != 1) throw std::runtime_error("execution manager project-scoped create/current/list failed");
    bool duplicate_rejected = false;
    try { manager.create({{"workflow_revision", 1}}); } catch (const Error &) { duplicate_rejected = true; }
    if (!duplicate_rejected) throw std::runtime_error("active project accepted a second workflow execution");
    if (manager.scheduler_tick("worker-a").at("status") != "running") throw std::runtime_error("scheduler did not claim queued execution");
    bool stale_claim_rejected = false;
    try { manager.scheduler_tick("worker-b"); } catch (const Error &) { stale_claim_rejected = true; }
    if (!stale_claim_rejected) throw std::runtime_error("stale worker claim was accepted");
    bool stale_release_rejected = false;
    try { manager.release_worker("worker-b", ExecutionState::completed); } catch (const Error &) { stale_release_rejected = true; }
    if (!stale_release_rejected) throw std::runtime_error("stale worker release was accepted");
    bool workflow_mutation_blocked = false;
    try { project.set_workflow(project.get_workflow()); } catch (const Error &) { workflow_mutation_blocked = true; }
    if (!workflow_mutation_blocked) throw std::runtime_error("workflow mutation was not blocked during active execution");
    if (manager.cancel().at("status") != "cancelling") throw std::runtime_error("execution manager running cancellation failed");
    manager.transition(ExecutionState::cancelled);
    bool rejected = false;
    try { manager.transition(ExecutionState::running); } catch (const Error &) { rejected = true; }
    if (!rejected) throw std::runtime_error("execution manager accepted invalid transition");
    manager.create({{"workflow_revision", 2}});
    if (manager.cancel().at("status") != "cancelled") throw std::runtime_error("execution manager queued cancellation failed");
    const auto generated = manager.create({{"workflow_revision", 4}});

    if (generated.contains("execution_id") || generated.at("status") != "queued") {
        throw std::runtime_error("execution manager exposed internal identity: " + generated.dump());
    }
    const auto other_path = streamfind::test::tmp_projects_dir() / "execution-manager-other.duckdb";
    std::filesystem::remove(other_path);
    auto other = Project::create({other_path.string(), "mass_spec", {}});
    WorkflowExecutionManager other_manager(other);
    if (!other_manager.list().empty()) throw std::runtime_error("execution manager leaked rows across projects");
    if (other_manager.create({{"workflow_revision", 1}}).at("status") != "queued") throw std::runtime_error("independent project could not queue execution");
    if (other.run_worker("other-worker").at("status") != "completed") throw std::runtime_error("one-shot worker could not execute and release its workflow");
    manager.scheduler_tick("worker-a");
    project.close();
    auto reopened = Project::open(options);
    if (WorkflowExecutionManager(reopened).current().at("status") != "interrupted") throw std::runtime_error("execution manager restart recovery failed");
    std::filesystem::remove(path);
    other.close();
    std::filesystem::remove(other_path);
}

} // namespace streamfind::execution_contract

int main() {
    try {
        streamfind::execution_contract::run();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
