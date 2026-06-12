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
                       xvec_member("doomed"),                                  \
                       ^^nanobind::exclude_member_<^^exclude_test::XVec,       \
                                                   "named_out">>

// The full pack bound by the suite. Box<float> is referenced by no signature;
// it is bound only because it is listed explicitly here (the explicit opt-in
// for specializations the walk can't reach). identity<int> is a free-function-
// template specialization, also explicit-only. The trampoline_ marker is the
// emit backend's analogue of test_reflect.cpp's hand-written PyShape
// (NB_REFLECT_TRAMPOLINE); the constexpr backend treats it as inert
// configuration -- so the two backends trampoline the SAME single class.
// The matcher-API entries: match_test is reached ONLY through the match_
// marker (its scope is not a pack namespace), so unmatched members must be
// absent; the exclude_if_ predicate drops match_test::detail everywhere;
// the instantiate_ rules mint Grid<int,2> (explicit with_),
// Grid<float,3>/Grid<double,3> (product_ grid), and Cell<int> through a
// matcher-target rule (its named_ sweeps the pack's namespace roots,
// inst_test among them -- a namespace holding only templates, which the
// plain walk binds nothing from).
#define MATCH_MARKER                                                           \
    nanobind::match_<^^match_test,                                             \
                     nanobind::any_of_<nanobind::named_<"Vec*">,               \
                                       nanobind::named_<"vec*">>>
#define INST_GRID_MARKER                                                       \
    nanobind::instantiate_<^^inst_test::Grid,                                  \
        nanobind::with_<^^int, nanobind::val_<2>>,                             \
        nanobind::product_<nanobind::set_<^^float, ^^double>,                  \
                           nanobind::set_<nanobind::val_<3>>>>
#define INST_CELL_MARKER                                                       \
    nanobind::instantiate_<^^nanobind::named_<"Cell">, nanobind::with_<^^int>>

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
    ^^parens_init_test,                                                        \
    ^^MATCH_MARKER,                                                            \
    ^^nanobind::exclude_if_<                                                   \
        nanobind::in_namespace_<^^match_test::detail>>,                        \
    ^^inst_test, ^^INST_GRID_MARKER, ^^INST_CELL_MARKER
