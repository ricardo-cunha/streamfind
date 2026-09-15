//! Minimal MCP JSON-RPC adapter for the Rust Streamfind core.

use serde_json::{json, Value};
use streamfind_rust_core::{
    api, catalogue, MethodRegistry, OperationRegistry, Project, ProjectOptions,
};

fn tool_description(entry: &Value) -> String {
    let mut description = format!(
        "{}: {}",
        entry["label"].as_str().unwrap_or("streamfind capability"),
        entry["definition"].as_str().unwrap_or("")
    );
    if let Some(guidance) = entry["interface"]["guidance"].as_str() {
        if !guidance.is_empty() {
            description.push_str(" Guidance: ");
            description.push_str(guidance);
        }
    }
    if let Some(model) = entry["interface"]["invocation_model"].as_str() {
        description.push_str(" Invocation model: ");
        description.push_str(model);
        description.push('.');
    }
    description
}

fn enrich_methods(value: Value) -> Value {
    let Some(methods) = value.as_array() else {
        return value;
    };
    Value::Array(
        methods
            .iter()
            .map(|method| {
                let mut method = method.clone();
                if let Some(entry) = catalogue::entries()
                    .iter()
                    .find(|entry| entry["canonical_id"] == method["id"])
                {
                    method["inputSchema"] = entry["method_schema"].clone();
                    method["interface"] = entry["interface"].clone();
                }
                method
            })
            .collect(),
    )
}

pub struct Session<'a> {
    registry: &'a MethodRegistry,
    operations: &'a OperationRegistry,
    project: Option<Value>,
    domain: String,
}

impl<'a> Session<'a> {
    pub fn new(registry: &'a MethodRegistry, operations: &'a OperationRegistry) -> Self {
        Self {
            registry,
            operations,
            project: None,
            domain: String::new(),
        }
    }

    pub fn handle(&mut self, request: &Value) -> Value {
        let id = request.get("id").cloned().unwrap_or(Value::Null);
        let method = request
            .get("method")
            .and_then(Value::as_str)
            .unwrap_or_default();
        if method == "tools/list" {
            let mut catalogue = tools().as_array().cloned().unwrap_or_default();
            // All exposed domain operations are advertised from the first
            // tools/list. Operations are stateless and carry database_path, so discovery must not depend on a prior connect.
            // Methods remain session-scoped and are not tools.
            for definition in self.operations.list("") {
                if let Some(entry) = catalogue::entries()
                    .iter()
                    .find(|entry| entry["canonical_id"] == definition["id"])
                {
                    let description = entry
                        .get("definition")
                        .and_then(Value::as_str)
                        .filter(|value| !value.is_empty())
                        .map(|_| tool_description(entry))
                        .unwrap_or_else(|| {
                            definition["description"].as_str().unwrap_or("").to_owned()
                        });
                    catalogue.push(json!({
                        "name": entry["canonical_id"],
                        "description": description,
                        "inputSchema": entry["mcp"]["input_schema"],
                        "annotations": {
                            "title": entry["label"],
                            "readOnlyHint": entry["effects"]["mutates_project"] == false,
                            "destructiveHint": entry["effects"]["mutates_project"] != false,
                        },
                        "_meta": {"streamfind": entry["interface"]},
                        "effects": entry["effects"],
                    }));
                }
            }
            return json!({"jsonrpc":"2.0","id":id,"result":{"tools":catalogue}});
        }
        if method == "tools/call" {
            let params = request.get("params").cloned().unwrap_or_else(|| json!({}));
            let name = params
                .get("name")
                .and_then(Value::as_str)
                .unwrap_or_default();
            if name == "connect" {
                let args = params.get("arguments").unwrap_or(&Value::Null);
                return match api::get_domain(args) {
                    Ok(domain) => {
                        self.domain = domain.as_str().unwrap_or_default().into();
                        self.project = Some(args.clone());
                        response(
                            id,
                            json!({"status": "finished", "info": "Project connected successfully."}),
                        )
                    }
                    Err(error) => error_response(id, error.to_string()),
                };
            }
            if !tools()
                .as_array()
                .unwrap()
                .iter()
                .any(|tool| tool["name"] == name)
                && self
                    .registry
                    .list(&self.domain)
                    .iter()
                    .any(|tool| tool["id"] == name)
            {
                let mut args = self.project.clone().unwrap_or_else(|| json!({}));
                args["method"] = json!(name);
                args["parameters"] = params
                    .get("arguments")
                    .cloned()
                    .unwrap_or_else(|| json!({}));
                return match api::run_method(&args, self.registry) {
                    Ok(value) => response(id, value),
                    Err(error) => error_response(id, error.to_string()),
                };
            }
            if self
                .operations
                .list("")
                .iter()
                .any(|tool| tool["id"] == name)
            {
                let result = (|| {
                    let empty_args = json!({});
                    let args = params.get("arguments").unwrap_or(&empty_args);
                    let database_path = args["database_path"]
                        .as_str()
                        .ok_or_else(|| "missing database_path".to_string())?;
                    let options = ProjectOptions {
                        database_path: database_path.into(),
                        domain: self
                            .operations
                            .list("")
                            .into_iter()
                            .find(|tool| tool["id"] == name)
                            .and_then(|tool| tool["domain"].as_str().map(Into::into))
                            .unwrap_or_default(),
                        create_if_missing: false,
                        read_only: false,
                    };
                    let mut project = Project::open(options).map_err(|e| e.to_string())?;
                    project
                        .run_operation(name, args, self.operations)
                        .map_err(|e| e.to_string())
                })();
                return match result {
                    Ok(value) => response(id, value),
                    Err(error) => error_response(id, error),
                };
            }
            if name == "close" {
                let result = handle(request, self.registry);
                if result["result"]["isError"] != true {
                    self.project = None;
                    self.domain.clear();
                }
                return result;
            }
            if name == "get_available_methods" {
                return match api::get_available_methods(
                    params.get("arguments").unwrap_or(&json!({})),
                    self.registry,
                ) {
                    Ok(value) => response(id, enrich_methods(value)),
                    Err(error) => error_response(id, error.to_string()),
                };
            }
        }
        handle(request, self.registry)
    }
}

