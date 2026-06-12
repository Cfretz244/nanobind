/*
    nanobind/nb_reflect_match.h: composable consteval predicates ("matchers")
    for selecting reflected entities, consumed by nb_reflect.h's match_ /
    exclude_if_ / instantiate_ pack markers.

    A matcher is any empty, default-constructible type callable as
    `M{}(std::meta::info) -> bool` in constant evaluation. This header supplies
    a small combinator vocabulary; arbitrary selection logic drops down to a
    user-written matcher type satisfying the same concept:

        struct small_vectors {
            consteval bool operator()(std::meta::info r) const {
                return ...;   // any reflection logic
            }
        };

        nb::reflect_<^^nb::match_<^^glm,
                         nb::any_of_<nb::named_<"vec*">, nb::named_<"mat*">>>,
                     ^^nb::exclude_if_<nb::in_namespace_<^^Eigen::internal>>>(m);

    Matchers are TYPES (not lambdas or function pointers) because they must
    travel through the reflect_ pack as template arguments, and GCC 16 forbids
    a lambda that splices an enclosing info NTTP from decaying to a function
    pointer. The built-in leaves are total over every reflection kind: they
    guard each kind-specific metafunction (GCC's P3560 semantics THROW
    std::meta::exception on a wrong-kind argument) and answer false instead of
    throwing. A user matcher must keep the same discipline -- an "uncaught
    std::meta::exception" error during binding points at the matcher body.

    named_ and in_namespace_ normalize their subject first: aliases are
    dealiased and a template specialization is resolved to its template, so
    named_<"Transpose"> matches Transpose<X> and in_namespace_ sees through
    member typedefs -- mirroring how nb::exclude_ treats its entries.

    Requires a C++26 compiler with P2996 support (GCC 16+,
    -std=c++26 -freflection).

    Copyright (c) 2025.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE file.
*/

#pragma once

#if __has_include(<meta>)

#include <meta>
#include <concepts>
#include <string_view>
#include <type_traits>
#include "nb_reflect_annotations.h"

