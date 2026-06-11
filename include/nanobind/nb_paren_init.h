/*
    nanobind/nb_paren_init.h: nb::init with parens-construction semantics.

    The constructor visitor used for every reflected constructor. nb::init
    constructs with BRACES (`new (p) T{args...}`), which a reflected
    constructor must not use: if the class also has an initializer_list ctor,
    braces hijack to it (immer::vector's (size_type, T) fill ctor brace-selects
    initializer_list<int> and the size_type NARROWS: a hard error; in
    non-narrowing shapes it silently runs the WRONG constructor). The binder
    binds a REAL declared constructor, so parens -- which select exactly the
    reflected overload -- are correct; braces remain only for the aggregate
    case (no parens ctor to select). Mirrors nb::init's body otherwise
    (Args... are real template parameters, so no spliced lambda signature).

    This header is deliberately reflection-free plain C++17: it is included by
    nb_reflect.h (the constexpr backend) AND by the source the emit backend
    generates, which is compiled by a production toolchain without P2996.

    Copyright (c) 2025.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE file.
*/

#pragma once

#include "nanobind.h"

NAMESPACE_BEGIN(NB_NAMESPACE)
NAMESPACE_BEGIN(detail)

template <typename T, typename... A>
NB_INLINE void reflect_construct_at(void *p, A&&... a) {
    if constexpr (std::is_constructible_v<T, A...>)
        new (p) T((A&&) a...);
    else
        new (p) T{(A&&) a...};   // aggregate-init fallback
}

template <typename... Args>
struct reflect_init : def_visitor<reflect_init<Args...>> {
    NB_INLINE reflect_init() {}

    template <typename Class, typename... Extra>
    NB_INLINE static void execute(Class &cl, const Extra&... extra) {
        using Type = typename Class::Type;
        using Alias = typename Class::Alias;
        cl.def(
            "__init__",
            [](pointer_and_handle<Type> v, Args... args) {
                if constexpr (!std::is_same_v<Type, Alias> &&
                              std::is_constructible_v<Type, Args...>) {
                    if (!detail::nb_inst_python_derived(v.h.ptr())) {
                        reflect_construct_at<Type>(
                            (void *) v.p, (detail::forward_t<Args>) args...);
                        return;
                    }
                }
                reflect_construct_at<Alias>(
                    (void *) v.p, (detail::forward_t<Args>) args...);
            },
            extra...);
    }
};

NAMESPACE_END(detail)
NAMESPACE_END(NB_NAMESPACE)