pub fn tools() -> Value {
    // Catalogue-backed tool definitions; on a catalogue miss this degrades to
    // a minimal toolset (empty array), matching the C++ behaviour.
    catalogue::tools_json()
}

pub fn handle(request: &Value, _registry: &MethodRegistry) -> Value {
    let id = request.get("id").cloned().unwrap_or(Value::Null);
    let method = request
        .get("method")
        .and_then(Value::as_str)
        .unwrap_or_default();
    if method == "initialize" {
        return json!({"jsonrpc":"2.0","id":id,"result":{"protocolVersion":"2025-03-26","capabilities":{"tools":{}},"serverInfo":{"name":"streamfind-rust","version":env!("CARGO_PKG_VERSION")},"instructions":catalogue::interface_guidance()}});
    }
    if method == "tools/list" {
        return json!({"jsonrpc":"2.0","id":id,"result":{"tools":tools()}});
    }
    if method != "tools/call" {
        return json!({"jsonrpc":"2.0","id":id,"error":{"code":-32601,"message":"Unsupported MCP method"}});
    }
    let params = request.get("params").cloned().unwrap_or_else(|| json!({}));
    let name = params
        .get("name")
        .and_then(Value::as_str)
        .unwrap_or_default();
    let args = params.get("arguments").unwrap_or(&Value::Null);
    let result = match name {
        "create" => api::create(args),
        "describe" => api::describe(args),
        "validate" => api::validate(args),
        "get_domain" => api::get_domain(args),
        "get_metadata" => api::get_metadata(args),
        "set_metadata" => api::set_metadata(args),
        "get_workflow" => api::get_workflow(args),
        "get_workflow_execution" => api::get_workflow_execution(args),
        "create_workflow_execution" => api::create_workflow_execution(args),
        "get_execution" => api::get_execution(args),
        "list_executions" => api::list_executions(args),
        "transition_execution" => api::transition_execution(args),
        "cancel_execution" => api::cancel_execution(args),
        "set_workflow" => api::set_workflow(args, _registry),
        "add_method" => api::add_method(args, _registry),
        "remove_method" => api::remove_method(args, _registry),
        "validate_workflow" => api::validate_workflow(args, _registry),
        "run_workflow" => api::run_workflow(args, _registry),
        "get_cache" => api::get_cache(args),
        "get_cache_size" => api::get_cache_size(args),
        "delete_cache" => api::delete_cache(args),
        "get_audit_trail" => api::get_audit_trail(args),
        "get_available_methods" => api::get_available_methods(args, _registry),
        "run_method" => api::run_method(args, _registry),
        "copy" => api::copy(args),
        "close" => api::close(args),
        "tools_status" => api::tools_status(args),
        "tools_install" => api::tools_install(args),
        "tools_install_java" => api::tools_install_java(args),
        "tools_install_metfrag" => api::tools_install_metfrag(args),
        _ => {
            return json!({"jsonrpc":"2.0","id":id,"error":{"code":-32602,"message":"Unknown MCP tool"}})
        }
    };
    match result {
        Ok(value) => {
            json!({"jsonrpc":"2.0","id":id,"result":{"content":[{"type":"text","text":value.to_string()}]}})
        }
        Err(error) => {
            json!({"jsonrpc":"2.0","id":id,"result":{"isError":true,"content":[{"type":"text","text":error.to_string()}]}})
        }
    }
}

fn response(id: Value, value: Value) -> Value {
    json!({"jsonrpc":"2.0","id":id,"result":{"content":[{"type":"text","text":value.to_string()}]}})
}

fn error_response(id: Value, error: String) -> Value {
    json!({"jsonrpc":"2.0","id":id,"result":{"isError":true,"content":[{"type":"text","text":error}]}})
}
