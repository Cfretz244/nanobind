/*
    nanobind/nb_reflect_emit.h: full source-codegen backend for the reflection
    binder.

    nb::write_bindings<Rs...>(path, module_name, preamble) walks EXACTLY the
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

    PERFORMANCE/LIMITS: per-entity text is memoized in its own constant
    evaluation (a fresh step budget per class, mirroring how the constexpr
    backend's per-class instantiations evaluate independently), lifted to
    static storage in bounded chunks (emit_item_chunk_v: consteval budgets,
    TC-0018's >=32K define_static_string miscompile, and the linker's
    symbol-length cap all forbid giant strings or pointer arrays), and
    streamed to the output file at the generator's RUNTIME. Renderers append
    into a shared std::string out-parameter (no temporary-chain churn:
    consteval string copies are interpreter-expensive).

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
#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <fstream>

NAMESPACE_BEGIN(NB_NAMESPACE)
NAMESPACE_BEGIN(detail)
NAMESPACE_BEGIN(emitgen)

// Static-text chunking, for two hard limits:
// - std::define_static_string silently MISCOMPILES for inputs >= 32768 chars
//   on the pinned toolchain: reflect_constant_string spells the whole string
//   as one NTTP pack, and each substituted element's PackIndex is a 15-bit
//   bitfield (SubstTemplateTypeParmType/SubstNonTypeTemplateParmExpr, clang
//   AST) that overflows -- "excess elements in array initializer" out of
//   FixedArray (TC-0018).
// - The backing FixedArray<char, ...> SYMBOL embeds every character at ~7
//   mangled bytes each, and Apple's linker caps symbol names (ld-prime
//   asserts in makeSymbolStringInPlace around 128K). 8K chunks keep the
//   worst-case array symbol near 57K. For the same reason chunk POINTERS are
//   never lifted into a define_static_array (its symbol would embed every
//   chunk's full mangled name); per-chunk variables keyed by small indices
//   carry them instead (emit_item_chunk_v below).
inline constexpr std::size_t emit_chunk_size = 8192;

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
// is skipped (truly anonymous, or its type unspellable).
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
            std::string bf = emit_fname_text<pybase, ^^void>();
            if (!bf.empty()) {
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


template <std::meta::info Fn>
consteval std::string emit_free_fn_text() {
    std::string out;
    append_free_function<Fn>(out);
    return out;
}


// --- The worklist (mirrors reflect_user_specs / reflect_dispatch order) ---
//
// The walks are VALUE-FORM (every classifier already is), producing one
// ordered list of (entity, reached-through, kind) items per pack. Each item's
// text is then memoized in a variable template keyed by INDEX into that list
// -- never by the entity reflection: a deep specialization's reflection NTTP
// mangling (nlohmann's detail types) blows past the linker's symbol-length
// cap (ld-prime asserts in makeSymbolStringInPlace). The heavy reflections
// only ever parameterize consteval FUNCTIONS, which produce no symbols.

enum class emit_kind { cls, enum_, free_fn };
struct emit_item {
    std::meta::info ent;
    std::meta::info named;
    emit_kind kind;
};

consteval void worklist_dispatch(std::meta::info R,
                                 std::span<const std::meta::info> ex,
                                 std::vector<emit_item>& out) {
    if (is_exclude_marker(R) || is_trampoline_marker(R)
        || is_excluded_entity(R, ex))
        return;
    if (std::meta::is_namespace(R)) {
        for (auto mem : namespace_members_for_binding(R)) {
            switch (classify_namespace_member(mem, ex)) {
            case ns_member_kind::cls:
                out.push_back({mem, mem, emit_kind::cls});
                break;
            case ns_member_kind::enum_:
                out.push_back({mem, mem, emit_kind::enum_});
                break;
            case ns_member_kind::free_fn:
                out.push_back({mem, ^^void, emit_kind::free_fn});
                break;
            case ns_member_kind::ns:
                worklist_dispatch(mem, ex, out);
                break;
            default:
                break;
            }
        }
    } else if (std::meta::is_type(R)) {
        if (std::meta::is_class_type(R))
            out.push_back({R, R, emit_kind::cls});
        else if (std::meta::is_enum_type(R))
            out.push_back({R, R, emit_kind::enum_});
    } else if (std::meta::is_function(R)) {
        out.push_back({R, ^^void, emit_kind::free_fn});
    }
}

template <std::meta::info... Rs>
consteval std::vector<emit_item> compute_emit_worklist() {
    std::vector<emit_item> out;
    std::vector<std::meta::info> ex = compute_excluded<Rs...>();
    // Mirror reflect_'s order: discovered template specializations first,
    // then the dispatch walks.
    auto specs = [&](std::meta::info R) {
        if (is_exclude_marker(R) || is_trampoline_marker(R))
            return;
        for (auto ty : required_user_specs(R, ex))
            out.push_back({ty, ^^void, emit_kind::cls});
    };
    (specs(Rs), ...);
    (worklist_dispatch(Rs, ex, out), ...);
    return out;
}

template <std::meta::info... Rs>
inline constexpr auto emit_worklist_v =
    std::define_static_array(compute_emit_worklist<Rs...>());

// Per-item memoized text, each in its OWN constant evaluation (a fresh step
// budget per entity, mirroring how the constexpr backend's per-class
// instantiations evaluate independently). The definition text is exposed as
// per-chunk const char* variables -- chunk count + J-indexed chunk -- so no
// symbol ever embeds more than one chunk's mangled characters.
template <std::size_t I, std::meta::info... Rs>
consteval std::string emit_item_text() {
    constexpr emit_item it = emit_worklist_v<Rs...>[I];
    if constexpr (it.kind == emit_kind::cls)
        return emit_class_def_text<it.ent, it.named, Rs...>();
    else if constexpr (it.kind == emit_kind::enum_)
        return emit_enum_def_text<it.ent, it.named>();
    else
        return emit_free_fn_text<it.ent>();
}

template <std::size_t I, std::meta::info... Rs>
inline constexpr std::size_t emit_item_nchunks_v =
    (emit_item_text<I, Rs...>().size() + emit_chunk_size - 1)
    / emit_chunk_size;

template <std::size_t I, std::size_t J, std::meta::info... Rs>
inline constexpr const char* emit_item_chunk_v = [] {
    // Named local: a nested-call temporary is not a constant expression when
    // read back by define_static_string (cf. spec_python_name).
    std::string s = emit_item_text<I, Rs...>();
    return std::define_static_string(
        std::string_view(s).substr(J * emit_chunk_size, emit_chunk_size));
}();

template <std::size_t I, std::meta::info... Rs>
inline constexpr const char* emit_item_fname_v = [] {
    constexpr emit_item it = emit_worklist_v<Rs...>[I];
    if constexpr (it.kind == emit_kind::free_fn)
        return std::define_static_string(std::string_view{});
    else
        return std::define_static_string(
            emit_fname_text<it.ent, it.named>());
}();

template <std::size_t I, std::meta::info... Rs>
inline constexpr const char* emit_item_decl_v = [] {
    constexpr emit_item it = emit_worklist_v<Rs...>[I];
    if constexpr (it.kind == emit_kind::free_fn) {
        return std::define_static_string(std::string_view{});
    } else {
        std::string fname = emit_fname_text<it.ent, it.named>();
        if (fname.empty())
            return std::define_static_string(std::string_view{});
        std::string s = "template <class Self = "
                      + type_spelling(std::meta::dealias(it.ent))
                      + "> static void " + fname + "(nb::module_ &m);\n";
        return std::define_static_string(s);
    }
}();

// The pack-dependent prologue piece (the auto-detected STL caster includes;
// small, one static string per seed).
template <std::meta::info R, std::meta::info... Rs>
inline constexpr const char* emit_stl_includes_v = std::define_static_string(
    codegen::emit_stl_includes(R, excluded_v<Rs...>));

consteval std::vector<std::size_t> iota_vec(std::size_t n) {
    std::vector<std::size_t> v;
    for (std::size_t i = 0; i < n; ++i)
        v.push_back(i);
    return v;
}

// ===== Spelling-probe TU (the hardening oracle) ==========================
//
// write_spelling_probe<Rs...> renders a SECOND generated TU asserting, in
// plain C++ with no nanobind headers, that every signature the emit backend
// SPELLS names the real entity exactly:
//   methods/operators/conversions -- overload-exact member-pointer casts
//     (`static_cast<Ret (Cls::*)(Args...) cv ref noexcept>(&Cls::f)`), the
//     same round-trip append_memfn_ptr_cast performs for properties;
//   static methods / free functions -- overload-exact function-pointer casts;
//   data members (incl. statics) -- `decltype(Cls::x)` identity asserts;
//   constructors -- a function-type alias over the spelled parameter list;
//   enums -- is_enum_v + a mention of every enumerator.
// A wrong spelling is a COMPILE error in this TU. Probed members mirror the
// binding TU's walks gate-for-gate; forms the binding TU does not spell
// through these constructs (entity proxies' unqualified calls, member
// templates' `self.template f<>`) are not probed.

// The probe's member-name text: named members, operator members
// ("operator+"), conversion functions ("operator <ret-spelling>").
consteval std::string probe_member_name(std::meta::info fn) {
    if (std::meta::has_identifier(fn))
        return std::string(std::meta::identifier_of(fn));
    if (std::meta::is_operator_function(fn)) {
        std::string s = "operator";
        s += std::meta::symbol_of(std::meta::operator_of(fn));
        return s;
    }
    if (std::meta::is_conversion_function(fn)) {
        std::string r = type_spelling(
            std::meta::return_type_of(std::meta::type_of(fn)));
        if (r.empty())
            return {};
        return "operator " + r;
    }
    return {};
}

// One overload-exact cast probe for a member or free function. Member
// pointers target the DECLARING class (an inherited member's pointer is what
// `&Base::f` yields, and it is exactly what the binding TU's qualified calls
// and def_rw pointers name).
template <std::meta::info Fn>
consteval void append_probe_fn(std::string& out, std::string_view tag,
                               std::size_t& n) {
    auto ft = std::meta::type_of(Fn);
    std::string name = probe_member_name(Fn);
    std::string ret = type_spelling(std::meta::return_type_of(ft));
    std::string tail = spell_fn_tail(ft);
    std::string owner = std::meta::parent_of(Fn) == ^^::
        ? std::string{}
        : (std::meta::is_namespace(std::meta::parent_of(Fn))
               ? spell_qualified(std::meta::parent_of(Fn))
               : type_spelling(std::meta::parent_of(Fn)));
    if (name.empty() || ret.empty() || tail.empty()
        || (owner.empty() && std::meta::parent_of(Fn) != ^^::))
        return;
    constexpr bool memptr = std::meta::is_class_member(Fn)
                            && !std::meta::is_static_member(Fn);
    out += "[[maybe_unused]] inline constexpr auto nbprobe_";
    out += tag;
    out += "_" + spell_num(n++) + " =\n    static_cast<" + ret + " (";
    if constexpr (memptr)
        out += owner + "::";
    out += "*)" + tail + ">(&" + owner + "::" + name + ");\n";
}

// decltype identity assert for a (static or non-static) data member.
template <std::meta::info Mem>
consteval void append_probe_data(std::string& out,
                                 std::string_view owner_spell) {
    std::string ty = type_spelling(std::meta::type_of(Mem));
    if (ty.empty())
        return;
    out += "static_assert(::std::is_same_v<decltype(";
    out += owner_spell;
    out += "::";
    out += std::meta::identifier_of(Mem);
    out += "), " + ty + ">);\n";
}

// Function-type alias over a bound constructor's spelled parameter list
// (type_of is not valid on a constructor reflection; walk parameters_of
// like append_ctor does).
template <std::meta::info Fn>
consteval void append_probe_ctor(std::string& out, std::string_view tag,
                                 std::size_t& n) {
    if (!fn_signature_spellable(Fn))
        return;
    std::string params;
    bool first = true;
    for (auto p : std::meta::parameters_of(Fn)) {
        if (!first)
            params += ", ";
        first = false;
        std::string ty = type_spelling(std::meta::type_of(p));
        if (ty.empty())
            return;
        params += ty;
    }
    out += "using nbprobe_" + std::string(tag) + "_" + spell_num(n++)
         + " = void (" + params + ");\n";
}

// The probeable member functions of `Owner`, with `Cls` as the routing
// context (mirrors append_class_contents / append_flatten_base gates).
template <std::meta::info Cls, std::meta::info Owner, std::meta::info... Rs>
consteval void probe_member_fns(std::string& out, std::string_view tag,
                                std::size_t& n) {
    template for (constexpr auto fn : std::define_static_array(
                      std::meta::members_of(
                          Owner, std::meta::access_context::unchecked()))) {
        if constexpr (classify_class_member(fn, excluded_v<Rs...>)
                      == class_member_kind::fn) {
            constexpr member_fn_route route = classify_member_fn(Cls, fn);
            if constexpr (route == member_fn_route::method
                          || route == member_fn_route::oper
                          || route == member_fn_route::conversion
                          || route == member_fn_route::static_method) {
                if constexpr (method_shape_bindable(fn)
                              && !std::meta::is_deleted(fn)
                              && fn_signature_spellable(fn))
                    append_probe_fn<fn>(out, tag, n);
            }
        }
    };
}

template <std::meta::info Owner, std::meta::info... Rs>
consteval void probe_data_members(std::string& out,
                                  std::string_view owner_spell) {
    template for (constexpr auto mem : std::define_static_array(
                      std::meta::nonstatic_data_members_of(
                          Owner, std::meta::access_context::unchecked()))) {
        if constexpr (std::meta::is_public(mem)
                      && std::meta::has_identifier(mem)
                      && !data_member_excluded(mem, excluded_v<Rs...>))
            append_probe_data<mem>(out, owner_spell);
    };
    template for (constexpr auto mem : std::define_static_array(
                      std::meta::static_data_members_of(
                          Owner, std::meta::access_context::unchecked()))) {
        if constexpr (std::meta::is_public(mem)
                      && std::meta::has_identifier(mem)
                      && !data_member_excluded(mem, excluded_v<Rs...>))
            append_probe_data<mem>(out, owner_spell);
    };
}

template <std::meta::info Cls, std::meta::info... Rs>
consteval std::string probe_class_text(std::string_view tag) {
    std::string spell = type_spelling(std::meta::dealias(Cls));
    std::string out;
    if (spell.empty())
        return out;
    out += "// probe " + spell + "\n";
    out += "static_assert(::std::is_class_v<" + spell + ">);\n";
    std::size_t n = 0;

    constexpr bool has_tramp = emit_wants_trampoline<Rs...>(Cls);
    if constexpr (class_constructs(Cls, has_tramp)) {
        template for (constexpr auto fn : std::define_static_array(
                          std::meta::members_of(
                              Cls, std::meta::access_context::unchecked()))) {
            if constexpr (ctor_binds(fn, excluded_v<Rs...>))
                append_probe_ctor<fn>(out, tag, n);
        };
    }

    probe_data_members<Cls, Rs...>(out, spell);
    probe_member_fns<Cls, Cls, Rs...>(out, tag, n);

    // Flattened bases: their spellings appear in the binding TU as
    // qualified calls and declaring-class member pointers.
    constexpr auto pybase = python_base_of(Cls, bind_set_v<Rs...>);
    template for (constexpr auto base : std::define_static_array(
                      flatten_bases_vec(Cls, pybase))) {
        if constexpr (!is_excluded_entity(base, excluded_v<Rs...>)) {
            constexpr std::string_view bspell =
                std::define_static_string(type_spelling(base));
            if constexpr (!bspell.empty()) {
                probe_data_members<base, Rs...>(out, bspell);
                probe_member_fns<Cls, base, Rs...>(out, tag, n);
            }
        }
    };
    return out;
}

template <std::meta::info EnumR>
consteval std::string probe_enum_text() {
    constexpr auto E = std::meta::dealias(EnumR);
    std::string out;
    if constexpr (!std::meta::is_enumerable_type(E)) {
        return out;
    } else {
        std::string spell = type_spelling(E);
        if (spell.empty())
            return out;
        out += "// probe enum " + spell + "\n";
        out += "static_assert(::std::is_enum_v<" + spell + ">);\n";
        template for (constexpr auto e : std::define_static_array(
                          std::meta::enumerators_of(E))) {
            out += "static_assert((static_cast<void>(";
            out += spell;
            out += "::";
            out += std::meta::identifier_of(e);
            out += "), true));\n";
        };
        return out;
    }
}

// Per-item probe text, memoized in the same bounded-chunk shape as the
// binding TU's text (same constraints: consteval budgets, TC-0018, the
// linker's symbol-length cap).
template <std::size_t I, std::meta::info... Rs>
consteval std::string probe_item_text() {
    constexpr emit_item it = emit_worklist_v<Rs...>[I];
    if constexpr (it.kind == emit_kind::cls)
        return probe_class_text<it.ent, Rs...>("i" + spell_num(I));
    else if constexpr (it.kind == emit_kind::enum_)
        return probe_enum_text<it.ent>();
    else {
        std::string out;
        std::size_t n = 0;
        if (!std::meta::is_deleted(it.ent) && fn_signature_spellable(it.ent))
            append_probe_fn<it.ent>(out, "i" + spell_num(I), n);
        return out;
    }
}

template <std::size_t I, std::meta::info... Rs>
inline constexpr std::size_t probe_item_nchunks_v =
    (probe_item_text<I, Rs...>().size() + emit_chunk_size - 1)
    / emit_chunk_size;

template <std::size_t I, std::size_t J, std::meta::info... Rs>
inline constexpr const char* probe_item_chunk_v = [] {
    std::string s = probe_item_text<I, Rs...>();
    return std::define_static_string(
        std::string_view(s).substr(J * emit_chunk_size, emit_chunk_size));
}();

NAMESPACE_END(emitgen)
NAMESPACE_END(detail)

/// Generate a COMPLETE binding TU for Rs... -- the same bind set, names,
/// overloads, base wiring, dunders, and annotations the constexpr backend
/// (nb::reflect_<Rs...>) produces -- as ordinary nanobind code a production
/// (non-reflection) toolchain compiles -- and write it to `path`. `preamble`
/// supplies the library #includes (only the generator knows them);
/// `module_name` names the NB_MODULE.
///
/// All reflection work happens at COMPILE time of the caller: per-entity text
/// is memoized in bounded static chunks (emit_item_chunk_v) -- never one
/// giant string or pointer array, which consteval budgets, the
/// define_static_string >=32K miscompile (TC-0018), and the linker's
/// symbol-length cap (mangled char packs / pointee names) all forbid. This
/// function streams the precomputed constants to the file at runtime,
/// deduplicating definitions by bind-function name (a class reachable as
/// both a discovered spec and a namespace member gets one definition; the
/// body calls stay idempotent regardless).
///
///   int main(int argc, char** argv) {
///       return nb::write_bindings<^^my_ns>(
///                  argv[1], "my_ext", "#include \"my/lib.h\"\n") ? 0 : 1;
///   }
template <std::meta::info... Rs>
bool write_bindings(const char* path, const char* module_name,
                    const char* preamble) {
    namespace eg = detail::emitgen;
    std::ofstream out(path);
    if (!out)
        return false;

    out << "// Generated by nanobind reflection emit backend -- do not edit.\n";
    out << preamble;
    out << "\n"
           "#include <nanobind/nanobind.h>\n"
           "#include <nanobind/nb_paren_init.h>\n"
           "#include <nanobind/trampoline.h>\n"
           "#include <nanobind/stl/string.h>\n"
           "#include <ostream>\n"
           "#include <sstream>\n"
           "#include <utility>\n";
    ((out << eg::emit_stl_includes_v<Rs, Rs...>), ...);
    out << "\n"
           "namespace nb = nanobind;\n"
           "\n"
           "namespace nbgen {\n"
           "\n"
           "template <long double, class = void> struct value_probe;\n"
           "\n"
           "// Forward declarations (a derived class's bind function calls\n"
           "// its base's regardless of definition order).\n";

    constexpr auto indices = std::define_static_array(
        eg::iota_vec(eg::emit_worklist_v<Rs...>.size()));

    // Pass 1: forward declarations (deduplicated like the definitions).
    {
        std::vector<std::string> seen;
        template for (constexpr auto I : indices) {
            constexpr eg::emit_item it = eg::emit_worklist_v<Rs...>[I];
            if constexpr (it.kind != eg::emit_kind::free_fn) {
                constexpr const char* fname = eg::emit_item_fname_v<I, Rs...>;
                if constexpr (fname[0] != '\0') {
                    if (std::find(seen.begin(), seen.end(), fname)
                        == seen.end()) {
                        seen.push_back(fname);
                        out << eg::emit_item_decl_v<I, Rs...>;
                    }
                }
            }
        };
    }
    out << "\n";

    // Pass 2: definitions (same dedup) + the NB_MODULE body accumulated in
    // reflect_'s walk order.
    std::string body;
    {
        std::vector<std::string> seen;
        template for (constexpr auto I : indices) {
            constexpr eg::emit_item it = eg::emit_worklist_v<Rs...>[I];
            if constexpr (it.kind == eg::emit_kind::free_fn) {
                template for (constexpr auto J : std::define_static_array(
                                  eg::iota_vec(
                                      eg::emit_item_nchunks_v<I, Rs...>))) {
                    body += eg::emit_item_chunk_v<I, J, Rs...>;
                };
            } else {
                constexpr const char* fname = eg::emit_item_fname_v<I, Rs...>;
                if constexpr (fname[0] != '\0') {
                    body += "    nbgen::";
                    body += fname;
                    body += "(m);\n";
                    if (std::find(seen.begin(), seen.end(), fname)
                        == seen.end()) {
                        seen.push_back(fname);
                        template for (constexpr auto J : std::define_static_array(
                                          eg::iota_vec(
                                              eg::emit_item_nchunks_v<I, Rs...>))) {
                            out << eg::emit_item_chunk_v<I, J, Rs...>;
                        };
                    }
                }
            }
        };
    }

    out << "} // namespace nbgen\n\nNB_MODULE(" << module_name << ", m) {\n"
        << body << "}\n";
    return bool(out);
}

/// Generate the SPELLING-PROBE TU for Rs... and write it to `path`: a plain
/// C++ source (no nanobind, no reflection) that re-states every signature
/// the emit backend spells as an overload-exact cast / decltype identity /
/// enumerator mention, so a wrong spelling in the binding TU is ALSO a
/// compile error here -- against the library headers alone, with the
/// production compiler. `preamble` supplies the library #includes (same one
/// passed to write_bindings).
///
///   nb::write_spelling_probe<CORPUS_REFLECT_ARGS>(
///       "spelling_probe.gen.cpp", "#include \"binding_includes.h\"\n");
template <std::meta::info... Rs>
bool write_spelling_probe(const char* path, const char* preamble) {
    namespace eg = detail::emitgen;
    std::ofstream out(path);
    if (!out)
        return false;

    out << "// Generated by nanobind reflection emit backend (spelling "
           "probe) -- do not edit.\n";
    out << preamble;
    out << "\n#include <type_traits>\n\n";

    constexpr auto indices = std::define_static_array(
        eg::iota_vec(eg::emit_worklist_v<Rs...>.size()));
    template for (constexpr auto I : indices) {
        template for (constexpr auto J : std::define_static_array(
                          eg::iota_vec(eg::probe_item_nchunks_v<I, Rs...>))) {
            out << eg::probe_item_chunk_v<I, J, Rs...>;
        };
    };
    return bool(out);
}

NAMESPACE_END(NB_NAMESPACE)

#endif // __has_include(<meta>)
