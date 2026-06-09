/*
    nanobind/nb_reflect.h: Automatic binding via C++26 static reflection (P2996)

    Requires a compiler with P2996 support (e.g. GCC 16+, Bloomberg clang-p2996)

    Copyright (c) 2025 Matthew Kolbe

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE file.
*/

#pragma once

#if __has_include(<meta>)

#include <meta>
#include "nanobind.h"
#include "nb_reflect_annotations.h"
#include "stl/string.h"
#include <sstream>          // std::ostringstream for streamable -> __str__ (bind_stream_str)
#include <string_view>
#include <type_traits>
#include <vector>

NAMESPACE_BEGIN(NB_NAMESPACE)
NAMESPACE_BEGIN(detail)

// --- Trampoline hook ---
//
// A trampoline (a class derived from T that overrides T's virtuals to forward
// into Python) cannot be synthesized in-language on this fork, so reflect_ never
// generates one itself. Instead it consults this trait: if a trampoline type is
// registered for T, reflect_class passes it to nanobind as the class_ "Alias"
// (enabling Python subclasses to override C++ virtuals). The trampoline may be
// hand-written or produced by the codegen fallback (nb_reflect_codegen.h); both
// register it the same way, via NB_REFLECT_TRAMPOLINE.
template <typename T> struct reflect_trampoline { using type = void; };
template <typename T> using reflect_trampoline_t = typename reflect_trampoline<T>::type;
template <typename T>
inline constexpr bool has_reflect_trampoline =
    !std::is_same_v<reflect_trampoline_t<T>, void>;

// Returns the K-th member reflection of class `Owner`. Generated trampoline code
// (nb_reflect_codegen.h) uses this to refer to a specific virtual method by index
// so that its return/parameter types can be spliced (typename [:type_of(...):])
// without spelling them as text -- which also resolves overloaded virtuals to the
// exact overload.
template <std::meta::info Owner, std::size_t K>
consteval std::meta::info codegen_member() {
    return std::meta::members_of(Owner, std::meta::access_context::unchecked())[K];
}

// --- Annotation reading (see nb_reflect_annotations.h for the vocabulary) ---

// True if entity R carries an annotation of (non-template) type A.
template <std::meta::info R, typename A>
consteval bool has_ann() {
    return !std::meta::annotations_of(R, ^^A).empty();
}

// Value of R's first annotation of type A (precondition: has_ann<R, A>()).
template <std::meta::info R, typename A>
consteval A get_ann() {
    return [: std::meta::constant_of(std::meta::annotations_of(R, ^^A)[0]) :];
}

// rename/doc are templates (reflect::rename<N>), so they are matched by template,
// not by exact type. Returns the stored string lifted to static storage, or the
// fallback if absent.
template <std::meta::info R, std::meta::info Tmpl>
consteval const char* ann_string_or(const char* fallback) {
    template for (constexpr auto ann :
                  std::define_static_array(std::meta::annotations_of(R))) {
        constexpr auto t = std::meta::type_of(ann);
        if constexpr (std::meta::has_template_arguments(t) &&
                      std::meta::template_of(t) == Tmpl) {
            constexpr auto v = [: std::meta::constant_of(ann) :];
            return std::define_static_string(
                std::string_view(v.str.data, sizeof(v.str.data) - 1));
        }
    }
    return fallback;
}

// --- Template-specialization naming (see spec_camel_name) ---

// Defined later (with the stl caster machinery); used by spec_camel_name to skip
// container "policy" args (allocator/comparator/...) and to name std string types.
consteval bool is_stl_policy(std::meta::info type);
consteval bool is_in_std(std::meta::info e);

