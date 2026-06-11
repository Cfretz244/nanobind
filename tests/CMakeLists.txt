# If leaks are found, abort() during interpreter shutdown to catch this in the CI
add_definitions(-DNB_ABORT_ON_LEAK)

if (NB_TEST_STABLE_ABI)
  set(NB_EXTRA_ARGS ${NB_EXTRA_ARGS} STABLE_ABI)
endif()

if (NB_TEST_FREE_THREADED)
  set(NB_EXTRA_ARGS ${NB_EXTRA_ARGS} FREE_THREADED)
endif()

if (NB_TEST_SHARED_BUILD)
  set(NB_EXTRA_ARGS ${NB_EXTRA_ARGS} NB_SHARED)
endif()

# ---------------------------------------------------------------------------
# Compile with a few more compiler warnings turned on
# ---------------------------------------------------------------------------

if (MSVC)
  if (CMAKE_CXX_FLAGS MATCHES "/W[0-4]")
    string(REGEX REPLACE "/W[0-4]" "/W4" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  elseif (NOT NB_TEST_CUDA)
    add_compile_options(/W4)
  endif()
elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU|IntelLLVM")
  add_compile_options(-Wall -Wextra)
endif()

if (UNIX AND (CMAKE_SIZEOF_VOID_P EQUAL 4) AND (CMAKE_SYSTEM_PROCESSOR STREQUAL i686))
  # Don't use the legacy 387 math coprocessor in 32 bit builds, this causes tests to fail
  add_compile_options(-mfpmath=sse -msse2)
endif()

# Enforce the use of the CUDA NVCC compiler if requested
if (NB_TEST_CUDA)
  enable_language(CUDA)
  set(CMAKE_CUDA_STANDARD 17)
  set(CMAKE_CUDA_STANDARD_REQUIRED ON)
endif()

set(NB_TEST_SANITIZER "")

if (NB_TEST_SANITIZERS_ASAN)
  list(APPEND NB_TEST_SANITIZERS address)
endif()

if (NB_TEST_SANITIZERS_UBSAN)
  list(APPEND NB_TEST_SANITIZERS undefined)
endif()

if (NB_TEST_SANITIZERS_TSAN)
  list(APPEND NB_TEST_SANITIZERS thread)
endif()


if (NB_TEST_SANITIZERS)
  string(REPLACE ";" "," NB_TEST_SANITIZERS "${NB_TEST_SANITIZERS}")
  add_compile_options(-fsanitize=${NB_TEST_SANITIZERS})
  add_link_options(-fsanitize=${NB_TEST_SANITIZERS})
endif()

set(TEST_NAMES
  accessor
  functions
  callbacks
  classes
  holders
  stl
  stl_bind_map
  stl_bind_vector
  chrono
  enum
  eval
  ndarray
  jax
  tensorflow
  exception
  make_iterator
  typing
  issue
  intrusive
  thread
)

foreach (NAME ${TEST_NAMES})
  if (NAME STREQUAL classes)
    nanobind_add_module(test_${NAME}_ext test_${NAME}.cpp test_${NAME}_extra.cpp ${NB_EXTRA_ARGS})
  else()
    nanobind_add_module(test_${NAME}_ext test_${NAME}.cpp ${NB_EXTRA_ARGS})
  endif()

  if (NB_TEST_CUDA)
    set_property(SOURCE test_${NAME}.cpp PROPERTY LANGUAGE CUDA)
  endif()
endforeach()

target_sources(test_intrusive_ext PRIVATE test_intrusive_impl.cpp)

if (CMAKE_CONFIGURATION_TYPES)
  set(OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>)
else()
  set(OUT_DIR ${CMAKE_CURRENT_BINARY_DIR})
endif()

foreach (NAME functions classes ndarray jax tensorflow stl enum typing make_iterator)
  if (NAME STREQUAL typing)
    set(EXTRA
      MARKER_FILE py.typed
      PATTERN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/pattern_file.nb"
      EXCLUDE_VALUES
    )
    set(EXTRA_DEPENDS "${OUT_DIR}/py_stub_test.py")
  else()
    set(EXTRA "")
    set(EXTRA_DEPENDS "")
  endif()

  if (CMAKE_CONFIGURATION_TYPES)
    # On multi-config generators like Visual Studio, put stubs in the 'Debug' / 'Release' /.. folders
    set(PYI_PREFIX $<CONFIG>/)
  endif()

  nanobind_add_stub(
    ${NAME}_ext_stub
    MODULE test_${NAME}_ext
    OUTPUT ${PYI_PREFIX}test_${NAME}_ext.pyi
    PYTHON_PATH $<TARGET_FILE_DIR:test_${NAME}_ext>
    DEPENDS test_${NAME}_ext ${EXTRA_DEPENDS}
    ${EXTRA})
endforeach()

find_package (Eigen3 3.3.1 NO_MODULE)
if (TARGET Eigen3::Eigen)
  nanobind_add_module(test_eigen_ext test_eigen.cpp ${NB_EXTRA_ARGS})
  target_link_libraries(test_eigen_ext PRIVATE Eigen3::Eigen)
endif()

# C++26 static reflection (P2996) test — only built if the compiler supports it.
# GCC trunk uses -freflection and has template-for built in.
# Bloomberg clang-p2996 uses -freflection-latest (includes -fexpansion-statements
# and -fparameter-reflection) and requires -stdlib=libc++ for <meta>.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS_OLD "${CMAKE_REQUIRED_FLAGS}")

# Test source that exercises template-for and define_static_array — these
# require different flags on GCC vs Bloomberg, so this distinguishes them.
set(NB_REFLECT_CHECK_SRC "
#include <meta>
struct S { int x; double y; };
consteval int count() {
    int n = 0;
    template for (constexpr auto m :
        std::define_static_array(std::meta::nonstatic_data_members_of(
            ^^S, std::meta::access_context::unchecked())))
        ++n;
    return n;
}
int main() { static_assert(count() == 2); }
")

# GCC: -freflection enables everything including template-for
set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS_OLD} -std=c++26 -freflection")
check_cxx_source_compiles("${NB_REFLECT_CHECK_SRC}" NB_HAS_REFLECTION_GCC)

if (NOT NB_HAS_REFLECTION_GCC)
  # Bloomberg: needs -freflection-latest and -stdlib=libc++
  set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS_OLD} -std=c++26 -freflection-latest -stdlib=libc++")
  check_cxx_source_compiles("${NB_REFLECT_CHECK_SRC}" NB_HAS_REFLECTION_BLOOMBERG)
