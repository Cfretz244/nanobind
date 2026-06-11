/*
    nanobind/nb_reflect_codegen.h: codegen fallback for the reflection binder.

    A trampoline (a class that derives from a bound type and overrides its virtual
    functions to forward into Python) cannot be synthesized in-language on a P2996
    compiler -- there is no way to inject member functions into a class. This header
    provides the "tier 2" fallback: it walks a namespace with reflection and emits,
    as C++ source text, a trampoline struct for every class that has overridable
    virtual functions, plus an NB_REFLECT_TRAMPOLINE registration for each.

    The generated source is meant to be written to a header by a tiny generator
    program (see emit_trampolines / write_trampolines) and then #included by the
    bindings translation unit *before* it calls nb::reflect_<...>. The generated
    header may be empty (no class needed a trampoline); the build can run the same
    steps unconditionally.

    The generated code refers to each virtual's return/parameter types via splices
    off the method's reflection (typename [: type_of(...) :]), so this header needs
    no general C++ type-name printer.

    Requires a compiler with P2996 support (e.g. Bloomberg clang-p2996).

    Copyright (c) 2025.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE file.
*/

#pragma once

#if __has_include(<meta>)

#include <meta>
#include "nb_reflect.h"
#include "nb_reflect_spell.h"
#include <string>
#include <string_view>
#include <vector>
#include <fstream>

NAMESPACE_BEGIN(NB_NAMESPACE)
NAMESPACE_BEGIN(detail)
NAMESPACE_BEGIN(codegen)

// consteval integer -> decimal string (std::to_string is not usable here).
consteval std::string num(std::size_t n) {
    if (n == 0)
        return "0";
    std::string s;
    while (n) {
        s.insert(s.begin(), char('0' + n % 10));
        n /= 10;
    }
    return s;
}

// The fully-qualified, compilable type renderer lives in nb_reflect_spell.h
// (detail::type_spelling), shared with the emit backend; unqualified
// type_spelling calls below resolve to it through the enclosing namespace.

// A signature key used to de-duplicate a virtual across a class hierarchy (so a
// derived override shadows the base declaration).
consteval std::string signature_key(std::meta::info m) {
    std::string k(std::meta::identifier_of(m));
    k += "(";
    for (auto p : std::meta::parameters_of(m)) {
        k += std::string(std::meta::display_string_of(std::meta::type_of(p)));
        k += ",";
    }
    k += ")";
    if (std::meta::is_const(m))
        k += "const";
    return k;
}

consteval std::size_t index_in_members(std::meta::info owner, std::meta::info m) {
    auto members = std::meta::members_of(owner, std::meta::access_context::unchecked());
    for (std::size_t i = 0; i < members.size(); ++i)
        if (members[i] == m)
            return i;
    return static_cast<std::size_t>(-1);
}

consteval bool str_vec_contains(const std::vector<std::string>& v, const std::string& x) {
    for (auto& e : v)
        if (e == x)
            return true;
    return false;
}

// The set of virtual functions overridable on `cls`: those declared in `cls` and
// in its public-base subtree, de-duplicated by signature (most-derived wins),
// excluding the destructor and private virtuals.
consteval std::vector<std::meta::info> overridable_virtuals(std::meta::info cls) {
    std::vector<std::meta::info> order;
    order.push_back(cls);
    collect_public_base_subtree(cls, order);  // appends the public-base subtree

    std::vector<std::meta::info> result;
    std::vector<std::string> seen;
    for (auto type : order) {
        for (auto mem :
             std::meta::members_of(type, std::meta::access_context::unchecked())) {
            if (!std::meta::is_function(mem) || !std::meta::is_virtual(mem))
                continue;
            if (std::meta::is_destructor(mem) || std::meta::is_private(mem))
                continue;
            std::string key = signature_key(mem);
            if (str_vec_contains(seen, key))
                continue;
            seen.push_back(key);
            result.push_back(mem);
        }
    }
    return result;
}

// Emit a single overriding member function for virtual `m`.
consteval std::string gen_override(std::meta::info m) {
    std::meta::info owner = std::meta::parent_of(m);
    std::string memexpr = "::nanobind::detail::codegen_member<^^ " +
                          type_spelling(owner) + ", " +
                          num(index_in_members(owner, m)) + ">()";
    std::string name(std::meta::identifier_of(m));
    auto params = std::meta::parameters_of(m);

    std::string s = "    typename [: ::std::meta::return_type_of(" + memexpr +
                    ") :] " + name + "(";
    for (std::size_t j = 0; j < params.size(); ++j) {
        if (j)
            s += ", ";
        s += "typename [: ::std::meta::type_of(::std::meta::parameters_of(" +
             memexpr + ")[" + num(j) + "]) :] a" + num(j);
    }
    s += ")";
    if (std::meta::is_const(m))
        s += " const";
    if (std::meta::is_noexcept(m))
        s += " noexcept";
    s += " override { ";
    s += std::meta::is_pure_virtual(m) ? "NB_OVERRIDE_PURE(" : "NB_OVERRIDE(";
    s += name;
    for (std::size_t j = 0; j < params.size(); ++j)
        s += ", a" + num(j);
    s += "); }\n";
    return s;
}

