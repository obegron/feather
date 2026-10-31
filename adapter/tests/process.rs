use std::process::{Command, Stdio};
#[test]
fn eof_with_invalid_interface_exits_with_structured_error() {
    let output = Command::new(env!("CARGO_BIN_EXE_feather-transfer"))
        .args([
            "--interface",
            "feather-no-interface",
            "--name",
            "test",
            "--directory",
            "/tmp",
        ])
        .stdin(Stdio::null())
        .output()
        .expect("run helper");
    assert!(!output.status.success());
    let lines: Vec<_> = output
        .stdout
        .split(|byte| *byte == b'\n')
        .filter(|line| !line.is_empty())
        .collect();
    assert_eq!(lines.len(), 1);
    let event: serde_json::Value = serde_json::from_slice(lines[0]).unwrap();
    assert_eq!(event["event"], "fatal");
    assert!(!event["reason"].as_str().unwrap().is_empty());
}
