#include <nanobind/nb_reflect.h>
#include <nanobind/nb_reflect_annotations.h>
#include <nanobind/nb_reflect_spell.h>
#include <nanobind/trampoline.h>
#include <functional>
#include <nanobind/stl/vector.h>
#include <nanobind/stl/string.h>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

// The bound C++ surface (plain header-safe C++, shared with the emit-mode
// generator and the generated TU, which compiles WITHOUT reflection) plus the
// shared reflect_ argument pack (TEST_REFLECT_ARGS / EX_MARKER / xvec_member).
#include "test_reflect_args.h"

namespace nb = nanobind;

// --- Emitter type-spelling: char-family fundamentals must spell by keyword ---
// On this toolchain display_string_of(^^wchar_t) is "int" (its underlying type),
// which would render std::wstring as basic_string<int, ...> and miscompile
// against the real overload. type_spelling must special-case the char family.
static_assert(nb::detail::type_spelling(^^wchar_t)  == "wchar_t");
static_assert(nb::detail::type_spelling(^^char8_t)  == "char8_t");
static_assert(nb::detail::type_spelling(^^char16_t) == "char16_t");
static_assert(nb::detail::type_spelling(^^char32_t) == "char32_t");
static_assert(nb::detail::type_spelling(^^char)     == "char");
static_assert(
    nb::detail::type_spelling(^^std::wstring)
        == "::std::basic_string<wchar_t, ::std::char_traits<wchar_t>, "
           "::std::allocator<wchar_t>>");

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

// --- Compile-time checks for the emit backend's type renderer (nb_reflect_spell.h) ---
// The cast-based round-trip probe (overload-exact member-pointer casts compiled
// WITHOUT reflection) lives with the emit test; these pin the renderer's output
// directly across the type grammar.
namespace spell_fixture {
enum class Color { Red = 1, Blue = 2 };
template <class T, Color C> struct WithEnum {};
template <template <class> class TT> struct TakesTT {};
struct Outer2 { struct Nested {}; };
template <class T> struct OuterT { struct Nested {}; };
struct Holder { int field; int meth(double) const noexcept; };
} // namespace spell_fixture

namespace {
consteval bool spelled(std::meta::info t, std::string_view want) {
    return nb::detail::type_spelling(t) == want;
}
}
static_assert(spelled(^^int, "int"));
static_assert(spelled(^^reflect_test::Struct, "::reflect_test::Struct"));
static_assert(spelled(^^const reflect_test::Struct&, "const ::reflect_test::Struct &"));
static_assert(spelled(^^reflect_test::Struct&&, "::reflect_test::Struct &&"));
static_assert(spelled(^^const char*, "const char *"));
static_assert(spelled(^^char* const, "char * const"));
static_assert(spelled(^^template_test::Box<int>, "::template_test::Box<int>"));
static_assert(spelled(^^template_test::Box<template_test::Box<double>>,
                      "::template_test::Box<::template_test::Box<double>>"));
static_assert(spelled(^^template_test::Array<int, 3>, "::template_test::Array<int, 3>"));
static_assert(spelled(^^spell_fixture::WithEnum<int, spell_fixture::Color::Red>,
                      "::spell_fixture::WithEnum<int, ::spell_fixture::Color::Red>"));
static_assert(spelled(^^spell_fixture::TakesTT<template_test::Box>,
                      "::spell_fixture::TakesTT<::template_test::Box>"));
// std inline namespace (__1) skipped; defaulted args rendered explicitly.
static_assert(spelled(^^std::vector<int>, "::std::vector<int, ::std::allocator<int>>"));
static_assert(spelled(^^std::string,
    "::std::basic_string<char, ::std::char_traits<char>, ::std::allocator<char>>"));
static_assert(spelled(^^void(int, double), "void (int, double)"));
static_assert(spelled(^^void(*)(int), "void (*)(int)"));
static_assert(spelled(^^std::function<void(int)>, "::std::function<void (int)>"));
static_assert(spelled(^^int spell_fixture::Holder::*, "int ::spell_fixture::Holder::*"));
static_assert(spelled(^^int (spell_fixture::Holder::*)(double) const noexcept,
                      "int (::spell_fixture::Holder::*)(double) const noexcept"));
static_assert(spelled(^^int[3], "int[3]"));
static_assert(spelled(^^const int[2], "const int[2]"));
// typedef-for-linkage anonymous record/enum (BINDER-0018's idiom, spelled).
static_assert(spelled(^^anon_typedef_test::point_t, "::anon_typedef_test::point_t"));
static_assert(spelled(^^anon_typedef_test::color_t, "::anon_typedef_test::color_t"));
static_assert(spelled(^^spell_fixture::Outer2::Nested, "::spell_fixture::Outer2::Nested"));
static_assert(spelled(^^spell_fixture::OuterT<int>::Nested,
                      "::spell_fixture::OuterT<int>::Nested"));
static_assert(nb::detail::fn_signature_spellable(^^reflect_test::kw_sub));

NB_MODULE(test_reflect_ext, m) {
    nb::reflect_<TEST_REFLECT_ARGS>(m);
}
