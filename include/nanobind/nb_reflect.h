/*
    nanobind/nb_reflect.h: Automatic binding via C++26 static reflection (P2996)

    Requires a C++26 compiler with P2996 support (GCC 16+, -std=c++26 -freflection)

    Copyright (c) 2025 Matthew Kolbe

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE file.
*/

#pragma once

#if __has_include(<meta>)

#include <meta>
#include "nanobind.h"
#include "nb_paren_init.h"
#include "nb_reflect_annotations.h"
#include "nb_reflect_match.h"
#include "stl/string.h"
#include <span>
#include <sstream>          // std::ostringstream for streamable -> __str__ (bind_stream_str)
#include <string_view>
#include <type_traits>
#include <vector>

NAMESPACE_BEGIN(NB_NAMESPACE)

/// Call-site exclusion marker: pass ^^exclude_<^^Tmpl, ^^Type, ^^ns, ...>
/// anywhere in the reflect_ pack to make the listed class templates (all their
/// specializations), concrete types, or whole namespaces OPAQUE to the binder:
/// they are never bound, never walked by the spec/caster discovery fixpoints,
/// never flattened as bases, and any member whose signature mentions one is
/// gracefully skipped. This is the out-of-line counterpart of the
/// [[=reflect::skip]] annotation, for code you do not own. It is what makes
/// expression-template libraries bindable at all: Eigen's facade methods return
/// ever-deeper new specializations (transpose() -> Transpose<Derived>, whose
/// facade has Transpose<Transpose<Derived>>, ...), so without exclusions the
/// discovery fixpoint diverges -- and walking some of those specs' members is
/// an outright hard error (NestByValue<expr>'s enable_if-gated members).
///
///   nb::reflect_<^^Eigen::Matrix<double,3,1>,
///                ^^nb::exclude_<^^Eigen::Transpose, ^^Eigen::Block>>(m);
template <std::meta::info... Excluded> struct exclude_ {};

/// Exclude an individual member BY NAME on a given (derived) class, without ever
/// materializing the member's reflection. This is the GCC-safe counterpart of
/// listing a member's exact reflection in exclude_<...>: GCC 16 instantiates a
/// constexpr member's BODY when its reflection is materialized as an NTTP (the
/// reflect_constant / define_static_array lift), so a member whose constexpr
/// body is lazily ill-formed for the bound specialization (Eigen's vector-only
/// accessors w()/x()/y()/z() on a 3x3 Matrix static_assert in their bodies)
/// cannot be named by reflection at all -- forming the marker is itself the hard
/// error. `Owner` is a CLASS reflection (safe to materialize; no body is
/// instantiated) and `Name` is the member identifier. Any member named `Name`
/// reachable while binding `Owner` (declared on it or any flattened base) is
/// dropped BEFORE the lift, so its body is never instantiated. Pass the markers
/// inside exclude_<...> exactly like entity reflections:
///
///   nb::exclude_<^^Eigen::Transpose,
///                ^^nb::exclude_member_<^^Eigen::Matrix<double,3,3>, "w">>
///
/// Honored identically by the constexpr and emit backends.
template <std::meta::info Owner, reflect::fixed_string Name>
struct exclude_member_ {};

/// Emit-mode trampoline selection markers (consumed by nb_reflect_emit.h;
/// inert configuration in the constexpr backend, where trampolines are
/// registered via NB_REFLECT_TRAMPOLINE / the two-stage codegen instead):
/// pass ^^trampoline_<^^Class...> in the pack to give exactly the listed
/// classes generated trampolines in the emitted source, or
/// ^^trampoline_all_ for every class with overridable virtuals (the
/// two-stage codegen tier's rule -- what a json-style run uses).
template <std::meta::info... Classes> struct trampoline_ {};
struct trampoline_all_ {};

/// Predicate-driven inclusion: pass ^^match_<^^scope, Matcher> in the
/// reflect_ pack to bind every member of `scope` (a namespace, walked
/// recursively) the matcher accepts, exactly as if each had been listed in
/// the pack explicitly. The matcher (see nb_reflect_match.h: the combinator
/// DSL, or any type satisfying nb::matcher) is consulted only for members the
/// binder could bind anyway -- classes, enums, free functions that survive
/// the usual skip/exclusion/completeness gates -- so accepting something
/// unbindable is harmless.
///
///   nb::reflect_<^^nb::match_<^^glm,
///                    nb::any_of_<nb::named_<"vec*">, nb::named_<"mat*">>>>(m);
template <std::meta::info Scope, matcher M> struct match_ {};

/// Predicate-driven exclusion: the predicate counterpart of listing entities
/// in exclude_. Every exclusion gate that consults the exclude_ list also
/// runs these matchers, so anything the matcher accepts -- entities, and
/// members via the signature/member gates -- becomes opaque:
///
///   nb::reflect_<^^Eigen::Matrix<double, 3, 1>,
///                ^^nb::exclude_if_<nb::in_namespace_<^^Eigen::internal>>,
///                ^^nb::exclude_if_<nb::named_<"*Solver*">>>(m);
///
/// Accepted at the top level of the pack or nested inside exclude_<...>.
/// NOT a replacement for exclude_member_: a member whose constexpr body is
/// lazily ill-formed must be dropped by name BEFORE its reflection is
/// lifted, and running any predicate that late is already too late.
template <matcher M> struct exclude_if_ {};

/// One explicit template-argument tuple for instantiate_ (below). Arguments
/// are reflections: types (^^int), constant values via nb::val_<3> /
/// std::meta::reflect_constant(...).
template <std::meta::info... Args> struct with_ {};

/// One axis of a product_: the candidate arguments for one template
/// parameter position.
template <std::meta::info... Es> struct set_ {};

/// Cross product of axes: product_<set_<^^int, ^^double>, set_<val_<2>,
/// val_<3>>> expands to every combination (int,2), (int,3), (double,2),
/// (double,3). Combinations that fail to substitute or complete are silently
/// skipped -- a grid legitimately has invalid corners.
template <typename... Sets> struct product_ {};

/// A constant value as a reflection, for NTTP arguments in with_/set_:
/// nb::val_<3>, nb::val_<glm::length_t(4)>.
template <auto V> inline constexpr std::meta::info val_ = std::meta::reflect_constant(V);

/// Default instantiations: pass ^^instantiate_<Target, ArgSets...> in the
/// reflect_ pack to mint class-template specializations in bulk and bind
/// them exactly as if each were listed explicitly (same discovery, naming,
/// and emit-lane treatment as writing ^^Box<int> in the pack). `Target` is a
/// class-template reflection, or a matcher TYPE reflection (e.g.
/// ^^nb::named_<"vec*">) applying the rule to every class template in the
/// pack's namespace seeds the matcher accepts. ArgSets are with_<...> tuples
/// and/or product_<set_<...>...> grids. An explicit with_ that fails to
/// substitute is a compile error (you named it; silence would lose it); a
/// product_ combination that fails is skipped.
///
///   nb::reflect_<^^nb::instantiate_<^^absl::btree_map,
///                    nb::with_<^^int, ^^std::string>,
///                    nb::product_<nb::set_<^^int, ^^std::string>,
///                                 nb::set_<^^int, ^^double>>>>(m);
template <std::meta::info Target, typename... ArgSets> struct instantiate_ {};

NAMESPACE_BEGIN(detail)

// --- Trampoline hook ---
//
// A trampoline (a class derived from T that overrides T's virtuals to forward
// into Python) cannot be synthesized in-language on a P2996 compiler, so
// reflect_ never generates one itself. Instead it consults this trait: if a trampoline type is
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
    return !std::meta::annotations_of_with_type(R, ^^A).empty();
}

// Value of R's first annotation of type A (precondition: has_ann<R, A>()).
template <std::meta::info R, typename A>
consteval A get_ann() {
    return [: std::meta::constant_of(std::meta::annotations_of_with_type(R, ^^A)[0]) :];
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

// Compiler-neutral CamelCase fragment for a builtin scalar type. Builtin types
// carry no identifier, so spec_camel_name otherwise falls back to
// display_string_of -- which spells the integral types differently across
// compilers (clang: "long long"/"long"/"short"/"unsigned long long";
// libstdc++/GCC: "long long int"/"long int"/"short int"/"long long unsigned
// int"). That made template-spec Python names diverge by backend
// (to_string<long long> -> "to_stringLonglong0" vs "to_stringLonglongint0").
// Mapping the canonical builtin types to a fixed fragment pins the Python
// names; returns an empty string for non-builtins (caller falls back). The
// fragments reproduce what the retired clang-p2996 lane produced, so every
// historical Python name (and corpus expectation) stays stable.
consteval std::string builtin_camel_fragment(std::meta::info type) {
    type = std::meta::remove_cvref(type);
    struct Entry { std::meta::info t; std::string_view frag; };
    const Entry table[] = {
        {^^bool, "Bool"},
        {^^char, "Char"},
        {^^signed char, "Signedchar"},
        {^^unsigned char, "Unsignedchar"},
        {^^char8_t, "Char8_t"},
        {^^char16_t, "Char16_t"},
        {^^char32_t, "Char32_t"},
        {^^short, "Short"},
        {^^unsigned short, "Unsignedshort"},
        {^^int, "Int"},
        {^^unsigned int, "Unsignedint"},
        {^^long, "Long"},
        {^^unsigned long, "Unsignedlong"},
        {^^long long, "Longlong"},
        {^^unsigned long long, "Unsignedlonglong"},
        {^^float, "Float"},
        {^^double, "Double"},
        {^^long double, "Longdouble"},
        {^^void, "Void"},
    };
    for (auto& e : table)
        if (type == e.t)
            return std::string(e.frag);
    // wchar_t deliberately omitted: clang's display_string_of spells it "int"
    // (-> "Int"), GCC spells it "wchar_t". Neither is depended on; leaving it to
    // the caller's fallback preserves each backend's historical name. (If a run
    // ever binds a wchar_t spec, pin it here.)
    return std::string();
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
            } else if (std::string frag = builtin_camel_fragment(a);
                       !frag.empty()) {
                out += frag;                             // compiler-neutral builtin
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

// True if `s` is usable as a Python identifier (what a class/attr name must be).
consteval bool is_python_identifier(std::string_view s) {
    if (s.empty() || (s[0] >= '0' && s[0] <= '9'))
        return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || c == '_'))
            return false;
    return true;
}

// Name for an entity with NO identifier: the C-style `typedef struct {...} name_t;`
// idiom (every tinyobjloader type) declares an ANONYMOUS record -- identifier_of is
// ill-formed on it -- but the typedef name for linkage survives in the display
// string ("name_t"). Use it when it forms a valid identifier; a truly anonymous
// type displays as "(unnamed struct at ...)" and yields nullptr, which the class/
// enum binders treat as a graceful skip rather than a hard error.
template <std::meta::info R>
consteval const char* anonymous_entity_name() {
    std::string_view s = std::meta::display_string_of(R);
    if (is_python_identifier(s))
        return std::define_static_string(s);
    return nullptr;
}