// Uppercase the first ASCII letter of s (used to CamelCase each appended arg).
consteval std::string capitalize_first(std::string s) {
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z')
        s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

// Reduce an arbitrary type/value spelling to an identifier fragment: drop every
// char outside [A-Za-z0-9_]. (Used only for fragments appended after a base name
// that already begins with a letter, so a leading digit here is harmless.)
consteval std::string sanitize_identifier(std::string_view in) {
    std::string s;
    for (char c : in)
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_')
            s += c;
    return s;
}

// Build a CamelCase Python name for a class/function template specialization:
// the template's base identifier followed by each template argument, capitalized
// and concatenated. Type args that are themselves specializations recurse
// (Box<Bar<int>> -> "BoxBarInt", Box<vector<int>> -> "BoxVectorInt"); plain named
// types use their identifier (int -> "Int"); non-type args render their value
// (Array<int,3> -> "ArrayInt3"). std::string/std::wstring/std::string_view get the
// friendly names String/WString/StringView. The base keeps its original case, so
// free function templates stay lowercase (identity<int> -> "identityInt").
// Container policy args (allocator/comparator/...) are skipped, matching the stl walk.
consteval std::string spec_camel_name(std::meta::info type) {
    // `type` may be a class-template specialization OR a function-template
    // specialization (identity<int>); remove_cvref is only valid on a type.
    if (std::meta::is_type(type))
        type = std::meta::remove_cvref(type);
    auto tmpl = std::meta::template_of(type);
    std::string_view base = std::meta::identifier_of(tmpl);
    if (is_in_std(tmpl)) {
        if (base == "basic_string") {
            auto args = std::meta::template_arguments_of(type);
            return (!args.empty() && args[0] == ^^wchar_t) ? "WString" : "String";
        }
        if (base == "basic_string_view") return "StringView";
    }
    std::string out(base);
    for (auto arg : std::meta::template_arguments_of(type)) {
        if (std::meta::is_type(arg)) {
            auto a = std::meta::remove_cvref(arg);
            if (std::meta::has_template_arguments(a)) {
                if (is_stl_policy(a))
                    continue;                            // allocator/comparator/...
                out += capitalize_first(spec_camel_name(a));
            } else if (std::meta::has_identifier(a)) {
                out += capitalize_first(std::string(std::meta::identifier_of(a)));
            } else {
                out += capitalize_first(
                    sanitize_identifier(std::meta::display_string_of(a)));
            }
        } else {
            out += capitalize_first(
                sanitize_identifier(std::meta::display_string_of(arg)));
        }
    }
    return out;
}

// spec_camel_name lifted to static storage. Goes through a named local so the
// (possibly heap-backed, long) std::string is a constant expression when read by
// define_static_string -- passing the nested-call temporary directly is not
// (cf. stl_missing_caster_msg, which uses the same named-local pattern).
consteval const char* spec_python_name(std::meta::info type) {
    std::string s = spec_camel_name(type);
    return std::define_static_string(s);
}

// Python name for R: an explicit reflect::rename, else its C++ identifier. For a
// template specialization, the CamelCase spec name (spec_camel_name): identifier_of
// is ill-formed on a specialization, and a single rename could not disambiguate two
// instantiations anyway, so rename is not consulted for specializations.
template <std::meta::info R>
consteval const char* entity_name() {
    if constexpr (std::meta::has_template_arguments(R))
        return spec_python_name(R);
    else
        return ann_string_or<R, ^^reflect::rename>(
            std::define_static_string(std::meta::identifier_of(R)));
}

// Docstring for R, or nullptr if none.
template <std::meta::info R>
consteval const char* entity_doc() {
    return ann_string_or<R, ^^reflect::doc>(nullptr);
}

// Property name annotated on R via [[=r::property{"name"}]], or nullptr if R is not
// a property accessor.
template <std::meta::info R>
consteval const char* prop_name() {
    return ann_string_or<R, ^^reflect::property>(nullptr);
}
template <std::meta::info R>
consteval bool is_property_accessor() {
    return prop_name<R>() != nullptr;
}
// A property accessor taking no parameters is the getter (the 1-parameter one is the
// setter); a read-only property annotates only the getter. (Kept in consteval helpers
// so parameters_of()'s vector is consumed within one constant evaluation -- an inline
// parameters_of().size() in an if-constexpr condition is not a constant expression.)
template <std::meta::info fn>
consteval bool is_property_getter() {
    return is_property_accessor<fn>() && std::meta::parameters_of(fn).size() == 0;
}
template <std::meta::info fn>
consteval bool is_property_setter() {
    return is_property_accessor<fn>() && std::meta::parameters_of(fn).size() == 1;
}

// Invoke `f` with R's docstring as a single const char* extra when [[=r::doc]] is
// present, or with no extra otherwise; returns f(...)'s result. Lets class_/enum_
// construction add an optional docstring without doubling its branch count -- the
// entity-kind branch lives inside f, so both arms deduce the same return type.
template <std::meta::info R, typename F>
auto with_doc_extra(F&& f) {
    constexpr const char* d = entity_doc<R>();
    if constexpr (d != nullptr) return f(d);
    else                        return f();
}

consteval rv_policy to_rv_policy(reflect::lifetime lt) {
    switch (lt) {
    case reflect::lifetime::take_ownership:     return rv_policy::take_ownership;
    case reflect::lifetime::copy:               return rv_policy::copy;
    case reflect::lifetime::move:               return rv_policy::move;
    case reflect::lifetime::reference:          return rv_policy::reference;
    case reflect::lifetime::reference_internal: return rv_policy::reference_internal;
    case reflect::lifetime::none:               return rv_policy::none;
    default:                                    return rv_policy::automatic;
    }
}

// Return-value policy annotated on R, or rv_policy::automatic (a no-op default).
template <std::meta::info R>
consteval rv_policy ann_rv_policy() {
    if constexpr (has_ann<R, reflect::return_policy>())
        return to_rv_policy(get_ann<R, reflect::return_policy>().value);
    else
        return rv_policy::automatic;
}

// Invoke `emit` with the def-extras implied by R's annotations: rv_policy always
// (automatic is a no-op), plus a docstring and/or keep_alive when present.
template <std::meta::info R, typename F>
void with_call_extras(F&& emit) {
    constexpr rv_policy pol = ann_rv_policy<R>();
    constexpr const char* d = entity_doc<R>();
    if constexpr (has_ann<R, reflect::keep_alive>()) {
        constexpr auto ka = get_ann<R, reflect::keep_alive>();
        if constexpr (d != nullptr)
            emit(d, pol, ::nanobind::keep_alive<ka.nurse, ka.patient>());
        else
            emit(pol, ::nanobind::keep_alive<ka.nurse, ka.patient>());
    } else if constexpr (d != nullptr) {
        emit(d, pol);
    } else {
        emit(pol);
    }
}

// Extras for a data member: a docstring when present, and rv_policy ONLY when
// explicitly annotated (def_rw/def_ro default to reference_internal, which we must
// not clobber by passing automatic). keep_alive does not apply to attributes.
template <std::meta::info R, typename F>
void with_data_extras(F&& emit) {
    constexpr const char* d = entity_doc<R>();
    if constexpr (has_ann<R, reflect::return_policy>()) {
        constexpr rv_policy pol = ann_rv_policy<R>();
        if constexpr (d != nullptr) emit(d, pol);
        else emit(pol);
    } else if constexpr (d != nullptr) {
        emit(d);
    } else {
        emit();
    }
}

// --- Keyword-argument names ---
//
// Emit one nb::arg("name") per parameter so Python callers can use keywords.
// Names come from P3096 parameter reflection (identifier_of); a parameter with no
// identifier yields nb::arg() (unnamed positional), which still keeps the count
// correct for nanobind's all-or-nothing arg-annotation rule.
//
// Default-argument *values* are intentionally not bound: the C++26 standard
// (P3096) exposes only has_default_argument, with no facility to read a default's
// value/expression -- a default argument is an arbitrary expression evaluated in
// the caller's context, not a reflectable entity -- so there is nothing to forward
// to nanobind's `nb::arg("x") = value`.

template <std::meta::info fn>
consteval std::size_t fn_param_count() {
    return std::meta::parameters_of(fn).size();
}

// True if at least one parameter of fn has an identifier (so naming adds value).
template <std::meta::info fn>
consteval bool fn_any_param_named() {
    for (auto p : std::meta::parameters_of(fn))
        if (std::meta::has_identifier(p))
            return true;
    return false;
}

// The I-th parameter's name as a static string, or nullptr when it is unnamed.
template <std::meta::info fn, std::size_t I>
consteval const char* param_name() {
    constexpr auto params = std::define_static_array(std::meta::parameters_of(fn));
    if constexpr (std::meta::has_identifier(params[I]))
        return std::define_static_string(std::meta::identifier_of(params[I]));
    else
        return nullptr;
}

template <std::meta::info fn, typename F, std::size_t... Is>
void with_arg_call_extras_impl(F&& emit, std::index_sequence<Is...>) {
    with_call_extras<fn>([&](auto&&... call_e) {
        emit(::nanobind::arg(param_name<fn, Is>())...,
             std::forward<decltype(call_e)>(call_e)...);
    });
}

// Like with_call_extras, but prepends an nb::arg("name") per parameter when any
// parameter is named. When none are named it degrades to with_call_extras (no
// arg annotations), preserving today's behavior and the all-or-nothing count.
template <std::meta::info fn, typename F>
void with_arg_call_extras(F&& emit) {
    if constexpr (fn_any_param_named<fn>())
        with_arg_call_extras_impl<fn>(std::forward<F>(emit),
            std::make_index_sequence<fn_param_count<fn>()>{});
    else
        with_call_extras<fn>(std::forward<F>(emit));
}

template <typename T, std::meta::info mem>
void reflect_bind_member(auto& cls) {
    // Skip [[=reflect::skip]] members and C-array-typed members. An array data member
    // (e.g. an internal `T storage[N]`, as in absl::FixedArray's storage wrapper) has no
    // def_rw-able form: the setter `c.*p = value` is ill-formed for an array, and even
    // def_ro can't expose it usefully. Mirrors the unnamed/volatile/template member skips.
    if constexpr (!has_ann<mem, reflect::skip>()
                  && !std::meta::is_array_type(std::meta::type_of(mem))) {
        constexpr auto name = entity_name<mem>();
        // Bind via a pointer-to-data-member (&[:mem:]) rather than getter/setter
        // lambdas: a lambda whose signature mentions the spliced member type
        // [:type_of(mem):] crashes the clang-p2996 mangler when passed to the
        // dependent `cls.def_*` call (placeholder-type mangling at parse time).
        with_data_extras<mem>([&](auto&&... e) {
            // def_rw's setter assigns (`c.*p = value`), so a const or non-copy-assignable
            // member (e.g. a move-only `node_handle` inside absl's insert_return_type) is
            // exposed read-only via def_ro instead of breaking the build.
            if constexpr (std::meta::is_const_type(std::meta::type_of(mem)) ||
                          !std::meta::is_copy_assignable_type(std::meta::type_of(mem)))
                cls.def_ro(name, &[:mem:], std::forward<decltype(e)>(e)...);
            else
                cls.def_rw(name, &[:mem:], std::forward<decltype(e)>(e)...);
        });
    }
}

template <typename T, std::meta::info fn, typename FnType>
struct reflect_method_binder;

// A method's function type carries its cv-, ref-, and noexcept-qualifiers, and a
// partial specialization must match them exactly. Stamp out one specialization per
// supported qualifier combination (cv in {-, const} x ref in {-, &} x noexcept).
// The forwarding lambda's signature uses the real Ret/Args... template parameters
// (never type splices), which keeps it clear of the clang-p2996 mangler crash that
// spliced-type lambda signatures trigger in a dependent cls.def call.
#define NB_REFLECT_DEFINE_METHOD_BINDER(QUALS, CONST)                          \
    template <typename T, std::meta::info fn, typename Ret, typename... Args>  \
    struct reflect_method_binder<T, fn, Ret(Args...) QUALS> {                  \
        static void bind(auto& cls, const char* name, auto&&... extra) {       \
            cls.def(name, [](CONST T& self, Args... args) -> Ret {            \
                return self.[:fn:](std::forward<Args>(args)...);              \
            }, std::forward<decltype(extra)>(extra)...);                       \
        }                                                                      \
    };

NB_REFLECT_DEFINE_METHOD_BINDER(, )
NB_REFLECT_DEFINE_METHOD_BINDER(noexcept, )
NB_REFLECT_DEFINE_METHOD_BINDER(&, )
NB_REFLECT_DEFINE_METHOD_BINDER(& noexcept, )
NB_REFLECT_DEFINE_METHOD_BINDER(const, const)
NB_REFLECT_DEFINE_METHOD_BINDER(const noexcept, const)
NB_REFLECT_DEFINE_METHOD_BINDER(const &, const)
NB_REFLECT_DEFINE_METHOD_BINDER(const & noexcept, const)

#undef NB_REFLECT_DEFINE_METHOD_BINDER

template <typename T, std::meta::info fn>
void reflect_bind_method(auto& cls) {
    // Skip shapes that cannot bind meaningfully to a persistent Python instance:
    // volatile and rvalue-ref-qualified (&&) member functions, and C-variadic
    // functions. Skipping leaves them simply unexposed rather than breaking the
    // build (an unmatched function type would select the incomplete primary).
    if constexpr (!std::meta::is_volatile(fn) &&
                  !std::meta::is_rvalue_reference_qualified(fn) &&
                  !std::meta::has_ellipsis_parameter(fn)) {
        using FnType = [:std::meta::type_of(fn):];
        with_arg_call_extras<fn>([&](auto&&... e) {
            reflect_method_binder<T, fn, FnType>::bind(
                cls, entity_name<fn>(), std::forward<decltype(e)>(e)...);
        });
    }
}

// --- Static methods ---

template <std::meta::info fn, typename FnType>
struct reflect_static_method_binder;

// Static methods have no cv/ref qualifiers, but may be noexcept.
#define NB_REFLECT_DEFINE_STATIC_BINDER(QUALS)                                 \
    template <std::meta::info fn, typename Ret, typename... Args>              \
    struct reflect_static_method_binder<fn, Ret(Args...) QUALS> {             \
        static void bind(auto& cls, const char* name, auto&&... extra) {       \
            cls.def_static(name, [](Args... args) -> Ret {                    \
                return [:fn:](std::forward<Args>(args)...);                   \
            }, std::forward<decltype(extra)>(extra)...);                       \
        }                                                                      \
    };

NB_REFLECT_DEFINE_STATIC_BINDER()
NB_REFLECT_DEFINE_STATIC_BINDER(noexcept)

#undef NB_REFLECT_DEFINE_STATIC_BINDER

template <std::meta::info fn>
void reflect_bind_static_method(auto& cls) {
    if constexpr (!std::meta::has_ellipsis_parameter(fn)) {
        using FnType = [:std::meta::type_of(fn):];
        with_arg_call_extras<fn>([&](auto&&... e) {
            reflect_static_method_binder<fn, FnType>::bind(
                cls, entity_name<fn>(), std::forward<decltype(e)>(e)...);
        });
    }
}

// --- Static data members ---

template <typename T, std::meta::info mem>
void reflect_bind_static_member(auto& cls) {
    if constexpr (!has_ann<mem, reflect::skip>()) {
        constexpr auto name = entity_name<mem>();
        // Bind via a pointer to the static (&[:mem:]); see reflect_bind_member for
        // why spliced-type lambdas are avoided.
        if constexpr (std::meta::is_const_type(std::meta::type_of(mem)))
            cls.def_ro_static(name, &[:mem:]);
        else
            cls.def_rw_static(name, &[:mem:]);
    }
}

// --- Free functions ---

template <std::meta::info fn, typename FnType>
struct reflect_free_fn_binder;

// Free functions have no cv/ref qualifiers, but may be noexcept.
#define NB_REFLECT_DEFINE_FREE_BINDER(QUALS)                                   \
    template <std::meta::info fn, typename Ret, typename... Args>              \
    struct reflect_free_fn_binder<fn, Ret(Args...) QUALS> {                   \
        static void bind(module_& m, const char* name, auto&&... extra) {      \
            m.def(name, [](Args... args) -> Ret {                            \
                return [:fn:](std::forward<Args>(args)...);                   \
            }, std::forward<decltype(extra)>(extra)...);                       \
        }                                                                      \
    };

NB_REFLECT_DEFINE_FREE_BINDER()
NB_REFLECT_DEFINE_FREE_BINDER(noexcept)

#undef NB_REFLECT_DEFINE_FREE_BINDER

template <std::meta::info fn>
void reflect_free_function(module_& m) {
    // Skip C-variadic free functions (their function type matches no binder), and
    // free operators (operator@ has no identifier) -- the latter are bound as class
    // dunders by bind_free_operators during their operand types' class binding. A
    // function-template specialization (identity<int>) also has no identifier, but it
    // IS bindable, so admit it via has_template_arguments (entity_name then derives
    // the CamelCase spec name from the template).
    if constexpr (!std::meta::has_ellipsis_parameter(fn) &&
                  (std::meta::has_identifier(fn) ||
                   std::meta::has_template_arguments(fn))) {
        using FnType = [:std::meta::type_of(fn):];
        with_arg_call_extras<fn>([&](auto&&... e) {
            reflect_free_fn_binder<fn, FnType>::bind(
                m, entity_name<fn>(), std::forward<decltype(e)>(e)...);
        });
    }
}

// --- Constructors ---

template <std::meta::info ctor>
consteval std::size_t ctor_param_count() {
    return std::meta::parameters_of(ctor).size();
}

template <std::meta::info ctor>
consteval auto ctor_param_infos() {
    return std::define_static_array(std::meta::parameters_of(ctor));
}

template <std::meta::info ctor, std::size_t... Is>
void reflect_bind_ctor_expand(auto& cls, std::index_sequence<Is...>) {
    if constexpr (sizeof...(Is) == 0) {
        cls.def(init<>());
    } else {
        constexpr auto params = ctor_param_infos<ctor>();
        // Attach nb::arg("name") per ctor parameter so Python callers can use
        // keywords (e.g. T(i=1, j=2)); see with_arg_call_extras for the rule.
        if constexpr (fn_any_param_named<ctor>())
            cls.def(init<typename [:std::meta::type_of(params[Is]):]...>(),
                    ::nanobind::arg(param_name<ctor, Is>())...);
        else
            cls.def(init<typename [:std::meta::type_of(params[Is]):]...>());
    }
}

template <std::meta::info ctor>
void reflect_bind_ctor(auto& cls) {
    if constexpr (!has_ann<ctor, reflect::skip>())
        reflect_bind_ctor_expand<ctor>(cls, std::make_index_sequence<ctor_param_count<ctor>()>{});
}

// --- Inheritance ---

// Count the public base classes of T.
template <typename T>
consteval std::size_t public_base_count() {
    std::size_t n = 0;
    for (auto b : std::meta::bases_of(^^T, std::meta::access_context::unchecked()))
        if (std::meta::is_public(b))
            ++n;
    return n;
}

// Reflection of T's first public base *type* (only valid when the count is > 0).
template <typename T>
consteval std::meta::info first_public_base() {
    for (auto b : std::meta::bases_of(^^T, std::meta::access_context::unchecked()))
        if (std::meta::is_public(b))
            return std::meta::type_of(b);
    return ^^void;  // unreachable: guarded by public_base_count<T>() > 0
}

consteval bool info_vec_contains(const std::vector<std::meta::info>& v,
                                 std::meta::info x) {
    for (auto e : v)
        if (e == x)
            return true;
    return false;
}

// Append every public base *type* in the subtree rooted at `type` (the type's
// direct public bases, their public bases, and so on) to `out`, de-duplicated.
consteval void collect_public_base_subtree(std::meta::info type,
                                           std::vector<std::meta::info>& out) {
    for (auto b : std::meta::bases_of(type, std::meta::access_context::unchecked())) {
        if (!std::meta::is_public(b))
            continue;
        auto bt = std::meta::type_of(b);
        if (!info_vec_contains(out, bt)) {
            out.push_back(bt);
            collect_public_base_subtree(bt, out);
        }
    }
}

// nanobind models a single base, so only T's first public base (and, through it,
// that base's own subtree) is reachable on the Python side via the MRO. Every
// other public base in T's subtree must have its members "flattened" directly
// onto T. This returns exactly those base types: T's whole public-base subtree
// minus the part already covered by the first public base. The subtraction makes
// it correct for diamonds and for bases nested under the primary base (no member
// is bound twice).
template <typename T>
consteval std::vector<std::meta::info> flatten_bases_vec() {
    std::vector<std::meta::info> all, covered, result;
    collect_public_base_subtree(^^T, all);
    for (auto b : std::meta::bases_of(^^T, std::meta::access_context::unchecked())) {
        if (std::meta::is_public(b)) {
            auto primary = std::meta::type_of(b);
            covered.push_back(primary);
            collect_public_base_subtree(primary, covered);
            break;  // only the first public base is the nanobind base
        }
    }
    for (auto t : all)
        if (!info_vec_contains(covered, t))
            result.push_back(t);
    return result;
}

// --- Operators and conversions ---

// Map a C++ operator (and member arity: 0 = unary, 1 = binary) to its Python
// dunder name, or nullptr if it has no clean Python equivalent (and is skipped).
consteval const char* operator_dunder(std::meta::operators op, std::size_t arity) {
    using enum std::meta::operators;
    switch (op) {
    case op_plus:              return arity == 1 ? "__add__" : "__pos__";
    case op_minus:             return arity == 1 ? "__sub__" : "__neg__";
    case op_star:              return arity == 1 ? "__mul__" : nullptr;  // unary * = deref
    case op_ampersand:         return arity == 1 ? "__and__" : nullptr;  // unary & = address-of
    case op_slash:             return "__truediv__";
    case op_percent:           return "__mod__";
    case op_caret:             return "__xor__";
    case op_pipe:              return "__or__";
    case op_tilde:             return "__invert__";
    case op_less_less:         return "__lshift__";
    case op_greater_greater:   return "__rshift__";
    case op_equals_equals:     return "__eq__";
    case op_exclamation_equals:return "__ne__";
    case op_less:              return "__lt__";
    case op_greater:           return "__gt__";
    case op_less_equals:       return "__le__";
    case op_greater_equals:    return "__ge__";
    case op_plus_equals:       return "__iadd__";
    case op_minus_equals:      return "__isub__";
    case op_star_equals:       return "__imul__";
    case op_slash_equals:      return "__itruediv__";
    case op_percent_equals:    return "__imod__";
    case op_caret_equals:      return "__ixor__";
    case op_ampersand_equals:  return "__iand__";
    case op_pipe_equals:       return "__ior__";
    case op_less_less_equals:  return "__ilshift__";
    case op_greater_greater_equals: return "__irshift__";
    case op_parentheses:       return "__call__";
    case op_square_brackets:   return "__getitem__";
    default:                   return nullptr;  // <=>, ++/--, &&/||/!, ->, =, new/delete, ...
    }
}

// The "reversed" dunder for a binary operator, used when a free operator's class
// operand is on the RIGHT (e.g. `2.0 * vec` needs `vec.__rmul__`). For arithmetic
// operators this is the `__r*__` form; for comparisons it is the swapped operator
// (a < b is computed as b.__gt__(a)). nullptr where no reversed form applies
// (in-place operators, etc.), so the reversed binding is then skipped.
consteval const char* operator_reversed_dunder(std::meta::operators op) {
    using enum std::meta::operators;
    switch (op) {
    case op_plus:              return "__radd__";
    case op_minus:             return "__rsub__";
    case op_star:              return "__rmul__";
    case op_slash:             return "__rtruediv__";
    case op_percent:           return "__rmod__";
    case op_caret:             return "__rxor__";
    case op_ampersand:         return "__rand__";
    case op_pipe:              return "__ror__";
    case op_less_less:         return "__rlshift__";
    case op_greater_greater:   return "__rrshift__";
    case op_equals_equals:     return "__eq__";   // symmetric
    case op_exclamation_equals:return "__ne__";   // symmetric
    case op_less:              return "__gt__";   // a < b  <=>  b > a
    case op_greater:           return "__lt__";
    case op_less_equals:       return "__ge__";
    case op_greater_equals:    return "__le__";
    default:                   return nullptr;
    }
}

consteval bool is_inplace_operator(std::meta::operators op) {
    using enum std::meta::operators;
    switch (op) {
    case op_plus_equals: case op_minus_equals: case op_star_equals:
    case op_slash_equals: case op_percent_equals: case op_caret_equals:
    case op_ampersand_equals: case op_pipe_equals:
    case op_less_less_equals: case op_greater_greater_equals:
        return true;
    default:
        return false;
    }
}

template <typename T, std::meta::info fn>
void reflect_bind_operator(auto& cls) {
    constexpr auto op = std::meta::operator_of(fn);
    constexpr const char* dunder =
        operator_dunder(op, std::meta::parameters_of(fn).size());
    if constexpr (dunder != nullptr && !std::meta::is_volatile(fn) &&
                  !std::meta::is_rvalue_reference_qualified(fn) &&
                  !std::meta::has_ellipsis_parameter(fn)) {
        using FnType = [:std::meta::type_of(fn):];
        // is_operator() makes mismatched-argument calls return NotImplemented
        // rather than raising TypeError, matching Python operator semantics.
        if constexpr (is_inplace_operator(op)) {
            // In-place operators return *this; rv_policy::reference returns the
            // existing Python object (preserving identity) instead of a copy.
            reflect_method_binder<T, fn, FnType>::bind(
                cls, dunder, is_operator(), rv_policy::reference);
        } else {
            reflect_method_binder<T, fn, FnType>::bind(cls, dunder, is_operator());
        }
    }
}

// Among T's public, non-template, non-skipped integral conversion operators (bool
// excluded -- it feeds __bool__), the reflection of the one with the widest result
// type, or ^^void if there is none. Multiple integral conversions would otherwise
// each bind __int__ with the last-bound silently winning, which can pick an
// arbitrarily narrow result (absl::int128's operator char/int/long/...).
template <typename T>
consteval std::meta::info widest_integral_conversion() {
    std::meta::info best = ^^void;
    std::size_t best_size = 0;
    for (auto fn : std::meta::members_of(^^T, std::meta::access_context::unchecked())) {
        if (!std::meta::is_function(fn) || std::meta::is_template(fn))
            continue;
        if (!std::meta::is_public(fn) || !std::meta::is_conversion_function(fn))
            continue;
        if (!std::meta::annotations_of(fn, ^^reflect::skip).empty())
            continue;
        auto R = std::meta::return_type_of(fn);
        if (!std::meta::is_integral_type(R) || std::meta::is_same_type(R, ^^bool))
            continue;
        if (best == ^^void || std::meta::size_of(R) > best_size) {
            best = fn;
            best_size = std::meta::size_of(R);
        }
    }
    return best;
}

template <typename T, std::meta::info fn>
void reflect_bind_conversion(auto& cls) {
    // Use fixed concrete lambda return types (bool/long long/double) and cast the
    // conversion result: a spliced type in a lambda signature crashes the
    // clang-p2996 mangler.
    constexpr auto R = std::meta::return_type_of(fn);
    if constexpr (std::meta::is_same_type(R, ^^bool))
        cls.def("__bool__", [](T& self) -> bool { return (bool) self.[:fn:](); });
    else if constexpr (std::meta::is_integral_type(R)) {
        // Only the widest integral conversion binds __int__ (see
        // widest_integral_conversion); the others have no Python equivalent.
        if constexpr (fn == widest_integral_conversion<T>())
            cls.def("__int__", [](T& self) -> long long { return (long long) self.[:fn:](); });
    }
    else if constexpr (std::meta::is_floating_point_type(R))
        cls.def("__float__", [](T& self) -> double { return (double) self.[:fn:](); });
    // else: no standard numeric dunder -- skip.
}

// --- Free (namespace-scope) operators -> dunders ---
//
// A free `operator@(L, R)` is attached as a dunder to the class of one of its
// operands. The forward dunder (`__add__`, ...) goes on the LEFT operand's class;
// the reversed dunder (`__radd__`, or a swapped comparison) goes on the RIGHT
// operand's class -- the latter is what makes `2.0 * vec` work when only the right
// operand is a bound type. A free unary `operator@(T)` is attached to T's class
// (`__neg__`/`__pos__`/`__invert__`). Binding happens while that class is being
// reflected, so its class_ object is in hand (no re-registration of an existing type).
//
// The forwarding lambda keeps the real Ret/P0/P1 template parameters in its
// signature (from the function-type partial specialization) and only splices the
// call in its body, staying clear of the clang-p2996 spliced-signature mangler crash.

template <typename T, std::meta::info fn, typename FnType>
struct reflect_free_operator_binder;

#define NB_REFLECT_DEFINE_FREE_OP_BINDER(QUALS)                                    \
    template <typename T, std::meta::info fn, typename Ret,                        \
              typename P0, typename P1>                                            \
    struct reflect_free_operator_binder<T, fn, Ret(P0, P1) QUALS> {                \
        using A = std::remove_cvref_t<P0>;                                         \
        using B = std::remove_cvref_t<P1>;                                         \
        static void bind(auto& cls) {                                              \
            constexpr auto op = std::meta::operator_of(fn);                        \
            /* Forward: this class is the LEFT operand. */                         \
            if constexpr (std::is_same_v<T, A>) {                                  \
                constexpr const char* d = operator_dunder(op, 1);                  \
                if constexpr (d != nullptr) {                                      \
                    if constexpr (is_inplace_operator(op))                         \
                        cls.def(d, [](P0 a, P1 b) -> Ret {                         \
                            return [:fn:](a, b); },                                \
                            is_operator(), rv_policy::reference);                  \
                    else                                                           \
                        cls.def(d, [](P0 a, P1 b) -> Ret {                         \
                            return [:fn:](a, b); }, is_operator());                \
                }                                                                  \
            }                                                                      \
            /* Reversed: this class is the RIGHT operand (and not also the left, */\
            /* which would be the symmetric same-type case already covered).     */\
            if constexpr (std::is_same_v<T, B> && !std::is_same_v<A, B>) {         \
                constexpr const char* rd = operator_reversed_dunder(op);           \
                if constexpr (rd != nullptr)                                       \
                    cls.def(rd, [](P1 b, P0 a) -> Ret {                            \
                        return [:fn:](a, b); }, is_operator());                    \
            }                                                                      \
        }                                                                          \
    };

NB_REFLECT_DEFINE_FREE_OP_BINDER()
NB_REFLECT_DEFINE_FREE_OP_BINDER(noexcept)

#undef NB_REFLECT_DEFINE_FREE_OP_BINDER

// Unary free operator (operator-(T) / operator+(T) / operator~(T)) -> __neg__ /
// __pos__ / __invert__ on T's class. operator_dunder(op, 0) is the member-arity-0
// mapping, which already yields nullptr for the unary forms with no Python
// equivalent (deref *, address-of &, !, prefix ++/--) -- those are skipped. A
// postfix ++/-- (the (T, int) form) routes to the binary specialization, where
// op_plus_plus/op_minus_minus also map to nullptr.
#define NB_REFLECT_DEFINE_FREE_UNARY_OP_BINDER(QUALS)                               \
    template <typename T, std::meta::info fn, typename Ret, typename P0>           \
    struct reflect_free_operator_binder<T, fn, Ret(P0) QUALS> {                    \
        using A = std::remove_cvref_t<P0>;                                         \
        static void bind(auto& cls) {                                              \
            constexpr auto op = std::meta::operator_of(fn);                        \
            if constexpr (std::is_same_v<T, A>) {                                  \
                constexpr const char* d = operator_dunder(op, 0);                  \
                if constexpr (d != nullptr)                                        \
                    cls.def(d, [](P0 a) -> Ret { return [:fn:](a); },              \
                            is_operator());                                        \
            }                                                                      \
        }                                                                          \
    };

NB_REFLECT_DEFINE_FREE_UNARY_OP_BINDER()
NB_REFLECT_DEFINE_FREE_UNARY_OP_BINDER(noexcept)

#undef NB_REFLECT_DEFINE_FREE_UNARY_OP_BINDER

// True if any parameter of fn is a std::basic_ostream / basic_istream / basic_iostream — i.e.
// fn is a stream I/O operator such as `operator<<(std::ostream&, T)`. nanobind has no caster for
// stream types (and they're typically incomplete at the binding site), so such free operators are
// NOT bound as dunders; a class's streamability is surfaced as __str__ instead (bind_stream_str).
// Keyed on the OPERAND type, not the operator symbol, so a genuine shift like
// `operator<<(absl::int128, int)` is untouched and still maps to __lshift__.
// True if `type` is a std::basic_ostream / basic_istream / basic_iostream (after stripping
// reference/cv and resolving the std::ostream/std::istream typedefs). nanobind has no caster for
// stream types, and recursing into the (often incomplete) basic_ostream breaks the caster walk.
consteval bool is_stream_type(std::meta::info type) {
    std::string_view n = std::meta::display_string_of(
        std::meta::dealias(std::meta::remove_cvref(type)));
    return n.find("basic_ostream") != std::string_view::npos ||
           n.find("basic_istream") != std::string_view::npos ||
           n.find("basic_iostream") != std::string_view::npos;
}

// True if any parameter of fn is a stream type — i.e. fn is a stream I/O operator such as
// operator<<(std::ostream&, T). Such free operators are NOT bound as dunders (a class's
// streamability is surfaced as __str__ instead, see bind_stream_str); keyed on the OPERAND type,
// not the operator symbol, so a genuine shift like operator<<(absl::int128, int) is untouched.
consteval bool involves_stream_type(std::meta::info fn) {
    for (std::meta::info p : std::meta::parameters_of(fn))
        if (is_stream_type(std::meta::type_of(p)))
            return true;
    return false;
}

// True if fn is a unary or binary namespace-scope operator the binder can attach as a
// dunder. Kept in a consteval helper so the vector from parameters_of() is fully consumed
// within one constant evaluation (an inline parameters_of().size() in the if-constexpr
// condition leaves the constant evaluator unable to prove the allocation is freed).
template <std::meta::info fn>
consteval bool is_bindable_free_operator() {
    if (!std::meta::is_function(fn)
        || !std::meta::is_operator_function(fn)
        || std::meta::is_template(fn)
        || std::meta::has_ellipsis_parameter(fn)
        || involves_stream_type(fn))
        return false;
    std::size_t n = std::meta::parameters_of(fn).size();
    return n == 1 || n == 2;
}

// Scan T's enclosing namespace for unary/binary free operators involving T and bind
// each onto T's class_ (`cls`). Runs once per class (reflect_class is idempotent), so a
// given (class, operator, side) pair binds exactly once. O(classes x namespace
// members); fine at this prove-out's scale.
// If T is insertable into a std::ostream (via a member or free `operator<<(ostream&, T)`),
// surface that as Python __str__: format through a std::ostringstream and return the resulting
// std::string. This makes str(x) work for streamable C++ types (absl::int128, absl::Duration,
// absl::Status, ...) WITHOUT ever exposing std::ostream to Python — sidestepping both the
// no-ostream-caster problem and the incomplete-basic_ostream compile error that binding the
// stream operator as a dunder would hit. Guarded by a requires-expression (only binds when
// actually streamable); the lambda keeps T (a real template parameter, not a splice) in its
// signature, avoiding the clang-p2996 spliced-signature mangler crash; the `oss << self` call
// resolves the operator by ADL in the lambda body.
template <typename T>
void bind_stream_str(auto& cls) {
    if constexpr (requires(std::ostream& os, const T& t) { os << t; }) {
        cls.def("__str__", [](const T& self) {
            std::ostringstream oss;
            oss << self;
            return oss.str();
        });
    }
}

template <typename T>
void bind_free_operators(auto& cls) {
    constexpr auto scope = std::meta::parent_of(^^T);
    if constexpr (std::meta::is_namespace(scope)) {
        template for (constexpr auto fn :
            std::define_static_array(std::meta::members_of(
                scope, std::meta::access_context::unchecked()))) {
            if constexpr (is_bindable_free_operator<fn>()
                && !has_ann<fn, reflect::skip>()) {
                using FnType = [:std::meta::type_of(fn):];
                reflect_free_operator_binder<T, fn, FnType>::bind(cls);
            }
        };
    }
}

// --- Properties (getter/setter pairs marked [[=r::property{"name"}]]) ---

// The setter paired with property getter `getter` in class T: a property accessor
// taking one parameter whose property name matches the getter's, or ^^void if none
// (a read-only property). Uses an internal `template for` because reading each
// candidate's property-name annotation requires a constexpr reflection.
template <typename T, std::meta::info getter>
consteval std::meta::info find_property_setter() {
    constexpr std::string_view gname = prop_name<getter>();
    std::meta::info found = ^^void;
    template for (constexpr auto fn : std::define_static_array(
            std::meta::members_of(^^T, std::meta::access_context::unchecked()))) {
        if constexpr (is_property_setter<fn>()) {
            if (std::string_view(prop_name<fn>()) == gname)
                found = fn;
        }
    };
    return found;
}

// Bind one property from its getter and (optional) setter. Uses pointer-to-member-
// functions (&[:getter:] / &[:setter:]) -- like data members use &[:mem:] -- so no
// lambda names a spliced type (clang-p2996 mangler rule). getter/setter are template
// parameters (not captured locals): a std::meta::info is a consteval-only type and
// cannot be captured by the runtime extras lambda. The getter's return-policy / doc
// annotations are threaded via with_data_extras.
template <std::meta::info getter, std::meta::info setter>
void reflect_bind_property_impl(auto& cls) {
    constexpr auto name = prop_name<getter>();
    with_data_extras<getter>([&](auto&&... e) {
        if constexpr (setter == ^^void)
            cls.def_prop_ro(name, &[:getter:], std::forward<decltype(e)>(e)...);
        else
            cls.def_prop_rw(name, &[:getter:], &[:setter:],
                            std::forward<decltype(e)>(e)...);
    });
}

template <typename T, std::meta::info getter>
void reflect_bind_property(auto& cls) {
    reflect_bind_property_impl<getter, find_property_setter<T, getter>()>(cls);
}

// Route a public member function to the right binder. Operators and conversion
// functions have no identifier, so they must be detected before reflect_bind_method
// (which names the binding via identifier_of).
template <typename T, std::meta::info fn>
void reflect_bind_member_function(auto& cls) {
    if constexpr (has_ann<fn, reflect::skip>())
        return;  // explicitly excluded
    else if constexpr (is_property_accessor<fn>())
        return;  // getter/setter handled by the property pass, not as a method
    else if constexpr (std::meta::is_operator_function(fn))
        reflect_bind_operator<T, fn>(cls);
    else if constexpr (std::meta::is_conversion_function(fn))
        reflect_bind_conversion<T, fn>(cls);
    else if constexpr (std::meta::is_static_member(fn))
        reflect_bind_static_method<fn>(cls);
    else if constexpr (std::meta::has_identifier(fn))
        reflect_bind_method<T, fn>(cls);
    // else: nameless and non-operator (e.g. a literal operator) -- skip.
}

// Bind the constructors, data members, static data members, and methods declared
// directly in T onto an already-created class_ object. Inherited members are not
// re-bound here -- they are exposed automatically through the Python base type.
template <typename T>
void bind_class_contents(auto& cls) {
    // Bind constructors
    template for (constexpr auto fn :
        std::define_static_array(std::meta::members_of(
            ^^T, std::meta::access_context::unchecked()))) {
        if constexpr (std::meta::is_constructor(fn)
            && std::meta::is_public(fn)
            && !std::meta::is_template(fn)   // skip constructor templates (cannot reflect)
            && !std::meta::is_copy_constructor(fn)
            && !std::meta::is_move_constructor(fn)) {
            reflect_bind_ctor<fn>(cls);
        }
    };

    // Bind data members. Skip unnamed members (anonymous union/struct fields, e.g. glm's
    // x/y/z/w swizzle aliasing): identifier_of() is ill-formed on them, and a pointer-to-member
    // of the enclosing class cannot be formed for an anonymous-union member anyway.
    template for (constexpr auto mem :
        std::define_static_array(std::meta::nonstatic_data_members_of(
            ^^T, std::meta::access_context::unchecked()))) {
        if constexpr (std::meta::is_public(mem) && std::meta::has_identifier(mem)) {
            reflect_bind_member<T, mem>(cls);
        }
    };

    // Bind static data members
    template for (constexpr auto mem :
        std::define_static_array(std::meta::static_data_members_of(
            ^^T, std::meta::access_context::unchecked()))) {
        if constexpr (std::meta::is_public(mem) && std::meta::has_identifier(mem)) {
            reflect_bind_static_member<T, mem>(cls);
        }
    };

    // Bind methods (instance, static, operators, conversions). Property accessors are
    // skipped here (see reflect_bind_member_function) and bound by the pass below.
    template for (constexpr auto fn :
        std::define_static_array(std::meta::members_of(
            ^^T, std::meta::access_context::unchecked()))) {
        if constexpr (std::meta::is_function(fn)
            && std::meta::is_public(fn)
            && !std::meta::is_template(fn)   // skip member function templates (unsupported)
            && !std::meta::is_constructor(fn)
            && !std::meta::is_destructor(fn)
            && !std::meta::is_special_member_function(fn)) {
            reflect_bind_member_function<T, fn>(cls);
        }
    };

    // Bind properties from [[=r::property{"name"}]] getter/setter pairs. The is_template
    // guard must gate the is_property_getter<fn>() call itself: that helper queries
    // annotations_of(fn), which is ill-formed on a template, so a templated member must be
    // excluded *before* it is instantiated (a nested if constexpr, not an && short-circuit).
    template for (constexpr auto fn :
        std::define_static_array(std::meta::members_of(
            ^^T, std::meta::access_context::unchecked()))) {
        if constexpr (std::meta::is_function(fn)
            && std::meta::is_public(fn)
            && !std::meta::is_template(fn)) {
            if constexpr (is_property_getter<fn>()) {
                reflect_bind_property<T, fn>(cls);
            }
        }
    };
}

// Flatten the public members declared directly in base type `Base` onto a
// derived class_ `cls` (of type T), using base-class member pointers
// (&[:mem:] yields a pointer-to-member of Base, which def_rw/def_ro accept
// because Base is a base of T). Constructors are not flattened.
template <typename T, std::meta::info Base>
void flatten_base_members(auto& cls) {
    template for (constexpr auto mem :
        std::define_static_array(std::meta::nonstatic_data_members_of(
            Base, std::meta::access_context::unchecked()))) {
        if constexpr (std::meta::is_public(mem)) {
            reflect_bind_member<T, mem>(cls);
        }
    };

    template for (constexpr auto mem :
        std::define_static_array(std::meta::static_data_members_of(
            Base, std::meta::access_context::unchecked()))) {
        if constexpr (std::meta::is_public(mem)) {
            reflect_bind_static_member<T, mem>(cls);
        }
    };

    template for (constexpr auto fn :
        std::define_static_array(std::meta::members_of(
            Base, std::meta::access_context::unchecked()))) {
        if constexpr (std::meta::is_function(fn)
            && std::meta::is_public(fn)
            && !std::meta::is_template(fn)   // skip member function templates (unsupported)
            && !std::meta::is_constructor(fn)
            && !std::meta::is_destructor(fn)
            && !std::meta::is_special_member_function(fn)) {
            reflect_bind_member_function<T, fn>(cls);
        }
    };
}

// Flatten every secondary public base (those nanobind cannot model as a real
// Python base) onto T's class_. See flatten_bases_vec for which types these are.
template <typename T>
void flatten_secondary_bases(auto& cls) {
    template for (constexpr auto base :
        std::define_static_array(flatten_bases_vec<T>())) {
        flatten_base_members<T, base>(cls);
    };
}

// --- STL type-caster coverage ---
//
// nanobind needs a type_caster (from <nanobind/stl/*.h>) for each std type that
// appears in a bound signature. These consteval helpers walk the bound signatures,
// identify the std template instantiations used, and map each to its caster header.
// The result drives two things: the codegen path emits the matching #includes
// (nb_reflect_codegen.h), and the header-only path static_asserts when a caster is
// missing (check_stl_casters below). #include cannot be emitted from template code
// into the current TU, so the header-only path can only detect/diagnose; only the
// codegen path can actually emit the includes.

// True if any enclosing namespace of e is named "std" (handles libc++'s inline
// namespace, where std::vector is really std::__1::vector).
consteval bool is_in_std(std::meta::info e) {
    for (auto p = std::meta::parent_of(e); p != ^^::; p = std::meta::parent_of(p))
        if (std::meta::has_identifier(p) && std::meta::identifier_of(p) == "std")
            return true;
    return false;
}

// True if a class type (or, for a specialization, its primary template) carries a
// [[=reflect::skip]] annotation. Defined here (before the STL-caster and user-spec
// collection walks that use it) so a skip-annotated type can be treated as opaque
// throughout. Annotating a class template skips ALL of its specializations.
consteval bool is_skip_annotated(std::meta::info e) {
    e = std::meta::remove_cvref(e);
    while (std::meta::is_pointer_type(e))
        e = std::meta::remove_cvref(std::meta::remove_pointer(e));
    // Only class types carry a [[=reflect::skip]] we care about; querying
    // annotations_of on a non-class entity is not meaningful and must be avoided
    // (this runs in the arbitrary-type STL/spec walks).
    if (!std::meta::is_type(e) || !std::meta::is_class_type(e))
        return false;
    if (!std::meta::annotations_of(e, ^^reflect::skip).empty())
        return true;
    return false;
}

// If `type` is a std template instantiation nanobind covers with a caster header,
// return that header path; otherwise nullptr. cv/ref-qualifiers are ignored.
consteval const char* stl_caster_header(std::meta::info type) {
    type = std::meta::remove_cvref(type);
    if (!std::meta::has_template_arguments(type))
        return nullptr;
    auto tmpl = std::meta::template_of(type);
    if (!is_in_std(tmpl) || !std::meta::has_identifier(tmpl))
        return nullptr;
    std::string_view n = std::meta::identifier_of(tmpl);
    if (n == "basic_string") {
        auto args = std::meta::template_arguments_of(type);
        return (!args.empty() && args[0] == ^^wchar_t)
            ? "nanobind/stl/wstring.h" : "nanobind/stl/string.h";
    }
    if (n == "basic_string_view") return "nanobind/stl/string_view.h";
    if (n == "vector")            return "nanobind/stl/vector.h";
    if (n == "list")              return "nanobind/stl/list.h";
    if (n == "array")             return "nanobind/stl/array.h";
    if (n == "set")               return "nanobind/stl/set.h";
    if (n == "unordered_set")     return "nanobind/stl/unordered_set.h";
    if (n == "map")               return "nanobind/stl/map.h";
    if (n == "unordered_map")     return "nanobind/stl/unordered_map.h";
    if (n == "pair")              return "nanobind/stl/pair.h";
    if (n == "tuple")             return "nanobind/stl/tuple.h";
    if (n == "optional")          return "nanobind/stl/optional.h";
    if (n == "variant")           return "nanobind/stl/variant.h";
    if (n == "shared_ptr")        return "nanobind/stl/shared_ptr.h";
    if (n == "unique_ptr")        return "nanobind/stl/unique_ptr.h";
    if (n == "function")          return "nanobind/stl/function.h";
    if (n == "complex")           return "nanobind/stl/complex.h";
    return nullptr;
}

// True for std "policy" templates that appear as container parameters but are not
// part of the value interface (allocator/comparator/hash/traits/deleter). We must
// not recurse into these: e.g. std::map's default allocator is
// std::allocator<std::pair<const Key,T>>, which would otherwise spuriously pull in
// pair.h even though the map caster never needs it.
consteval bool is_stl_policy(std::meta::info type) {
    type = std::meta::remove_cvref(type);
    if (!std::meta::has_template_arguments(type))
        return false;
    auto tmpl = std::meta::template_of(type);
    if (!is_in_std(tmpl) || !std::meta::has_identifier(tmpl))
        return false;
    std::string_view n = std::meta::identifier_of(tmpl);
    return n == "allocator" || n == "less" || n == "greater" || n == "hash" ||
           n == "equal_to" || n == "char_traits" || n == "default_delete";
}

// Append the std caster types reachable from `type` (itself and, recursively, its
// value-carrying template arguments) to `out`, de-duplicated by reflection. `visited`
// memoizes types whose argument subtree has already been walked, so they are not re-walked.
// Template-argument trees are finite, so this is not needed for termination -- it is a
// performance guard: collapsing the O(members x graph) redundant re-walking that an enormous
// type (e.g. nlohmann::json's basic_json) otherwise incurs, which on its own can exceed the
// constexpr step budget. The `out`-dedup prevents duplicate pushes; `visited` prevents
// redundant re-descent.
consteval void collect_stl_types(std::meta::info type,
                                 std::vector<std::meta::info>& out,
                                 std::vector<std::meta::info>& visited) {
    type = std::meta::remove_cvref(type);
    // A [[=reflect::skip]] type is opaque: do not walk its template arguments for
    // STL casters (e.g. nlohmann's output_adapter<uint8_t> resolves its default
    // StringType to the un-castable std::basic_string<unsigned char>).
    if (is_skip_annotated(type))
        return;
    if (!std::meta::has_template_arguments(type))
        return;
    if (info_vec_contains(visited, type))
        return;
    visited.push_back(type);
    if (stl_caster_header(type) != nullptr && !info_vec_contains(out, type))
        out.push_back(type);
    for (auto arg : std::meta::template_arguments_of(type))
        if (std::meta::is_type(arg) && !is_stl_policy(arg))
            collect_stl_types(arg, out, visited);
}
consteval void collect_stl_types(std::meta::info type,
                                 std::vector<std::meta::info>& out) {
    std::vector<std::meta::info> visited;
    collect_stl_types(type, out, visited);
}

// Collect std caster types from every signature directly declared in class `cls`
// (data members, static data members, and the return/parameter types of its
// constructors, methods, and operators). Inherited members are covered when the
// base class is itself walked.
// A single shared `visited` is threaded through the whole class/scope walk: without it,
// a class whose members repeatedly mention the same heavy type (e.g. nlohmann::json, whose
// ~hundreds of members each take/return basic_json) re-walks that type's full graph once per
// member, exploding constexpr step count (and, pushed far enough, ICEing the toolchain).
consteval void collect_class_stl_types(std::meta::info cls,
                                       std::vector<std::meta::info>& out,
                                       std::vector<std::meta::info>& visited) {
    for (auto mem : std::meta::members_of(cls, std::meta::access_context::unchecked())) {
        if (!std::meta::is_public(mem) || std::meta::is_template(mem))
            continue;
        if (std::meta::is_function(mem) && !std::meta::is_destructor(mem)) {
            if (!std::meta::is_constructor(mem))
                collect_stl_types(std::meta::return_type_of(mem), out, visited);
            for (auto p : std::meta::parameters_of(mem))
                collect_stl_types(std::meta::type_of(p), out, visited);
        }
    }
    for (auto mem : std::meta::nonstatic_data_members_of(
             cls, std::meta::access_context::unchecked()))
        if (std::meta::is_public(mem))
            collect_stl_types(std::meta::type_of(mem), out, visited);
    for (auto mem : std::meta::static_data_members_of(
             cls, std::meta::access_context::unchecked()))
        if (std::meta::is_public(mem))
            collect_stl_types(std::meta::type_of(mem), out, visited);
}
consteval void collect_class_stl_types(std::meta::info cls,
                                       std::vector<std::meta::info>& out) {
    std::vector<std::meta::info> visited;
    collect_class_stl_types(cls, out, visited);
}

// Walk a namespace (recursively) collecting std caster types from its classes and
// free functions; or, when given a class directly, just that class.
consteval void collect_scope_stl_types(std::meta::info r,
                                       std::vector<std::meta::info>& out,
                                       std::vector<std::meta::info>& visited) {
    if (std::meta::is_namespace(r)) {
        for (auto mem : std::meta::members_of(r, std::meta::access_context::unchecked())) {
            if (std::meta::is_type(mem) && std::meta::is_class_type(mem))
                collect_class_stl_types(mem, out, visited);
            else if (std::meta::is_function(mem) && !std::meta::is_template(mem)) {
                collect_stl_types(std::meta::return_type_of(mem), out, visited);
                for (auto p : std::meta::parameters_of(mem))
                    collect_stl_types(std::meta::type_of(p), out, visited);
            } else if (std::meta::is_namespace(mem))
                collect_scope_stl_types(mem, out, visited);
        }
    } else if (std::meta::is_type(r) && std::meta::is_class_type(r)) {
        collect_class_stl_types(r, out, visited);
    }
}
consteval void collect_scope_stl_types(std::meta::info r,
                                       std::vector<std::meta::info>& out) {
    std::vector<std::meta::info> visited;
    collect_scope_stl_types(r, out, visited);
}

// --- User (non-std) class-template specialization discovery ---
//
// A namespace's template *declarations* are not bindable and its *instantiations*
// are not enumerable members, so the only way to find the specializations a
// reflected set actually uses is to walk its concrete signatures (mirroring the stl
// caster walk above). These helpers collect every user class-template specialization
// reachable from the reflected set -- recursively through template args and through
// each discovered spec's own members/bases -- so reflect_ can bind them.

// True if `type` is a specialization of a user (non-std) class template. std
// specializations go to the type-caster path, not the class-binding path.
consteval bool is_user_class_template_spec(std::meta::info type) {
    type = std::meta::remove_cvref(type);
    if (!std::meta::is_type(type) || !std::meta::is_class_type(type))
        return false;
    if (!std::meta::has_template_arguments(type))
        return false;
    auto tmpl = std::meta::template_of(type);
    if (!std::meta::has_identifier(tmpl))
        return false;
    // A [[=reflect::skip]] on the type or its template excludes it from transitive
    // user-spec discovery (so it is neither pre-bound nor walked for STL casters).
    if (is_skip_annotated(type))
        return false;
    return !is_in_std(tmpl);
}

// Append the user class-template specializations reachable from `type` -- the
// (pointer/ref/cv-unwrapped) type itself if it is one, plus, recursively, its
// non-policy template args (Foo<Bar<int>> yields both Foo<Bar<int>> and Bar<int>)
// -- to `out`, de-duplicated.
// `visited` memoizes already-walked types to avoid redundant re-descent (same performance
// purpose as in collect_stl_types; not needed for termination -- arg trees are finite).
consteval void collect_user_specs_from_type(std::meta::info type,
                                            std::vector<std::meta::info>& out,
                                            std::vector<std::meta::info>& visited) {
    type = std::meta::remove_cvref(type);
    while (std::meta::is_pointer_type(type))
        type = std::meta::remove_cvref(std::meta::remove_pointer(type));
    if (!std::meta::has_template_arguments(type))
        return;
    if (info_vec_contains(visited, type))
        return;
    visited.push_back(type);
    if (is_user_class_template_spec(type) && !info_vec_contains(out, type))
        out.push_back(type);
    for (auto arg : std::meta::template_arguments_of(type))
        if (std::meta::is_type(arg) && !is_stl_policy(arg))
            collect_user_specs_from_type(arg, out, visited);
}
consteval void collect_user_specs_from_type(std::meta::info type,
                                            std::vector<std::meta::info>& out) {
    std::vector<std::meta::info> visited;
    collect_user_specs_from_type(type, out, visited);
}

// Scan one class's own signatures -- data members, static data, the return/param
// types of its functions, and its bases -- for user specializations. Skips template
// members (a member template has no concrete signature) and destructors.
consteval void collect_class_user_specs(std::meta::info cls,
                                        std::vector<std::meta::info>& out,
                                        std::vector<std::meta::info>& visited) {
    for (auto mem : std::meta::members_of(cls, std::meta::access_context::unchecked())) {
        if (!std::meta::is_public(mem) || std::meta::is_template(mem))
            continue;
        if (std::meta::is_function(mem) && !std::meta::is_destructor(mem)) {
            if (!std::meta::is_constructor(mem))
                collect_user_specs_from_type(std::meta::return_type_of(mem), out, visited);
            for (auto p : std::meta::parameters_of(mem))
                collect_user_specs_from_type(std::meta::type_of(p), out, visited);
        }
    }
    for (auto mem : std::meta::nonstatic_data_members_of(
             cls, std::meta::access_context::unchecked()))
        if (std::meta::is_public(mem))
            collect_user_specs_from_type(std::meta::type_of(mem), out, visited);
    for (auto mem : std::meta::static_data_members_of(
             cls, std::meta::access_context::unchecked()))
        if (std::meta::is_public(mem))
            collect_user_specs_from_type(std::meta::type_of(mem), out, visited);
    for (auto b : std::meta::bases_of(cls, std::meta::access_context::unchecked()))
        if (std::meta::is_public(b))
            collect_user_specs_from_type(std::meta::type_of(b), out, visited);
}

// Seed pass: walk a namespace (recursively) collecting user specs from its classes
// and free functions; or, given a class/spec or function directly, from that entity
// (including the entity itself when it is a spec).
consteval void collect_scope_user_specs(std::meta::info r,
                                        std::vector<std::meta::info>& out,
                                        std::vector<std::meta::info>& visited) {
    if (std::meta::is_namespace(r)) {
        for (auto mem : std::meta::members_of(r, std::meta::access_context::unchecked())) {
            if (std::meta::is_type(mem) && std::meta::is_class_type(mem))
                collect_class_user_specs(mem, out, visited);
            else if (std::meta::is_function(mem) && !std::meta::is_template(mem)) {
                collect_user_specs_from_type(std::meta::return_type_of(mem), out, visited);
                for (auto p : std::meta::parameters_of(mem))
                    collect_user_specs_from_type(std::meta::type_of(p), out, visited);
            } else if (std::meta::is_namespace(mem))
                collect_scope_user_specs(mem, out, visited);
        }
    } else if (std::meta::is_type(r) && std::meta::is_class_type(r)) {
        collect_user_specs_from_type(r, out, visited);     // r itself, if a spec
        collect_class_user_specs(r, out, visited);         // and its members/bases
    } else if (std::meta::is_function(r)) {
        collect_user_specs_from_type(std::meta::return_type_of(r), out, visited);
        for (auto p : std::meta::parameters_of(r))
            collect_user_specs_from_type(std::meta::type_of(p), out, visited);
    }
}

// The de-duplicated, fixpoint-closed list of user class-template specializations a
// reflected entity needs bound. Seeds from the entity's concrete signatures, then
// expands each newly found spec by scanning ITS members/bases (so Wrap<int> surfaces
// Box<int>). The index loop over the growing vector plus the dedup is a worklist
// fixpoint that visits each spec once -- terminating on CRTP/self-referential specs.
consteval std::vector<std::meta::info> required_user_specs(std::meta::info r) {
    std::vector<std::meta::info> out, visited;
    collect_scope_user_specs(r, out, visited);
    for (std::size_t i = 0; i < out.size(); ++i)
        collect_class_user_specs(out[i], out, visited);
    return out;
}

consteval std::vector<std::meta::info> required_stl_types(std::meta::info r) {
    std::vector<std::meta::info> out, visited;
    collect_scope_stl_types(r, out, visited);
    return out;
}

// Like required_stl_types, but also sweeps the members of every discovered template
// specialization (a Holder<int> bound as a class needs the caster for its
// std::vector<int> member, but the spec is not a namespace member so the scope walk
// above misses it). This is a second full walk, so it is kept out of required_stl_types
// -- the codegen path (emit_stl_includes), which must emit the #includes, calls this;
// the header-only path leaves spec-member casters to surface at bind time.
consteval std::vector<std::meta::info> required_stl_types_with_specs(std::meta::info r) {
    std::vector<std::meta::info> out, visited;
    collect_scope_stl_types(r, out, visited);
    for (auto spec : required_user_specs(r))
        collect_class_stl_types(spec, out, visited);
    return out;
}

// The de-duplicated list of <nanobind/stl/*.h> header paths needed by R's bound
// signatures, as a static array of const char*. Reusable by codegen and callers.
template <std::meta::info R>
consteval auto required_stl_headers() {
    std::vector<std::string_view> hdrs;
    for (auto ty : required_stl_types(R)) {
        std::string_view h = stl_caster_header(ty);
        bool seen = false;
        for (auto e : hdrs) if (e == h) { seen = true; break; }
        if (!seen) hdrs.push_back(h);
    }
    std::vector<const char*> out;
    for (auto h : hdrs) out.push_back(std::define_static_string(h));
    return std::define_static_array(out);
}

// Per-missing-type diagnostic message (P2741 constexpr static_assert message).
template <std::meta::info Ty>
consteval std::string_view stl_missing_caster_msg() {
    std::string s =
        "nb::reflect_: a bound signature uses the std type '";
    s += std::meta::display_string_of(Ty);
    s += "', whose nanobind type caster is not included in this translation unit. "
         "Add: #include <";
    s += stl_caster_header(Ty);
    s += ">  (or use the codegen path, which emits it automatically).";
    return std::string_view(std::define_static_string(s));
}

// Header-only diagnostic: for each std caster type used by R whose specialized
// caster is absent (make_caster falls back to the class-binding base caster),
// fail with a message naming the exact header to include. Runs only on types
// reflection identifies as std templates, so user classes never trip it.
template <std::meta::info R>
void check_stl_casters() {
    template for (constexpr auto ty :
                  std::define_static_array(required_stl_types(R))) {
        static_assert(!is_base_caster_v<make_caster<typename [:ty:]>>,
                      stl_missing_caster_msg<ty>());
    };
}

template <typename T>
void reflect_class(module_& m) {
    // Idempotent: skip if T is already registered. This makes binding
    // order-independent and lets a base be reached both directly (via the
    // namespace walk / another reflect_ argument) and transitively (below)
    // without triggering nanobind's "already registered" warning.
    if (type<T>().is_valid())
        return;

    constexpr auto name = entity_name<^^T>();

    // nanobind supports a single base class. When T has one or more public
    // bases, bind it as class_<T, Base> using the first public base. Any
    // additional (secondary) public bases cannot be real Python bases, so their
    // members are flattened directly onto T instead (see flatten_secondary_bases);
    // the only thing lost for those bases is the isinstance/issubclass relation.
    //
    // When a trampoline is registered for T, it is passed as the class_ "Alias"
    // (the extra template arg that nanobind distinguishes from the base via
    // is_base_of<T, Alias>), enabling Python subclasses to override C++ virtuals.
    constexpr bool HasBase = public_base_count<T>() > 0;
    constexpr bool HasTramp = has_reflect_trampoline<T>;

    if constexpr (HasBase) {
        // Ensure the base (and, recursively, its ancestors) is bound first --
        // nanobind requires the base registered before the derived type. The
        // guard above keeps this a no-op if the base is already bound.
        reflect_class<typename [:first_public_base<T>():]>(m);
    }

    // Construct the class_ with the right template arguments. The lambda's return
    // type is deduced from whichever if-constexpr branch is active. with_doc_extra
    // supplies the optional [[=r::doc]] string as a trailing const char* extra.
    auto cls = with_doc_extra<^^T>([&](auto&&... doc) {
        if constexpr (HasBase && HasTramp)
            return class_<T, typename [:first_public_base<T>():],
                          reflect_trampoline_t<T>>(m, name, doc...);
        else if constexpr (HasBase)
            return class_<T, typename [:first_public_base<T>():]>(m, name, doc...);
        else if constexpr (HasTramp)
            return class_<T, reflect_trampoline_t<T>>(m, name, doc...);
        else
            return class_<T>(m, name, doc...);
    });

    bind_class_contents<T>(cls);
    if constexpr (HasBase)
        flatten_secondary_bases<T>(cls);
    // Attach namespace-scope operators that take T as an operand (e.g. a free
    // operator+(T, T) or a scalar operator*(double, T)) as dunders on T.
    bind_free_operators<T>(cls);
    // If T is ostream-insertable, expose str(x) via __str__ (the stream operator itself is
    // not bound as a dunder — see involves_stream_type / bind_stream_str).
    bind_stream_str<T>(cls);
}

template <typename E>
void reflect_enum(module_& m) {
    constexpr auto name = entity_name<^^E>();

    auto e = with_doc_extra<^^E>([&](auto&&... doc) {
        return enum_<E>(m, name, doc...);
    });

    template for (constexpr auto val :
        std::define_static_array(std::meta::enumerators_of(^^E))) {
        constexpr auto vname =
            std::define_static_string(std::meta::identifier_of(val));
        e.value(vname, [:val:]);
    };
}

template <std::meta::info r>
void reflect_dispatch(module_& m) {
    if constexpr (std::meta::is_namespace(r)) {
        template for (constexpr auto mem :
            std::define_static_array(std::meta::members_of(
                r, std::meta::access_context::unchecked()))) {
            if constexpr (std::meta::is_template(mem)) {
                // A class/function template declaration: not bindable directly (only
                // its specializations are). Skip it here -- the specializations used
                // by the reflected set are discovered and bound by reflect_user_specs,
                // and others can be passed explicitly. Guarded first because
                // annotations_of (used by has_ann) is ill-formed on a template.
            } else if constexpr (has_ann<mem, reflect::skip>()) {
                // explicitly excluded -- bind nothing
            } else if constexpr (std::meta::is_type(mem)
                && std::meta::is_class_type(mem)) {
                reflect_class<typename [:mem:]>(m);
            } else if constexpr (std::meta::is_type(mem)
                && std::meta::is_enum_type(mem)) {
                reflect_enum<typename [:mem:]>(m);
            } else if constexpr (std::meta::is_function(mem)
                && !std::meta::is_template(mem)) {
                reflect_free_function<mem>(m);
            } else if constexpr (std::meta::is_namespace(mem)) {
                reflect_dispatch<mem>(m);
            }
        };
    } else if constexpr (std::meta::is_type(r)) {
        // Nest the type-kind checks under is_type: is_class_type/is_enum_type are
        // ill-formed on a non-type reflection (e.g. a function-template
        // specialization like ^^identity<int> passed directly to reflect_).
        if constexpr (std::meta::is_class_type(r))
            reflect_class<typename [:r:]>(m);
        else if constexpr (std::meta::is_enum_type(r))
            reflect_enum<typename [:r:]>(m);
    } else if constexpr (std::meta::is_function(r)) {
        reflect_free_function<r>(m);
    }
}

// Bind every user class-template specialization discovered in R's signatures (see
// required_user_specs). Specializations are not namespace members, so reflect_dispatch
// never reaches them; this pre-pass does. reflect_class's is_valid() guard makes each
// bind idempotent, so a spec also reached transitively (as a base/member) binds once.
template <std::meta::info R>
void reflect_user_specs(module_& m) {
    template for (constexpr auto ty :
                  std::define_static_array(required_user_specs(R))) {
        reflect_class<typename [:ty:]>(m);
    };
}

NAMESPACE_END(detail)

/// Automatically bind classes, enums, and namespaces via C++26 reflection.
///
/// Pass any mix of reflected types and namespaces:
///   nb::reflect_<^^Point, ^^Player, ^^Color, ^^game>(m);
///
template <std::meta::info... Rs>
void reflect_(module_& m) {
    // Diagnose any std type used in a bound signature whose <nanobind/stl/*.h>
    // caster was not included (a no-op when every needed caster is present).
    (detail::check_stl_casters<Rs>(), ...);
    // Bind user class-template specializations reachable from the signatures, then
    // the namespaces/classes/enums/functions themselves (order-independent: the
    // reflect_class is_valid() guard dedups specs reached by both passes).
    (detail::reflect_user_specs<Rs>(m), ...);
    (detail::reflect_dispatch<Rs>(m), ...);
}

NAMESPACE_END(NB_NAMESPACE)

/// Register a trampoline type for a polymorphic class so that reflect_ wires it
/// in as nanobind's class_ "Alias" (enabling Python subclasses to override its
/// C++ virtual functions). `Type` is the bound class; `Tramp` is a class derived
/// from `Type` using NB_TRAMPOLINE + NB_OVERRIDE[_PURE] for each virtual. Used by
/// both hand-written trampolines and the codegen fallback. Invoke at global scope.
#define NB_REFLECT_TRAMPOLINE(Type, Tramp)                                       \
    template <>                                                                  \
    struct nanobind::detail::reflect_trampoline<Type> { using type = Tramp; }

#endif // __has_include(<meta>)
