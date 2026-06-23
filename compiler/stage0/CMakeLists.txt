add_executable(zeno-stage0
  src/main.cpp
)

target_compile_features(zeno-stage0 PRIVATE cxx_std_20)

find_package(LLVM 21 QUIET CONFIG)
if(LLVM_FOUND)
  message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
  target_compile_definitions(zeno-stage0 PRIVATE ZENO_LLVM_VERSION="${LLVM_PACKAGE_VERSION}")
else()
  message(WARNING "LLVM 21 was not found; stage0 will build with codegen placeholders only")
  target_compile_definitions(zeno-stage0 PRIVATE ZENO_LLVM_VERSION="unavailable")
endif()
