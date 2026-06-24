add_executable(zeno-stage0
  src/main.cpp
)

target_compile_features(zeno-stage0 PRIVATE cxx_std_20)

find_package(LLVM 21.1 QUIET CONFIG)
find_program(ZENO_CLANG clang)
if(LLVM_FOUND)
  message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
  target_compile_definitions(zeno-stage0 PRIVATE ZENO_LLVM_VERSION="${LLVM_PACKAGE_VERSION}")
elseif(ZENO_CLANG)
  message(STATUS "LLVM 21 C++ package was not found; stage0 still emits LLVM IR and uses clang for native object/executable artifacts")
  target_compile_definitions(zeno-stage0 PRIVATE ZENO_LLVM_VERSION="llvm-ir-via-clang")
else()
  message(WARNING "LLVM 21 C++ package and clang were not found; native artifact generation will fail until clang is available")
  target_compile_definitions(zeno-stage0 PRIVATE ZENO_LLVM_VERSION="unavailable")
endif()
