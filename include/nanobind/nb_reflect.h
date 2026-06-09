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

// Python name for R: an explicit reflect::rename, else its C++ identifier.
template <std::meta::info R>
consteval const char* entity_name() {
    return ann_string_or<R, ^^reflect::rename>(
        std::define_static_string(std::meta::identifier_of(R)));
}

// Docstring for R, or nullptr if none.
template <std::meta::info R>
consteval const char* entity_doc() {
    return ann_string_or<R, ^^reflect::doc>(nullptr);
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
    if constexpr (!has_ann<mem, reflect::skip>()) {
        constexpr auto name = entity_name<mem>();
        // Bind via a pointer-to-data-member (&[:mem:]) rather than getter/setter
        // lambdas: a lambda whose signature mentions the spliced member type
        // [:type_of(mem):] crashes the clang-p2996 mangler when passed to the
        // dependent `cls.def_*` call (placeholder-type mangling at parse time).
        with_data_extras<mem>([&](auto&&... e) {
            if constexpr (std::meta::is_const_type(std::meta::type_of(mem)))
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
    // free operators (operator@ has no identifier -- binding free operators as
    // reversed dunders is future work).
    if constexpr (!std::meta::has_ellipsis_parameter(fn) &&
                  std::meta::has_identifier(fn)) {
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

template <typename T, std::meta::info fn>
void reflect_bind_conversion(auto& cls) {
    // Use fixed concrete lambda return types (bool/long long/double) and cast the
    // conversion result: a spliced type in a lambda signature crashes the
    // clang-p2996 mangler.
    constexpr auto R = std::meta::return_type_of(fn);
    if constexpr (std::meta::is_same_type(R, ^^bool))
        cls.def("__bool__", [](T& self) -> bool { return (bool) self.[:fn:](); });
    else if constexpr (std::meta::is_integral_type(R))
        cls.def("__int__", [](T& self) -> long long { return (long long) self.[:fn:](); });
    else if constexpr (std::meta::is_floating_point_type(R))
        cls.def("__float__", [](T& self) -> double { return (double) self.[:fn:](); });
    // else: no standard numeric dunder -- skip.
}

// Route a public member function to the right binder. Operators and conversion
// functions have no identifier, so they must be detected before reflect_bind_method
// (which names the binding via identifier_of).
template <typename T, std::meta::info fn>
void reflect_bind_member_function(auto& cls) {
    if constexpr (has_ann<fn, reflect::skip>())
        return;  // explicitly excluded
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
            && !std::meta::is_copy_constructor(fn)
            && !std::meta::is_move_constructor(fn)) {
            reflect_bind_ctor<fn>(cls);
        }
    };

    // Bind data members
    template for (constexpr auto mem :
        std::define_static_array(std::meta::nonstatic_data_members_of(
            ^^T, std::meta::access_context::unchecked()))) {
        if constexpr (std::meta::is_public(mem)) {
            reflect_bind_member<T, mem>(cls);
        }
    };

    // Bind static data members
    template for (constexpr auto mem :
        std::define_static_array(std::meta::static_data_members_of(
            ^^T, std::meta::access_context::unchecked()))) {
        if constexpr (std::meta::is_public(mem)) {
            reflect_bind_static_member<T, mem>(cls);
        }
    };

    // Bind methods (instance, static, operators, conversions)
    template for (constexpr auto fn :
        std::define_static_array(std::meta::members_of(
            ^^T, std::meta::access_context::unchecked()))) {
        if constexpr (std::meta::is_function(fn)
            && std::meta::is_public(fn)
            && !std::meta::is_constructor(fn)
            && !std::meta::is_destructor(fn)
            && !std::meta::is_special_member_function(fn)) {
            reflect_bind_member_function<T, fn>(cls);
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
            if constexpr (has_ann<mem, reflect::skip>()) {
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
    } else if constexpr (std::meta::is_type(r)
        && std::meta::is_class_type(r)) {
        reflect_class<typename [:r:]>(m);
    } else if constexpr (std::meta::is_type(r)
        && std::meta::is_enum_type(r)) {
        reflect_enum<typename [:r:]>(m);
    } else if constexpr (std::meta::is_function(r)) {
        reflect_free_function<r>(m);
    }
}

NAMESPACE_END(detail)

/// Automatically bind classes, enums, and namespaces via C++26 reflection.
///
/// Pass any mix of reflected types and namespaces:
///   nb::reflect_<^^Point, ^^Player, ^^Color, ^^game>(m);
///
template <std::meta::info... Rs>
void reflect_(module_& m) {
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
