/*
    nanobind/nb_reflect_annotations.h: vocabulary for controlling reflection-based
    binding (nb_reflect.h) via P3394 annotation-attributes.

    Apply these to your C++ declarations with the `[[=value]]` syntax; reflect_
    reads them and adjusts what/how it binds. This header is intentionally
    dependency-free (no nanobind or Python includes) so it can be included by
    library headers that are merely *annotated* without pulling in nanobind:

        #include <nanobind/nb_reflect_annotations.h>
        namespace r = nanobind::reflect;

        struct [[=r::skip{}]] Internal { ... };           // not bound

        struct Widget {
            [[=r::rename{"size"}]] int get_size() const;   // bound as "size"
            [[=r::reference_internal]] Buffer& buffer();    // return policy
            [[=r::doc{"Reset to defaults."}]] void reset();
            [[=r::property{"value"}]] int  value() const;   // a Python property
            [[=r::property{"value"}]] void value(int);      //   (getter + setter)
        };

        [[=r::keep_alive{0, 1}]] Child* make_child(Parent&);  // tie lifetimes

    Note: annotation values must be usable as template arguments, so the
    string-bearing annotations (rename, doc) store the text in a fixed-size array
    (a `const char*` member would not be a valid template argument).

    Requires a C++26 compiler with P2996 + P3394 support (GCC 16+,
    -std=c++26 -freflection); without one the annotations are inert values.

    Copyright (c) 2025.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE file.
*/

#pragma once

namespace nanobind {
namespace reflect {

/// Exclude the annotated entity (class, enum, member, or free function) from the
/// generated bindings.
struct skip {};

/// A compile-time string usable as a template/annotation argument. Both
/// string-bearing annotations expose it under the member name `str` so reflect_
/// can read either with one code path.
template <unsigned N> struct fixed_string {
    char data[N];
    consteval fixed_string(const char (&s)[N]) {
        for (unsigned i = 0; i < N; ++i)
            data[i] = s[i];
    }
};

/// Expose the annotated entity under a different Python name: [[=r::rename{"x"}]].
template <unsigned N> struct rename {
    fixed_string<N> str;
    consteval rename(const char (&s)[N]) : str(s) {}
};
template <unsigned N> rename(const char (&)[N]) -> rename<N>;

/// Attach a docstring to the annotated function/method/data member: [[=r::doc{"..."}]].
template <unsigned N> struct doc {
    fixed_string<N> str;
    consteval doc(const char (&s)[N]) : str(s) {}
};
template <unsigned N> doc(const char (&)[N]) -> doc<N>;

/// Bind a getter/setter pair as a Python property. Annotate BOTH accessors with the
/// same name: [[=r::property{"value"}]]. The string is the Python property name and
/// the grouping key; within a group the parameter-less method is the getter and the
/// one-parameter method the setter. Annotate only the getter for a read-only
/// property. The getter's return-policy / doc annotations apply to the property.
template <unsigned N> struct property {
    fixed_string<N> str;
    consteval property(const char (&s)[N]) : str(s) {}
};
template <unsigned N> property(const char (&)[N]) -> property<N>;

/// Return-value ownership/lifetime policy (mirrors nanobind::rv_policy).
enum class lifetime {
    automatic,
    take_ownership,
    copy,
    move,
    reference,
    reference_internal,
    none,
};

/// Apply a return-value policy to the annotated function or method.
struct return_policy { lifetime value; };

// Convenience constants so callers can write e.g. `[[=nb::reflect::reference]]`.
inline constexpr return_policy take_ownership{lifetime::take_ownership};
inline constexpr return_policy copy{lifetime::copy};
inline constexpr return_policy move{lifetime::move};
inline constexpr return_policy reference{lifetime::reference};
inline constexpr return_policy reference_internal{lifetime::reference_internal};
inline constexpr return_policy take_nothing{lifetime::none};

/// Tie the lifetime of one argument/return to another, keeping the "patient"
/// alive as long as the "nurse" lives. Index 0 = return value, 1 = the implicit
/// `self` (for methods) or the first argument (for free functions), 2 = next, ...
/// (matches nanobind::keep_alive semantics).
struct keep_alive { unsigned nurse; unsigned patient; };

} // namespace reflect
} // namespace nanobind
