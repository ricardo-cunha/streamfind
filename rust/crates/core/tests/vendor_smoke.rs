use duckdb::Connection;
use std::process::Command;

#[test]
fn duckdb_executes_query() {
    let connection = Connection::open_in_memory().unwrap();
    let value: i32 = connection
        .query_row("SELECT 42", [], |row| row.get(0))
        .unwrap();
    assert_eq!(value, 42);
}

#[test]
fn openbabel_cli_parses_benzene() {
    let output = Command::new("obabel")
        .args(["-:c1ccccc1", "-osmi"])
        .output()
        .expect("obabel must be available on PATH");
    assert!(
        output.status.success(),
        "OpenBabel failed: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    assert!(String::from_utf8_lossy(&output.stdout).contains("c1ccccc1"));
}
