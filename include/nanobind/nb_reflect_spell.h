/*
    nanobind/nb_reflect_spell.h: fully-qualified, compilable C++ type spelling.

    The emit backend (nb_reflect_emit.h) renders binding decisions as ordinary
    C++ source for a production toolchain, so every type appearing in a bound
    signature must be spelled as text that compiles in a fresh TU which only
    includes the target library's public headers. display_string_of cannot do
    this: it drops qualification on compound types ("const Foo &" for
    ::ns::Foo) and punts on several reflection kinds. This header is a full
    recursive renderer over the type grammar, modeled on the case analysis of
    the toolchain's <meta> pretty-printer but emitting fully-qualified names
    (leading ::) and working value-form (no NTTPs), so consteval walks can call
    it on computed reflections.

    Unspellable types -- anonymous records/enums with no typedef-for-linkage
    name, lambda closures, entities inside anonymous namespaces (TU-local, so
    meaningless in the generated TU), enum-typed template arguments that match
    no enumerator -- yield the EMPTY STRING, which composite cases propagate.
    Callers gate on type_spellable / fn_signature_spellable and skip such
    members (an emit-mode limitation; the constexpr backend can still bind
    them via splices).

    Known policies:
    - Alias sugar is canonicalized (dealias) before rendering: a member
      typedef may be private/inaccessible while the canonical type is public,
      and the remove_* decomposition helpers lose sugar anyway. The
      typedef-for-linkage idiom (`typedef struct {...} name_t;`, BINDER-0018)
      is recovered at the anonymous record itself via its linkage name.
    - Inline std namespaces (std::__1) are skipped in qualified names via the
      __-prefix-under-std heuristic; GCC 16 has no is_inline_namespace
      metafunction. Inline namespaces elsewhere (absl::lts_*) are kept --
      explicitly naming an inline namespace is valid, just verbose.
    - Template-ids render EVERY argument explicitly (including defaulted
      ones): never rely on default arguments matching across stdlibs.

    Requires a C++26 compiler with P2996 support (GCC 16+, -std=c++26 -freflection).

    Copyright (c) 2025.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE file.
*/

#pragma once

#if __has_include(<meta>)

#include <meta>
#include "nb_defs.h"
#include <string>
#include <string_view>

NAMESPACE_BEGIN(NB_NAMESPACE)
NAMESPACE_BEGIN(detail)

consteval std::string type_spelling(std::meta::info t);

// consteval integer -> decimal string (std::to_string is not usable here).
consteval std::string spell_num(std::size_t n) {
    if (n == 0)
        return "0";
    std::string s;
    while (n) {
        s.insert(s.begin(), char('0' + n % 10));
        n /= 10;
    }
    return s;
}

// True for a namespace component to omit from qualified names: libc++'s
// inline namespace std::__1 (and friends). Names resolve identically without
// inline-namespace components; emitting "__1" would also tie the generated
// source to one stdlib's internals. Heuristic (__-prefixed under std) because
// the pinned toolchain lacks std::meta::is_inline_namespace.
consteval bool spell_skips_ns_component(std::meta::info ns) {
    std::string_view id = std::meta::identifier_of(ns);
    if (id.size() < 2 || id[0] != '_' || id[1] != '_')
        return false;
    for (auto p = std::meta::parent_of(ns); p != ^^::;
         p = std::meta::parent_of(p))
        if (std::meta::has_identifier(p) && std::meta::identifier_of(p) == "std")
            return true;
    return false;
}

