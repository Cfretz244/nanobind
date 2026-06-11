#include <nanobind/nb_reflect.h>
#include <nanobind/nb_reflect_annotations.h>
#include <nanobind/trampoline.h>
#include <nanobind/stl/vector.h>
#include <nanobind/stl/string.h>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

// The bound C++ surface. Shared with the emit-mode generator and the generated
// TU (which compiles WITHOUT reflection), so the fixture is plain header-safe
// C++; everything P2996-only -- the trampoline registration, the reflection
// markers, and the binder static_asserts -- stays in this TU.
#include "test_reflect_fixture.h"

namespace nb = nanobind;

// The trampoline lives OUTSIDE the reflected namespace so reflect_ does not try
// to bind it as a class; it is wired in only via NB_REFLECT_TRAMPOLINE.
namespace reflect_test_tramp {
struct PyShape : reflect_test::Shape {
    NB_TRAMPOLINE(reflect_test::Shape, 2);
    double area() const override { NB_OVERRIDE_PURE(area); }
    std::string kind() const override { NB_OVERRIDE(kind); }
};
} // namespace reflect_test_tramp

NB_REFLECT_TRAMPOLINE(reflect_test::Shape, reflect_test_tramp::PyShape);

// --- Compile-time checks for the STL-caster detection core (roadmap #5) ---
// reflect_test binds std::string (Struct::s, ...) and std::vector<int>
// (Nested::items), and no other std container, so its required-caster set is
// exactly {string.h, vector.h}.
namespace {
consteval bool reflect_test_needs(std::string_view want) {
    for (const char* h : nb::detail::required_stl_headers<^^reflect_test>())
        if (std::string_view(h) == want)
            return true;
    return false;
}
}
static_assert(std::string_view(nb::detail::stl_caster_header(^^std::vector<int>)) ==
              "nanobind/stl/vector.h");
static_assert(nb::detail::stl_caster_header(^^int) == nullptr);
static_assert(reflect_test_needs("nanobind/stl/string.h"));
static_assert(reflect_test_needs("nanobind/stl/vector.h"));
static_assert(!reflect_test_needs("nanobind/stl/map.h"));

// Discovery must find exactly the user specializations reachable from the signatures
// (and not pull in std types, which go to the caster path).
namespace {
consteval bool tt_has_spec(std::meta::info t) {
    for (auto s : nb::detail::required_user_specs(^^template_test))
        if (s == t)
            return true;
    return false;
}
}
static_assert(tt_has_spec(^^template_test::Box<int>));
static_assert(tt_has_spec(^^template_test::Box<double>));
static_assert(tt_has_spec(^^template_test::Box<template_test::Box<int>>));
static_assert(tt_has_spec(^^template_test::Wrap<int>));
static_assert(tt_has_spec(^^template_test::Pair<int, double>));
static_assert(tt_has_spec(^^template_test::Array<int, 3>));
static_assert(!tt_has_spec(^^std::vector<int>));     // std -> caster path, not bound
// Reachability: the spec itself is bound, its policy-only template arg is not.
static_assert(tt_has_spec(^^template_test::Cont<int, template_test::Pol<int>>));
static_assert(!tt_has_spec(^^template_test::Pol<int>));

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
    nb::exclude_<^^exclude_test::Expr, ^^exclude_test::detail,                 \
                 ^^exclude_test::Opaque, ^^exclude_test::ExBase,               \
                 xvec_member("doomed")>
namespace {
consteval bool ex_has_spec(std::meta::info t) {
    std::vector<std::meta::info> ex = nb::detail::compute_excluded<^^EX_MARKER>();
    for (auto s : nb::detail::required_user_specs(^^exclude_test, ex))
        if (s == t)
            return true;
    return false;
}
}
static_assert(nb::detail::is_exclude_marker(^^EX_MARKER));
// With the exclusions, discovery converges and surfaces NO Expr spec.
static_assert(!ex_has_spec(^^exclude_test::Expr<int>));
static_assert(!ex_has_spec(^^exclude_test::Expr<exclude_test::Expr<int>>));

NB_MODULE(test_reflect_ext, m) {
    // Box<float> is referenced by no signature; it is bound only because it is listed
    // explicitly here (the explicit opt-in for specializations the walk can't reach).
    // identity<int> is a free-function-template specialization, also explicit-only.
    nb::reflect_<^^reflect_test, ^^template_test,
                 ^^template_test::Box<float>,
                 ^^template_test::identity<int>,
                 ^^stream_test::Streamable,
                 ^^member_template_test, ^^proxy_test,
                 ^^exclude_test, ^^EX_MARKER,
                 ^^unbindable_shapes, ^^ownership_test,
                 ^^anon_typedef_test, ^^static_const_test,
                 ^^collide_a, ^^collide_b, ^^shadow_test,
                 ^^alias_fixture, ^^ref_return_test,
                 ^^parens_init_test>(m);
}
