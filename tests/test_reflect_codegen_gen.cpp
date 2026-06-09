/*
    Generator program for the codegen (tier 2) reflection test. Walks the
    reflect_codegen_test namespace and writes trampoline source for every class
    that has overridable virtuals to the path given as argv[1]. The build runs
    this before compiling the bindings module; the output may be empty.
*/

#include <nanobind/nb_reflect_codegen.h>
#include "test_reflect_codegen.h"

namespace nb = nanobind;

int main(int argc, char** argv) {
    const char* out = (argc > 1) ? argv[1] : "test_reflect_codegen_tramp.gen.h";
    return nb::write_trampolines(
               out, nb::emit_trampolines<^^reflect_codegen_test>())
               ? 0
               : 1;
}
