# DuckDB Rust Extension Makefile
# Delegates to cargo for building and extension-ci-tools for metadata.

PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Extension configuration
EXT_NAME=jq
EXT_CONFIG=$(PROJ_DIR)extension_config.cmake

# DuckDB C API version (NOT the DuckDB release version)
# See: https://github.com/tomtom215/quack-rs/blob/main/LESSONS.md (Pitfall P2)
USE_UNSTABLE_C_API=1
DUCKDB_PLATFORM_VERSION=v1.5.0

# Include extension-ci-tools build rules
include extension-ci-tools/makefiles/c_api_extensions/base.Makefile
include extension-ci-tools/makefiles/c_api_extensions/rust.Makefile
