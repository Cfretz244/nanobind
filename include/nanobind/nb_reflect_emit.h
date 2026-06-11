/*
    nanobind/nb_reflect_emit.h: full source-codegen backend for the reflection
    binder.

    nb::emit_bindings<Rs...>(module_name, preamble) walks EXACTLY the same
    reflection metadata as nb::reflect_<Rs...>(m) -- through the same shared
    decision classifiers in nb_reflect.h (classify_*, *_route, ctor_binds,
    plan_free_operator, ...) -- but instead of binding at constexpr time it
    renders the decisions as ONE self-contained C++ translation unit of
    ordinary nanobind binding code: plain C++17/20, no reflection constructs,
    no nb_reflect*.h includes. A small generator program (compiled by the
    experimental P2996 toolchain) writes that TU to disk; a PRODUCTION
    toolchain then compiles it into the extension module. The constexpr
    backend is unchanged and remains the default.

    Structure of the generated TU:

        <caller-supplied preamble: the library's #includes>
        #include <nanobind/nanobind.h> + support headers + STL caster includes
        namespace nbgen {
          template <long double, class = void> struct value_probe;
          struct NbGenTramp_...;                            // opt-in trampolines
          template <class Self = T>                         // one per class/enum
          static void nbgen_bind_<Class>(nb::module_ &m);
        }
        NB_MODULE(<module_name>, m) { nbgen::nbgen_bind_...(m); ... }

    Every bind function opens with the same idempotence guard as
    reflect_class (nb::type<T>().is_valid()) and ensures its Python base is
    bound first, so binding stays order-independent. The bind functions are
    TEMPLATES (Self defaulted to the class) because the decisions answered by
    the COMPILER in the constexpr backend -- the static-const value probe
    (BINDER-0020) and the __str__ streamability probe -- are emitted as the
    identical probes into the generated source, and requires-expressions only
    SFINAE / if-constexpr branches are only discarded inside templates.

    Trampolines are OPT-IN via pack markers (nb::trampoline_<^^Cls...> /
    nb::trampoline_all_) so the two backends trampoline the same classes; see
    emit_wants_trampoline.

    PERFORMANCE: the per-entity text is memoized into its own
    std::define_static_string initializer (emit_class_def_v et al.), so each
    class renders in a SEPARATE constant evaluation with a fresh step budget
    -- mirroring how the constexpr backend's per-class instantiations each
    evaluate independently -- and the top-level emit_bindings evaluation only
    concatenates precomputed pointers. Renderers append into a shared
    std::string out-parameter (no temporary-chain churn: consteval string
    copies are interpreter-expensive).

    This file contains NO binding decisions -- only text rendering. Every
    what-to-bind question is answered by the shared classifiers; every type is
    rendered by nb_reflect_spell.h. The one emit-ONLY gate is spellability:
    a member whose signature contains an unnameable type (anonymous record
    with no typedef-for-linkage, lambda, anonymous-namespace entity) is
    skipped with a comment -- the constexpr backend can bind it via splices,
    text cannot (documented mode limitation).

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
#include "nb_reflect_codegen.h"   // emit_stl_includes, overridable_virtuals
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <fstream>

NAMESPACE_BEGIN(NB_NAMESPACE)
NAMESPACE_BEGIN(detail)
NAMESPACE_BEGIN(emitgen)

// std::define_static_string silently MISCOMPILES for inputs >= 32768 chars on
// the pinned toolchain: reflect_constant_string spells the whole string as
// one NTTP pack, and each substituted element's PackIndex is a 15-bit
// bitfield (SubstNonTypeTemplateParmExpr::PackIndex, clang AST) that
// overflows -- "excess elements in array initializer" out of FixedArray
// (TC-0018). Generated TUs run to megabytes, so per-entity text is lifted to
// static storage as CHUNKS below this limit and the generated file is written
// from the chunk sequence; no giant single pack is ever formed (which is also
// kinder to compile time once the bitfield is widened).
inline constexpr std::size_t emit_chunk_size = 16384;

consteval std::span<const char* const> make_static_chunks(const std::string& s) {
    std::vector<const char*> chunks;
    std::string_view v(s);
    for (std::size_t i = 0; i < v.size(); i += emit_chunk_size)
        chunks.push_back(std::define_static_string(
            v.substr(i, emit_chunk_size)));
    return std::define_static_array(chunks);
}

consteval void push_chunked(std::vector<const char*>& parts,
                            const std::string& s) {
    std::string_view v(s);
    for (std::size_t i = 0; i < v.size(); i += emit_chunk_size)
        parts.push_back(std::define_static_string(
            v.substr(i, emit_chunk_size)));
}

// --- Small text utilities (append-style: out += ... only) ---

// Escape into a generated "..." literal, appending `"..."` WITH the quotes.
// Non-printables use fixed-width 3-digit octal (\NNN) -- unlike \x, a
// following digit cannot extend the escape.
consteval void append_quoted(std::string& out, std::string_view s) {
    out += '"';
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default:
            if ((unsigned char) c < 0x20 || (unsigned char) c == 0x7f) {
                out += '\\';
                out += char('0' + ((c >> 6) & 7));
                out += char('0' + ((c >> 3) & 7));
                out += char('0' + (c & 7));
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

// Deterministic identifier-safe encoding of a type spelling, used to name the
// per-class bind functions and trampoline structs. Unlike sanitize_identifier
// (which DROPS punctuation and would collide Box<int> with Box<int*>), every
// structural character maps to a distinct token.
consteval void append_mangled(std::string& out, std::string_view spelling) {
    for (char c : spelling) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9') || c == '_') {
            out += c;
        } else {
            switch (c) {
            case ':': out += '_'; break;
            case '<': out += "_L"; break;
            case '>': out += "_R"; break;
            case ',': out += "_C"; break;
            case '*': out += "_P"; break;
            case '&': out += "_A"; break;
            case '[': out += "_B"; break;
            case ']': out += "_E"; break;
            case '(': out += "_O"; break;
            case ')': out += "_Q"; break;
            case ' ': break;
            default:  out += '_'; break;
            }
        }
    }
}

consteval const char* rv_policy_spelling(rv_policy p) {
    switch (p) {
    case rv_policy::take_ownership:      return "take_ownership";
    case rv_policy::copy:                return "copy";
    case rv_policy::move:                return "move";
    case rv_policy::reference:           return "reference";
    case rv_policy::reference_internal:  return "reference_internal";
    case rv_policy::none:                return "none";
    case rv_policy::automatic_reference: return "automatic_reference";
    default:                             return "automatic";
    }
}

// A skipped-member marker: keeps the generated file auditable (silent drops
// read as "bound" to a reviewer).
consteval void append_skip_note(std::string& out, std::string_view what) {
    out += "    // skipped (unspellable signature): ";
    out += what;
    out += "\n";
}

// --- Call-site expression rendering ---

// Append the qualified spelling of a free function's NAME for a call
// expression -- including operator functions ("::ns::operator+") and
// function-template specializations ("::ns::identity<int>"). Naming the
// function qualified reproduces the splice's exact-overload-set semantics (no
// ADL surprises); overload selection within the set is then driven by the
// forwarded declared-type arguments, which reselect the reflected overload.
// Returns false when unspellable.
consteval bool append_free_fn_callee(std::string& out, std::meta::info fn) {
    auto scope = std::meta::parent_of(fn);
    if (scope == ^^::) {
        out += "::";
    } else {
        std::string prefix = spell_qualified(scope);
        if (prefix.empty())
            return false;
        out += prefix;
        out += "::";
    }
    if (std::meta::is_operator_function(fn)) {
        out += "operator";
        out += std::meta::symbol_of(std::meta::operator_of(fn));
        return true;
    }
    if (std::meta::has_template_arguments(fn)) {
        out += std::meta::identifier_of(std::meta::template_of(fn));
        out += "<";
        bool first = true;
        for (auto arg : std::meta::template_arguments_of(fn)) {
            if (!first)
                out += ", ";
            first = false;
            std::string as = spell_template_arg(arg);
            if (as.empty())
                return false;
            out += as;
        }
        out += ">";
        return true;
    }
    if (!std::meta::has_identifier(fn))
        return false;
    out += std::meta::identifier_of(fn);
    return true;
}

// "(P0S a0, P1S a1" parameter declarations and the matching forwarded call
// arguments "::std::forward<P0S>(a0), ..." for a function DECLARATION's
// parameters. ok=false when any parameter type is unspellable.
struct param_text { std::string decls; std::string args; bool ok; };
consteval param_text params_for(std::meta::info fn) {
    param_text out{{}, {}, true};
    std::size_t i = 0;
    for (auto p : std::meta::parameters_of(fn)) {
        std::string ps = type_spelling(std::meta::type_of(p));
        if (ps.empty())
            return {{}, {}, false};
        std::string an = "a" + spell_num(i++);
        out.decls += ", ";
        out.decls += ps;
        out.decls += " ";
        out.decls += an;
        if (!out.args.empty())
            out.args += ", ";
        out.args += "::std::forward<";
        out.args += ps;
        out.args += ">(";
        out.args += an;
        out.args += ")";
    }
    return out;
}

// Same, but without forwarding (the free-operator binders pass operands
// through by declared type, mirroring the constexpr macros) and without the
// leading ", ".
consteval param_text params_plain(std::meta::info fn) {
    param_text out{{}, {}, true};
    std::size_t i = 0;
    for (auto p : std::meta::parameters_of(fn)) {
        std::string ps = type_spelling(std::meta::type_of(p));
        if (ps.empty())
            return {{}, {}, false};
        std::string an = "a" + spell_num(i++);
        if (i > 1) {
            out.decls += ", ";
            out.args += ", ";
        }
        out.decls += ps;
        out.decls += " ";
        out.decls += an;
        out.args += an;
    }
    return out;
}

// --- Def-extras rendering (mirrors with_arg_call_extras / with_call_extras /
// --- with_data_extras; same order: nb::arg per parameter, doc, rv_policy,
// --- keep_alive) ---

// nb::arg("name") per parameter, all-or-nothing (the constexpr path's rule).
consteval void append_arg_names(std::string& out, std::meta::info fn) {
    bool any = false;
    for (auto p : std::meta::parameters_of(fn))
        if (std::meta::has_identifier(p))
            any = true;
    if (!any)
        return;
    for (auto p : std::meta::parameters_of(fn)) {
        if (std::meta::has_identifier(p)) {
            out += ", nb::arg(";
            append_quoted(out, std::meta::identifier_of(p));
            out += ")";
        } else {
            out += ", nb::arg()";
        }
    }
}

template <std::meta::info Fn>
consteval void append_call_extras(std::string& out) {
    append_arg_names(out, Fn);
    constexpr const char* d = entity_doc<Fn>();
    if (d != nullptr) {
        out += ", ";
        append_quoted(out, d);
    }
    constexpr rv_policy pol = effective_rv_policy<Fn>();
    if (pol != rv_policy::automatic) {
        out += ", nb::rv_policy::";
        out += rv_policy_spelling(pol);
    }
    if constexpr (has_ann<Fn, reflect::keep_alive>()) {
        constexpr auto ka = get_ann<Fn, reflect::keep_alive>();
        out += ", nb::keep_alive<" + spell_num(ka.nurse) + ", "
             + spell_num(ka.patient) + ">()";
    }
}

template <std::meta::info Mem>
consteval void append_data_extras(std::string& out) {
    constexpr const char* d = entity_doc<Mem>();
    if (d != nullptr) {
        out += ", ";
        append_quoted(out, d);
    }
    // rv_policy ONLY when explicitly annotated (def_rw/def_ro default to
    // reference_internal, which must not be clobbered) -- with_data_extras' rule.
    if constexpr (has_ann<Mem, reflect::return_policy>()) {
        constexpr rv_policy pol = ann_rv_policy<Mem>();
        out += ", nb::rv_policy::";
        out += rv_policy_spelling(pol);
    }
}

// --- Constructors ---

template <std::meta::info Ctor>
consteval void append_ctor(std::string& out) {
    if (!fn_signature_spellable(Ctor)) {
        append_skip_note(out, "a constructor of " + std::string(
            std::meta::display_string_of(std::meta::parent_of(Ctor))));
        return;
    }
    // reflect_init, not nb::init: parens-construction semantics (BINDER-0026).
    out += "    cls.def(::nanobind::detail::reflect_init<";
    bool first = true;
    for (auto p : std::meta::parameters_of(Ctor)) {
        if (!first)
            out += ", ";
        first = false;
        out += type_spelling(std::meta::type_of(p));
    }
    out += ">()";
    append_arg_names(out, Ctor);
    out += ");\n";
}

// --- Data members / static data members ---

template <std::meta::info Mem>
consteval void append_data_member(std::string& out, std::string_view owner_spell) {
    constexpr data_route route = data_member_route(Mem);
    if constexpr (route == data_route::skip) {
        return;
    } else {
        out += (route == data_route::ro ? "    cls.def_ro("
                                        : "    cls.def_rw(");
        append_quoted(out, entity_name<Mem>());
        out += ", &";
        out += owner_spell;
        out += "::";
        out += std::meta::identifier_of(Mem);
        append_data_extras<Mem>(out);
        out += ");\n";
    }
}

template <std::meta::info Mem>
consteval void append_static_member(std::string& out,
                                    std::string_view owner_spell) {
    constexpr static_data_route route = static_member_route(Mem);
    if constexpr (route == static_data_route::skip) {
        return;
    } else {
        std::string qual(owner_spell);
        qual += "::";
        qual += std::meta::identifier_of(Mem);
        if constexpr (route == static_data_route::mutable_member) {
            out += "    cls.def_rw_static(";
            append_quoted(out, entity_name<Mem>());
            out += ", &" + qual + ");\n";
        } else {
            // The by-value-vs-by-address split is the value_as_nttp_probe,
            // emitted into the source so the PRODUCTION compiler answers the
            // identical question (constant-readable -> by value, no ODR-use;
            // BINDER-0020). The copy into `v` applies lvalue-to-rvalue
            // immediately, which is what avoids the ODR-use. The trailing
            // `Self` argument makes the probe DEPENDENT (see the bind-function
            // template note in the header comment).
            out += "    if constexpr (requires { typename nbgen::value_probe<"
                   "(long double)(" + qual + "), Self>; })\n";
            out += "        cls.def_prop_ro_static(";
            append_quoted(out, entity_name<Mem>());
            out += ", [](nb::handle) -> nb::object { auto v = " + qual
                 + "; return nb::cast(v); });\n";
            out += "    else\n";
            out += "        cls.def_ro_static(";
            append_quoted(out, entity_name<Mem>());
            out += ", &" + qual + ");\n";
        }
    }
}

// --- Methods / static methods / free functions ---

// One instance-method binding. `call_prefix` is "" for T's own members and
// proxies (unqualified call: preserves virtual dispatch, and a private-base
// re-export is callable ONLY unqualified), or "::ns::Base::" for flattened
// base members (qualified call: names the exact member like the splice does,
// immune to shadowing). The lambda mirrors the constexpr method binders:
// declared-type parameters, forwarded arguments, fixed return type.
template <std::meta::info Fn>
consteval void append_method(std::string& out, std::string_view cls_spell,
                             std::string_view bind_name,
                             std::string_view callee_name,
                             std::string_view call_prefix,
                             std::string_view extra_extras) {
    auto ft = std::meta::type_of(Fn);
    std::string ret = type_spelling(std::meta::return_type_of(ft));
    param_text pt = params_for(Fn);
    if (ret.empty() || !pt.ok) {
        append_skip_note(out, callee_name);
        return;
    }
    out += "    cls.def(";
    append_quoted(out, bind_name);
    out += ", [](";
    if (std::meta::is_const(ft))
        out += "const ";
    out += cls_spell;
    out += " &self";
    out += pt.decls;
    out += ") -> " + ret + " { return self.";
    out += call_prefix;
    out += callee_name;
    out += "(" + pt.args + "); }";
    out += extra_extras;
    out += ");\n";
}

template <std::meta::info Fn>
consteval void append_static_method(std::string& out,
                                    std::string_view bind_name,
                                    std::string_view callee) {
    auto ft = std::meta::type_of(Fn);
    std::string ret = type_spelling(std::meta::return_type_of(ft));
    param_text pt = params_for(Fn);
    if (ret.empty() || !pt.ok) {
        append_skip_note(out, callee);
        return;
    }
    out += "    cls.def_static(";
    append_quoted(out, bind_name);
    out += ", [](";
    if (!pt.decls.empty())
        out += std::string_view(pt.decls).substr(2);
    out += ") -> " + ret + " { return ";
    out += callee;
    out += "(" + pt.args + "); }";
    append_call_extras<Fn>(out);
    out += ");\n";
}

template <std::meta::info Fn>
consteval void append_free_function(std::string& out) {
    // Mirrors reflect_free_function's gate (deleted / variadic / move-only /
    // unbindable / nameless-non-spec), plus the emit-only spellability gate.
    if constexpr (std::meta::is_deleted(Fn)
                  || std::meta::has_ellipsis_parameter(Fn)
                  || has_move_only_by_value_param(Fn)
                  || has_unbindable_signature(Fn)
                  || (!std::meta::has_identifier(Fn)
                      && !std::meta::has_template_arguments(Fn))) {
        return;
    } else {
        constexpr const char* name = entity_name<Fn>();
        std::string callee;
        std::string ret = type_spelling(std::meta::return_type_of(Fn));
        param_text pt = params_for(Fn);
        if (!append_free_fn_callee(callee, Fn) || ret.empty() || !pt.ok) {
            append_skip_note(out, name);
            return;
        }
        out += "    m.def(";
        append_quoted(out, name);
        out += ", [](";
        if (!pt.decls.empty())
            out += std::string_view(pt.decls).substr(2);
        out += ") -> " + ret + " { return " + callee + "(" + pt.args + "); }";
        append_call_extras<Fn>(out);
        out += ");\n";
    }
}

// --- Member operators and conversions ---

// Mirrors reflect_bind_operator: dunder mapping, the qualifier-matrix gate
// (method_shape_bindable = the binder-spec completeness gate's value twin),
// in-place identity preservation. The call spells the operator explicitly
// ("self.operator+(...)"), naming the exact member like the splice does.
template <std::meta::info Fn>
consteval void append_member_operator(std::string& out,
                                      std::string_view cls_spell) {
    constexpr auto op = std::meta::operator_of(Fn);
    constexpr const char* dunder =
        operator_dunder(op, std::meta::parameters_of(Fn).size());
    if constexpr (dunder == nullptr || !method_shape_bindable(Fn)) {
        return;
    } else {
        std::string extras = ", nb::is_operator()";
        if constexpr (is_inplace_operator(op))
            extras += ", nb::rv_policy::reference";
        std::string callee = "operator";
        callee += std::meta::symbol_of(op);
        append_method<Fn>(out, cls_spell, dunder, callee, "", extras);
    }
}

// Mirrors reflect_bind_conversion: fixed concrete lambda return types, cast
// from the explicitly-named conversion.
template <std::meta::info Cls, std::meta::info Fn>
consteval void append_conversion(std::string& out, std::string_view cls_spell) {
    constexpr const char* d = conversion_dunder(Cls, Fn);
    if constexpr (d == nullptr) {
        return;
    } else {
        constexpr auto R = std::meta::return_type_of(Fn);
        std::string rs = type_spelling(R);
        if (rs.empty()) {
            append_skip_note(out, d);
            return;
        }
        const char* lret = std::meta::is_same_type(R, ^^bool) ? "bool"
                         : std::meta::is_integral_type(R)     ? "long long"
                                                              : "double";
        out += "    cls.def(";
        append_quoted(out, d);
        out += ", [](";
        out += cls_spell;
        out += " &self) -> ";
        out += lret;
        out += " { return (";
        out += lret;
        out += ") self.operator " + rs + "(); });\n";
    }
}

// --- Free operators (forward + reversed dunders) and __str__ ---

template <std::meta::info Cls, std::meta::info Fn>
consteval void append_free_operator(std::string& out) {
    constexpr free_op_plan plan = plan_free_operator(Cls, Fn);
    if constexpr (plan.fwd_dunder == nullptr && plan.rev_dunder == nullptr) {
        return;
    } else {
        std::string callee;
        std::string ret = type_spelling(std::meta::return_type_of(Fn));
        param_text pt = params_plain(Fn);
        if (!append_free_fn_callee(callee, Fn) || ret.empty() || !pt.ok) {
            append_skip_note(out, "a free operator");
            return;
        }
        if constexpr (plan.fwd_dunder != nullptr) {
            out += "    cls.def(";
            append_quoted(out, plan.fwd_dunder);
            out += ", [](" + pt.decls + ") -> " + ret + " { return " + callee
                 + "(" + pt.args + "); }, nb::is_operator()";
            if constexpr (plan.inplace)
                out += ", nb::rv_policy::reference";
            out += ");\n";
        }
        if constexpr (plan.rev_dunder != nullptr) {
            // Reversed: swapped lambda parameters, same call order.
            auto params = std::define_static_array(std::meta::parameters_of(Fn));
            std::string p0 = type_spelling(std::meta::type_of(params[0]));
            std::string p1 = type_spelling(std::meta::type_of(params[1]));
            out += "    cls.def(";
            append_quoted(out, plan.rev_dunder);
            out += ", [](" + p1 + " a1, " + p0 + " a0) -> " + ret
                 + " { return " + callee + "(a0, a1); }, nb::is_operator());\n";
        }
    }
}

template <std::meta::info Cls, std::meta::info... Rs>
consteval void append_class_free_operators(std::string& out) {
    constexpr auto scope = std::meta::parent_of(Cls);
    if constexpr (std::meta::is_namespace(scope)) {
        template for (constexpr auto fn : std::define_static_array(
                          namespace_members_for_binding(scope))) {
            if constexpr (is_bindable_free_operator<fn>()
                          && !has_ann<fn, reflect::skip>()
                          && !fn_mentions_excluded(fn, excluded_v<Rs...>)) {
                append_free_operator<Cls, fn>(out);
            }
        };
    }
}

// Mirrors bind_stream_str: the streamability probe is EMITTED so the
// production compiler answers it. Both the probe and the lambda are spelled
// in terms of the bind function's dependent `Self` (= the class): a discarded
// if-constexpr branch skips instantiation, but non-dependent constructs in it
// are still validity-checked at template definition time.
consteval void append_stream_str(std::string& out) {
    out += "    if constexpr (requires(::std::ostream &os, const Self &t) "
           "{ os << t; })\n"
           "        cls.def(\"__str__\", [](const Self &self) { "
           "::std::ostringstream oss; oss << self; return oss.str(); });\n";
}

// --- Properties ---

// An overload-exact member-pointer cast: compiles only if the spelled
// signature names an existing overload exactly (also the round-trip oracle
// the spelling probe uses). Returns false when unspellable.
consteval bool append_memfn_ptr_cast(std::string& out,
                                     std::string_view owner_spell,
                                     std::meta::info fn) {
    auto ft = std::meta::type_of(fn);
    std::string ret = type_spelling(std::meta::return_type_of(ft));
    std::string tail = spell_fn_tail(ft);
    if (ret.empty() || tail.empty())
        return false;
    out += "static_cast<" + ret + " (";
    out += owner_spell;
    out += "::*)" + tail + ">(&";
    out += owner_spell;
    out += "::";
    out += std::meta::identifier_of(fn);
    out += ")";
    return true;
}

// The setter paired with property getter `Getter` (mirrors
// find_property_setter, with the class as a reflection).
template <std::meta::info Cls, std::meta::info Getter>
consteval std::meta::info emit_find_setter() {
    constexpr std::string_view gname = prop_name<Getter>();
    std::meta::info found = ^^void;
    template for (constexpr auto fn : std::define_static_array(
                      std::meta::members_of(
                          Cls, std::meta::access_context::unchecked()))) {
        if constexpr (is_property_setter<fn>() && !std::meta::is_deleted(fn)) {
            if (std::string_view(prop_name<fn>()) == gname)
                found = fn;
        }
    };
    return found;
}

template <std::meta::info Cls, std::meta::info Getter>
consteval void append_property(std::string& out, std::string_view cls_spell) {
    constexpr auto name = prop_name<Getter>();
    constexpr auto setter = emit_find_setter<Cls, Getter>();
    std::string body;
    if (!append_memfn_ptr_cast(body, cls_spell, Getter)) {
        append_skip_note(out, name);
        return;
    }
    if constexpr (setter != ^^void) {
        body += ", ";
        if (!append_memfn_ptr_cast(body, cls_spell, setter)) {
            append_skip_note(out, name);
            return;
        }
    }
    out += (setter == ^^void ? "    cls.def_prop_ro(" : "    cls.def_prop_rw(");
    append_quoted(out, name);
    out += ", " + body;
    append_data_extras<Getter>(out);
    out += ");\n";
}

// --- Member function routing (mirrors reflect_bind_member_function) ---

template <std::meta::info Cls, std::meta::info Fn>
consteval void append_member_function(std::string& out,
                                      std::string_view cls_spell,
                                      std::string_view call_prefix) {
    constexpr member_fn_route route = classify_member_fn(Cls, Fn);
    if constexpr (route == member_fn_route::oper) {
        append_member_operator<Fn>(out, cls_spell);
    } else if constexpr (route == member_fn_route::conversion) {
        append_conversion<Cls, Fn>(out, cls_spell);
    } else if constexpr (route == member_fn_route::static_method) {
        // Statics call through the DECLARING class (the splice's exactness).
        std::string callee = type_spelling(std::meta::parent_of(Fn));
        if (callee.empty()) {
            append_skip_note(out, entity_name<Fn>());
            return;
        }
        callee += "::";
        callee += std::meta::identifier_of(Fn);
        append_static_method<Fn>(out, entity_name<Fn>(), callee);
    } else if constexpr (route == member_fn_route::method) {
        if constexpr (method_shape_bindable(Fn)) {  // the binder-matrix gate
            std::string extras;
            append_call_extras<Fn>(extras);
            append_method<Fn>(out, cls_spell, entity_name<Fn>(),
                              std::meta::identifier_of(Fn), call_prefix,
                              extras);
        }
    }
}

// --- Member function templates (default instantiation) ---

template <std::meta::info Cls, std::meta::info Tmpl>
consteval void append_member_template(std::string& out,
                                      std::string_view cls_spell,
                                      std::string_view call_prefix) {
    constexpr member_tmpl_route route = classify_member_template(Cls, Tmpl);
    if constexpr (route == member_tmpl_route::skip) {
        return;
    } else {
        constexpr auto spec = default_spec(Tmpl);
        if constexpr (route == member_tmpl_route::oper) {
            // Natural operator-call syntax performs overload resolution over
            // the template and its siblings with the spec's declared argument
            // types -- SFINAE-false pack siblings (TC-0004's shape) drop out,
            // selecting the same instantiation the splice names.
            append_member_operator<spec>(out, cls_spell);
        } else if constexpr (route == member_tmpl_route::static_method) {
            std::string callee = type_spelling(std::meta::parent_of(spec));
            if (callee.empty()) {
                append_skip_note(out, std::meta::identifier_of(Tmpl));
                return;
            }
            callee += "::template ";
            callee += std::meta::identifier_of(Tmpl);
            callee += "<>";
            append_static_method<spec>(
                out, std::meta::identifier_of(Tmpl), callee);
        } else {
            // self.[prefix::]template f<>(args): the explicit (empty)
            // argument list forces the template; deduction against the spec's
            // declared parameter types re-derives the default instantiation.
            std::string callee = "template ";
            callee += std::meta::identifier_of(Tmpl);
            callee += "<>";
            std::string extras;
            append_call_extras<spec>(extras);
            append_method<spec>(out, cls_spell,
                                std::meta::identifier_of(Tmpl), callee,
                                call_prefix, extras);
        }
    }
}

// --- Entity proxies (using-redeclarations) ---

template <std::meta::info Cls, std::meta::info Proxy>
consteval void append_proxy(std::string& out, std::string_view cls_spell) {
    constexpr proxy_route route = classify_proxy(Cls, Proxy);
    if constexpr (route == proxy_route::skip) {
        return;
    } else {
        constexpr auto u = proxy_underlying(Proxy);
        if constexpr (route == proxy_route::oper) {
            constexpr const char* d = operator_dunder(
                std::meta::operator_of(u),
                std::meta::parameters_of(u).size());
            if constexpr (d != nullptr) {
                // Unqualified call through the using-declared name: the ONLY
                // valid spelling for a private-base re-export.
                std::string callee = "operator";
                callee += std::meta::symbol_of(std::meta::operator_of(u));
                append_method<u>(out, cls_spell, d, callee, "",
                                 ", nb::is_operator()");
            }
        } else if constexpr (route == proxy_route::static_method) {
            // The underlying entity is callable directly (its class is
            // public-in-itself even when a private base of Cls).
            std::string callee = type_spelling(std::meta::parent_of(u));
            if (callee.empty()) {
                append_skip_note(out, entity_name<u>());
                return;
            }
            callee += "::";
            callee += std::meta::identifier_of(u);
            append_static_method<u>(out, entity_name<u>(), callee);
        } else {
            std::string extras;
            append_call_extras<u>(extras);
            append_method<u>(out, cls_spell, entity_name<u>(),
                             std::meta::identifier_of(u), "", extras);
        }
    }
}

// --- Class contents (mirrors bind_class_contents) ---

template <std::meta::info Cls, bool HasTramp, std::meta::info... Rs>
consteval void append_class_contents(std::string& out,
                                     std::string_view cls_spell) {
    // Constructors (class_constructs / ctor_binds hold the BINDER-0011/0012
    // rationale).
    if constexpr (class_constructs(Cls, HasTramp)) {
        template for (constexpr auto fn : std::define_static_array(
                          std::meta::members_of(
                              Cls, std::meta::access_context::unchecked()))) {
            if constexpr (ctor_binds(fn, excluded_v<Rs...>))
                append_ctor<fn>(out);
        };
    }

    // Python-side COPY construction (BINDER-0013; rationale on binds_copy_ctor).
    if constexpr (binds_copy_ctor(Cls, HasTramp)) {
        out += "    cls.def(::nanobind::detail::reflect_init<const ";
        out += cls_spell;
        out += " &>());\n";
    }

    // Data members.
    template for (constexpr auto mem : std::define_static_array(
                      std::meta::nonstatic_data_members_of(
                          Cls, std::meta::access_context::unchecked()))) {
        if constexpr (std::meta::is_public(mem)
                      && std::meta::has_identifier(mem)
                      && !data_member_excluded(mem, excluded_v<Rs...>))
            append_data_member<mem>(out, cls_spell);
    };

    // Static data members.
    template for (constexpr auto mem : std::define_static_array(
                      std::meta::static_data_members_of(
                          Cls, std::meta::access_context::unchecked()))) {
        if constexpr (std::meta::is_public(mem)
                      && std::meta::has_identifier(mem)
                      && !data_member_excluded(mem, excluded_v<Rs...>))
            append_static_member<mem>(out, cls_spell);
    };

    // Methods / member templates / proxies (one shared kind-router).
    template for (constexpr auto fn : std::define_static_array(
                      std::meta::members_of(
                          Cls, std::meta::access_context::unchecked()))) {
        constexpr class_member_kind kind =
            classify_class_member(fn, excluded_v<Rs...>);
        if constexpr (kind == class_member_kind::fn)
            append_member_function<Cls, fn>(out, cls_spell, "");
        else if constexpr (kind == class_member_kind::tmpl)
            append_member_template<Cls, fn>(out, cls_spell, "");
        else if constexpr (kind == class_member_kind::proxy)
            append_proxy<Cls, fn>(out, cls_spell);
    };

    // Properties.
    template for (constexpr auto fn : std::define_static_array(
                      std::meta::members_of(
                          Cls, std::meta::access_context::unchecked()))) {
        if constexpr (classify_class_member(fn, excluded_v<Rs...>)
                      == class_member_kind::fn) {
            if constexpr (is_property_getter<fn>())
                append_property<Cls, fn>(out, cls_spell);
        }
    };
}

// --- Base flattening (mirrors flatten_base_members / flatten_unmodeled_bases) ---

template <std::meta::info Cls, std::meta::info Base, std::meta::info... Rs>
consteval void append_flatten_base(std::string& out,
                                   std::string_view cls_spell) {
    std::string base_spell = type_spelling(Base);
    if (base_spell.empty()) {
        append_skip_note(out, "a flattened base");
        return;
    }

    template for (constexpr auto mem : std::define_static_array(
                      std::meta::nonstatic_data_members_of(
                          Base, std::meta::access_context::unchecked()))) {
        if constexpr (std::meta::is_public(mem)
                      && std::meta::has_identifier(mem)
                      && !data_member_excluded(mem, excluded_v<Rs...>))
            // Pointer through the DECLARING base: int Base::* is what the
            // splice yields, and def_rw/def_ro accept it for the derived class.
            append_data_member<mem>(out, base_spell);
    };

    template for (constexpr auto mem : std::define_static_array(
                      std::meta::static_data_members_of(
                          Base, std::meta::access_context::unchecked()))) {
        if constexpr (std::meta::is_public(mem)
                      && std::meta::has_identifier(mem)
                      && !data_member_excluded(mem, excluded_v<Rs...>))
            append_static_member<mem>(out, base_spell);
    };

    // Qualified calls (self.::ns::Base::f(...)): name the exact member like
    // the splice does -- a same-named member of Cls cannot shadow it. The
    // `template` disambiguator for member templates is part of their callee.
    std::string call_prefix = base_spell + "::";
    template for (constexpr auto fn : std::define_static_array(
                      std::meta::members_of(
                          Base, std::meta::access_context::unchecked()))) {
        constexpr class_member_kind kind =
            classify_class_member(fn, excluded_v<Rs...>);
        if constexpr (kind == class_member_kind::fn)
            append_member_function<Cls, fn>(out, cls_spell, call_prefix);
        else if constexpr (kind == class_member_kind::tmpl)
            append_member_template<Cls, fn>(out, cls_spell, call_prefix);
        else if constexpr (kind == class_member_kind::proxy)
            append_proxy<Cls, fn>(out, cls_spell);
    };
}

template <std::meta::info Cls, std::meta::info... Rs>
consteval void append_flattened_bases(std::string& out,
                                      std::string_view cls_spell) {
    constexpr auto pybase = python_base_of(Cls, bind_set_v<Rs...>);
    template for (constexpr auto base : std::define_static_array(
                      flatten_bases_vec(Cls, pybase))) {
        if constexpr (!is_excluded_entity(base, excluded_v<Rs...>))
            append_flatten_base<Cls, base, Rs...>(out, cls_spell);
    };
}

// --- Trampolines (inline; subsumes the two-stage trampoline codegen) ---

// Whether emit mode gives Cls a trampoline. Trampolines are OPT-IN via pack
// markers so the two lanes stay surface-identical: ^^nb::trampoline_<^^Cls...>
// lists exact classes (the analogue of a hand-written NB_REFLECT_TRAMPOLINE),
// ^^nb::trampoline_all_ covers every class with overridable virtuals (the
// two-stage codegen tier's rule). Without a marker, no class is trampolined
// -- matching a constexpr run with no registrations: trampolining every
// virtual class unasked would CHANGE the surface (an abstract class would
// gain __init__, a trampolined copyable class would lose its copy ctor).
template <std::meta::info... Rs>
consteval bool emit_wants_trampoline(std::meta::info cls) {
    bool want = false;
    auto check = [&](std::meta::info r) {
        if (!std::meta::is_type(r))
            return;
        auto d = std::meta::dealias(r);
        if (d == ^^trampoline_all_) {
            want = true;
        } else if (std::meta::is_class_type(d)
                   && std::meta::has_template_arguments(d)
                   && std::meta::template_of(d) == ^^trampoline_) {
            for (auto a : std::meta::template_arguments_of(d))
                if (std::meta::dealias(std::meta::extract<std::meta::info>(a))
                    == cls)
                    want = true;
        }
    };
    (check(Rs), ...);
    return want && !codegen::overridable_virtuals(cls).empty();
}

// A requested trampoline is VIABLE when every pure virtual is spellable (an
// unoverridden pure virtual would make the trampoline abstract, breaking ctor
// binding) and at least one override can be generated. A non-pure unspellable
// virtual just loses Python overridability.
consteval bool emit_tramp_viable(std::meta::info cls) {
    bool any = false;
    for (auto m : codegen::overridable_virtuals(cls)) {
        bool ok = fn_signature_spellable(m);
        if (!ok && std::meta::is_pure_virtual(m))
            return false;
        any = any || ok;
    }
    return any;
}

// One spelled override (gen_override's text twin: type_spelling instead of
// codegen_member splices, NB_OVERRIDE[_PURE] body).
consteval void append_override_spelled(std::string& out, std::meta::info m) {
    if (!fn_signature_spellable(m))
        return;
    out += "    " + type_spelling(std::meta::return_type_of(m)) + " ";
    out += std::meta::identifier_of(m);
    out += "(";
    std::size_t j = 0;
    std::string call;
    for (auto p : std::meta::parameters_of(m)) {
        if (j)
            out += ", ";
        out += type_spelling(std::meta::type_of(p)) + " a" + spell_num(j);
        call += ", a" + spell_num(j);
        ++j;
    }
    out += ")";
    if (std::meta::is_const(m))
        out += " const";
    if (std::meta::is_noexcept(m))
        out += " noexcept";
    out += " override { ";
    out += std::meta::is_pure_virtual(m) ? "NB_OVERRIDE_PURE(" : "NB_OVERRIDE(";
    out += std::meta::identifier_of(m);
    out += call;
    out += "); }\n";
}

consteval void append_trampoline_struct(std::string& out,
                                        std::string_view cls_spell,
                                        std::string_view tramp_name,
                                        std::meta::info cls) {
    std::vector<std::meta::info> virts = codegen::overridable_virtuals(cls);
    std::string body;
    std::size_t n = 0;
    for (auto m : virts) {
        std::size_t before = body.size();
        append_override_spelled(body, m);
        if (body.size() != before)
            ++n;
    }
    out += "struct ";
    out += tramp_name;
    out += " : ";
    out += cls_spell;
    out += " {\n    NB_TRAMPOLINE(";
    out += cls_spell;
    out += ", " + spell_num(n) + ");\n";
    out += body;
    out += "};\n\n";
}

// --- Per-class / per-enum bind functions ---

// The reached-name rule (reached_entity_name's info-form): the resolved
// type's own name wins; an anonymous type falls back to the declaration it
// was reached through (typedef-for-linkage, BINDER-0018).
template <std::meta::info Cls, std::meta::info Named>
consteval const char* emit_reached_name() {
    constexpr const char* tn = entity_name<Cls>();
    if constexpr (tn != nullptr)
        return tn;
    else if constexpr (Named != ^^void) {
        if constexpr (std::meta::has_identifier(Named))
            return std::define_static_string(std::meta::identifier_of(Named));
        else
            return nullptr;
    } else {
        return nullptr;
    }
}

// The bind-function NAME for one class/enum occurrence, or "" when the entity
// is skipped (truly anonymous, or its type unspellable). Memoized below: a
// tiny per-entity constant evaluation.
template <std::meta::info EntR, std::meta::info Named>
consteval std::string emit_fname_text() {
    constexpr auto Ent = std::meta::dealias(EntR);
    constexpr const char* name = emit_reached_name<Ent, Named>();
    if constexpr (name == nullptr) {
        return {};
    } else {
        std::string spell = type_spelling(Ent);
        if (spell.empty())
            return {};
        std::string out = "nbgen_bind_";
        append_mangled(out, spell);
        return out;
    }
}

template <std::meta::info EntR, std::meta::info Named>
inline constexpr const char* emit_fname_v =
    std::define_static_string(emit_fname_text<EntR, Named>());

// The COMPLETE per-class definition text: opt-in trampoline struct + the
// templated bind function (idempotence guard, base-first call, BINDER-0022
// collision naming, contents, flattening, free operators, __str__). "" when
// skipped. Memoized below: each class renders in its OWN constant evaluation.
template <std::meta::info ClsR, std::meta::info Named, std::meta::info... Rs>
consteval std::string emit_class_def_text() {
    constexpr auto Cls = std::meta::dealias(ClsR);
    constexpr const char* name = emit_reached_name<Cls, Named>();
    if constexpr (name == nullptr) {
        return {};
    } else {
        std::string spell = type_spelling(Cls);
        if (spell.empty())
            return {};
        std::string out;
        out.reserve(8192);

        constexpr auto pybase = python_base_of(Cls, bind_set_v<Rs...>);
        constexpr bool has_tramp =
            emit_wants_trampoline<Rs...>(Cls) && emit_tramp_viable(Cls);

        std::string tramp_name;
        if constexpr (has_tramp) {
            tramp_name = "NbGenTramp_";
            append_mangled(tramp_name, spell);
            append_trampoline_struct(out, spell, tramp_name, Cls);
        }

        // The `Self` default lives on the forward DECLARATION (emit_class
        // emits it); restating it here would be a default-argument
        // redefinition.
        out += "template <class Self>\nstatic void nbgen_bind_";
        append_mangled(out, spell);
        out += "(nb::module_ &m) {\n";
        out += "    if (nb::type<" + spell + ">().is_valid()) return;\n";

        // Ensure the Python base is bound first (reflect_class's recursion).
        // The base's own bind function is emitted by the walk (every bind-set
        // member gets one); forward declarations at the top of the namespace
        // make the call well-formed regardless of definition order.
        if constexpr (pybase != ^^void) {
            constexpr const char* bf = emit_fname_v<pybase, ^^void>;
            if (bf[0] != '\0') {
                out += "    ";
                out += bf;
                out += "(m);\n";
            }
        }

        constexpr const char* qual = parent_qualified_name<Cls>(name);
        out += "    const char *py_name = nb::hasattr(m, ";
        append_quoted(out, name);
        out += ") ? ";
        append_quoted(out, qual);
        out += " : ";
        append_quoted(out, name);
        out += ";\n";

        out += "    auto cls = nb::class_<" + spell;
        if constexpr (pybase != ^^void) {
            std::string bspell = type_spelling(pybase);
            if (!bspell.empty())
                out += ", " + bspell;
        }
        if constexpr (has_tramp)
            out += ", " + tramp_name;
        out += ">(m, py_name";
        constexpr const char* doc = entity_doc<Cls>();
        if (doc != nullptr) {
            out += ", ";
            append_quoted(out, doc);
        }
        out += ");\n";

        append_class_contents<Cls, has_tramp, Rs...>(out, spell);
        append_flattened_bases<Cls, Rs...>(out, spell);
        append_class_free_operators<Cls, Rs...>(out);
        append_stream_str(out);
        out += "}\n\n";
        return out;
    }
}

template <std::meta::info ClsR, std::meta::info Named, std::meta::info... Rs>
inline constexpr std::span<const char* const> emit_class_def_v =
    make_static_chunks(emit_class_def_text<ClsR, Named, Rs...>());

template <std::meta::info EnumR, std::meta::info Named>
consteval std::string emit_enum_def_text() {
    constexpr auto E = std::meta::dealias(EnumR);
    constexpr const char* name = emit_reached_name<E, Named>();
    if constexpr (name == nullptr) {
        return {};
    } else {
        std::string spell = type_spelling(E);
        if (spell.empty())
            return {};
        std::string out;
        out.reserve(2048);
        out += "template <class Self>\nstatic void nbgen_bind_";
        append_mangled(out, spell);
        out += "(nb::module_ &m) {\n";
        out += "    if (nb::type<" + spell + ">().is_valid()) return;\n";
        constexpr const char* qual = parent_qualified_name<E>(name);
        out += "    const char *py_name = nb::hasattr(m, ";
        append_quoted(out, name);
        out += ") ? ";
        append_quoted(out, qual);
        out += " : ";
        append_quoted(out, name);
        out += ";\n";
        out += "    auto e = nb::enum_<" + spell + ">(m, py_name";
        constexpr const char* doc = entity_doc<E>();
        if (doc != nullptr) {
            out += ", ";
            append_quoted(out, doc);
        }
        out += ");\n";
        template for (constexpr auto val : std::define_static_array(
                          std::meta::enumerators_of(E))) {
            // Enum-name qualification is valid for scoped AND unscoped enums
            // (and through a typedef name).
            out += "    e.value(";
            append_quoted(out, std::meta::identifier_of(val));
            out += ", " + spell + "::";
            out += std::meta::identifier_of(val);
            out += ");\n";
        };
        out += "}\n\n";
        return out;
    }
}

template <std::meta::info EnumR, std::meta::info Named>
inline constexpr std::span<const char* const> emit_enum_def_v =
    make_static_chunks(emit_enum_def_text<EnumR, Named>());

template <std::meta::info Fn>
consteval std::string emit_free_fn_text() {
    std::string out;
    append_free_function<Fn>(out);
    return out;
}

template <std::meta::info Fn>
inline constexpr std::span<const char* const> emit_free_fn_v =
    make_static_chunks(emit_free_fn_text<Fn>());

// --- Dispatch (mirrors reflect_dispatch / reflect_user_specs / reflect_) ---
//
// The walk concatenates the precomputed per-entity texts (cheap pointer
// appends), deduplicates definitions by bind-function name across every seed
// path (a class reachable as both a discovered spec and a namespace member
// gets one definition; calls stay idempotent regardless), and accumulates the
// NB_MODULE body in reflect_'s order.

consteval bool str_in(const std::vector<std::string>& v, std::string_view x) {
    for (auto& e : v)
        if (e == x)
            return true;
    return false;
}

template <std::meta::info ClsR, std::meta::info Named, std::meta::info... Rs>
consteval void emit_class(std::vector<const char*>& defs, std::string& decls,
                          std::string& body, std::vector<std::string>& seen) {
    constexpr const char* fname = emit_fname_v<ClsR, Named>;
    if (fname[0] == '\0')
        return;  // anonymous / unspellable: skipped (mirrors reflect_class)
    body += "    nbgen::";
    body += fname;
    body += "(m);\n";
    if (str_in(seen, fname))
        return;
    seen.push_back(std::string(fname));
    constexpr auto Cls = std::meta::dealias(ClsR);
    std::string spell = type_spelling(Cls);
    decls += "template <class Self = " + spell + "> static void ";
    decls += fname;
    decls += "(nb::module_ &m);\n";
    for (const char* chunk : emit_class_def_v<ClsR, Named, Rs...>)
        defs.push_back(chunk);
}

template <std::meta::info EnumR, std::meta::info Named>
consteval void emit_enum(std::vector<const char*>& defs, std::string& decls,
                         std::string& body, std::vector<std::string>& seen) {
    constexpr const char* fname = emit_fname_v<EnumR, Named>;
    if (fname[0] == '\0')
        return;
    body += "    nbgen::";
    body += fname;
    body += "(m);\n";
    if (str_in(seen, fname))
        return;
    seen.push_back(std::string(fname));
    constexpr auto E = std::meta::dealias(EnumR);
    std::string spell = type_spelling(E);
    decls += "template <class Self = " + spell + "> static void ";
    decls += fname;
    decls += "(nb::module_ &m);\n";
    for (const char* chunk : emit_enum_def_v<EnumR, Named>)
        defs.push_back(chunk);
}

template <std::meta::info R, std::meta::info... Rs>
consteval void emit_dispatch(std::vector<const char*>& defs,
                             std::string& decls, std::string& body,
                             std::vector<std::string>& seen) {
    if constexpr (is_exclude_marker(R) || is_trampoline_marker(R)
                  || is_excluded_entity(R, excluded_v<Rs...>)) {
        // configuration / excluded seed: nothing
    } else if constexpr (std::meta::is_namespace(R)) {
        template for (constexpr auto mem : std::define_static_array(
                          namespace_members_for_binding(R))) {
            constexpr ns_member_kind kind =
                classify_namespace_member(mem, excluded_v<Rs...>);
            if constexpr (kind == ns_member_kind::cls)
                emit_class<mem, mem, Rs...>(defs, decls, body, seen);
            else if constexpr (kind == ns_member_kind::enum_)
                emit_enum<mem, mem>(defs, decls, body, seen);
            else if constexpr (kind == ns_member_kind::free_fn) {
                for (const char* chunk : emit_free_fn_v<mem>)
                    body += chunk;
            } else if constexpr (kind == ns_member_kind::ns)
                emit_dispatch<mem, Rs...>(defs, decls, body, seen);
        };
    } else if constexpr (std::meta::is_type(R)) {
        if constexpr (std::meta::is_class_type(R))
            emit_class<R, R, Rs...>(defs, decls, body, seen);
        else if constexpr (std::meta::is_enum_type(R))
            emit_enum<R, R>(defs, decls, body, seen);
    } else if constexpr (std::meta::is_function(R)) {
        for (const char* chunk : emit_free_fn_v<R>)
            body += chunk;
    }
}

template <std::meta::info R, std::meta::info... Rs>
consteval void emit_user_specs(std::vector<const char*>& defs,
                               std::string& decls, std::string& body,
                               std::vector<std::string>& seen) {
    template for (constexpr auto ty : std::define_static_array(
                      required_user_specs(R, excluded_v<Rs...>))) {
        emit_class<ty, ^^void, Rs...>(defs, decls, body, seen);
    };
}

NAMESPACE_END(emitgen)
NAMESPACE_END(detail)

/// Generate a COMPLETE binding TU for Rs... -- the same bind set, names,
/// overloads, base wiring, dunders, and annotations the constexpr backend
/// (nb::reflect_<Rs...>) produces -- as ordinary nanobind code a production
/// (non-reflection) toolchain compiles. `preamble` supplies the library
/// #includes (only the generator knows them); `module_name` names the
/// NB_MODULE. The TU is returned as a sequence of static text chunks
/// (concatenate in order, see write_bindings): per-entity text is rendered
/// and lifted to static storage in bounded pieces, never as one giant string
/// (consteval budgets, and define_static_string miscompiles >= 32K, TC-0018).
template <std::meta::info... Rs>
consteval std::span<const char* const> emit_bindings(const char* module_name,
                                                     const char* preamble) {
    std::vector<const char*> defs;
    std::string decls, body;
    std::vector<std::string> seen;
    // Mirror reflect_'s order: discovered template specializations first
    // (their members feed caster detection), then the dispatch walks.
    (detail::emitgen::emit_user_specs<Rs, Rs...>(defs, decls, body, seen), ...);
    (detail::emitgen::emit_dispatch<Rs, Rs...>(defs, decls, body, seen), ...);

    std::vector<const char*> parts;
    std::string head;
    head.reserve(4096 + decls.size());
    head += "// Generated by nanobind reflection emit backend -- do not edit.\n";
    head += preamble;
    head +=
        "\n"
        "#include <nanobind/nanobind.h>\n"
        "#include <nanobind/nb_paren_init.h>\n"
        "#include <nanobind/trampoline.h>\n"
        "#include <nanobind/stl/string.h>\n"
        "#include <ostream>\n"
        "#include <sstream>\n"
        "#include <utility>\n";
    ((head += detail::codegen::emit_stl_includes(Rs)), ...);
    head +=
        "\n"
        "namespace nb = nanobind;\n"
        "\n"
        "namespace nbgen {\n"
        "\n"
        "template <long double, class = void> struct value_probe;\n"
        "\n"
        "// Forward declarations (a derived class's bind function calls its\n"
        "// base's regardless of definition order).\n";
    head += decls;
    head += "\n";
    detail::emitgen::push_chunked(parts, head);
    for (const char* chunk : defs)
        parts.push_back(chunk);
    std::string tail = "} // namespace nbgen\n\nNB_MODULE(";
    tail += module_name;
    tail += ", m) {\n";
    tail += body;
    tail += "}\n";
    detail::emitgen::push_chunked(parts, tail);
    return std::define_static_array(parts);
}

/// Write generated binding source to `path`. For the generator program:
///   int main(int argc, char** argv) {
///       return nb::write_bindings(argv[1],
///                  nb::emit_bindings<^^my_ns>("my_ext",
///                                             "#include <my/lib.h>\n"))
///                  ? 0 : 1;
///   }
inline bool write_bindings(const char* path,
                           std::span<const char* const> chunks) {
    std::ofstream out(path);
    if (!out)
        return false;
    for (const char* c : chunks)
        out << c;
    return bool(out);
}

inline bool write_bindings(const char* path, const char* source) {
    std::ofstream out(path);
    if (!out)
        return false;
    out << source;
    return bool(out);
}

NAMESPACE_END(NB_NAMESPACE)

#endif // __has_include(<meta>)