namespace nanobind {

/// Any empty default-constructible type invocable as M{}(info) -> bool during
/// constant evaluation. The requires-expression is unevaluated, so naming the
/// consteval call operator here is legal outside immediate function contexts.
template <typename M>
concept matcher = std::is_empty_v<M> && std::is_default_constructible_v<M> &&
    requires(std::meta::info r) {
        { M{}(r) } -> std::same_as<bool>;
    };

namespace detail {

// Anchored glob over identifiers: `*` matches any sequence, `?` exactly one
// character. Two-pointer with single-star backtracking -- linear, no
// pathological inputs, and expressive enough for every exclusion family the
// corpus needed (exact names, "Cwise*", "*View"). Full regex at consteval
// would cost an engine and step budget for no demonstrated need.
consteval bool glob_match(std::string_view pat, std::string_view s) {
    std::size_t p = 0, i = 0, star = std::string_view::npos, si = 0;
    while (i < s.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == s[i])) {
            ++p; ++i;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++; si = i;
        } else if (star != std::string_view::npos) {
            p = star + 1; i = ++si;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*')
        ++p;
    return p == pat.size();
}

// The subject normalization shared by the name/scope leaves: dealias, then
// resolve a specialization to its template (the is_excluded_entity rule, so
// matchers and exclude_ entries agree on what an entity "is").
consteval std::meta::info match_normalize(std::meta::info r) {
    if (std::meta::is_type(r))
        r = std::meta::dealias(r);
    if (std::meta::has_template_arguments(r))
        r = std::meta::template_of(r);
    return r;
}

// True when `r` is the kind of entity that HAS an enclosing scope worth
// walking (the parent_of chain throws on builtins et al.).
consteval bool match_scoped(std::meta::info r) {
    return std::meta::is_namespace(r) || std::meta::is_function(r)
        || std::meta::is_template(r) || std::meta::is_variable(r)
        || std::meta::is_enumerator(r)
        || (std::meta::is_type(r)
            && (std::meta::is_class_type(r) || std::meta::is_enum_type(r)));
}

// Public-or-not transitive base walk for derived_from_ (self-contained: this
// header is included before nb_reflect.h's own base helpers exist).
consteval bool match_derives(std::meta::info d, std::meta::info base) {
    for (auto b : std::meta::bases_of(d, std::meta::access_context::unchecked())) {
        auto bt = std::meta::dealias(std::meta::type_of(b));
        if (bt == base)
            return true;
        if (std::meta::is_class_type(bt) && std::meta::is_complete_type(bt)
            && match_derives(bt, base))
            return true;
    }
    return false;
}

} // namespace detail

// --- Leaves -----------------------------------------------------------------

/// Matches class types (after dealiasing). Specializations included.
struct is_class_ {
    consteval bool operator()(std::meta::info r) const {
        return std::meta::is_type(r)
            && std::meta::is_class_type(std::meta::dealias(r));
    }
};

/// Matches enumeration types (after dealiasing).
struct is_enum_ {
    consteval bool operator()(std::meta::info r) const {
        return std::meta::is_type(r)
            && std::meta::is_enum_type(std::meta::dealias(r));
    }
};

/// Matches functions (free or member; not function templates).
struct is_function_ {
    consteval bool operator()(std::meta::info r) const {
        return std::meta::is_function(r);
    }
};

/// Matches templates of every kind (class/function/alias/variable).
struct is_template_ {
    consteval bool operator()(std::meta::info r) const {
        return std::meta::is_template(r);
    }
};

/// Matches entities whose identifier matches the glob `Pat` (`*` / `?`,
/// anchored). The subject is normalized first (dealias; specialization ->
/// template), so named_<"Transpose"> matches every Transpose<X>, and
/// named_<"vec*"> matches the vec class template itself.
template <reflect::fixed_string Pat> struct named_ {
    consteval bool operator()(std::meta::info r) const {
        r = detail::match_normalize(r);
        if (!std::meta::has_identifier(r))
            return false;
        return detail::glob_match(
            std::string_view(Pat.data, sizeof(Pat.data) - 1),
            std::meta::identifier_of(r));
    }
};

/// Matches entities declared (transitively) inside namespace `Ns` -- the same
/// parent-chain rule a namespace entry in nb::exclude_ applies, so the walk
/// looks through enclosing CLASS scopes too (a member class of a class in Ns
/// is inside Ns). The namespace itself does not match (it is not inside
/// itself).
template <std::meta::info Ns> struct in_namespace_ {
    static_assert(std::meta::is_namespace(Ns),
                  "nb::in_namespace_ requires a namespace reflection");
    consteval bool operator()(std::meta::info r) const {
        r = detail::match_normalize(r);
        if (!detail::match_scoped(r) || r == ^^::)
            return false;
        for (auto p = std::meta::parent_of(r);; p = std::meta::parent_of(p)) {
            if (p == Ns)
                return true;
            if (p == ^^::)
                return false;
        }
    }
};

/// Matches `Base` itself and every class type (transitively) derived from it,
/// through public AND non-public bases. Including the base means a matched
/// hierarchy enters the bind set whole, so the reachability rule wires real
/// Python base classes instead of flattening. Incomplete types answer false.
template <std::meta::info Base> struct derived_from_ {
    consteval bool operator()(std::meta::info r) const {
        constexpr auto base = std::meta::dealias(Base);
        static_assert(std::meta::is_type(base) && std::meta::is_class_type(base),
                      "nb::derived_from_ requires a class type reflection");
        if (!std::meta::is_type(r))
            return false;
        auto d = std::meta::dealias(r);
        if (!std::meta::is_class_type(d) || !std::meta::is_complete_type(d))
            return false;
        return d == base || detail::match_derives(d, base);
    }
};

/// Matches entities carrying a [[=...]] annotation of exact type `A` (e.g. a
/// user-defined tag type). The string-bearing built-ins (reflect::rename /
/// doc / property) are length-parameterized templates -- match those by name
/// or with a custom matcher instead.
template <typename A> struct has_annotation_ {
    consteval bool operator()(std::meta::info r) const {
        if (std::meta::is_type(r))
            r = std::meta::dealias(r);
        // annotations_of throws on templates and non-declarations (P3560);
        // restrict to the kinds annotations can appear on.
        if (std::meta::is_template(r))
            return false;
        if (!((std::meta::is_type(r) && (std::meta::is_class_type(r)
                                         || std::meta::is_enum_type(r)))
              || std::meta::is_function(r) || std::meta::is_variable(r)))
            return false;
        return !std::meta::annotations_of_with_type(r, ^^A).empty();
    }
};

// --- Combinators --------------------------------------------------------------

/// Matches when every component matcher matches (empty all_of_ matches all).
template <typename... Ms> struct all_of_ {
    consteval bool operator()(std::meta::info r) const {
        return (Ms{}(r) && ...);
    }
};

/// Matches when any component matcher matches (empty any_of_ matches none).
template <typename... Ms> struct any_of_ {
    consteval bool operator()(std::meta::info r) const {
        return (Ms{}(r) || ...);
    }
};

/// Inverts a matcher.
template <typename M> struct not_ {
    consteval bool operator()(std::meta::info r) const {
        return !M{}(r);
    }
};

} // namespace nanobind

#endif // __has_include(<meta>)
