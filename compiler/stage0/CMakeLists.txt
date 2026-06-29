add_executable(zeno-stage0
  src/main.cpp
)

set_target_properties(zeno-stage0 PROPERTIES OUTPUT_NAME zeno)

target_compile_features(zeno-stage0 PRIVATE cxx_std_20)

find_package(LLVM REQUIRED CONFIG)
if(NOT LLVM_PACKAGE_VERSION MATCHES "^21\\.")
  message(FATAL_ERROR "Zeno stage0 requires LLVM 21.x, found ${LLVM_PACKAGE_VERSION}")
endif()

find_program(ZENO_LLVM_CLANG
  NAMES clang
  HINTS "${LLVM_TOOLS_BINARY_DIR}"
  NO_DEFAULT_PATH
)
if(NOT ZENO_LLVM_CLANG)
  message(FATAL_ERROR "Zeno stage0 requires clang from LLVM 21 tools; LLVM_TOOLS_BINARY_DIR=${LLVM_TOOLS_BINARY_DIR}")
endif()

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVM clang ${ZENO_LLVM_CLANG}")

target_compile_definitions(zeno-stage0 PRIVATE
  ZENO_LLVM_VERSION="${LLVM_PACKAGE_VERSION}"
  ZENO_NATIVE_CLANG="${ZENO_LLVM_CLANG}"
)