// Fully-qualified name of a NAMED entity (class/enum/alias/template/
// enumerator/namespace member): "::ns::Inner::Foo". Empty when any enclosing
// scope is unnameable (anonymous namespace: the entity is TU-local and cannot
// be named from the generated TU). An enclosing class-template SPECIALIZATION
// is rendered via type_spelling ("::ns::Outer<int>::Inner").
consteval std::string spell_qualified(std::meta::info e) {
    if (!std::meta::has_identifier(e))
        return {};
    std::string result(std::meta::identifier_of(e));
    for (auto p = std::meta::parent_of(e); p != ^^::;
         p = std::meta::parent_of(p)) {
        if (std::meta::is_namespace(p)) {
            if (!std::meta::has_identifier(p))
                return {};  // anonymous namespace: TU-local
            if (spell_skips_ns_component(p))
                continue;
            result = std::string(std::meta::identifier_of(p)) + "::" + result;
        } else if (std::meta::is_type(p)
                   && std::meta::has_template_arguments(p)) {
            std::string outer = type_spelling(p);  // already fully qualified
            if (outer.empty())
                return {};
            return outer + "::" + result;
        } else if (std::meta::has_identifier(p)) {
            result = std::string(std::meta::identifier_of(p)) + "::" + result;
        } else {
            return {};  // anonymous enclosing scope
        }
    }
    return "::" + result;
}

// Spelling for an anonymous (identifier-less) record/enum: the qualified name
// of its typedef-for-linkage, when one exists (`typedef struct {...} point_t;`
// -> "::ns::point_t", BINDER-0018's naming rule applied to spelling). The
// typedef name does not survive dealias (display_string_of on the canonical
// record is "(anonymous type)"), so the naming alias is recovered by scanning
// the record's own scope -- where the typedef-for-linkage idiom always
// declares it. A truly anonymous type (no alias; lambda closures) is
// unspellable.
consteval std::string spell_anonymous(std::meta::info t) {
    auto scope = std::meta::parent_of(t);
    if (!std::meta::is_namespace(scope)
        && !(std::meta::is_type(scope) && std::meta::is_complete_type(scope)))
        return {};
    for (auto m : std::meta::members_of(
             scope, std::meta::access_context::unchecked()))
        if (std::meta::is_type_alias(m) && std::meta::has_identifier(m)
            && std::meta::dealias(m) == t)
            return spell_qualified(m);
    return {};
}

// The trait used to decompose member-pointer TYPES value-form: substitute
// instantiates nb_memptr_traits<M C::*>, whose partial specialization binds M
// and C as member aliases readable via members_of + dealias. (The <meta>
// pretty-printer decomposes via an NTTP splice, unusable from value-form
// code.)
template <typename MP> struct nb_memptr_traits;
template <typename M, typename C> struct nb_memptr_traits<M C::*> {
    using member_type = M;
    using class_type = C;
};

consteval std::meta::info spell_memptr_part(std::meta::info mpty,
                                            std::string_view which) {
    auto spec = std::meta::substitute(^^nb_memptr_traits, {mpty});
    for (auto m : std::meta::members_of(
             spec, std::meta::access_context::unchecked()))
        if (std::meta::is_type_alias(m) && std::meta::has_identifier(m)
            && std::meta::identifier_of(m) == which)
            return std::meta::dealias(m);
    return ^^void;
}

// "(Args...) [const] [volatile] [&|&&] [noexcept]" -- the shared tail of
// function-type / function-pointer / member-function-pointer spellings.
// `t` is a function TYPE; parameters_of on a function type yields TYPES.
consteval std::string spell_fn_tail(std::meta::info t) {
    std::string s = "(";
    bool first = true;
    for (auto p : std::meta::parameters_of(t)) {
        if (!first)
            s += ", ";
        first = false;
        std::string ps = type_spelling(p);
        if (ps.empty())
            return {};
        s += ps;
    }
    s += ")";
    if (std::meta::is_const(t))
        s += " const";
    if (std::meta::is_volatile(t))
        s += " volatile";
    if (std::meta::is_lvalue_reference_qualified(t))
        s += " &";
    else if (std::meta::is_rvalue_reference_qualified(t))
        s += " &&";
    if (std::meta::is_noexcept(t))
        s += " noexcept";
    return s;
}