// Python name for R: an explicit reflect::rename, else its C++ identifier. For a
// template specialization, the CamelCase spec name (spec_camel_name): identifier_of
// is ill-formed on a specialization, and a single rename could not disambiguate two
// instantiations anyway, so rename is not consulted for specializations. An
// identifier-less entity (typedef'd anonymous record) falls back to its typedef
// name for linkage, or nullptr when there is none.
template <std::meta::info R>
consteval const char* entity_name() {
    if constexpr (std::meta::has_template_arguments(R))
        return spec_python_name(R);
    else if constexpr (!std::meta::has_identifier(R))
        return anonymous_entity_name<R>();
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
// Value-form presence check (reading the NAME needs the NTTP form above, which
// splices the annotation constant; presence only needs template matching).
consteval bool is_property_accessor(std::meta::info fn) {
    for (auto ann : std::meta::annotations_of(fn)) {
        auto t = std::meta::type_of(ann);
        if (std::meta::has_template_arguments(t)
            && std::meta::template_of(t) == ^^reflect::property)
            return true;
    }
    return false;
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

// True if R returns a borrowed indirection to a class type: a bare pointer OR
// an lvalue reference (after dealiasing; cv is irrelevant). References joined
// the set in wave 2: a T& return fell through to rv_policy::automatic, which
// nanobind resolves to COPY for references -- a runtime abort for
// non-copyable classes (tf::Taskflow&/tf::Executor& accessors) and a silently
// detached copy for copyable ones, both wrong for accessor semantics.
consteval bool returns_borrowed_class_indirection(std::meta::info fn) {
    auto t = std::meta::dealias(std::meta::return_type_of(fn));
    if (std::meta::is_lvalue_reference_type(t)) {
        auto r = std::meta::remove_cv(
            std::meta::dealias(std::meta::remove_reference(t)));
        return std::meta::is_class_type(r);
    }
    t = std::meta::remove_cv(t);
    if (!std::meta::is_pointer_type(t))
        return false;
    auto p = std::meta::remove_cv(
        std::meta::dealias(std::meta::remove_pointer(t)));
    return std::meta::is_class_type(p);
}

// The rv_policy a reflected callable actually binds with. An explicit
// [[=reflect::return_policy]] annotation always wins. Otherwise a bare
// class-pointer OR class-lvalue-reference return does NOT get nanobind's
// `automatic` (= take_ownership for pointers, COPY for references):
// reflected APIs overwhelmingly return BORROWED pointers (fluent builders
// returning self, accessors into owned containers -- CLI11's add_option()
// returns an Option* the App owns), and take_ownership double-frees them the
// moment Python collects the wrapper. The reflection default is
// reference_internal (pointee outlives self) on instance methods and reference
// on static/free functions; ownership-TRANSFERRING raw returns are the rare
// case and annotate take_ownership explicitly. Smart-pointer and by-value
// returns keep `automatic` -- their casters convey ownership correctly.
template <std::meta::info R>
consteval rv_policy effective_rv_policy() {
    constexpr rv_policy ann = ann_rv_policy<R>();
    if (ann != rv_policy::automatic)
        return ann;
    if (returns_borrowed_class_indirection(R)) {
        if (std::meta::is_static_member(R)
            || std::meta::is_namespace(std::meta::parent_of(R)))
            return rv_policy::reference;
        return rv_policy::reference_internal;
    }
    return rv_policy::automatic;
}

// Invoke `emit` with the def-extras implied by R's annotations: rv_policy always
// (automatic is a no-op), plus a docstring and/or keep_alive when present.
template <std::meta::info R, typename F>
void with_call_extras(F&& emit) {
    constexpr rv_policy pol = effective_rv_policy<R>();
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

// --- Shared binding-decision classifiers (value-form) ---
//
// Each classifier below is the single source of truth for one WHAT-to-bind
// decision, shared verbatim by the constexpr backend (reflect_) and the emit
// backend (nb_reflect_emit.h, which renders the same decisions as generated
// source for a production toolchain). Emission stays in the reflect_bind_* /
// emit_* layers; no binding decision may live only there.

/// How a non-static data member binds: not at all, read-only, or read-write.
/// skip: [[=reflect::skip]] members and C-array-typed members -- an array data
/// member (e.g. an internal `T storage[N]`, as in absl::FixedArray's storage
/// wrapper) has no def_rw-able form: the setter `c.*p = value` is ill-formed
/// for an array, and even def_ro can't expose it usefully. ro: def_rw's setter
/// assigns (`c.*p = value`), so a const or non-copy-assignable member (e.g. a
/// move-only `node_handle` inside absl's insert_return_type) is exposed
/// read-only via def_ro instead of breaking the build.
enum class data_route { skip, ro, rw };
consteval data_route data_member_route(std::meta::info mem) {
    auto t = std::meta::type_of(mem);
    if (!std::meta::annotations_of_with_type(mem, ^^reflect::skip).empty()
        || std::meta::is_array_type(t))
        return data_route::skip;
    if (std::meta::is_const_type(t) || !std::meta::is_copy_assignable_type(t))
        return data_route::ro;
    return data_route::rw;
}

/// How a static data member binds. const_member splits further at COMPILE time
/// in the consuming TU (value_as_nttp_probe below: by VALUE when the member is
/// constant-readable, by address otherwise); both backends compile the
/// identical probe, so the production compiler answers the same question for
/// generated source.
enum class static_data_route { skip, const_member, mutable_member };
consteval static_data_route static_member_route(std::meta::info mem) {
    if (!std::meta::annotations_of_with_type(mem, ^^reflect::skip).empty())
        return static_data_route::skip;
    return std::meta::is_const_type(std::meta::type_of(mem))
        ? static_data_route::const_member : static_data_route::mutable_member;
}

template <typename T, std::meta::info mem>
void reflect_bind_member(auto& cls) {
    constexpr data_route route = data_member_route(mem);
    if constexpr (route != data_route::skip) {
        constexpr auto name = entity_name<mem>();
        // Bind via a pointer-to-data-member (&[:mem:]): def_rw/def_ro take it
        // directly, with no synthesized getter/setter lambdas.
        with_data_extras<mem>([&](auto&&... e) {
            if constexpr (route == data_route::ro)
                cls.def_ro(name, &[:mem:], std::forward<decltype(e)>(e)...);
            else
                cls.def_rw(name, &[:mem:], std::forward<decltype(e)>(e)...);
        });
    }
}

/// Shape filter for instance methods (and the matrix the method binders below
/// model): volatile and rvalue-ref-qualified (&&) member functions, and
/// C-variadic functions, cannot bind meaningfully to a persistent Python
/// instance. The operator/member-template paths express the SAME filter
/// structurally, as the binder-spec completeness gate (`sizeof` on the
/// undefined reflect_method_binder primary is a substitution failure for any
/// unmodeled shape) -- this predicate and that gate must accept exactly the
/// same function types.
consteval bool method_shape_bindable(std::meta::info fn) {
    return !std::meta::is_volatile(fn)
        && !std::meta::is_rvalue_reference_qualified(fn)
        && !std::meta::is_vararg_function(fn);
}

// GCC 16 keeps a member's DEPENDENT noexcept-specifier deferred until
// something forces its resolution; matching such a function type against a
// binder-spec partial-specialization matrix then ICEs the compiler
// (most_specialized_partial_spec -> nothrow_spec_p assert; GCC-5,
// gcc16-proveout/probes/xfail_gcc5_deferred_noexcept_partial_spec.cpp --
// nlohmann basic_json's swap(reference) with its trait-expression noexcept
// was the field shape). is_noexcept resolves the specifier as a side effect,
// so every matrix dispatch takes the function type through this helper
// instead of splicing type_of directly.
consteval std::meta::info nb_fn_type_of(std::meta::info fn) {
    (void)std::meta::is_noexcept(fn);
    return std::meta::type_of(fn);
}

template <typename T, std::meta::info fn, typename FnType>
struct reflect_method_binder;

// A method's function type carries its cv-, ref-, and noexcept-qualifiers, and a
// partial specialization must match them exactly. Stamp out one specialization per
// supported qualifier combination (cv in {-, const} x ref in {-, &} x noexcept).
// The matrix is also the shape gate: an unmodeled qualifier combination matches
// no specialization, and `sizeof` on the undefined primary is the completeness
// probe the operator/member-template paths use.
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
    // Skipping unmodeled shapes leaves them simply unexposed rather than
    // breaking the build (an unmatched function type would select the
    // incomplete binder primary).
    if constexpr (method_shape_bindable(fn)) {
        using FnType = [:nb_fn_type_of(fn):];
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
    if constexpr (!std::meta::is_vararg_function(fn)) {
        using FnType = [:nb_fn_type_of(fn):];
        with_arg_call_extras<fn>([&](auto&&... e) {
            reflect_static_method_binder<fn, FnType>::bind(
                cls, entity_name<fn>(), std::forward<decltype(e)>(e)...);
        });
    }
}

// --- Static data members ---

// Probe target: `typename value_as_nttp_probe<(long double)([:mem:])>` is
// well-formed exactly when the member's VALUE is readable at compile time and
// arithmetic/enum -- i.e. when it can be bound by value with no ODR-use of the
// member. (In-class initializers are only permitted on integral/enum consts
// and constexpr members, so the arithmetic restriction loses nothing.) The
// NTTP is a FIXED type, not `auto`: the identical probe is emitted verbatim
// into generated source (BINDER-0020), the fixed type loses nothing, and an
// auto NTTP deduced from a dependent splice ICE'd the retired clang-p2996
// lane at parse (TC-0013).
template <long double> struct value_as_nttp_probe;

template <typename T, std::meta::info mem>
void reflect_bind_static_member(auto& cls) {
    constexpr static_data_route route = static_member_route(mem);
    if constexpr (route != static_data_route::skip) {
        constexpr auto name = entity_name<mem>();
        if constexpr (route == static_data_route::const_member) {
            // def_ro_static binds by ADDRESS (&[:mem:]), which ODR-uses the
            // member; an in-class-initialized `static const` with no
            // out-of-line definition (moodycamel::ConcurrentQueue's BLOCK_SIZE
            // et al.) then dies at link with an undefined symbol. When the
            // value is compile-time readable, bind it by VALUE instead: no
            // ODR-use, and a class constant surfaces as a plain read-only
            // attribute, the Python-natural form. The getter keeps a concrete
            // (handle) -> object signature; the splice stays in the body
            // (spliced-signature mangler rule).
            if constexpr (requires {
                    typename value_as_nttp_probe<(long double)([:mem:])>; })
                cls.def_prop_ro_static(name, [](::nanobind::handle) -> object {
                    // The copy into `v` applies lvalue-to-rvalue IMMEDIATELY,
                    // which is what avoids the ODR-use; passing [:mem:] to
                    // cast() by reference would re-introduce it.
                    auto v = [:mem:];
                    return cast(v);
                });
            else
                cls.def_ro_static(name, &[:mem:]);
        } else {
            // A non-const static must be inline (or out-of-line defined) to be
            // mutable at all; the address form is required for writes.
            cls.def_rw_static(name, &[:mem:]);
        }
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

// True if fn has a by-value parameter of a non-copy-constructible class type.
// nanobind's generic class caster produces a by-value argument with a COPY out of
// the caster's storage (`operator T()`), so binding such an overload is a hard
// compile error -- e.g. absl::Cord::Append(absl::CordBuffer) (move-only buffer
// taken by value). Such functions are gracefully skipped, mirroring the
// non-copy-assignable-member -> def_ro fallback (BINDER-0010, found via Abseil).
consteval bool has_move_only_by_value_param(std::meta::info fn) {
    for (auto p : std::meta::parameters_of(fn)) {
        auto t = std::meta::type_of(p);
        if (std::meta::is_reference_type(t) || std::meta::is_pointer_type(t))
            continue;
        auto c = std::meta::remove_cv(t);
        if (std::meta::is_class_type(c) && !std::meta::is_copy_constructible_type(c))
            return true;
    }
    return false;
}

// True if `t` is a shape nanobind has no Python representation for at all: a
// pointer to pointer (argv fronts like CLI11's ensure_utf8(char**)), a pointer
// to function, or a non-const lvalue reference to a pointer (the T*& out-param
// idiom -- toml++'s virtual is_homogeneous(node_type, node*&)). The method
// binder used to synthesize a forwarding lambda for these and nanobind's
// func_create then failed to match it: a TU-wide hard error instead of a skip.
consteval bool is_unbindable_shape(std::meta::info t) {
    t = std::meta::dealias(t);
    if (std::meta::is_lvalue_reference_type(t)) {
        auto r = std::meta::dealias(std::meta::remove_reference(t));
        if (std::meta::is_pointer_type(std::meta::remove_cv(r))
            && !std::meta::is_const_type(r))
            return true;                        // T*& out-param
        t = r;
    } else if (std::meta::is_rvalue_reference_type(t)) {
        t = std::meta::dealias(std::meta::remove_reference(t));
    }
    t = std::meta::remove_cv(t);
    if (std::meta::is_pointer_type(t)) {
        auto pte = std::meta::dealias(std::meta::remove_pointer(t));
        auto p = std::meta::remove_cv(pte);
        // cv-qualified void* (SQLiteCpp's Column::getBlob() -> const void*):
        // nanobind's capsule caster takes plain void* only, so the const
        // mismatch is a hard error in the forwarding lambda -- skip instead.
        if ((std::meta::is_const_type(pte) || std::meta::is_volatile_type(pte))
            && std::meta::is_same_type(p, ^^void))
            return true;
        return std::meta::is_pointer_type(p) || std::meta::is_function_type(p);
    }
    return false;
}

// Graceful-skip gate: any parameter or the return type is an unbindable shape.
// Sits alongside has_move_only_by_value_param on every binding path (and is
// mirrored in the STL caster-collection walk, which must not demand casters
// for a function the binder will never bind).
consteval bool has_unbindable_signature(std::meta::info fn) {
    if (!std::meta::is_constructor(fn) && !std::meta::is_destructor(fn)
        && is_unbindable_shape(std::meta::return_type_of(fn)))
        return true;
    for (auto p : std::meta::parameters_of(fn))
        if (is_unbindable_shape(std::meta::type_of(p)))
            return true;
    return false;
}

// Value-form of has_ann<fn, reflect::skip>() for the consteval signature walks,
// where the entity is a loop value rather than an NTTP.
consteval bool fn_skip_annotated(std::meta::info fn) {
    return !std::meta::annotations_of_with_type(fn, ^^reflect::skip).empty();
}

// (Entity proxies -- binding `using Base::f;` re-exports from PRIVATE bases,
// BINDER-0009 -- were REMOVED: P3687R1's poll 2b made `^^` on a using-declarator
// ill-formed and deferred shadow-declaration reflection past C++26, so the
// feature existed only as the clang-p2996 fork's -fentity-proxy-reflection
// extension. Public-base re-exports remain covered by inheritance/flattening;
// private-base re-exports no longer bind.)

// (Deduction guides need no special handling on GCC 16: a namespace walk
// enumerates them, but they classify as templates and are skipped by every
// routing classifier, and lifting a guide reflection into define_static_array
// works. The clang-p2996 fork needed a pre-lift stripping pass here -- its
// mangler ICE'd on guide reflections as template arguments, TC-0008.)

// True if a function template instantiates with ZERO explicit template
// arguments (every parameter defaulted / SFINAE-satisfied) and has no trailing
// parameter pack. This exactly captures the heterogeneous-lookup shape --
// template <class K = key_type> bool contains(const key_arg<K>&) -- that
// hash/btree container query APIs use, while excluding emplace/try_emplace
// (packs) and templates needing explicit arguments. The pack rejection probes
// whether the template would absorb ONE MORE argument than the default
// instantiation took; the probe argument is ^^int, NEVER ^^void (a void
// pack element forms void&& during substitution -- it crashed the retired
// clang lane outright, and no compiler is obliged to take it gracefully).
consteval bool fn_template_default_instantiable(std::meta::info tmpl) {
    if (!std::meta::can_substitute(tmpl, std::vector<std::meta::info>{}))
        return false;                       // some parameter lacks a default
    auto spec = std::meta::substitute(tmpl, std::vector<std::meta::info>{});
    auto args = std::meta::template_arguments_of(spec);
    if (args.empty())
        return false;                       // pure pack (emplace<>)
    args.push_back(^^int);
    return !std::meta::can_substitute(tmpl, args);  // absorbs one more => pack
}

template <std::meta::info fn>
void reflect_free_function(module_& m) {
    // Skip C-variadic free functions (their function type matches no binder), and
    // free operators (operator@ has no identifier) -- the latter are bound as class
    // dunders by bind_free_operators during their operand types' class binding. A
    // function-template specialization (identity<int>) also has no identifier, but it
    // IS bindable, so admit it via has_template_arguments (entity_name then derives
    // the CamelCase spec name from the template).
    if constexpr (!std::meta::is_deleted(fn) &&
                  !std::meta::is_vararg_function(fn) &&
                  !has_move_only_by_value_param(fn) &&
                  !has_unbindable_signature(fn) &&
                  (std::meta::has_identifier(fn) ||
                   std::meta::has_template_arguments(fn))) {
        using FnType = [:nb_fn_type_of(fn):];
        with_arg_call_extras<fn>([&](auto&&... e) {
            reflect_free_fn_binder<fn, FnType>::bind(
                m, entity_name<fn>(), std::forward<decltype(e)>(e)...);
        });
    }
}

// --- Constructors ---

// reflect_init (the parens-construction nb::init counterpart, BINDER-0026)
// lives in nb_paren_init.h: it is reflection-free and shared with the source
// the emit backend generates for production toolchains.

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
        cls.def(reflect_init<>());
    } else {
        constexpr auto params = ctor_param_infos<ctor>();
        // Attach nb::arg("name") per ctor parameter so Python callers can use
        // keywords (e.g. T(i=1, j=2)); see with_arg_call_extras for the rule.
        if constexpr (fn_any_param_named<ctor>())
            cls.def(reflect_init<typename [:std::meta::type_of(params[Is]):]...>(),
                    ::nanobind::arg(param_name<ctor, Is>())...);
        else
            cls.def(reflect_init<typename [:std::meta::type_of(params[Is]):]...>());
    }
}

// Eligibility (skip-annotation, move-only by-value params, unbindable shapes,
// exclusions, ...) is decided by ctor_binds at the call site in
// bind_class_contents; this only expands the parameter list.
template <std::meta::info ctor>
void reflect_bind_ctor(auto& cls) {
    reflect_bind_ctor_expand<ctor>(cls, std::make_index_sequence<ctor_param_count<ctor>()>{});
}

// --- Inheritance ---

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

// nanobind models a single base, and reflect_class wires one only when an
// ancestor along the first-public-base chain is itself bound (`PyBase`, from
// python_base_of; ^^void when there is none). Everything else in T's public-base
// subtree must have its members "flattened" directly onto T. This returns
// exactly those base types: T's whole public-base subtree minus PyBase and the
// part already covered by it (exposed via the Python MRO). The subtraction makes
// it correct for diamonds and for bases nested under PyBase (no member is bound
// twice); with PyBase == ^^void the ENTIRE subtree flattens (e.g. an internal
// facade chain like flat_hash_map -> raw_hash_map -> raw_hash_set). Note an
// unbound link BETWEEN T and an indirect PyBase is not in PyBase's subtree, so
// its members correctly flatten onto T.
consteval std::vector<std::meta::info> flatten_bases_vec(std::meta::info cls,
                                                         std::meta::info pybase) {
    std::vector<std::meta::info> all, covered, result;
    collect_public_base_subtree(cls, all);
    if (pybase != ^^void) {
        covered.push_back(pybase);
        collect_public_base_subtree(pybase, covered);
    }
    for (auto t : all)
        if (!info_vec_contains(covered, t))
            result.push_back(t);
    return result;
}

template <typename T, std::meta::info PyBase>
consteval std::vector<std::meta::info> flatten_bases_vec() {
    return flatten_bases_vec(^^T, PyBase);
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

/// How a free unary/binary operator attaches to class `cls` (a shared
/// classifier, see data_member_route's section note). The forward dunder binds
/// when cls is the LEFT operand (or the sole operand of a unary); the reversed
/// dunder when cls is the RIGHT operand of a binary whose operand types differ
/// (the symmetric same-type case is already covered by the forward binding) --
/// that is what makes `2.0 * vec` work when only the right operand is a bound
/// type. nullptr fields mean "no binding on that side".
struct free_op_plan {
    const char* fwd_dunder;
    const char* rev_dunder;
    bool inplace;
};
consteval free_op_plan plan_free_operator(std::meta::info cls,
                                          std::meta::info fn) {
    free_op_plan plan{nullptr, nullptr, false};
    auto op = std::meta::operator_of(fn);
    plan.inplace = is_inplace_operator(op);
    auto params = std::meta::parameters_of(fn);
    auto operand = [](std::meta::info p) {
        return std::meta::dealias(
            std::meta::remove_cvref(std::meta::type_of(p)));
    };
    auto self = std::meta::dealias(cls);
    if (params.size() == 1) {
        if (std::meta::is_same_type(operand(params[0]), self))
            plan.fwd_dunder = operator_dunder(op, 0);
    } else if (params.size() == 2) {
        auto a = operand(params[0]), b = operand(params[1]);
        if (std::meta::is_same_type(a, self))
            plan.fwd_dunder = operator_dunder(op, 1);
        if (std::meta::is_same_type(b, self) && !std::meta::is_same_type(a, b))
            plan.rev_dunder = operator_reversed_dunder(op);
    }
    return plan;
}

template <typename T, std::meta::info fn>
void reflect_bind_operator(auto& cls) {
    constexpr auto op = std::meta::operator_of(fn);
    constexpr const char* dunder =
        operator_dunder(op, std::meta::parameters_of(fn).size());
    if constexpr (dunder != nullptr && !std::meta::is_vararg_function(fn)) {
        using FnType = [:nb_fn_type_of(fn):];
        // The volatile/&&-qualified shapes are excluded by the binder-spec
        // completeness gate (no partial specialization exists for them; sizeof
        // on the undefined primary is a substitution failure) rather than by
        // decl predicates -- one gate for every shape the binder matrix does
        // not model, with no duplicated qualifier logic to drift.
        if constexpr (requires { sizeof(reflect_method_binder<T, fn, FnType>); }) {
            // is_operator() makes mismatched-argument calls return NotImplemented
            // rather than raising TypeError, matching Python operator semantics.
            if constexpr (is_inplace_operator(op)) {
                // In-place operators return *this; rv_policy::reference returns
                // the existing Python object (preserving identity), not a copy.
                reflect_method_binder<T, fn, FnType>::bind(
                    cls, dunder, is_operator(), rv_policy::reference);
            } else {
                reflect_method_binder<T, fn, FnType>::bind(
                    cls, dunder, is_operator());
            }
        }
    }
}

// Among T's public, non-template, non-skipped integral conversion operators (bool
// excluded -- it feeds __bool__), the reflection of the one with the widest result
// type, or ^^void if there is none. Multiple integral conversions would otherwise
// each bind __int__ with the last-bound silently winning, which can pick an
// arbitrarily narrow result (absl::int128's operator char/int/long/...).
consteval std::meta::info widest_integral_conversion(std::meta::info cls) {
    std::meta::info best = ^^void;
    std::size_t best_size = 0;
    for (auto fn : std::meta::members_of(cls, std::meta::access_context::unchecked())) {
        if (!std::meta::is_function(fn) || std::meta::is_template(fn))
            continue;
        if (!std::meta::is_public(fn) || !std::meta::is_conversion_function(fn))
            continue;
        // A deleted conversion must not win the contest: it never binds, and the
        // surviving narrower one would then fail the equality test in
        // reflect_bind_conversion and __int__ would silently vanish (BINDER-0012).
        if (!std::meta::annotations_of_with_type(fn, ^^reflect::skip).empty()
            || std::meta::is_deleted(fn))
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

template <typename T>
consteval std::meta::info widest_integral_conversion() {
    return widest_integral_conversion(^^T);
}

/// The dunder a conversion operator binds as, or nullptr (skip). bool ->
/// __bool__; the WIDEST integral conversion -> __int__ (the others have no
/// Python equivalent, see widest_integral_conversion); floating -> __float__.
consteval const char* conversion_dunder(std::meta::info cls, std::meta::info fn) {
    auto R = std::meta::return_type_of(fn);
    if (std::meta::is_same_type(R, ^^bool))
        return "__bool__";
    if (std::meta::is_integral_type(R))
        return fn == widest_integral_conversion(cls) ? "__int__" : nullptr;
    if (std::meta::is_floating_point_type(R))
        return "__float__";
    return nullptr;  // no standard numeric dunder
}

template <typename T, std::meta::info fn>
void reflect_bind_conversion(auto& cls) {
    // Fixed concrete lambda return types (bool/long long/double), casting the
    // conversion result: they are exactly what __bool__/__int__/__float__
    // need to produce.
    constexpr const char* d = conversion_dunder(^^T, fn);
    if constexpr (d != nullptr) {
        constexpr auto R = std::meta::return_type_of(fn);
        // The splice is hoisted out of the lambda bodies into a
        // pointer-to-member: GCC treats a lambda whose body splices an info
        // NTTP as consteval-only, and these capture-less concrete-signature
        // lambdas must decay to the plain function pointer nanobind wants.
        constexpr auto mp = &[:fn:];
        if constexpr (std::meta::is_same_type(R, ^^bool))
            cls.def(d, [](T& self) -> bool { return (bool) (self.*mp)(); });
        else if constexpr (std::meta::is_integral_type(R))
            cls.def(d, [](T& self) -> long long { return (long long) (self.*mp)(); });
        else
            cls.def(d, [](T& self) -> double { return (double) (self.*mp)(); });
    }
}

// --- Call-site exclusions (nb::exclude_) ---
//
// compute_excluded collects the union of every ^^nb::exclude_<...> marker in the
// reflect_ pack. Entries may be class templates (excluding every specialization),
// concrete types, or namespaces (excluding everything declared inside,
// transitively). is_excluded_entity / type_mentions_excluded / fn_mentions_excluded
// are the query side, used by the consteval discovery walks (an excluded type is
// opaque, exactly like a [[=reflect::skip]]-annotated one) and by the bind-time
// gates (a member whose signature mentions an excluded entity is skipped).

consteval bool is_exclude_marker(std::meta::info r) {
    if (!std::meta::is_type(r))
        return false;
    // Dealias: a marker reached through a `using` alias (or a consteval helper
    // returning the alias) must still be recognized -- an unrecognized marker
    // would silently exclude NOTHING.
    r = std::meta::dealias(r);
    return std::meta::is_class_type(r)
        && std::meta::has_template_arguments(r)
        && std::meta::template_of(r) == ^^exclude_;
}

/// True for the emit-mode trampoline markers (trampoline_<...> /
/// trampoline_all_): configuration, not binding seeds -- every walk skips
/// them exactly like exclude_ markers.
consteval bool is_trampoline_marker(std::meta::info r) {
    if (!std::meta::is_type(r))
        return false;
    r = std::meta::dealias(r);
    if (r == ^^trampoline_all_)
        return true;
    return std::meta::is_class_type(r)
        && std::meta::has_template_arguments(r)
        && std::meta::template_of(r) == ^^trampoline_;
}

// Recognizers for the matcher/instantiation markers (same dealias discipline
// as is_exclude_marker: a marker reached through an alias must still be
// recognized).
consteval bool is_marker_of(std::meta::info r, std::meta::info tmpl) {
    if (!std::meta::is_type(r))
        return false;
    r = std::meta::dealias(r);
    return std::meta::is_class_type(r)
        && std::meta::has_template_arguments(r)
        && std::meta::template_of(r) == tmpl;
}
consteval bool is_match_marker(std::meta::info r) {
    return is_marker_of(r, ^^match_);
}
consteval bool is_exclude_if_marker(std::meta::info r) {
    return is_marker_of(r, ^^exclude_if_);
}
consteval bool is_instantiate_marker(std::meta::info r) {
    return is_marker_of(r, ^^instantiate_);
}

/// True for every CONFIGURATION marker in a reflect_ pack -- anything the raw
/// entity walks must skip rather than treat as a binding seed. (match_ and
/// instantiate_ DO produce seeds, but only through the seeds_of expansion;
/// at the raw-walk level they are configuration like the rest.)
consteval bool is_config_marker(std::meta::info r) {
    return is_exclude_marker(r) || is_trampoline_marker(r)
        || is_match_marker(r) || is_exclude_if_marker(r)
        || is_instantiate_marker(r);
}

template <std::meta::info... Rs>
consteval std::vector<std::meta::info> compute_excluded() {
    std::vector<std::meta::info> out;
    auto collect = [&](std::meta::info r) {
        if (is_exclude_marker(r))
            for (auto a : std::meta::template_arguments_of(r))
                out.push_back(std::meta::extract<std::meta::info>(a));
    };
    (collect(Rs), ...);
    return out;
}

// Memoized once per reflect_ pack (same pattern as bind_set_v below).
template <std::meta::info... Rs>
inline constexpr auto excluded_v = std::define_static_array(compute_excluded<Rs...>());

// --- Name-based member exclusions (nb::exclude_member_<Owner, "name">) ---
//
// A by-name rule drops any member named `name` reached while binding class
// `owner` (declared on it or any flattened base), without ever materializing
// the member's reflection. See exclude_member_ for why GCC needs this. The
// rules are consulted in liftable_class_members (the single pre-lift drop
// point), so a lazily-ill-formed constexpr member never reaches the
// define_static_array lift that would instantiate its body. Each rule is
// resolved (owner reflection + name as a define_static_string pointer) at
// collection time, where the marker is a constant; the {info, const char*}
// struct is structural, so define_static_array carries the rule table.
struct member_excl_rule {
    std::meta::info owner;        // the DERIVED class the rule applies to
    const char* name;             // the member identifier/dunder to drop
};

consteval bool is_exclude_member_marker(std::meta::info r) {
    if (!std::meta::is_type(r))
        return false;
    r = std::meta::dealias(r);
    return std::meta::is_class_type(r)
        && std::meta::has_template_arguments(r)
        && std::meta::template_of(r) == ^^exclude_member_;
}

// Resolve one exclude_member_<Owner, "name"> marker into a rule. `Marker` is a
// constant here, so its fixed_string NTTP can be spliced; the name characters
// are copied into a define_static_string so the rule is storable. Reads only the
// marker's template arguments -- never the named member -- so nothing is
// instantiated.
template <std::meta::info Marker>
consteval member_excl_rule resolve_exclude_member() {
    constexpr auto owner = std::meta::extract<std::meta::info>(
        std::meta::template_arguments_of(std::meta::dealias(Marker))[0]);
    std::string_view name(
        [:std::meta::template_arguments_of(std::meta::dealias(Marker))[1]:].data);
    return {owner, std::define_static_string(name)};
}

// Collect every exclude_member_<Owner, "name"> rule appearing inside one
// exclude_ marker.
template <std::meta::info R>
consteval void collect_excluded_members(std::vector<member_excl_rule>& out) {
    if constexpr (is_exclude_marker(R))
        template for (constexpr auto a :
                      std::define_static_array(
                          std::meta::template_arguments_of(R))) {
            constexpr auto e = std::meta::extract<std::meta::info>(a);
            if constexpr (is_exclude_member_marker(e))
                out.push_back(resolve_exclude_member<e>());
        };
}

template <std::meta::info... Rs>
consteval std::vector<member_excl_rule> compute_excluded_members() {
    std::vector<member_excl_rule> out;
    (collect_excluded_members<Rs>(out), ...);
    return out;
}

// Memoized once per reflect_ pack: the by-name member-exclusion rule table
// ({info, const char*} is structural, so define_static_array carries it).
template <std::meta::info... Rs>
inline constexpr auto excluded_members_v =
    std::define_static_array(compute_excluded_members<Rs...>());

// True if member `m` is dropped by an exclude_member_ rule applying to derived
// class `derived`. The rule name matches either the member's plain identifier
// (w/x/y/z/resize/...) or -- for operator members, which have no identifier --
// its Python dunder (operator[] is named "__getitem__"). The dunder route lets
// an operator whose constexpr body is lazily ill-formed (Eigen's vector-only
// operator[] on a 3x3) be dropped by name without forming its reflection.
consteval bool member_excluded_by_name(
        std::meta::info m, std::meta::info derived,
        std::span<const member_excl_rule> rules) {
    if (rules.empty())
        return false;
    std::string_view id;
    if (std::meta::has_identifier(m))
        id = std::meta::identifier_of(m);
    else if (std::meta::is_function(m) && std::meta::is_operator_function(m)) {
        const char* d = operator_dunder(std::meta::operator_of(m),
                                        std::meta::parameters_of(m).size());
        if (!d)
            return false;
        id = d;
    } else
        return false;
    for (auto& r : rules) {
        if (id != r.name)
            continue;
        // The rule fires when binding the owner spec itself, OR when binding any
        // class in the owner's public-base subtree directly (Eigen's facade
        // bases -- DenseCoeffsBase<Vec3,1> etc. -- are bound as REAL Python
        // bases, and the rule must reach the member there too, not only on the
        // flattened path where derived == owner).
        if (r.owner == derived)
            return true;
        std::vector<std::meta::info> owners{r.owner};
        collect_public_base_subtree(r.owner, owners);
        if (info_vec_contains(owners, derived))
            return true;
    }
    return false;
}

consteval bool info_span_contains(std::span<const std::meta::info> v,
                                  std::meta::info x) {
    for (auto e : v)
        if (e == x)
            return true;
    return false;
}

// A pointer to a consteval invoker for matcher type M. Such pointers are
// formable and callable INSIDE constant evaluation only; they must never be
// persisted (no define_static_array / NTTP / static storage), which is why
// exclusion_set below is rebuilt per evaluation rather than memoized.
using matcher_fn = bool (*)(std::meta::info);
template <typename M>
consteval bool matcher_invoke(std::meta::info r) { return M{}(r); }

/// The exclusion context threaded through every discovery walk and bind-time
/// gate: the nb::exclude_-listed entities plus the nb::exclude_if_ matcher
/// predicates. Predicates participate at exactly the points the listed span
/// is consulted, so an entity a matcher accepts is opaque in precisely the
/// ways a listed entity is. The listed half stays memoized (excluded_v); the
/// predicate half is rebuilt per constant evaluation by excluded_q.
struct exclusion_set {
    std::span<const std::meta::info> listed = {};
    std::vector<matcher_fn> preds = {};

    consteval exclusion_set() = default;
    consteval exclusion_set(std::span<const std::meta::info> l) : listed(l) {}
    consteval exclusion_set(const std::vector<std::meta::info>& l)
        : listed(l) {}
    consteval exclusion_set(std::span<const std::meta::info> l,
                            std::vector<matcher_fn> p)
        : listed(l), preds(std::move(p)) {}

    consteval bool empty() const { return listed.empty() && preds.empty(); }

    // Direct hit on one reflection: listed, or accepted by any predicate.
    // (No parent/template resolution here -- is_excluded_entity adds that,
    // for the listed half and predicates alike.)
    consteval bool contains(std::meta::info e) const {
        if (info_span_contains(listed, e))
            return true;
        for (auto p : preds)
            if (p(e))
                return true;
        return false;
    }
};

// True if entity `e` is excluded: listed itself (or matched by an exclude_if_
// predicate), a specialization of a listed template, or declared
// (transitively) inside a listed namespace or class. Predicates run at each
// of the same points: on the entity, its template, and every enclosing scope
// -- so exclude_if_<named_<"internal">> excludes a namespace's contents just
// like listing the namespace does.
consteval bool is_excluded_entity(std::meta::info e,
                                  const exclusion_set& ex) {
    if (ex.empty())
        return false;
    if (ex.contains(e))
        return true;
    // Resolve a specialization to its template; only scoped entities (classes,
    // enums, namespaces, templates) have a parent chain worth walking.
    bool scoped = std::meta::is_namespace(e);
    if (std::meta::is_type(e)) {
        if (std::meta::has_template_arguments(e)) {
            auto tmpl = std::meta::template_of(e);
            if (ex.contains(tmpl))
                return true;
            e = tmpl;
            scoped = true;
        } else if (std::meta::is_class_type(e) || std::meta::is_enum_type(e)) {
            scoped = true;
        }
    }
    if (!scoped || e == ^^::)
        return false;
    for (auto p = std::meta::parent_of(e);; p = std::meta::parent_of(p)) {
        if (ex.contains(p))
            return true;
        if (p == ^^::)
            return false;
    }
}

// Collect the matcher invoker from one pack element: a top-level
// exclude_if_<M> marker, or any exclude_if_<M> nested inside exclude_<...>
// (the same two placements exclude_member_ supports). The marker type is an
// NTTP here, so the matcher type argument splices out statically.
template <std::meta::info R>
consteval void collect_exclude_if_preds(std::vector<matcher_fn>& out) {
    if constexpr (is_exclude_if_marker(R)) {
        constexpr auto margs = std::define_static_array(
            std::meta::template_arguments_of(std::meta::dealias(R)));
        out.push_back(&matcher_invoke<typename [:margs[0]:]>);
    } else if constexpr (is_exclude_marker(R)) {
        template for (constexpr auto a :
                      std::define_static_array(
                          std::meta::template_arguments_of(std::meta::dealias(R)))) {
            constexpr auto e = std::meta::extract<std::meta::info>(a);
            if constexpr (is_exclude_if_marker(e)) {
                constexpr auto margs = std::define_static_array(
                    std::meta::template_arguments_of(std::meta::dealias(e)));
                out.push_back(&matcher_invoke<typename [:margs[0]:]>);
            }
        };
    }
}

template <std::meta::info... Rs>
consteval std::vector<matcher_fn> excluded_matchers() {
    std::vector<matcher_fn> out;
    (collect_exclude_if_preds<Rs>(out), ...);
    return out;
}

// The full exclusion context for a reflect_ pack: the memoized listed half
// (excluded_v) plus the exclude_if_ predicates. Rebuilt per constant
// evaluation -- a pointer to a consteval function is not a persistable
// constant, so this CANNOT be memoized in a variable template; every gate
// that previously read excluded_v<Rs...> calls this instead. The rebuild is
// O(#markers) and is noise next to the walks it gates.
template <std::meta::info... Rs>
consteval exclusion_set excluded_q() {
    return {excluded_v<Rs...>, excluded_matchers<Rs...>()};
}

// True if `type` (after stripping cv/ref/pointers) is, or mentions anywhere in
// its template-argument tree, an entity the binder cannot represent: an
// nb::exclude_-listed one, or a user class-template specialization that
// cannot be COMPLETED in this TU (template forward-declared here, defined in
// a header the user never included -- Eigen's MatrixExponentialReturnValue
// lives in unsupported/, yet MatrixBase::exp() returns it regardless).
// Binding a member whose signature carries such a spec would instantiate
// nanobind's caster on the incomplete type, a hard error: this is the
// bind-time half of BINDER-0014 (the discovery half is the is_complete_type
// gate in is_user_class_template_spec).
consteval bool type_mentions_excluded(std::meta::info type,
                                      const exclusion_set& ex) {
    // Dealias: a member typedef (Eigen's `DenseBase<D>::iterator` =
    // internal::pointer_based_stl_iterator<D>) must not hide the entity it
    // names from the exclusion/completeness tests below.
    type = std::meta::dealias(std::meta::remove_cvref(type));
    while (std::meta::is_pointer_type(type))
        type = std::meta::dealias(
            std::meta::remove_cvref(std::meta::remove_pointer(type)));
    if (is_excluded_entity(type, ex))
        return true;
    if (!std::meta::is_type(type))
        return false;
    // A forward-declared, never-defined PLAIN class (pugixml's pImpl structs,
    // `struct xml_node_struct;` behind an xml_node_struct* accessor) is as
    // unrepresentable as a non-completable spec: routing it to nanobind
    // instantiates typeid/is_base_of on the incomplete type, a hard error.
    // The template-spec analogue is the gate below (BINDER-0014).
    if (std::meta::is_class_type(type)
        && !std::meta::has_template_arguments(type)
        && !std::meta::is_complete_type(type))
        return true;
    if (!std::meta::has_template_arguments(type))
        return false;
    if (std::meta::is_class_type(type)) {
        auto tmpl = std::meta::template_of(type);
        if (std::meta::has_identifier(tmpl) && !is_in_std(tmpl)
            && !std::meta::is_complete_type(type))
            return true;
    }
    for (auto arg : std::meta::template_arguments_of(type))
        if (std::meta::is_type(arg) && type_mentions_excluded(arg, ex))
            return true;
    return false;
}

// True if function `fn`'s signature (return + parameter types; parameters only
// for a constructor/destructor, whose return type is not reflectable) mentions
// an excluded or non-completable entity -- or if `fn` ITSELF is listed. The
// latter is the per-member escape hatch for unowned code: a member whose BODY
// is lazily ill-formed for the bound specialization (Eigen's Matrix(x, y, z)
// ctor static_asserts size == 3 in its body, declared on every Matrix) cannot
// be detected by any reflection query, so the binding author lists its exact
// reflection (obtained via members_of) in the exclude_ marker.
consteval bool fn_mentions_excluded(std::meta::info fn,
                                    const exclusion_set& ex) {
    if (ex.contains(fn))
        return true;
    if (!std::meta::is_constructor(fn) && !std::meta::is_destructor(fn)
        && type_mentions_excluded(std::meta::return_type_of(fn), ex))
        return true;
    for (auto p : std::meta::parameters_of(fn))
        if (type_mentions_excluded(std::meta::type_of(p), ex))
            return true;
    return false;
}

// Gate for a data member: listed itself, or of an excluded type.
consteval bool data_member_excluded(std::meta::info mem,
                                    const exclusion_set& ex) {
    return ex.contains(mem)
        || type_mentions_excluded(std::meta::type_of(mem), ex);
}

// Gate for a member function TEMPLATE: listed itself, or its default
// instantiation (the only thing reflect_bind_member_template binds) mentions an
// excluded entity. Templates that do not default-instantiate are skipped by
// the binder anyway.
consteval bool member_template_mentions_excluded(std::meta::info tmpl,
                                                 const exclusion_set& ex) {
    if (ex.contains(tmpl))
        return true;
    if (!std::meta::is_function_template(tmpl)
        || std::meta::is_constructor_template(tmpl)
        || std::meta::is_conversion_function_template(tmpl)
        || !fn_template_default_instantiable(tmpl))
        return false;
    return fn_mentions_excluded(
        std::meta::substitute(tmpl, std::vector<std::meta::info>{}), ex);
}

// --- Constructor classifiers (shared, see data_member_route's section note) ---

/// True when class `cls` may bind declared constructors at all. An abstract
/// class WITHOUT a trampoline (e.g. an interface base bound only so its
/// concrete descendants have a Python base, like spdlog::sinks::sink) gets
/// none: nb::init would have to instantiate it, ill-formed -- Python-side
/// instantiation raises TypeError instead (BINDER-0011). With a registered
/// trampoline the ctors DO bind: nb::init then constructs the Alias, which is
/// exactly how a Python subclass overriding pure virtuals is instantiated.
consteval bool class_constructs(std::meta::info cls, bool has_trampoline) {
    return !std::meta::is_abstract_type(cls) || has_trampoline;
}

/// True when Python-side COPY construction binds (BINDER-0013, found via
/// tl::expected): the ctor pass skips copy/move ctors (a bound init<T&&> would
/// gut its Python source object), but copying a bound instance is part of any
/// copyable type's real API -- init<const T&> binds when T is publicly
/// copy-constructible. Trampolined classes are excluded: nb::init
/// placement-news the Alias for Python-derived instances, and a trampoline has
/// no (const T&) constructor. Move ctors never bind.
consteval bool binds_copy_ctor(std::meta::info cls, bool has_trampoline) {
    return !std::meta::is_abstract_type(cls) && !has_trampoline
        && std::meta::is_copy_constructible_type(cls);
}

/// The full eligibility gate for one declared constructor. `T() = delete;` is
/// enumerable but must not bind: init<> would call it, a TU-wide hard error
/// (BINDER-0012, tl::unexpected<E>). Constructor templates cannot reflect and
/// are skipped.
consteval bool ctor_binds(std::meta::info fn, const exclusion_set& ex) {
    return std::meta::is_constructor(fn)
        && std::meta::is_public(fn)
        && !std::meta::is_deleted(fn)
        && !std::meta::is_template(fn)
        && !std::meta::is_copy_constructor(fn)
        && !std::meta::is_move_constructor(fn)
        && !fn_skip_annotated(fn)
        && !has_move_only_by_value_param(fn)
        && !has_unbindable_signature(fn)
        && !fn_mentions_excluded(fn, ex);
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
// The forwarding lambda takes the real Ret/P0/P1 template parameters from the
// function-type partial specialization and splices the call in its body.

template <typename T, std::meta::info fn, typename FnType>
struct reflect_free_operator_binder;

#define NB_REFLECT_DEFINE_FREE_OP_BINDER(QUALS)                                    \
    template <typename T, std::meta::info fn, typename Ret,                        \
              typename P0, typename P1>                                            \
    struct reflect_free_operator_binder<T, fn, Ret(P0, P1) QUALS> {                \
        static void bind(auto& cls) {                                              \
            constexpr free_op_plan plan = plan_free_operator(^^T, fn);             \
            if constexpr (plan.fwd_dunder != nullptr) {                            \
                if constexpr (plan.inplace)                                        \
                    cls.def(plan.fwd_dunder, [](P0 a, P1 b) -> Ret {               \
                        return [:fn:](a, b); },                                    \
                        is_operator(), rv_policy::reference);                      \
                else                                                               \
                    cls.def(plan.fwd_dunder, [](P0 a, P1 b) -> Ret {               \
                        return [:fn:](a, b); }, is_operator());                    \
            }                                                                      \
            if constexpr (plan.rev_dunder != nullptr)                              \
                cls.def(plan.rev_dunder, [](P1 b, P0 a) -> Ret {                   \
                    return [:fn:](a, b); }, is_operator());                        \
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
        static void bind(auto& cls) {                                              \
            constexpr free_op_plan plan = plan_free_operator(^^T, fn);             \
            if constexpr (plan.fwd_dunder != nullptr)                              \
                cls.def(plan.fwd_dunder, [](P0 a) -> Ret { return [:fn:](a); },    \
                        is_operator());                                            \
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
        || std::meta::is_deleted(fn)   // `operator==(T, T) = delete;` (BINDER-0012)
        || std::meta::is_vararg_function(fn)
        || involves_stream_type(fn)
        || has_move_only_by_value_param(fn)
        || has_unbindable_signature(fn))
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
// actually streamable); the `oss << self` call resolves the operator by ADL in
// the lambda body.
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

template <typename T, std::meta::info... Rs>
void bind_free_operators(auto& cls) {
    constexpr auto scope = std::meta::parent_of(^^T);
    if constexpr (std::meta::is_namespace(scope)) {
        template for (constexpr auto fn :
            std::define_static_array(std::meta::members_of(
                scope, std::meta::access_context::unchecked()))) {
            if constexpr (is_bindable_free_operator<fn>()
                && !has_ann<fn, reflect::skip>()
                && !fn_mentions_excluded(fn, excluded_q<Rs...>())) {
                using FnType = [:nb_fn_type_of(fn):];
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
        if constexpr (is_property_setter<fn>() && !std::meta::is_deleted(fn)) {
            if (std::string_view(prop_name<fn>()) == gname)
                found = fn;
        }
    };
    return found;
}

// Bind one property from its getter and (optional) setter, via pointer-to-
// member-functions (&[:getter:] / &[:setter:]) -- like data members use
// &[:mem:]. getter/setter are template parameters (not captured locals): a
// std::meta::info is a consteval-only type and cannot be captured by the
// runtime extras lambda. The getter's return-policy / doc
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

// True if T declares a public, non-template, non-deleted INSTANCE method named
// `name`. nanobind cannot make one Python attribute both an instancemethod and
// a staticmethod: binding a member and a same-named static (SQLiteCpp's
// Database::getHeaderInfo -- const member + static overload) compiles clean
// and then ABORTS at import during type finalization. The instance method
// wins; the shadowed static is skipped.
consteval bool instance_method_shadows(std::meta::info cls, std::string_view name) {
    for (auto m : std::meta::members_of(
             cls, std::meta::access_context::unchecked())) {
        if (!std::meta::is_function(m) || std::meta::is_template(m))
            continue;
        if (std::meta::is_public(m) && !std::meta::is_deleted(m)
            && !std::meta::is_static_member(m)
            && !std::meta::is_constructor(m) && !std::meta::is_destructor(m)
            && !std::meta::is_special_member_function(m)
            && std::meta::has_identifier(m)
            && std::meta::identifier_of(m) == name)
            return true;
    }
    return false;
}

template <typename T>
consteval bool instance_method_shadows(std::string_view name) {
    return instance_method_shadows(^^T, name);
}

// Routing for a public, non-deleted, non-template member function of `cls`
// (a shared classifier, see data_member_route's section note). Operators and
// conversion functions have no identifier, so they are detected before the
// method route (which names the binding via identifier_of).
enum class member_fn_route { skip, oper, conversion, static_method, method };
consteval member_fn_route classify_member_fn(std::meta::info cls,
                                             std::meta::info fn) {
    if (fn_skip_annotated(fn)                // explicitly excluded
        || has_move_only_by_value_param(fn)  // class caster cannot produce it
        || has_unbindable_signature(fn)      // ptr-to-ptr / T*& / fn-ptr shape
        || is_property_accessor(fn))         // handled by the property pass
        return member_fn_route::skip;
    if (std::meta::is_operator_function(fn))
        return member_fn_route::oper;
    if (std::meta::is_conversion_function(fn))
        return member_fn_route::conversion;
    if (std::meta::is_static_member(fn)) {
        // A static shadowed by a same-named instance method is skipped
        // (nanobind aborts at import binding both under one name, BINDER-0024).
        if (!std::meta::has_identifier(fn)
            || instance_method_shadows(cls, std::meta::identifier_of(fn)))
            return member_fn_route::skip;
        return member_fn_route::static_method;
    }
    if (std::meta::has_identifier(fn))
        return member_fn_route::method;
    return member_fn_route::skip;  // nameless non-operator (e.g. literal operator)
}

template <typename T, std::meta::info fn>
void reflect_bind_member_function(auto& cls) {
    constexpr member_fn_route route = classify_member_fn(^^T, fn);
    if constexpr (route == member_fn_route::oper)
        reflect_bind_operator<T, fn>(cls);
    else if constexpr (route == member_fn_route::conversion)
        reflect_bind_conversion<T, fn>(cls);
    else if constexpr (route == member_fn_route::static_method)
        reflect_bind_static_method<fn>(cls);
    else if constexpr (route == member_fn_route::method)
        reflect_bind_method<T, fn>(cls);
}

/// The template's default instantiation -- the only thing the binder binds for
/// a member function template. (Same-named sibling templates need the
/// member_tmpl_mangle_hint disambiguator below when used as dispatcher NTTPs;
/// GCC-8.)
consteval std::meta::info default_spec(std::meta::info tmpl) {
    return std::meta::substitute(tmpl, std::vector<std::meta::info>{});
}

/// Routing for a public member function TEMPLATE (a shared classifier, see
/// data_member_route's section note). Every spec-level gate folds in here:
/// is_deleted / the skip annotation must be asked of the substituted SPEC --
/// on a Template reflection they silently answer false (BINDER-0012) -- and
/// the qualifier-matrix filter (method_shape_bindable, the value twin of the
/// binder-spec completeness gate) decides the instance-method route.
enum class member_tmpl_route { skip, oper, static_method, method };
consteval member_tmpl_route classify_member_template(std::meta::info cls,
                                                     std::meta::info tmpl) {
    if (!fn_template_default_instantiable(tmpl))
        return member_tmpl_route::skip;
    auto spec = default_spec(tmpl);
    if (fn_skip_annotated(spec) || std::meta::is_deleted(spec)
        || has_move_only_by_value_param(spec)
        || has_unbindable_signature(spec))
        return member_tmpl_route::skip;
    if (std::meta::is_operator_function(spec))
        return member_tmpl_route::oper;
    if (!std::meta::has_identifier(tmpl)
        || std::meta::is_vararg_function(spec))
        return member_tmpl_route::skip;
    if (std::meta::is_static_member(spec)) {
        if (instance_method_shadows(cls, std::meta::identifier_of(tmpl)))
            return member_tmpl_route::skip;
        return member_tmpl_route::static_method;
    }
    return method_shape_bindable(spec) ? member_tmpl_route::method
                                       : member_tmpl_route::skip;
}

// Mangling disambiguator for the dispatcher below. GCC 16 mangles a
// TEMPLATE-kind reflection NTTP by NAME only, so the dispatcher's
// instantiations for two same-named sibling member templates (a
// const/non-const heterogeneous-lookup `at` pair -- ankerl unordered_dense's
// shape) get ONE assembly symbol: "symbol ... is already defined" at
// assembly time (GCC-8, gcc16-proveout/probes/xfail_gcc8_*.cpp; the
// clang-p2996 fork had the same family as TC-0004/TC-0009, fixed in its
// mangler). Folding the default-instantiation SPEC (a Declaration-kind
// reflection, mangled by signature) into the template-id keeps sibling
// instantiations distinct on both compilers. A non-default-instantiable
// sibling contributes ^^void: it can collide only with another uninstantiable
// same-named sibling, and those instantiations are both empty (route ==
// skip), so the fold stays harmless there.
consteval std::meta::info member_tmpl_mangle_hint(std::meta::info tmpl) {
    return fn_template_default_instantiable(tmpl) ? default_spec(tmpl)
                                                  : ^^void;
}

// Bind a member function template via its default instantiation. The Python
// name is the TEMPLATE's identifier (`contains`, not `containsInt` -- the
// defaulted argument is an implementation detail), so multiple such templates
// and their non-template overloads stack as normal Python overloads.
template <typename T, std::meta::info tmpl,
          std::meta::info Disambig = member_tmpl_mangle_hint(tmpl)>
void reflect_bind_member_template(auto& cls) {
    constexpr member_tmpl_route route = classify_member_template(^^T, tmpl);
    if constexpr (route == member_tmpl_route::oper) {
        reflect_bind_operator<T, default_spec(tmpl)>(cls);
    } else if constexpr (route != member_tmpl_route::skip) {
        constexpr auto spec = default_spec(tmpl);
        using FnType = [:nb_fn_type_of(spec):];
        constexpr const char* nm = std::define_static_string(
            std::meta::identifier_of(tmpl));
        if constexpr (route == member_tmpl_route::static_method) {
            with_arg_call_extras<spec>([&](auto&&... e) {
                reflect_static_method_binder<spec, FnType>::bind(
                    cls, nm, std::forward<decltype(e)>(e)...);
            });
        } else {
            with_arg_call_extras<spec>([&](auto&&... e) {
                reflect_method_binder<T, spec, FnType>::bind(
                    cls, nm, std::forward<decltype(e)>(e)...);
            });
        }
    }
}

/// Kind-routing for one members_of entry of a class walk (a shared classifier,
/// see data_member_route's section note); used identically by the direct-member
/// pass (bind_class_contents) and the base-flattening pass. fn: a public,
/// non-deleted, non-template, non-special member function (operators and
/// conversions included; reflect_bind_member_function / classify_member_fn
/// route further). tmpl: a public member function template (binds via its
/// default instantiation when one exists).
/// Each kind is gated against the pack's nb::exclude_ set.
enum class class_member_kind { skip, fn, tmpl };
// The class-member list every `template for` walk lifts via
// define_static_array. On GCC 16, lifting the reflection of an
// IMPLICITLY-declared special member into static storage instantiates that
// member's DEFINITION -- for a member that is declared but ill-formed to
// instantiate (the vector<unique_ptr<T>> member shape: vector's copy
// ctor/operator= are declared unconditionally and poisoned on instantiation)
// the lift itself is a hard error. Implicit copy/move ctors and assignment
// operators are never bound (ctor_binds / classify_class_member skip them),
// so they are dropped BEFORE the lift; the implicit default constructor,
// which the init<> path does consume, is kept.
// GCC-2-family: a member function whose reflection is lifted into
// define_static_array is body-instantiated by GCC 16 if it is `constexpr`
// (the lift forces GCC to determine usability in constant evaluation). For a
// member whose constexpr body is LAZILY ill-formed in this specialization --
// tl::expected<void,E>'s `constexpr operator->()` calls `valptr()` ->
// `addressof(this->m_val)`, and the void storage base has no m_val -- that
// turns the lift itself into a hard error, even though no binding pass would
// ever consume the member (probe xfail_gcc6_constexpr_lift.cpp).
// The general, surface-preserving workaround: do not lift a member function
// that NO binding pass can consume. liftable_class_members feeds the ctor /
// method / member-template / property passes; a non-template, non-special,
// non-ctor/dtor member function is consumed by the method pass ONLY when it is
// public AND (not an operator, or an operator that maps to a Python dunder).
// A non-public function, or a public operator with no dunder mapping
// (operator-> / unary operator* / address-of / prefix ++/-- / <=> / ...), is
// always skipped downstream, so dropping it before the lift changes no bound
// surface while keeping its lazily-ill-formed constexpr body out of static
// storage. (Mapped operators, accessors, and every other kind stay in the
// list -- their bodies are well-formed wherever they are reachable.)
consteval bool never_bound_plain_member_fn(std::meta::info m) {
    if (!std::meta::is_function(m)
        || std::meta::is_template(m)
        || std::meta::is_constructor(m)
        || std::meta::is_destructor(m)
        || std::meta::is_special_member_function(m))
        return false;                       // ctor/dtor/template/special: other passes
    if (!std::meta::is_public(m))
        return true;                        // private/protected: never bound anywhere
    if (std::meta::is_operator_function(m)) {
        // Unmapped member operator (operator-> etc.) is skipped at bind time.
        std::size_t arity = std::meta::parameters_of(m).size();
        return operator_dunder(std::meta::operator_of(m), arity) == nullptr;
    }
    return false;                           // ordinary public method: the method pass binds it
}

// `cls` is the class whose members are enumerated for the lift; `derived` is
// the class currently being bound (== cls for own members, the derived type
// when flattening a base) -- the key a by-name rule applies to. `rules` carries
// the nb::exclude_member_<...> by-name drops. Dropping a by-name-excluded member
// here, before the lift, keeps a lazily-ill-formed constexpr body out of the
// define_static_array lift (it is the single pre-lift drop point).
consteval std::vector<std::meta::info> liftable_class_members(
        std::meta::info cls, std::meta::info derived,
        std::span<const member_excl_rule> rules) {
    std::vector<std::meta::info> out;
    for (auto m : std::meta::members_of(
             cls, std::meta::access_context::unchecked())) {
        if (member_excluded_by_name(m, derived, rules))
            continue;
        if (std::meta::is_function(m)
            && std::meta::is_special_member_function(m)
            && !std::meta::is_user_declared(m)
            && !std::meta::is_default_constructor(m))
            continue;
        if (never_bound_plain_member_fn(m))
            continue;
        out.push_back(m);
    }
    return out;
}

// The lifted member list, memoized once per (class, derived, pack): the
// constructor, method, and property passes (and their emit twins) all walk the
// same list, and recomputing it per pass repeats the members_of walk plus -- in
// the presence of exclude_member_ rules -- a base-subtree scan per member. A
// variable template also satisfies GCC's expansion-statement-range rule (a
// constexpr local would be rejected inside a template, the emit_indices_v
// pattern).
template <std::meta::info Cls, std::meta::info Derived, std::meta::info... Rs>
inline constexpr auto liftable_members_v = std::define_static_array(
    liftable_class_members(Cls, Derived, excluded_members_v<Rs...>));

consteval class_member_kind classify_class_member(
        std::meta::info fn, const exclusion_set& ex) {
    if (std::meta::is_function(fn)
        && std::meta::is_public(fn)
        && !std::meta::is_deleted(fn)
        && !std::meta::is_template(fn)
        && !std::meta::is_constructor(fn)
        && !std::meta::is_destructor(fn)
        && !std::meta::is_special_member_function(fn))
        return fn_mentions_excluded(fn, ex) ? class_member_kind::skip
                                            : class_member_kind::fn;
    if (std::meta::is_function_template(fn)
        && std::meta::is_public(fn)
        && !std::meta::is_constructor_template(fn)
        && !std::meta::is_conversion_function_template(fn))
        return member_template_mentions_excluded(fn, ex) ? class_member_kind::skip
                                                         : class_member_kind::tmpl;
    return class_member_kind::skip;
}

// Bind the constructors, data members, static data members, and methods declared
// directly in T onto an already-created class_ object. Inherited members are not
// re-bound here -- they are exposed automatically through the Python base type.
// `Rs...` is the whole reflect_ pack: every loop below gates each member against
// the pack's nb::exclude_ set (a signature mentioning an excluded entity skips).
template <typename T, std::meta::info... Rs>
void bind_class_contents(auto& cls) {
    // Bind constructors; class_constructs / ctor_binds (the shared classifiers)
    // hold the BINDER-0011/0012 rationale.
    if constexpr (class_constructs(^^T, has_reflect_trampoline<T>)) {
        template for (constexpr auto fn : liftable_members_v<^^T, ^^T, Rs...>) {
            if constexpr (ctor_binds(fn, excluded_q<Rs...>())) {
                reflect_bind_ctor<fn>(cls);
            }
        };
    }

    // Python-side COPY construction (BINDER-0013; rationale on binds_copy_ctor).
    if constexpr (binds_copy_ctor(^^T, has_reflect_trampoline<T>))
        cls.def(reflect_init<const T&>());

    // Bind data members. Skip unnamed members (anonymous union/struct fields, e.g. glm's
    // x/y/z/w swizzle aliasing): identifier_of() is ill-formed on them, and a pointer-to-member
    // of the enclosing class cannot be formed for an anonymous-union member anyway.
    template for (constexpr auto mem :
        std::define_static_array(std::meta::nonstatic_data_members_of(
            ^^T, std::meta::access_context::unchecked()))) {
        if constexpr (std::meta::is_public(mem) && std::meta::has_identifier(mem)
            && !data_member_excluded(mem, excluded_q<Rs...>())) {
            reflect_bind_member<T, mem>(cls);
        }
    };

    // Bind static data members
    template for (constexpr auto mem :
        std::define_static_array(std::meta::static_data_members_of(
            ^^T, std::meta::access_context::unchecked()))) {
        if constexpr (std::meta::is_public(mem) && std::meta::has_identifier(mem)
            && !data_member_excluded(mem, excluded_q<Rs...>())) {
            reflect_bind_static_member<T, mem>(cls);
        }
    };

    // Bind methods (instance, static, operators, conversions). Property accessors are
    // skipped here (see classify_member_fn) and bound by the pass below. Member
    // function templates bind via their default instantiation when every template
    // parameter is defaulted; others skip. Deleted functions are filtered on every
    // path: public + enumerable, but calling one is a hard error (BINDER-0012).
    template for (constexpr auto fn : liftable_members_v<^^T, ^^T, Rs...>) {
        constexpr class_member_kind kind =
            classify_class_member(fn, excluded_q<Rs...>());
        if constexpr (kind == class_member_kind::fn)
            reflect_bind_member_function<T, fn>(cls);
        else if constexpr (kind == class_member_kind::tmpl)
            reflect_bind_member_template<T, fn>(cls);
    };

    // Bind properties from [[=r::property{"name"}]] getter/setter pairs. The kind
    // guard must gate the is_property_getter<fn>() call itself: that helper queries
    // annotations_of(fn), which is ill-formed on a template, so a templated member must
    // be excluded *before* it is instantiated (a nested if constexpr, not an &&
    // short-circuit).
    template for (constexpr auto fn : liftable_members_v<^^T, ^^T, Rs...>) {
        if constexpr (classify_class_member(fn, excluded_q<Rs...>())
                      == class_member_kind::fn) {
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
template <typename T, std::meta::info Base, std::meta::info... Rs>
void flatten_base_members(auto& cls) {
    template for (constexpr auto mem :
        std::define_static_array(std::meta::nonstatic_data_members_of(
            Base, std::meta::access_context::unchecked()))) {
        if constexpr (std::meta::is_public(mem)
            && !data_member_excluded(mem, excluded_q<Rs...>())) {
            reflect_bind_member<T, mem>(cls);
        }
    };

    template for (constexpr auto mem :
        std::define_static_array(std::meta::static_data_members_of(
            Base, std::meta::access_context::unchecked()))) {
        if constexpr (std::meta::is_public(mem)
            && !data_member_excluded(mem, excluded_q<Rs...>())) {
            reflect_bind_static_member<T, mem>(cls);
        }
    };

    // Same kind-routing as bind_class_contents. Member function templates with
    // all-defaulted parameters bind via their default instantiation -- this is
    // where flat_hash_map's heterogeneous contains/find/erase/operator[] live
    // (declared on the flattened raw_hash_map/raw_hash_set ancestry).
    template for (constexpr auto fn : liftable_members_v<Base, ^^T, Rs...>) {
        constexpr class_member_kind kind =
            classify_class_member(fn, excluded_q<Rs...>());
        if constexpr (kind == class_member_kind::fn)
            reflect_bind_member_function<T, fn>(cls);
        else if constexpr (kind == class_member_kind::tmpl)
            reflect_bind_member_template<T, fn>(cls);
    };
}

// Flatten every public base nanobind cannot model as the real Python base
// (everything in T's public-base subtree outside PyBase's) onto T's class_.
// See flatten_bases_vec for which types these are. An EXCLUDED base is opaque:
// its members are not flattened at all (the per-member gates would catch most
// of them anyway, but exclusion means "pretend this type does not exist").
template <typename T, std::meta::info PyBase, std::meta::info... Rs>
void flatten_unmodeled_bases(auto& cls) {
    template for (constexpr auto base :
        std::define_static_array(flatten_bases_vec<T, PyBase>())) {
        if constexpr (!is_excluded_entity(base, excluded_q<Rs...>()))
            flatten_base_members<T, base, Rs...>(cls);
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

// True if e belongs to the standard library implementation: any enclosing
// namespace is named "std" (handles libc++'s inline namespace, where
// std::vector is really std::__1::vector) or is a RESERVED implementation
// namespace. The latter matters on libstdc++, which keeps vendor internals
// OUTSIDE std -- std::vector<T>::iterator dealiases to
// __gnu_cxx::__normal_iterator<T*, vector<T>> -- and such types must take the
// type-caster path, never the user-class-binding path (the user-spec discovery
// fixpoint would otherwise drag every container's iterator into the bind set).
consteval bool is_in_std(std::meta::info e) {
    for (auto p = std::meta::parent_of(e); p != ^^::; p = std::meta::parent_of(p)) {
        if (!std::meta::has_identifier(p))
            continue;
        std::string_view n = std::meta::identifier_of(p);
        if (n == "std")
            return true;
        // Namespace names reserved to the implementation (double underscore, or
        // underscore + capital): __gnu_cxx, __gnu_debug, __cxxabiv1, _LIBCPP_*.
        if (n.size() >= 2 && n[0] == '_'
            && (n[1] == '_' || (n[1] >= 'A' && n[1] <= 'Z')))
            return true;
    }
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
    if (!std::meta::annotations_of_with_type(e, ^^reflect::skip).empty())
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
    // std::chrono types (std::chrono is nested under std, which the is_in_std
    // walk already accepts). Found by spdlog's emit lane: logger::log's
    // system_clock::time_point overload bound with a RAW chrono signature in
    // the generated TU while the constexpr TU hand-included the caster -- the
    // surface diff's exact purpose (D.surface, not a behavioral failure).
    if (n == "time_point" || n == "duration")
        return "nanobind/stl/chrono.h";
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
                                 std::vector<std::meta::info>& visited,
                                 const exclusion_set& ex = {}) {
    type = std::meta::remove_cvref(type);
    // A [[=reflect::skip]] type is opaque: do not walk its template arguments for
    // STL casters (e.g. nlohmann's output_adapter<uint8_t> resolves its default
    // StringType to the un-castable std::basic_string<unsigned char>).
    // An nb::exclude_-listed type is opaque the same way.
    if (is_skip_annotated(type) || is_excluded_entity(type, ex))
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
            collect_stl_types(arg, out, visited, ex);
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
consteval void collect_own_stl_member_types(std::meta::info owner,
                                            std::vector<std::meta::info>& out,
                                            std::vector<std::meta::info>& visited,
                                            const exclusion_set& ex = {}) {
    for (auto mem : std::meta::members_of(owner, std::meta::access_context::unchecked())) {
        if (!std::meta::is_public(mem))
            continue;
        // Mirror the bind-path skip predicates: a function the binder will never
        // bind ([[=reflect::skip]] / by-value move-only param, BINDER-0010 /
        // a signature mentioning an nb::exclude_-listed entity) must not demand
        // casters for its signature either -- e.g. spdlog's
        // set_formatter(std::unique_ptr<formatter>) is skipped at bind time but
        // used to static_assert here for the missing unique_ptr caster.
        if (std::meta::is_template(mem)) {
            // A member function template that binds via its default instantiation
            // (reflect_bind_member_template) contributes that instantiation's
            // signature types to the bound surface.
            if (std::meta::is_function_template(mem)
                && !std::meta::is_constructor_template(mem)
                && !std::meta::is_conversion_function_template(mem)
                && fn_template_default_instantiable(mem)) {
                auto spec = std::meta::substitute(mem, std::vector<std::meta::info>{});
                if (fn_skip_annotated(spec) || std::meta::is_deleted(spec)
                    || has_move_only_by_value_param(spec)
                    || has_unbindable_signature(spec)
                    || fn_mentions_excluded(spec, ex))
                    continue;
                collect_stl_types(std::meta::return_type_of(spec), out, visited, ex);
                for (auto p : std::meta::parameters_of(spec))
                    collect_stl_types(std::meta::type_of(p), out, visited, ex);
            }
            continue;
        }
        if (std::meta::is_function(mem) && !std::meta::is_destructor(mem)
            && !std::meta::is_deleted(mem)
            && !fn_skip_annotated(mem) && !has_move_only_by_value_param(mem)
            && !has_unbindable_signature(mem)
            && !fn_mentions_excluded(mem, ex)) {
            if (!std::meta::is_constructor(mem))
                collect_stl_types(std::meta::return_type_of(mem), out, visited, ex);
            for (auto p : std::meta::parameters_of(mem))
                collect_stl_types(std::meta::type_of(p), out, visited, ex);
        }
    }
    for (auto mem : std::meta::nonstatic_data_members_of(
             owner, std::meta::access_context::unchecked()))
        if (std::meta::is_public(mem) && !ex.contains(mem))
            collect_stl_types(std::meta::type_of(mem), out, visited, ex);
    for (auto mem : std::meta::static_data_members_of(
             owner, std::meta::access_context::unchecked()))
        if (std::meta::is_public(mem) && !ex.contains(mem))
            collect_stl_types(std::meta::type_of(mem), out, visited, ex);
}
// A class's bound surface includes its public-base subtree (exposed via the real
// Python base or flattened by flatten_unmodeled_bases), so std types in base
// member signatures need casters too -- e.g. flat_hash_map's surface is almost
// entirely raw_hash_map/raw_hash_set members mentioning std::pair et al.
consteval void collect_class_stl_types(std::meta::info cls,
                                       std::vector<std::meta::info>& out,
                                       std::vector<std::meta::info>& visited,
                                       const exclusion_set& ex = {}) {
    collect_own_stl_member_types(cls, out, visited, ex);
    std::vector<std::meta::info> bases;
    collect_public_base_subtree(cls, bases);
    for (auto b : bases)
        if (!is_excluded_entity(b, ex))   // an excluded base is not flattened
            collect_own_stl_member_types(b, out, visited, ex);
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
                                       std::vector<std::meta::info>& visited,
                                       const exclusion_set& ex = {}) {
    if (std::meta::is_namespace(r)) {
        for (auto mem : std::meta::members_of(r, std::meta::access_context::unchecked())) {
            if (is_excluded_entity(mem, ex))
                continue;  // nb::exclude_-listed class/namespace: opaque
            if (std::meta::is_type(mem) && std::meta::is_class_type(mem)) {
                // A forward-declared namespace member (`struct Opaque;`,
                // BINDER-0019) has no members to walk -- skip, like every
                // other walk does.
                if (std::meta::is_complete_type(mem))
                    collect_class_stl_types(mem, out, visited, ex);
            }
            else if (std::meta::is_function(mem) && !std::meta::is_template(mem)
                     && !std::meta::is_deleted(mem)
                     && !fn_skip_annotated(mem)
                     && !has_move_only_by_value_param(mem)
                     && !has_unbindable_signature(mem)
                     && !fn_mentions_excluded(mem, ex)) {
                collect_stl_types(std::meta::return_type_of(mem), out, visited, ex);
                for (auto p : std::meta::parameters_of(mem))
                    collect_stl_types(std::meta::type_of(p), out, visited, ex);
            } else if (std::meta::is_namespace(mem)
                       // An alias is a shorthand, not contents: following it
                       // pulls the whole target namespace into the walk.
                       && !std::meta::is_namespace_alias(mem))
                collect_scope_stl_types(mem, out, visited, ex);
        }
    } else if (std::meta::is_type(r) && std::meta::is_class_type(r)) {
        collect_class_stl_types(r, out, visited, ex);
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
    type = std::meta::dealias(std::meta::remove_cvref(type));
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
    if (!is_in_std(tmpl))
        // A spec that cannot be COMPLETED can be neither bound nor walked --
        // members_of on it is a constant-evaluation hard error. The shape: a
        // template forward-declared in this TU with its definition in a header
        // the user never included (Eigen's SparseView<Derived>, returned by
        // DenseBase::sparseView() but defined only in <Eigen/SparseCore>).
        // is_complete_type instantiates when a definition is reachable and
        // gracefully answers false when none is (BINDER-0014).
        return std::meta::is_complete_type(type);
    return false;
}

// Append the user class-template specializations reachable from `type` -- the
// (pointer/ref/cv-unwrapped) type itself if it is one, or, recursively through
// NON-user templates, any user spec in their args (std::vector<Foo<int>> yields
// Foo<int>; std::pair<It, bool> yields It) -- to `out`, de-duplicated.
//
// Reachability rule: a user spec found in a signature is bound, but its OWN
// template arguments do not qualify anything for binding -- policy arguments
// (raw_hash_map<FlatHashMapPolicy<K,V>, Hash, Eq, Alloc>'s policies) never
// appear in callable signatures and must not be dragged in. The spec's genuine
// interface types still surface: the fixpoint in required_user_specs walks every
// discovered spec's own member signatures (Box<Box<int>>'s `Box<int> value`
// member surfaces Box<int>).
//
// `visited` memoizes already-walked types to avoid redundant re-descent (same performance
// purpose as in collect_stl_types; not needed for termination -- arg trees are finite).
consteval void collect_user_specs_from_type(std::meta::info type,
                                            std::vector<std::meta::info>& out,
                                            std::vector<std::meta::info>& visited,
                                            const exclusion_set& ex = {}) {
    // Dealias (like type_mentions_excluded): spdlog's stdout_sink_base
    // reaches signatures through member typedefs, and the sugar would
    // otherwise defeat both the exclusion test and the is_complete_type gate
    // (on unpatched toolchains is_complete_type is itself sugar-blind,
    // TC-0012) -- and the spec must be pushed under its CANONICAL identity.
    type = std::meta::dealias(std::meta::remove_cvref(type));
    while (std::meta::is_pointer_type(type))
        type = std::meta::dealias(
            std::meta::remove_cvref(std::meta::remove_pointer(type)));
    // An nb::exclude_-listed entity is opaque: neither bound nor recursed into
    // (its template args are policy-like internals, e.g. Eigen's functor args).
    // The check is the full MENTIONS test: a spec parameterized by an excluded
    // type anywhere in its argument tree (EigenBase<PermutationWrapper<M>>) is
    // transitively tainted -- it wraps something that does not exist for the
    // binder, and every member mentioning it is skipped at bind time anyway.
    if (type_mentions_excluded(type, ex))
        return;
    if (!std::meta::has_template_arguments(type))
        return;
    if (info_vec_contains(visited, type))
        return;
    visited.push_back(type);
    if (is_user_class_template_spec(type)) {
        if (!info_vec_contains(out, type))
            out.push_back(type);
        return;  // do not recurse into the spec's own template args
    }
    for (auto arg : std::meta::template_arguments_of(type))
        if (std::meta::is_type(arg) && !is_stl_policy(arg))
            collect_user_specs_from_type(arg, out, visited, ex);
}
consteval void collect_user_specs_from_type(std::meta::info type,
                                            std::vector<std::meta::info>& out) {
    std::vector<std::meta::info> visited;
    collect_user_specs_from_type(type, out, visited);
}

// Scan the member signatures declared directly in `owner` -- data members, static
// data, the return/param types of its functions -- for user specializations. Skips
// template members (a member template has no concrete signature) and destructors.
consteval void collect_own_member_specs(std::meta::info owner,
                                        std::vector<std::meta::info>& out,
                                        std::vector<std::meta::info>& visited,
                                        const exclusion_set& ex = {}) {
    for (auto mem : std::meta::members_of(owner, std::meta::access_context::unchecked())) {
        if (!std::meta::is_public(mem))
            continue;
        if (std::meta::is_template(mem)) {
            // Default-instantiable member templates bind (see
            // reflect_bind_member_template), so their instantiation's signature
            // types are reachable like any other bound signature.
            if (std::meta::is_function_template(mem)
                && !std::meta::is_constructor_template(mem)
                && !std::meta::is_conversion_function_template(mem)
                && fn_template_default_instantiable(mem)) {
                auto spec = std::meta::substitute(mem, std::vector<std::meta::info>{});
                if (std::meta::is_deleted(spec) || fn_mentions_excluded(spec, ex))
                    continue;
                collect_user_specs_from_type(std::meta::return_type_of(spec), out, visited, ex);
                for (auto p : std::meta::parameters_of(spec))
                    collect_user_specs_from_type(std::meta::type_of(p), out, visited, ex);
            }
            continue;
        }
        if (std::meta::is_function(mem) && !std::meta::is_destructor(mem)
            && !std::meta::is_deleted(mem)
            && !fn_mentions_excluded(mem, ex)) {
            if (!std::meta::is_constructor(mem))
                collect_user_specs_from_type(std::meta::return_type_of(mem), out, visited, ex);
            for (auto p : std::meta::parameters_of(mem))
                collect_user_specs_from_type(std::meta::type_of(p), out, visited, ex);
        }
    }
    for (auto mem : std::meta::nonstatic_data_members_of(
             owner, std::meta::access_context::unchecked()))
        if (std::meta::is_public(mem) && !ex.contains(mem))
            collect_user_specs_from_type(std::meta::type_of(mem), out, visited, ex);
    for (auto mem : std::meta::static_data_members_of(
             owner, std::meta::access_context::unchecked()))
        if (std::meta::is_public(mem) && !ex.contains(mem))
            collect_user_specs_from_type(std::meta::type_of(mem), out, visited, ex);
}

// Scan one class's full bound surface for user specializations: its own member
// signatures plus those of every base in its public-base subtree (base members
// are part of the bound surface either way -- through the real Python base or
// flattened). Base TYPES themselves no longer qualify for binding (reachability
// rule: being a base does not surface a type; see python_base_of). `walked`
// memoizes classes whose own-member scan already ran, so a base shared by many
// derived classes is scanned once per fixpoint, not once per derived class.
consteval void collect_class_user_specs(std::meta::info cls,
                                        std::vector<std::meta::info>& out,
                                        std::vector<std::meta::info>& visited,
                                        std::vector<std::meta::info>& walked,
                                        const exclusion_set& ex = {}) {
    if (!info_vec_contains(walked, cls)) {
        walked.push_back(cls);
        collect_own_member_specs(cls, out, visited, ex);
    }
    std::vector<std::meta::info> bases;
    collect_public_base_subtree(cls, bases);
    for (auto b : bases) {
        if (!info_vec_contains(walked, b) && !is_excluded_entity(b, ex)) {
            walked.push_back(b);
            collect_own_member_specs(b, out, visited, ex);
        }
    }
}

// Seed pass: walk a namespace (recursively) collecting user specs from its classes
// and free functions; or, given a class/spec or function directly, from that entity
// (including the entity itself when it is a spec -- collect_user_specs_from_type
// pushes a seed spec without recursing into its template args, so an explicitly
// listed container's policy args stay unbound).
consteval void collect_scope_user_specs(std::meta::info r,
                                        std::vector<std::meta::info>& out,
                                        std::vector<std::meta::info>& visited,
                                        std::vector<std::meta::info>& walked,
                                        const exclusion_set& ex = {}) {
    if (is_config_marker(r) || is_excluded_entity(r, ex))
        return;  // a marker is not a seed; an excluded seed is opaque
    if (std::meta::is_namespace(r)) {
        for (auto mem : std::meta::members_of(r, std::meta::access_context::unchecked())) {
            if (is_excluded_entity(mem, ex))
                continue;  // nb::exclude_-listed class/namespace: opaque
            if (std::meta::is_type(mem) && std::meta::is_class_type(mem)) {
                if (std::meta::is_complete_type(mem))  // skip fwd decls (BINDER-0019)
                    collect_class_user_specs(mem, out, visited, walked, ex);
            }
            else if (std::meta::is_function(mem) && !std::meta::is_template(mem)
                     && !std::meta::is_deleted(mem)
                     && !fn_mentions_excluded(mem, ex)) {
                collect_user_specs_from_type(std::meta::return_type_of(mem), out, visited, ex);
                for (auto p : std::meta::parameters_of(mem))
                    collect_user_specs_from_type(std::meta::type_of(p), out, visited, ex);
            } else if (std::meta::is_namespace(mem)
                       // An alias is a shorthand, not contents: following it
                       // pulls the whole target namespace into the walk.
                       && !std::meta::is_namespace_alias(mem))
                collect_scope_user_specs(mem, out, visited, walked, ex);
        }
    } else if (std::meta::is_type(r) && std::meta::is_class_type(r)) {
        collect_user_specs_from_type(r, out, visited, ex);       // r itself, if a spec
        collect_class_user_specs(r, out, visited, walked, ex);   // and its bound surface
    } else if (std::meta::is_function(r)) {
        if (fn_mentions_excluded(r, ex))
            return;
        collect_user_specs_from_type(std::meta::return_type_of(r), out, visited, ex);
        for (auto p : std::meta::parameters_of(r))
            collect_user_specs_from_type(std::meta::type_of(p), out, visited, ex);
    }
}

// The de-duplicated, fixpoint-closed list of user class-template specializations a
// reflected entity needs bound. Seeds from the entity's concrete signatures, then
// expands each newly found spec by scanning ITS members (and base-subtree members;
// so Wrap<int> surfaces Box<int>). The index loop over the growing vector plus the
// dedup is a worklist fixpoint that visits each spec once -- terminating on
// CRTP/self-referential specs.
// Deliberately NOT constexpr: calling it during constant evaluation makes the
// compiler emit "call to non-'constexpr' function 'reflect_discovery_diverged'"
// pointing here. It means the user-spec discovery fixpoint found an absurd
// number of specializations and is almost certainly DIVERGING -- the signature
// of an expression-template library, where every walked spec's facade members
// mint new specs (Eigen: Transpose<Derived> begets Transpose<Transpose<...>>).
// Fix: add the offending templates (or their whole namespace) to an
// ^^nb::exclude_<...> marker in the reflect_ pack.
// Deliberately left undefined: GCC requires a function with a consteval-only
// parameter type to be consteval, and calling an undefined consteval function
// produces a pointed "called before its definition" diagnostic when the
// fixpoint diverges.
consteval void reflect_discovery_diverged(std::meta::info last_discovered_spec);

consteval std::vector<std::meta::info> required_user_specs(
        std::meta::info r, const exclusion_set& ex = {}) {
    std::vector<std::meta::info> out, visited, walked;
    collect_scope_user_specs(r, out, visited, walked, ex);
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out.size() > 1024)
            reflect_discovery_diverged(out.back());
        collect_class_user_specs(out[i], out, visited, walked, ex);
    }
    return out;
}

// --- Reachability-based bind set ---
//
// The set of class types reflect_<Rs...> registers: the seeds themselves (the
// namespace members reflect_dispatch walks, or directly listed classes/specs)
// plus every user template specialization reachable from bound signatures
// (required_user_specs). Base classes are NOT in the set merely for being
// bases: a base becomes the real Python base only when it is independently in
// this set; otherwise its public members are flattened onto the derived class
// (python_base_of / reflect_class).

// The class types reflect_dispatch would bind for seed `r` (mirrors its walk:
// skips templates, [[=reflect::skip]] types, nb::exclude_ markers and excluded
// entities; recurses into sub-namespaces).
consteval void collect_seed_classes(std::meta::info r,
                                    std::vector<std::meta::info>& out,
                                    const exclusion_set& ex = {}) {
    if (is_config_marker(r) || is_excluded_entity(r, ex))
        return;
    if (std::meta::is_namespace(r)) {
        for (auto mem : std::meta::members_of(r, std::meta::access_context::unchecked())) {
            if (std::meta::is_template(mem))
                continue;
            if (std::meta::is_type(mem) && std::meta::is_class_type(mem)) {
                if (std::meta::is_complete_type(mem)   // skip fwd decls (BINDER-0019)
                    && !is_skip_annotated(mem) && !is_excluded_entity(mem, ex)
                    && !info_vec_contains(out, mem))
                    out.push_back(mem);
            } else if (std::meta::is_namespace(mem)
                       // An alias is a shorthand, not contents: following it
                       // pulls the whole target namespace into the walk.
                       && !std::meta::is_namespace_alias(mem)) {
                collect_seed_classes(mem, out, ex);
            }
        }
    } else if (std::meta::is_type(r) && std::meta::is_class_type(r)) {
        auto t = std::meta::remove_cvref(r);
        if (!info_vec_contains(out, t))
            out.push_back(t);
    }
}

template <std::meta::info... Rs>
consteval std::vector<std::meta::info> compute_bind_set() {
    std::vector<std::meta::info> out;
    std::vector<std::meta::info> exl = compute_excluded<Rs...>();
    exclusion_set ex{exl, excluded_matchers<Rs...>()};
    (collect_seed_classes(Rs, out, ex), ...);
    auto merge = [&](std::vector<std::meta::info> specs) {
        for (auto s : specs)
            if (!info_vec_contains(out, s))
                out.push_back(s);
    };
    (merge(required_user_specs(Rs, ex)), ...);
    return out;
}

// Memoized once per reflect_ pack: the (expensive) seed+fixpoint computation
// runs a single time and every per-class membership query is a cheap scan.
template <std::meta::info... Rs>
inline constexpr auto bind_set_v = std::define_static_array(compute_bind_set<Rs...>());

// The first type along `type`'s first-public-base chain that is in the bind
// set, or ^^void. nanobind can only wire a REGISTERED type as the Python base,
// so an unbound first base is looked through to ITS first public base, and so
// on. Members of the looked-through (unbound) links are flattened onto the
// derived class by flatten_unmodeled_bases.
consteval std::meta::info python_base_of(std::meta::info type,
                                         std::span<const std::meta::info> set) {
    for (auto t = type;;) {
        std::meta::info fb = ^^void;
        for (auto b : std::meta::bases_of(t, std::meta::access_context::unchecked()))
            if (std::meta::is_public(b)) {
                fb = std::meta::type_of(b);
                break;
            }
        if (fb == ^^void)
            return ^^void;
        bool in_set = false;
        for (auto e : set)
            if (e == fb) {
                in_set = true;
                break;
            }
        if (in_set)
            return fb;
        t = fb;
    }
}

// reflect_class's entry point to the above. A consteval function (immediately
// invoked at each use, incl. inside lambdas) rather than a constexpr local:
// a local variable of consteval-only type cannot be referenced from the
// class_-construction lambda's body.
template <typename T, std::meta::info... Rs>
consteval std::meta::info python_base_for() {
    return python_base_of(^^T, bind_set_v<Rs...>);
}

consteval std::vector<std::meta::info> required_stl_types(
        std::meta::info r, const exclusion_set& ex = {}) {
    std::vector<std::meta::info> out, visited;
    collect_scope_stl_types(r, out, visited, ex);
    return out;
}

// Like required_stl_types, but also sweeps the members of every discovered template
// specialization (a Holder<int> bound as a class needs the caster for its
// std::vector<int> member, but the spec is not a namespace member so the scope walk
// above misses it). This is a second full walk, so it is kept out of required_stl_types
// -- the codegen path (emit_stl_includes), which must emit the #includes, calls this;
// the header-only path leaves spec-member casters to surface at bind time.
consteval std::vector<std::meta::info> required_stl_types_with_specs(
        std::meta::info r, const exclusion_set& ex = {}) {
    std::vector<std::meta::info> out, visited;
    collect_scope_stl_types(r, out, visited, ex);
    for (auto spec : required_user_specs(r, ex))
        collect_class_stl_types(spec, out, visited, ex);
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
template <std::meta::info R, std::meta::info... Rs>
void check_stl_casters() {
    template for (constexpr auto ty :
                  std::define_static_array(required_stl_types(R, excluded_q<Rs...>()))) {
        static_assert(!is_base_caster_v<make_caster<typename [:ty:]>>,
                      stl_missing_caster_msg<ty>());
    };
}

// Parent-qualified fallback name for module-level collisions: two enums (or
// classes) with the same unqualified identifier in different scopes (yaml-cpp's
// NodeType::value vs EmitterStyle::value) used to register under ONE module
// attribute, the second silently clobbering the first. When the plain name is
// already taken in the module dict at bind time, the entity binds as
// "<Parent>_<name>" instead.
template <std::meta::info R>
consteval const char* parent_qualified_name(const char* name) {
    // Tolerate the anonymous-skip path: the caller's `if constexpr (name ==
    // nullptr) return;` doesn't stop THIS constexpr initializer from
    // evaluating in the same instantiation.
    if (name == nullptr)
        return nullptr;
    auto p = std::meta::parent_of(R);
    if (p != ^^:: && std::meta::has_identifier(p)
        && (std::meta::is_namespace(p)
            || (std::meta::is_type(p) && std::meta::is_class_type(p)))) {
        std::string s{std::meta::identifier_of(p)};
        s += '_';
        s += name;
        return std::define_static_string(s);
    }
    return name;   // no scoped parent to qualify with: keep the plain name
}

// The Python name for a class/enum reached through declaration `Named` (the
// namespace member or explicit reflect_ argument -- possibly a TYPEDEF over an
// anonymous record, the C idiom `typedef struct {...} point_t;`). The resolved
// type's own name wins when it has one; an anonymous type falls back to the
// typedef name the caller reached it through (BINDER-0018: substitution strips
// the alias sugar, and the record's display string is just "(anonymous type)",
// so the alias reflection must be threaded down explicitly).
template <typename T, std::meta::info Named>
consteval const char* reached_entity_name() {
    const char* tn = entity_name<^^T>();
    if (tn != nullptr)
        return tn;
    if constexpr (Named != ^^void) {
        if (std::meta::has_identifier(Named))
            return std::define_static_string(std::meta::identifier_of(Named));
    }
    return nullptr;
}

template <typename T, std::meta::info Named, std::meta::info... Rs>
void reflect_class(module_& m) {
    // A class with no Python-expressible name (a truly anonymous record with no
    // typedef name to reach it through) is skipped gracefully (the rest of the
    // body still instantiates; it just never runs).
    constexpr auto name = reached_entity_name<T, Named>();
    if constexpr (name == nullptr)
        return;

    // Idempotent: skip if T is already registered. This makes binding
    // order-independent and lets a class be reached both directly (via the
    // namespace walk / another reflect_ argument) and transitively (below)
    // without triggering nanobind's "already registered" warning.
    if (type<T>().is_valid())
        return;

    // nanobind supports a single base class -- and only a REGISTERED type can be
    // one. Reachability rule: a base is wired as the real Python base only when
    // it is independently in the reflect_<Rs...> bind set (a reflected-namespace
    // member, an explicit argument, or signature-reachable); python_base_of
    // returns the first such ancestor along the first-public-base chain (an
    // unbound link is looked through). Every other base in T's public-base
    // subtree -- secondary bases, unbound links, whole unbound facade chains
    // like flat_hash_map's container_internal ancestry -- has its public
    // members flattened directly onto T instead (flatten_unmodeled_bases); the
    // only thing lost is the isinstance/issubclass relation to those types.
    //
    // When a trampoline is registered for T, it is passed as the class_ "Alias"
    // (the extra template arg that nanobind distinguishes from the base via
    // is_base_of<T, Alias>), enabling Python subclasses to override C++ virtuals.
    constexpr bool HasBase = (python_base_for<T, Rs...>() != ^^void);
    constexpr bool HasTramp = has_reflect_trampoline<T>;

    if constexpr (HasBase) {
        // Ensure the base (and, recursively, its ancestors) is bound first --
        // nanobind requires the base registered before the derived type. The
        // guard above keeps this a no-op if the base is already bound.
        reflect_class<typename [:python_base_for<T, Rs...>():], ^^void, Rs...>(m);
    }

    // A DIFFERENT type already bound under this module attribute (same
    // unqualified identifier in another scope) would be silently clobbered:
    // fall back to the parent-qualified name (BINDER-0022). T itself being
    // registered already returned above.
    constexpr auto qual_name = parent_qualified_name<^^T>(name);
    const char* py_name = hasattr(m, name) ? qual_name : name;

    // Construct the class_ with the right template arguments. The lambda's return
    // type is deduced from whichever if-constexpr branch is active. with_doc_extra
    // supplies the optional [[=r::doc]] string as a trailing const char* extra.
    auto cls = with_doc_extra<^^T>([&](auto&&... doc) {
        if constexpr (HasBase && HasTramp)
            return class_<T, typename [:python_base_for<T, Rs...>():],
                          reflect_trampoline_t<T>>(m, py_name, doc...);
        else if constexpr (HasBase)
            return class_<T, typename [:python_base_for<T, Rs...>():]>(m, py_name, doc...);
        else if constexpr (HasTramp)
            return class_<T, reflect_trampoline_t<T>>(m, py_name, doc...);
        else
            return class_<T>(m, py_name, doc...);
    });

    bind_class_contents<T, Rs...>(cls);
    // Unconditional: even with no Python base there can be unbound bases to
    // flatten (the no-bases case is an empty list).
    flatten_unmodeled_bases<T, python_base_for<T, Rs...>(), Rs...>(cls);
    // Attach namespace-scope operators that take T as an operand (e.g. a free
    // operator+(T, T) or a scalar operator*(double, T)) as dunders on T.
    bind_free_operators<T, Rs...>(cls);
    // If T is ostream-insertable, expose str(x) via __str__ (the stream operator itself is
    // not bound as a dunder — see involves_stream_type / bind_stream_str).
    bind_stream_str<T>(cls);
}

template <typename E, std::meta::info Named = ^^void>
void reflect_enum(module_& m) {
    // Same anonymous-name handling as reflect_class (`typedef enum {...} e_t;`
    // binds under its typedef name; a truly anonymous enum is skipped).
    constexpr auto name = reached_entity_name<E, Named>();
    if constexpr (name == nullptr)
        return;

    // Idempotent, like reflect_class: an enum reached twice (explicit arg +
    // namespace walk) must not re-register -- and must not trip the collision
    // fallback below into binding the SAME enum under a second name.
    if (type<E>().is_valid())
        return;

    // Module-attribute collision (yaml-cpp's NodeType::value vs
    // EmitterStyle::value): the second same-named enum binds parent-qualified
    // instead of silently clobbering the first (BINDER-0022).
    constexpr auto qual_name = parent_qualified_name<^^E>(name);
    const char* py_name = hasattr(m, name) ? qual_name : name;

    auto e = with_doc_extra<^^E>([&](auto&&... doc) {
        return enum_<E>(m, py_name, doc...);
    });

    template for (constexpr auto val :
        std::define_static_array(std::meta::enumerators_of(^^E))) {
        constexpr auto vname =
            std::define_static_string(std::meta::identifier_of(val));
        e.value(vname, [:val:]);
    };
}

// `r` is the entity being dispatched; `Rs...` is the WHOLE reflect_ pack,
// threaded through so reflect_class can consult the pack-wide bind set
// (bind_set_v<Rs...>) when deciding Python-base wiring.
/// Kind-routing for one member of a namespace walk (a shared classifier, see
/// data_member_route's section note). Guard ORDER is load-bearing: templates
/// are rejected before any annotation query (annotations_of is ill-formed on
/// a template). Templates are skipped because only their specializations bind
/// (discovered by reflect_user_specs or passed
/// explicitly). cls requires a COMPLETE type: a forward-declared
/// namespace member (`struct Opaque;` -- the pImpl idiom, BINDER-0019) cannot
/// be bound, exactly like the discovery walks treat it. A namespace ALIAS
/// member is a shorthand, not a declaration of contents -- following it binds
/// the entire aliased namespace (a fixture's `namespace sd = simdjson;` pulled
/// in the world, BINDER-0028).
enum class ns_member_kind { skip, cls, enum_, free_fn, ns };
consteval ns_member_kind classify_namespace_member(
        std::meta::info mem, const exclusion_set& ex) {
    if (std::meta::is_template(mem))
        return ns_member_kind::skip;
    if (fn_skip_annotated(mem) || is_excluded_entity(mem, ex))
        return ns_member_kind::skip;
    if (std::meta::is_type(mem)) {
        if (std::meta::is_class_type(mem))
            return std::meta::is_complete_type(mem) ? ns_member_kind::cls
                                                    : ns_member_kind::skip;
        if (std::meta::is_enum_type(mem))
            return ns_member_kind::enum_;
        return ns_member_kind::skip;
    }
    if (std::meta::is_function(mem))
        return fn_mentions_excluded(mem, ex) ? ns_member_kind::skip
                                             : ns_member_kind::free_fn;
    if (std::meta::is_namespace(mem) && !std::meta::is_namespace_alias(mem))
        return ns_member_kind::ns;
    return ns_member_kind::skip;
}

// Dispatch forwarders: the expanded body of a `template for` is not a
// dependent context on GCC, so a splice of the loop variable inside a
// DISCARDED if-constexpr branch is still checked -- and a function/namespace
// reflection is not usable in a splice type. Routing through an info-NTTP
// forwarder keeps the splice dependent until the branch is actually taken.
template <std::meta::info mem, std::meta::info... Rs>
void reflect_class_of(module_& m) {
    reflect_class<typename [:mem:], mem, Rs...>(m);
}
template <std::meta::info mem>
void reflect_enum_of(module_& m) {
    reflect_enum<typename [:mem:], mem>(m);
}

template <std::meta::info r, std::meta::info... Rs>
void reflect_dispatch(module_& m) {
    if constexpr (is_config_marker(r)
                  || is_excluded_entity(r, excluded_q<Rs...>())) {
        // A configuration marker (exclude_ / trampoline_ / match_ /
        // exclude_if_ / instantiate_ -- the latter two seed through the
        // seeds_of expansion, not this walk), or a seed that is itself
        // excluded -- bind nothing.
    } else if constexpr (std::meta::is_namespace(r)) {
        template for (constexpr auto mem :
            std::define_static_array(std::meta::members_of(
                r, std::meta::access_context::unchecked()))) {
            constexpr ns_member_kind kind =
                classify_namespace_member(mem, excluded_q<Rs...>());
            if constexpr (kind == ns_member_kind::cls)
                reflect_class_of<mem, Rs...>(m);
            else if constexpr (kind == ns_member_kind::enum_)
                reflect_enum_of<mem>(m);
            else if constexpr (kind == ns_member_kind::free_fn)
                reflect_free_function<mem>(m);
            else if constexpr (kind == ns_member_kind::ns)
                reflect_dispatch<mem, Rs...>(m);
        };
    } else if constexpr (std::meta::is_type(r)) {
        // Nest the type-kind checks under is_type: is_class_type/is_enum_type are
        // ill-formed on a non-type reflection (e.g. a function-template
        // specialization like ^^identity<int> passed directly to reflect_).
        if constexpr (std::meta::is_class_type(r))
            reflect_class<typename [:r:], r, Rs...>(m);
        else if constexpr (std::meta::is_enum_type(r))
            reflect_enum<typename [:r:], r>(m);
    } else if constexpr (std::meta::is_function(r)) {
        reflect_free_function<r>(m);
    }
}

// Bind every user class-template specialization discovered in R's signatures (see
// required_user_specs). Specializations are not namespace members, so reflect_dispatch
// never reaches them; this pre-pass does. reflect_class's is_valid() guard makes each
// bind idempotent, so a spec also reached transitively (as a member type) binds once.
template <std::meta::info R, std::meta::info... Rs>
void reflect_user_specs(module_& m) {
    template for (constexpr auto ty :
                  std::define_static_array(required_user_specs(R, excluded_q<Rs...>()))) {
        reflect_class<typename [:ty:], ^^void, Rs...>(m);
    };
}

NAMESPACE_END(detail)

/// Automatically bind classes, enums, and namespaces via C++26 reflection.
///
/// Pass any mix of reflected types and namespaces (plus, optionally,
/// ^^exclude_<...> markers -- see exclude_ above):
///   nb::reflect_<^^Point, ^^Player, ^^Color, ^^game>(m);
///
template <std::meta::info... Rs>
void reflect_(module_& m) {
    // Diagnose any std type used in a bound signature whose <nanobind/stl/*.h>
    // caster was not included (a no-op when every needed caster is present).
    (detail::check_stl_casters<Rs, Rs...>(), ...);
    // Bind user class-template specializations reachable from the signatures, then
    // the namespaces/classes/enums/functions themselves (order-independent: the
    // reflect_class is_valid() guard dedups specs reached by both passes). Each
    // per-entity call also receives the whole pack: Python-base wiring consults
    // the pack-wide bind set (a base seeded by ANY pack element counts).
    (detail::reflect_user_specs<Rs, Rs...>(m), ...);
    (detail::reflect_dispatch<Rs, Rs...>(m), ...);
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
