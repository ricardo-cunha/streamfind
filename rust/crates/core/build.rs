use std::{
    env, fs,
    path::{Path, PathBuf},
};

fn copy_runtime(profile_dir: &Path, source: &Path) {
    let Some(name) = source.file_name() else {
        return;
    };
    for destination_dir in [profile_dir.to_path_buf(), profile_dir.join("deps")] {
        if let Err(error) = fs::create_dir_all(&destination_dir) {
            panic!(
                "cannot create DuckDB runtime directory {}: {error}",
                destination_dir.display()
            );
        }
        let destination = destination_dir.join(name);
        if let Err(error) = fs::copy(source, &destination) {
            panic!(
                "cannot copy DuckDB runtime {} to {}: {error}",
                source.display(),
                destination.display()
            );
        }
        println!(
            "cargo:warning=Copied DuckDB runtime to {}",
            destination.display()
        );
    }
}

fn main() {
    println!("cargo:rerun-if-changed=../../../core/vendor/duckdb/lib/windows-x64/duckdb.dll");
    if env::var_os("CARGO_CFG_WINDOWS").is_none() {
        return;
    }

    let manifest_dir = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let runtime = manifest_dir.join("../../../core/vendor/duckdb/lib/windows-x64/duckdb.dll");
    if !runtime.is_file() {
        panic!(
            "repository DuckDB runtime is missing: {}",
            runtime.display()
        );
    }

    let out_dir = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    let profile_dir = out_dir
        .parent()
        .and_then(Path::parent)
        .and_then(Path::parent)
        .expect("Cargo OUT_DIR does not have the expected profile layout");
    copy_runtime(profile_dir, &runtime);
}