// One template argument: a type, a template (template-template argument), or
// a value. Integral/bool values render via display_string_of; enum-typed
// values are resolved to the matching ENUMERATOR's qualified name
// (display_string_of punts on enum value reflections); a non-enumerator enum
// value is unspellable for now.
consteval std::string spell_template_arg(std::meta::info arg) {
    if (std::meta::is_type(arg))
        return type_spelling(arg);
    if (std::meta::is_template(arg))
        return spell_qualified(arg);
    auto vt = std::meta::dealias(std::meta::type_of(arg));
    if (std::meta::is_enum_type(vt)) {
        for (auto en : std::meta::enumerators_of(vt))
            if (std::meta::constant_of(en) == arg)
                return spell_qualified(en);
        return {};
    }
    std::string_view d = std::meta::display_string_of(arg);
    if (d.empty() || d[0] == '(')  // "(unsupported-reflection)" et al.
        return {};
    return std::string(d);
}

consteval std::string type_spelling(std::meta::info t) {
    t = std::meta::dealias(t);

    // References.
    if (std::meta::is_lvalue_reference_type(t)) {
        std::string inner = type_spelling(std::meta::remove_reference(t));
        return inner.empty() ? std::string{} : inner + " &";
    }
    if (std::meta::is_rvalue_reference_type(t)) {
        std::string inner = type_spelling(std::meta::remove_reference(t));
        return inner.empty() ? std::string{} : inner + " &&";
    }

    // Arrays before cv: element qualifiers travel with remove_all_extents.
    if (std::meta::is_array_type(t)) {
        std::string s = type_spelling(std::meta::remove_all_extents(t));
        if (s.empty())
            return {};
        for (std::size_t k = 0; k < std::meta::rank(t); ++k) {
            std::size_t e = std::meta::extent(t, (unsigned) k);
            s += e ? "[" + spell_num(e) + "]" : std::string("[]");
        }
        return s;
    }

    // Function types (a member-function pointee, or a template argument like
    // std::function<void(int)>'s). Their cv/ref qualifiers live in the tail;
    // is_const_type answers false on them, so the cv branch below is safe.
    if (std::meta::is_function_type(t)) {
        std::string r = type_spelling(std::meta::return_type_of(t));
        std::string tail = spell_fn_tail(t);
        return (r.empty() || tail.empty()) ? std::string{} : r + " " + tail;
    }

    // cv-qualification: prefix style for named types, suffix style after the
    // star for (member) pointers ("char *const" is a const pointer; "const
    // char *" would be a pointer to const).
    if (std::meta::is_const_type(t) || std::meta::is_volatile_type(t)) {
        std::string cv_pre, cv_post;
        if (std::meta::is_const_type(t))
            cv_pre = "const ", cv_post = " const";
        if (std::meta::is_volatile_type(t))
            cv_pre += "volatile ", cv_post += " volatile";
        auto bare = std::meta::remove_cv(t);
        std::string inner = type_spelling(bare);
        if (inner.empty())
            return {};
        if (std::meta::is_pointer_type(bare)
            || std::meta::is_member_pointer_type(bare))
            return inner + cv_post;
        return cv_pre + inner;
    }

    // Pointers (function pointers spell "Ret (*)(Args...)").
    if (std::meta::is_pointer_type(t)) {
        auto pte = std::meta::dealias(std::meta::remove_pointer(t));
        if (std::meta::is_function_type(pte)) {
            std::string r = type_spelling(std::meta::return_type_of(pte));
            std::string tail = spell_fn_tail(pte);
            return (r.empty() || tail.empty()) ? std::string{}
                                               : r + " (*)" + tail;
        }
        std::string inner = type_spelling(std::meta::remove_pointer(t));
        return inner.empty() ? std::string{} : inner + " *";
    }

    // Member pointers: "M C::*" / "Ret (C::*)(Args...) cv ref noexcept".
    if (std::meta::is_member_pointer_type(t)) {
        auto cls = spell_memptr_part(t, "class_type");
        auto mem = spell_memptr_part(t, "member_type");
        std::string cs = type_spelling(cls);
        if (cs.empty())
            return {};
        if (std::meta::is_member_function_pointer_type(t)) {
            std::string r = type_spelling(std::meta::return_type_of(mem));
            std::string tail = spell_fn_tail(mem);
            return (r.empty() || tail.empty())
                       ? std::string{}
                       : r + " (" + cs + "::*)" + tail;
        }
        std::string ms = type_spelling(mem);
        return ms.empty() ? std::string{} : ms + " " + cs + "::*";
    }

    // Template specializations: qualified template name + ALL arguments.
    if (std::meta::has_template_arguments(t)) {
        std::string s = spell_qualified(std::meta::template_of(t));
        if (s.empty())
            return {};
        s += "<";
        bool first = true;
        for (auto arg : std::meta::template_arguments_of(t)) {
            if (!first)
                s += ", ";
            first = false;
            std::string as = spell_template_arg(arg);
            if (as.empty())
                return {};
            s += as;
        }
        s += ">";
        return s;
    }

    // Named (or typedef-named anonymous) classes, unions, and enums.
    if (std::meta::is_class_type(t) || std::meta::is_union_type(t)
        || std::meta::is_enum_type(t)) {
        if (std::meta::has_identifier(t))
            return spell_qualified(t);
        return spell_anonymous(t);
    }

    // Fundamentals (int, unsigned long, void, decltype(nullptr), ...).
    // `display_string_of` spells most of these canonically, but on this
    // toolchain a distinct char-family fundamental can render as its UNDERLYING
    // type rather than its own keyword -- `wchar_t` displays as "int", so
    // `std::wstring` (= basic_string<wchar_t>) would emit
    // basic_string<int, char_traits<int>, ...> and fail to compile against the
    // real overload (CLI11's parse(std::wstring)). Spell the char family by
    // type identity so the keyword is always exact; the rest fall through.
    if (std::meta::dealias(t) == ^^wchar_t)  return "wchar_t";
    if (std::meta::dealias(t) == ^^char8_t)  return "char8_t";
    if (std::meta::dealias(t) == ^^char16_t) return "char16_t";
    if (std::meta::dealias(t) == ^^char32_t) return "char32_t";
    // The GNU/Clang builtin 128-bit integers: display_string_of punts with
    // "(anonymous type)" (absl::int128's ctor/conversion surface), but both
    // toolchains spell them as keywords.
    if (std::meta::dealias(t) == ^^__int128)          return "__int128";
    if (std::meta::dealias(t) == ^^unsigned __int128) return "unsigned __int128";
    // Safety net for any remaining display punt ("(anonymous type)",
    // "(unsupported-reflection)", ...): propagate UNSPELLABLE rather than
    // emitting a parse error into the generated TU; the spellability gates
    // then skip the member.
    std::string_view d = std::meta::display_string_of(t);
    if (d.empty() || d[0] == '(')
        return {};
    return std::string(d);
}

consteval bool type_spellable(std::meta::info t) {
    return !type_spelling(t).empty();
}

// Emitter-only gate: every type in fn's signature must be spellable, or the
// member is skipped in emit mode (the constexpr backend can still bind it).
// `fn` is a function DECLARATION (parameters_of yields parameter decls).
consteval bool fn_signature_spellable(std::meta::info fn) {
    if (!std::meta::is_constructor(fn) && !std::meta::is_destructor(fn)
        && !type_spellable(std::meta::return_type_of(fn)))
        return false;
    for (auto p : std::meta::parameters_of(fn))
        if (!type_spellable(std::meta::type_of(p)))
            return false;
    return true;
}

NAMESPACE_END(detail)
NAMESPACE_END(NB_NAMESPACE)

#endif // __has_include(<meta>)