endif()

set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS_OLD}")

if (NB_HAS_REFLECTION_GCC)
  set(NB_REFLECT_FLAGS -std=c++26 -freflection)
  # GCC spells the constant-evaluation budget differently from clang.
  set(NB_REFLECT_BIG_STEPS -fconstexpr-ops-limit=2000000000)
elseif(NB_HAS_REFLECTION_BLOOMBERG)
  # The raised -fconstexpr-steps covers the whole-namespace consteval walks
  # (required_stl_types / required_user_specs) over the growing test fixture
  # namespace; the default budget is no longer enough. Safe since the TC-0002
  # Sema use-after-free fix (the budget itself was never the bug).
  # -fentity-proxy-reflection (NOT implied by -freflection-latest) makes
  # using-redeclarations enumerable so they can bind (BINDER-0009).
  set(NB_REFLECT_FLAGS -std=c++26 -freflection-latest -fentity-proxy-reflection
      -stdlib=libc++ -fconstexpr-steps=100000000)
  set(NB_REFLECT_BIG_STEPS -fconstexpr-steps=2000000000)
endif()

if (NB_REFLECT_FLAGS)
  nanobind_add_module(test_reflect_ext test_reflect.cpp ${NB_EXTRA_ARGS})
  target_compile_options(test_reflect_ext PRIVATE ${NB_REFLECT_FLAGS})
  target_link_options(test_reflect_ext PRIVATE ${NB_REFLECT_FLAGS})

  # Codegen (tier 2) trampoline test: a generator program emits trampoline source
  # via reflection, the build runs it to produce a header, and the bindings module
  # includes that header. The same steps run whether or not a trampoline is needed.
  set(NB_REFLECT_GEN_HDR
      ${CMAKE_CURRENT_BINARY_DIR}/test_reflect_codegen_tramp.gen.h)

  add_executable(test_reflect_codegen_gen test_reflect_codegen_gen.cpp)
  target_compile_options(test_reflect_codegen_gen PRIVATE ${NB_REFLECT_FLAGS})
  target_link_options(test_reflect_codegen_gen PRIVATE ${NB_REFLECT_FLAGS})
  target_include_directories(test_reflect_codegen_gen
      PRIVATE ${NB_DIR}/include ${Python_INCLUDE_DIRS})

  add_custom_command(
      OUTPUT ${NB_REFLECT_GEN_HDR}
      COMMAND test_reflect_codegen_gen ${NB_REFLECT_GEN_HDR}
      DEPENDS test_reflect_codegen_gen
      COMMENT "Generating nanobind trampolines via reflection")
  add_custom_target(test_reflect_codegen_generate DEPENDS ${NB_REFLECT_GEN_HDR})

  nanobind_add_module(test_reflect_codegen_ext test_reflect_codegen.cpp ${NB_EXTRA_ARGS})
  target_compile_options(test_reflect_codegen_ext PRIVATE ${NB_REFLECT_FLAGS})
  target_link_options(test_reflect_codegen_ext PRIVATE ${NB_REFLECT_FLAGS})
  target_include_directories(test_reflect_codegen_ext PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
  add_dependencies(test_reflect_codegen_ext test_reflect_codegen_generate)

  # Emit (full source codegen) test: a generator program renders the COMPLETE
  # binding TU for the shared fixture via reflection; the build runs it and
  # compiles the GENERATED source as the module -- at c++20 with NO reflection
  # flags, which is the in-tree proof that the output needs no reflection
  # toolchain (the corpus's emit lane repeats this with Apple Clang).
  set(NB_REFLECT_EMIT_SRC ${CMAKE_CURRENT_BINARY_DIR}/test_reflect_emit.gen.cpp)
  set(NB_REFLECT_PROBE_SRC ${CMAKE_CURRENT_BINARY_DIR}/test_reflect_spelling_probe.gen.cpp)

  add_executable(test_reflect_emit_gen test_reflect_emit_gen.cpp)
  # Rendering the whole module as a consteval string outspends the default
  # walk budget; the bump is harmless (TC-0002 made budgets safe to raise).
  target_compile_options(test_reflect_emit_gen PRIVATE ${NB_REFLECT_FLAGS}
      ${NB_REFLECT_BIG_STEPS})
  target_link_options(test_reflect_emit_gen PRIVATE ${NB_REFLECT_FLAGS})
  target_include_directories(test_reflect_emit_gen
      PRIVATE ${NB_DIR}/include ${Python_INCLUDE_DIRS})

  add_custom_command(
      OUTPUT ${NB_REFLECT_EMIT_SRC} ${NB_REFLECT_PROBE_SRC}
      COMMAND test_reflect_emit_gen ${NB_REFLECT_EMIT_SRC} ${NB_REFLECT_PROBE_SRC}
      DEPENDS test_reflect_emit_gen
      COMMENT "Generating nanobind binding source via reflection (emit backend)")
  add_custom_target(test_reflect_emit_generate DEPENDS ${NB_REFLECT_EMIT_SRC})

  # NB_DOMAIN: the emit module binds the SAME C++ types as test_reflect_ext;
  # in one interpreter they would share nanobind's type registry and the
  # second import's idempotence guards would skip everything. A separate
  # domain gives each backend its own registry, so the suite can compare them
  # side by side.
  nanobind_add_module(test_reflect_emit_ext NB_DOMAIN reflect_emit
                      ${NB_REFLECT_EMIT_SRC} ${NB_EXTRA_ARGS})
  # Deliberately NO ${NB_REFLECT_FLAGS}: the generated TU is plain C++20.
  target_compile_features(test_reflect_emit_ext PRIVATE cxx_std_20)
  target_include_directories(test_reflect_emit_ext PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_dependencies(test_reflect_emit_ext test_reflect_emit_generate)

  # The spelling-probe TU: pure C++ (no nanobind, no reflection flags),
  # asserting every spelled signature names the real entity exactly. A wrong
  # spelling is a compile error of THIS object library.
  add_library(test_reflect_spelling_probe OBJECT ${NB_REFLECT_PROBE_SRC})
  target_compile_features(test_reflect_spelling_probe PRIVATE cxx_std_20)
  # NB_DIR include: the fixture header pulls nb_reflect_annotations.h (a
  # dependency-free vocabulary header, fine for a no-nanobind TU).
  target_include_directories(test_reflect_spelling_probe
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${NB_DIR}/include
              ${Python_INCLUDE_DIRS})
  add_dependencies(test_reflect_spelling_probe test_reflect_emit_generate)
endif()

add_library(
  inter_module
  SHARED
  inter_module.h
  inter_module.cpp
)

target_compile_definitions(inter_module PRIVATE -DSHARED_BUILD)
target_compile_features(inter_module PRIVATE cxx_std_17)
target_include_directories(inter_module PRIVATE ${NB_DIR}/include)

nanobind_add_module(test_inter_module_1_ext NB_DOMAIN mydomain test_inter_module_1.cpp ${NB_EXTRA_ARGS})
nanobind_add_module(test_inter_module_2_ext NB_DOMAIN mydomain test_inter_module_2.cpp ${NB_EXTRA_ARGS})
target_link_libraries(test_inter_module_1_ext PRIVATE inter_module)
target_link_libraries(test_inter_module_2_ext PRIVATE inter_module)

set(TEST_FILES
  common.py
  conftest.py
  test_accessor.py
  test_callbacks.py
  test_classes.py
  test_eigen.py
  test_enum.py
  test_eval.py
  test_exception.py
  test_functions.py
  test_holders.py
  test_inter_module.py
  test_intrusive.py
  test_make_iterator.py
  test_stl.py
  test_stl_bind_map.py
  test_stl_bind_vector.py
  test_chrono.py
  test_ndarray.py
  test_jax.py
  test_tensorflow.py
  test_stubs.py
  test_typing.py
  test_thread.py
  test_specialization.py
  test_reflect.py
  test_reflect_codegen.py
  test_reflect_emit.py

  # Stub reference files
  test_classes_ext.pyi.ref
  test_functions_ext.pyi.ref
  test_make_iterator_ext.pyi.ref
  test_ndarray_ext.pyi.ref
  test_jax_ext.pyi.ref
  test_tensorflow_ext.pyi.ref
  test_stl_ext.pyi.ref
  test_enum_ext.pyi.ref
  test_typing_ext.pyi.ref
  py_stub_test.py
  py_stub_test.pyi.ref
  py_recursive_stub_test/__init__.pyi.ref
  py_recursive_stub_test/bar.pyi.ref
  test_prefix_module/__init__.pyi.ref
  test_prefix_module/prefix.pyi.ref
  test_prefix_module/prefixabc.pyi.ref
)

set (PY_STUB_TEST py_stub_test.py)
if (NOT (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_CURRENT_BINARY_DIR) OR MSVC)
  foreach(TEST_FILE IN LISTS TEST_FILES)
    set(IN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/${TEST_FILE})
    set(OUT_FILE ${OUT_DIR}/${TEST_FILE})
    set(TEST_FILES_OUT ${TEST_FILES_OUT} ${OUT_FILE})
    add_custom_command(
      DEPENDS ${IN_FILE} OUTPUT ${OUT_FILE}
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${IN_FILE} ${OUT_FILE})
  endforeach()

  add_custom_target(copy-tests ALL DEPENDS ${TEST_FILES_OUT})
  set(PY_STUB_TEST ${OUT_DIR}/py_stub_test.py)
endif()

nanobind_add_stub(
  py_stub
  MODULE py_stub_test
  OUTPUT ${PYI_PREFIX}py_stub_test.pyi
  PYTHON_PATH $<TARGET_FILE_DIR:test_stl_ext>
  DEPENDS ${PY_STUB_TEST}
)

nanobind_add_stub(
  py_recursive_stub
  RECURSIVE
  MODULE py_recursive_stub_test
  OUTPUT_PATH
    ${PYI_PREFIX}py_recursive_stub_test
  OUTPUT
    ${PYI_PREFIX}py_recursive_stub_test/__init__.pyi
    ${PYI_PREFIX}py_recursive_stub_test/bar.pyi
  PYTHON_PATH ${CMAKE_CURRENT_SOURCE_DIR}
)

nanobind_add_stub(
  test_prefix_module_stub
  RECURSIVE
  MODULE test_prefix_module
  OUTPUT_PATH
    ${PYI_PREFIX}test_prefix_module
  OUTPUT
    ${PYI_PREFIX}test_prefix_module/__init__.pyi
    ${PYI_PREFIX}test_prefix_module/prefix.pyi
    ${PYI_PREFIX}test_prefix_module/prefixabc.pyi
  PYTHON_PATH ${CMAKE_CURRENT_SOURCE_DIR}
)
