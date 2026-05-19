cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME jq)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

# --- libjq (built from the ./jq submodule via its own autotools build) ---
include(ExternalProject)

set(JQ_SRC_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/jq)
set(JQ_BUILD_DIR   ${CMAKE_CURRENT_BINARY_DIR}/jq-build)
set(JQ_INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/jq-install)
set(JQ_STATIC_LIB    ${JQ_INSTALL_DIR}/lib/libjq.a)
set(ONIG_STATIC_LIB  ${JQ_INSTALL_DIR}/lib/libonig.a)

# When CMake is cross-compiling on macOS (e.g. arm64 host targeting x86_64),
# CMAKE_OSX_ARCHITECTURES carries the target arch. Autotools won't see that
# without explicit env + --host, so libjq/libonig would end up built for the
# host arch and fail to link.
set(JQ_EXTRA_CONFIGURE_ARGS "")
set(JQ_CONFIGURE_ENV ${CMAKE_COMMAND} -E env)
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
  list(LENGTH CMAKE_OSX_ARCHITECTURES _jq_n_archs)
  if(NOT _jq_n_archs EQUAL 1)
    message(FATAL_ERROR "jq sub-build needs exactly one CMAKE_OSX_ARCHITECTURES; got: ${CMAKE_OSX_ARCHITECTURES}")
  endif()
  set(_jq_arch_flag "-arch ${CMAKE_OSX_ARCHITECTURES}")
  list(APPEND JQ_CONFIGURE_ENV
    "CFLAGS=${_jq_arch_flag}"
    "CXXFLAGS=${_jq_arch_flag}"
    "LDFLAGS=${_jq_arch_flag}")
  if(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
    list(APPEND JQ_EXTRA_CONFIGURE_ARGS --host=x86_64-apple-darwin)
  elseif(CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    list(APPEND JQ_EXTRA_CONFIGURE_ARGS --host=aarch64-apple-darwin)
  endif()
endif()

ExternalProject_Add(jq_external
  SOURCE_DIR        ${JQ_SRC_DIR}
  BINARY_DIR        ${JQ_BUILD_DIR}
  INSTALL_DIR       ${JQ_INSTALL_DIR}
  CONFIGURE_COMMAND ${CMAKE_COMMAND} -E chdir ${JQ_SRC_DIR} autoreconf -i
            COMMAND ${JQ_CONFIGURE_ENV} ${JQ_SRC_DIR}/configure
                      ${JQ_EXTRA_CONFIGURE_ARGS}
                      --prefix=${JQ_INSTALL_DIR}
                      --disable-shared --enable-static --with-pic
                      --disable-maintainer-mode --disable-docs
                      --with-oniguruma=builtin
  BUILD_COMMAND      make
  INSTALL_COMMAND    make install
  BUILD_BYPRODUCTS   ${JQ_STATIC_LIB} ${ONIG_STATIC_LIB}
)

# Make headers visible at configure time (the dir exists after install; create
# it now so include_directories doesn't warn on first run).
file(MAKE_DIRECTORY ${JQ_INSTALL_DIR}/include)

add_library(libjq STATIC IMPORTED GLOBAL)
add_dependencies(libjq jq_external)
set_target_properties(libjq PROPERTIES
  IMPORTED_LOCATION             ${JQ_STATIC_LIB}
  INTERFACE_INCLUDE_DIRECTORIES ${JQ_INSTALL_DIR}/include
)

add_library(libonig STATIC IMPORTED GLOBAL)
add_dependencies(libonig jq_external)
set_target_properties(libonig PROPERTIES
  IMPORTED_LOCATION ${ONIG_STATIC_LIB}
)

set(EXTENSION_SOURCES src/jq_extension.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Link libjq (with its bundled oniguruma) in both the static library and the loadable extension
target_link_libraries(${EXTENSION_NAME}          libjq libonig)
target_link_libraries(${LOADABLE_EXTENSION_NAME} libjq libonig)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
