/*
    tests/test_reflect_emit_gen.cpp: generator program for the emit-backend
    test. Compiled by the P2996 toolchain; renders the COMPLETE binding TU for
    the shared fixture (same TEST_REFLECT_ARGS pack as test_reflect.cpp) as
    ordinary nanobind source, which the build then compiles -- with NO
    reflection flags -- into test_reflect_emit_ext.
*/

#include <nanobind/nb_reflect_emit.h>
#include "test_reflect_args.h"

namespace nb = nanobind;

int main(int argc, char** argv) {
    const char* out = (argc > 1) ? argv[1] : "test_reflect_emit.gen.cpp";
    return nb::write_bindings(
               out, nb::emit_bindings<TEST_REFLECT_ARGS>(
                        "test_reflect_emit_ext",
                        "#include \"test_reflect_fixture.h\"\n"))
               ? 0
               : 1;
}
