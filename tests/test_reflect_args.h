/*
    tests/test_reflect_args.h: the reflect_ argument pack for the test
    fixture, defined ONCE and shared by every backend consumer -- the
    constexpr binding TU (test_reflect.cpp) and the emit-mode generator
    (test_reflect_emit_gen.cpp). P2996-only (reflection markers); the plain
    fixture types live in test_reflect_fixture.h.
*/

#pragma once

#include <nanobind/nb_reflect.h>
#include "test_reflect_fixture.h"

// A member listed by its REFLECTION (not spellable as ^^name for an overload/
// ctor in general) -- exercises the per-member exclusion path.
consteval std::meta::info xvec_member(std::string_view name) {
    for (auto m : std::meta::members_of(^^exclude_test::XVec,
                                        std::meta::access_context::unchecked()))
        if (std::meta::is_function(m) && !std::meta::is_template(m)
            && std::meta::has_identifier(m) && std::meta::identifier_of(m) == name)
            return m;
    return ^^void;
}

// The marker is spelled in full at each use: a `using` alias would put an
// ALIAS reflection in the pack, and the marker test keys on template_of.
#define EX_MARKER                                                              \
    nanobind::exclude_<^^exclude_test::Expr, ^^exclude_test::detail,           \
                       ^^exclude_test::Opaque, ^^exclude_test::ExBase,         \
                       xvec_member("doomed")>

// The full pack bound by the suite. Box<float> is referenced by no signature;
// it is bound only because it is listed explicitly here (the explicit opt-in
// for specializations the walk can't reach). identity<int> is a free-function-
// template specialization, also explicit-only. The trampoline_ marker is the
// emit backend's analogue of test_reflect.cpp's hand-written PyShape
// (NB_REFLECT_TRAMPOLINE); the constexpr backend treats it as inert
// configuration -- so the two backends trampoline the SAME single class.
#define TEST_REFLECT_ARGS                                                      \
    ^^nanobind::trampoline_<^^reflect_test::Shape>,                            \
    ^^reflect_test, ^^template_test,                                           \
    ^^template_test::Box<float>,                                               \
    ^^template_test::identity<int>,                                            \
    ^^stream_test::Streamable,                                                 \
    ^^member_template_test,                                                    \
    ^^exclude_test, ^^EX_MARKER,                                               \
    ^^unbindable_shapes, ^^ownership_test,                                     \
    ^^anon_typedef_test, ^^static_const_test,                                  \
    ^^collide_a, ^^collide_b, ^^shadow_test,                                   \
    ^^alias_fixture, ^^ref_return_test,                                        \
    ^^parens_init_test
