//! Evaluate jq programs against DuckDB JSON values
//!
//! A DuckDB extension built with [quack-rs](https://github.com/tomtom215/quack-rs).

use quack_rs::prelude::*;

// ---------------------------------------------------------------------------
// Example: a simple SQL macro. Replace with your own functions.
// ---------------------------------------------------------------------------

/// Registers all extension functions on the given connection.
fn register(con: libduckdb_sys::duckdb_connection) -> Result<(), ExtensionError> {
    // Example: register a scalar SQL macro (no unsafe callbacks needed).
    // Replace this with your own aggregate, scalar, or table functions.
    unsafe {
        SqlMacro::scalar(
            "jq_hello",
            &["name"],
            "concat('Hello from jq! ', name)",
        )?
        .register(con)?;
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// Entry point — the C Extension API handles everything, no C++ glue needed.
// ---------------------------------------------------------------------------

quack_rs::entry_point!(jq_init_c_api, |con| register(con));
