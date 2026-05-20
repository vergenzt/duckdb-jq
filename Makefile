PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=jq
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# On Windows (mingw / rtools42), jq's autotools build needs autoconf,
# automake, and libtool, which aren't pre-installed. Install them via
# pacman before the build runs. This overrides the no-op configure_ci
# target from extension-ci-tools.
ifeq ($(OS),Windows_NT)
configure_ci:
	pacman -S --noconfirm --needed autoconf automake libtool
endif