//! Minimal command-line interface for the Rust project backend.
//!
//! Supported commands are `create` and `describe`. Both require
//! `--database-path`. The `tools` subcommand manages the
//! `~/.streamfind` external-tools layout (Java + MetFragCL), mirroring
//! `bindings/r`.

use std::env;
use std::path::PathBuf;
use streamfind_rust_core::{Project, ProjectOptions, Result};

fn option(args: &[String], name: &str) -> Result<String> {
    args.windows(2)
        .find(|pair| pair[0] == name)
        .map(|pair| pair[1].clone())
        .ok_or_else(|| streamfind_rust_core::Error {
            code: streamfind_rust_core::ErrorCode::InvalidArgument,
            message: format!("missing required option --{name}"),
        })
}

fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
        std::process::exit(1);
    }
}

fn tool_error(message: String) -> streamfind_rust_core::Error {
    streamfind_rust_core::Error {
        code: streamfind_rust_core::ErrorCode::InvalidArgument,
        message,
    }
}

/// Subcommands around the `~/.streamfind` external-tools layout
/// (Java/Temurin JDK 21 + MetFragCL), mirroring `bindings/r`.
fn run_tools(args: &[String]) -> Result<()> {
    use streamfind_external::tools;
    let sub = args.get(2).map(String::as_str).unwrap_or("status");
    match sub {
        "status" => {
            let home = tools::streamfind_home();
            let java = tools::resolve_java();
            let jar = tools::resolve_metfrag_jar();
            println!(
                "{}",
                serde_json::json!({
                    "home": home,
                    "java": java.map(|p| p.to_string_lossy().into_owned()),
                    "metfrag": jar.map(|p| p.to_string_lossy().into_owned()),
                })
            );
            Ok(())
        }
        "install-java" => {
            let java = tools::install_java().map_err(tool_error)?;
            println!("java: {}", java.to_string_lossy());
            Ok(())
        }
        "install-metfrag" => {
            let jar = tools::install_metfrag().map_err(tool_error)?;
            println!("metfrag: {}", jar.to_string_lossy());
            Ok(())
        }
        "install" => {
            let (java, jar) = tools::install_metfrag_stack().map_err(tool_error)?;
            println!(
                "java: {}\nmetfrag: {}",
                java.to_string_lossy(),
                jar.to_string_lossy()
            );
            Ok(())
        }
        _ => Err(tool_error(format!(
            "unknown tools subcommand '{sub}' (status | install | install-java | install-metfrag)"
        ))),
    }
}

fn run() -> Result<()> {
    let args: Vec<String> = env::args().collect();
    if args.get(1).map(String::as_str) == Some("tools") {
        return run_tools(&args);
    }
    let command = args.get(1).map(String::as_str).unwrap_or("describe");
    let options = ProjectOptions {
        database_path: PathBuf::from(option(&args, "--database-path")?),
        domain: args
            .windows(2)
            .find(|pair| pair[0] == "--domain")
            .map(|pair| pair[1].clone())
            .unwrap_or_default(),
        create_if_missing: false,
        read_only: command == "describe",
    };
    let project = if command == "create" {
        Project::create(options)?
    } else {
        Project::open(options)?
    };
    println!(
        "{}",
        serde_json::json!({"domain": project.info().domain, "metadata": project.info().metadata})
    );
    Ok(())
}