// Emit the trampoline struct + registration for `cls`, or "" if it has no
// overridable virtuals or was already emitted. `seen` holds the trampoline
// struct names emitted so far and is threaded across the whole emit_trampolines
// run, so a class reachable from more than one reflected arg (e.g. a virtual
// detail:: type pulled in by both `^^nlohmann::json` and a helper namespace that
// returns json) is emitted exactly once — otherwise the generated header has a
// C++ redefinition and fails to compile.
consteval std::string emit_one_class(std::meta::info cls,
                                     std::vector<std::string>& seen) {
    std::vector<std::meta::info> virts = overridable_virtuals(cls);
    if (virts.empty())
        return "";

    // Fully-qualified C++ spelling (handles template specializations); the struct
    // name drops the non-identifier chars in a template-id ("<", ">", ",", " ").
    std::string cq = type_spelling(cls);
    std::string tname = "Tramp" + sanitize_identifier(cq);

    if (str_vec_contains(seen, tname))
        return "";
    seen.push_back(tname);

    std::string s = "namespace nanobind { namespace reflect_generated {\n";
    s += "struct " + tname + " : " + cq + " {\n";
    s += "    NB_TRAMPOLINE(" + cq + ", " + num(virts.size()) + ");\n";
    for (auto m : virts)
        s += gen_override(m);
    s += "};\n} } // namespace nanobind::reflect_generated\n";
    s += "NB_REFLECT_TRAMPOLINE(" + cq +
         ", ::nanobind::reflect_generated::" + tname + ");\n\n";
    return s;
}

// Recurse into a namespace (or handle a single class) and emit trampolines.
consteval std::string emit_subtree(std::meta::info r,
                                   std::vector<std::string>& seen) {
    std::string s;
    if (std::meta::is_namespace(r)) {
        for (auto mem :
             std::meta::members_of(r, std::meta::access_context::unchecked())) {
            if (std::meta::is_type(mem) && std::meta::is_class_type(mem) &&
                !std::meta::is_template(mem))
                s += emit_one_class(mem, seen);
            else if (std::meta::is_namespace(mem))
                s += emit_subtree(mem, seen);
        }
    } else if (std::meta::is_type(r) && std::meta::is_class_type(r)) {
        s += emit_one_class(r, seen);
    }
    return s;
}

// Emit a trampoline (when needed) for every user class-template specialization
// reachable from r's signatures. Specializations are not namespace members, so
// emit_subtree never reaches them; this covers a templated class with virtuals
// (e.g. Processor<int>). required_user_specs is de-duplicated per call; `seen`
// additionally de-duplicates across reflected args and against emit_subtree.
consteval std::string emit_spec_classes(std::meta::info r,
                                        std::vector<std::string>& seen) {
    std::string s;
    for (auto spec : required_user_specs(r))
        s += emit_one_class(spec, seen);
    return s;
}

// Emit the #include directives for the <nanobind/stl/*.h> type casters required by
// the std types appearing in signatures reachable from r, de-duplicated. Unlike the
// header-only path, codegen CAN emit these (the generated header is #included before
// reflect_), so a generated module needs no hand-listed stl casters. Uses the
// spec-aware walk so a discovered specialization's members (e.g. a Holder<int> with a
// std::vector<int> field) pull their casters too.
consteval std::string emit_stl_includes(std::meta::info r,
                                        std::span<const std::meta::info> ex = {}) {
    std::vector<std::string_view> hdrs;
    // The exclusion set MUST reach the spec-discovery fixpoint: without it,
    // an expression-template shape (exclude_test's Expr, Eigen) diverges --
    // the exclusions are exactly what bound that walk (BINDER-0014).
    for (auto ty : required_stl_types_with_specs(r, ex)) {
        std::string_view h = stl_caster_header(ty);
        bool seen = false;
        for (auto e : hdrs)
            if (e == h) { seen = true; break; }
        if (!seen)
            hdrs.push_back(h);
    }
    std::string s;
    for (auto h : hdrs) {
        s += "#include <";
        s += h;
        s += ">\n";
    }
    return s;
}

NAMESPACE_END(codegen)
NAMESPACE_END(detail)

/// Generate, as a static C++ source string, an includable header for `Rs...`:
/// the required <nanobind/stl/*.h> caster includes for every std type in a bound
/// signature, plus trampoline classes (and their NB_REFLECT_TRAMPOLINE
/// registrations) for every reachable class that has overridable virtual functions.
/// The result is complete; it may contain only the prologue + caster includes if
/// nothing needed a trampoline.
///
///   constexpr const char* src = nb::emit_trampolines<^^my_namespace>();
template <std::meta::info... Rs>
consteval const char* emit_trampolines() {
    std::string out =
        "// Generated by nanobind reflection codegen -- do not edit.\n"
        "#pragma once\n"
        "#include <nanobind/nb_reflect.h>\n"
        "#include <nanobind/trampoline.h>\n";
    ((out += detail::codegen::emit_stl_includes(Rs)), ...);
    out += "\n";
    // One `seen` set across every reflected arg and both emission passes, so each
    // trampoline struct is emitted exactly once (no redefinition in the header).
    std::vector<std::string> seen;
    ((out += detail::codegen::emit_subtree(Rs, seen)), ...);
    // Trampolines for discovered template specializations (not namespace members,
    // so emit_subtree misses them).
    ((out += detail::codegen::emit_spec_classes(Rs, seen)), ...);
    return std::define_static_string(out);
}

/// Write generated trampoline source to `path`. Intended for use from a small
/// generator program: int main() { nb::write_trampolines("x.gen.h",
/// nb::emit_trampolines<^^my_ns>()); }
inline bool write_trampolines(const char* path, const char* source) {
    std::ofstream out(path);
    if (!out)
        return false;
    out << source;
    return bool(out);
}

NAMESPACE_END(NB_NAMESPACE)

#endif // __has_include(<meta>)
