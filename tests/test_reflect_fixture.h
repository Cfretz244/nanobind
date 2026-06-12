/*
    tests/test_reflect_fixture.h: the C++ surface bound by the reflection test
    suite. Shared by every backend: test_reflect.cpp (constexpr reflect_), the
    emit-mode generator, and the GENERATED binding TU -- which is compiled
    WITHOUT reflection. Everything here is therefore plain C++ (header-safe:
    inline functions/variables), with the P3394 annotations behind
    NB_FIXTURE_ANN so the file parses on a non-P2996 compiler. The P2996-only
    pieces (reflection markers, trampoline registration, static_asserts on the
    binder) live with the consumers, not here.
*/

#pragma once

#include <nanobind/nb_reflect_annotations.h>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#if defined(__has_feature)
#  if __has_feature(annotation_attributes)
#    define NB_FIXTURE_ANN(...) [[=__VA_ARGS__]]
#  endif
#endif
// GCC 16 (P3394 annotations under -freflection): no __has_feature name for
// it; key off the reflection feature-test macro instead.
#if !defined(NB_FIXTURE_ANN) && defined(__cpp_impl_reflection)
#  define NB_FIXTURE_ANN(...) [[=__VA_ARGS__]]
#endif
#ifndef NB_FIXTURE_ANN
#  define NB_FIXTURE_ANN(...)
#endif

namespace r = nanobind::reflect;

// Base classes that live OUTSIDE the reflected namespace and are never passed to
// reflect_<...>. Reachability rule: being a base does not surface a type, so none
// of these become Python types -- their public members are FLATTENED onto the
// derived classes instead (and an in-set ancestor further up the chain is still
// wired as the real Python base, looked up through the unbound links).
namespace external_bases {

struct ExtBase {
    int eb;
    ExtBase() : eb(0) {}
    int ext_method() const { return eb; }
};

// A two-level unbound chain: both levels flatten onto the derived class.
struct DeepUnbound {
    int deep;
    DeepUnbound() : deep(0) {}
    int deep_method() const { return deep * 10; }
};
struct MidUnbound : DeepUnbound {
    int mid;
    MidUnbound() : mid(0) {}
    int mid_method() const { return mid * 100; }
};

} // namespace external_bases

namespace reflect_test {

enum class Enum { A, B, C };

struct Struct {
    int i;
    double d;
    std::string s;

    static constexpr int static_const = 42;
    static int static_mut;

    Struct() : i(0), d(0), s() { ++static_mut; }
    Struct(int i, double d) : i(i), d(d), s() { ++static_mut; }

    int get_i() const { return i; }
    void set_i(int v) { i = v; }
    double sum() const { return i + d; }

    double overloaded(double x) const { return d + x; }
    double overloaded(int x, int y) const { return d + x + y; }

    static int create_count() { return static_mut; }
};

inline int Struct::static_mut = 0;

struct Nested {
    std::string name;
    int value;
    std::vector<int> items;
    Struct inner;
};

class Mixed {
public:
    int pub_i;
    std::string pub_s;

    Mixed() : pub_i(0), pub_s(), priv_i(0), priv_d(0) {}

    int get_pub() const { return pub_i; }

private:
    int priv_i;
    double priv_d;

    void priv_method() {}

protected:
    int prot_i;
};

namespace ns {

struct A {
    int x;
    double y;
};

struct B {
    std::string s;
};

enum class E { X, Y, Z };

inline double free_fn(double a, double b) { return a + b; }
inline double free_fn(double a, double b, double c) { return a + b + c; }

} // namespace ns

// --- Inheritance ---

// Single inheritance: derived exposes its own + inherited members.
struct Base {
    int b;
    Base() : b(0) {}
    int base_method() const { return b; }
};

struct Derived : Base {
    int d;
    Derived() : d(0) {}
    int derived_method() const { return d; }
};

// Multi-level chain L0 <- L1 <- L2.
struct L0 {
    int v0;
    L0() : v0(0) {}
    int m0() const { return v0; }
};

struct L1 : L0 {
    int v1;
    L1() : v1(0) {}
    int m1() const { return v1; }
};

struct L2 : L1 {
    int v2;
    L2() : v2(0) {}
    int m2() const { return v2; }
};

// Derived from a base outside the reflected namespace: the base is NOT bound
// (reachability rule) -- its public members flatten onto this class.
struct UsesExtBase : external_bases::ExtBase {
    int ub;
    UsesExtBase() : ub(0) {}
};

// Two-level unbound chain: MidUnbound and DeepUnbound both flatten onto this.
struct UsesDeepChain : external_bases::MidUnbound {
    int own;
    UsesDeepChain() : own(0) {}
};

// Multiple public bases: only the first (MixinA) becomes the nanobind base.
struct MixinA {
    int a;
    MixinA() : a(0) {}
    int from_a() const { return a; }
};

struct MixinB {
    int b2;
    MixinB() : b2(0) {}
    int from_b() const { return b2; }
};

struct MultiDerived : MixinA, MixinB {
    int md;
    MultiDerived() : md(0) {}
};

// A secondary base that itself has a base, to exercise flattening of a whole
// secondary subtree (Mid's members + SecBase's members onto Combo).
struct SecBase {
    int sb;
    SecBase() : sb(0) {}
    int sec_method() const { return sb; }
};

struct Mid : SecBase {
    int mid;
    Mid() : mid(0) {}
};

struct PrimaryX {
    int px;
    PrimaryX() : px(0) {}
};

struct Combo : PrimaryX, Mid {
    int cm;
    Combo() : cm(0) {}
};

// Diamond: Dia -> DiaL -> DiaTop and Dia -> DiaR -> DiaTop. DiaTop is reached via
// the primary chain (DiaL), so it must NOT be flattened again through DiaR.
struct DiaTop {
    int dt;
    DiaTop() : dt(0) {}
    int top_method() const { return dt; }
};

struct DiaL : DiaTop {
    int dl;
    DiaL() : dl(0) {}
};

struct DiaR : DiaTop {
    int dr;
    DiaR() : dr(0) {}
};

struct Dia : DiaL, DiaR {
    int db;
    Dia() : db(0) {}
};

// --- Function-type qualifiers (noexcept / ref-qualified / skipped shapes) ---

struct Quals {
    int v;
    Quals() : v(0) {}

    int plain(int x) { return x + 1; }
    int plain_ne(int x) noexcept { return x + 2; }          // noexcept
    int c_ne(int x) const noexcept { return x + v; }        // const noexcept
    int lref() & { return 10; }                             // lvalue-ref-qualified -> bound

    int rref() && { return 20; }                            // rvalue-ref-qualified -> skipped
    int vol() volatile { return 30; }                       // volatile -> skipped
    int va(int a, ...) { return a; }                        // C-variadic -> skipped

    static int sfn_ne() noexcept { return 99; }             // static noexcept -> bound
};

inline double free_ne(double a) noexcept { return a * 2; }  // free noexcept -> bound

// --- Annotation-driven control (skip / rename / doc / lifetime) ---

struct NB_FIXTURE_ANN(r::skip{}) HiddenClass {
    int x;
    HiddenClass() : x(0) {}
};

struct Annotated {
    int kept;
    NB_FIXTURE_ANN(r::skip{}) int secret;                    // skipped data member
    Annotated() : kept(0), secret(0) {}

    int visible() const { return kept; }
    NB_FIXTURE_ANN(r::skip{}) int hidden_method() const { return 1; }        // skipped
    NB_FIXTURE_ANN(r::rename{"renamed"}) int original_name() const { return 7; }
    NB_FIXTURE_ANN(r::doc{"documented method"}) int documented() const { return 9; }
};

struct Inner {
    int v;
    Inner() : v(0) {}
};

struct Holder {
    Inner inner;
    Holder() = default;
    // reference_internal: repeated calls return the SAME Python object (and keep
    // the Holder alive); without the annotation a ref return would be copied.
    NB_FIXTURE_ANN(r::reference_internal) Inner& get_inner() { return inner; }
};

struct KA {
    int total;
    KA() : total(0) {}
    // keep_alive{1,2}: nurse = self (1), patient = the argument (2).
    NB_FIXTURE_ANN(r::keep_alive{1, 2}) void absorb(KA& other) { total += other.total + 1; }
};

NB_FIXTURE_ANN(r::skip{}) inline int hidden_free() { return 1; }
NB_FIXTURE_ANN(r::rename{"renamed_free"}) inline int original_free() { return 3; }

// --- Operators -> Python dunders ---

struct Ops {
    int x;
    Ops() : x(0) {}
    explicit Ops(int v) : x(v) {}

    Ops operator+(const Ops& o) const { return Ops(x + o.x); }  // __add__
    Ops operator-() const { return Ops(-x); }                   // __neg__ (unary)
    bool operator==(const Ops& o) const { return x == o.x; }    // __eq__
    bool operator<(const Ops& o) const { return x < o.x; }      // __lt__
    Ops& operator+=(const Ops& o) { x += o.x; return *this; }   // __iadd__
    int operator()(int m) const { return x * m; }               // __call__
    int operator[](int i) const { return x + i; }               // __getitem__
    explicit operator bool() const { return x != 0; }           // __bool__
};

// Multiple integral conversion operators: only the WIDEST binds __int__ (each used
// to bind it, last-bound silently winning -- `int`, declared after `long long`
// here, would truncate). bool still binds __bool__. Mirrors absl::int128's
// operator char/int/long/... cluster.
// A by-value parameter of a move-only class type cannot be produced by
// nanobind's generic class caster (it copies out of caster storage), so such
// overloads are skipped while their siblings still bind (BINDER-0010; the real
// case is absl::Cord::Append(absl::CordBuffer)).
struct MoveOnlyBuf {
    MoveOnlyBuf() = default;
    MoveOnlyBuf(const MoveOnlyBuf&) = delete;
    MoveOnlyBuf(MoveOnlyBuf&&) = default;
};
struct Sink {
    int n;
    Sink() : n(0) {}
    void put(MoveOnlyBuf) { ++n; }   // skipped: by-value move-only param
    void put(int v) { n += v; }      // stays: ordinary overload
    int get() const { return n; }
};

// An ABSTRACT interface (the spdlog::sinks::sink shape) must get NO constructor
// bound: nb::init<> would have to instantiate it, a hard TU error (BINDER-0011).
// Python-side instantiation raises TypeError instead, and the concrete derived
// class binds with the abstract base as its real Python base.
// The unique_ptr signatures double as the caster-matrix regression: both methods
// are skipped at bind time (by-value move-only / [[=r::skip]]), so their
// std::unique_ptr<int> must not static_assert for the unique_ptr caster header,
// which the binding TU deliberately does not include.
struct AbstractIface {
    virtual ~AbstractIface() = default;
    virtual int compute(int x) const = 0;
    void consume(std::unique_ptr<int>) {}                       // skipped: move-only by value
    NB_FIXTURE_ANN(r::skip{}) void hidden(const std::unique_ptr<int>&) {}  // skipped: annotation
};
struct ConcreteImpl : AbstractIface {
    int compute(int x) const override { return 2 * x + 1; }
};

struct ManyConv {
    long long v;
    ManyConv() : v(0) {}
    explicit ManyConv(long long n) : v(n) {}
    explicit operator char() const { return (char) v; }       // narrow -- skipped
    explicit operator long long() const { return v; }         // widest -> __int__
    explicit operator int() const { return (int) v; }         // declared last -- skipped
    explicit operator bool() const { return v != 0; }         // __bool__ unaffected
};

// Deleted functions are public, enumerable member declarations; none may bind
// (BINDER-0012; the field case is tl::unexpected<E>'s `unexpected() = delete;`,
// where cls.def(init<>()) called the deleted ctor -- a TU-wide hard error).
// poisoned()'s unique_ptr reference doubles as the caster-matrix regression: a
// deleted method must not demand the unique_ptr caster header, which the
// binding TU deliberately does not include (same trick as AbstractIface).
struct NoDefault {
    NoDefault() = delete;                                  // ctor pass filter
    explicit NoDefault(int n) : v(n) {}
    int v;
    int dbl() const { return 2 * v; }
    void poisoned(const std::unique_ptr<int>&) = delete;   // method pass + caster walk
    explicit operator long long() const = delete;          // loses the __int__ contest...
    explicit operator int() const { return (int) v; }      // ...so __int__ binds from int
    template <class T = int> void tpoison(T) = delete;     // member-template spec filter
};
// A class whose ONLY constructors are deleted binds with no __init__: Python
// instantiation raises TypeError (the BINDER-0011 abstract-class contract).
struct OnlyDeletedCtors {
    OnlyDeletedCtors() = delete;
    explicit OnlyDeletedCtors(int) = delete;
    static int probe() { return 7; }
};
int free_deleted(int) = delete;                            // free-function pass filter
bool operator==(const NoDefault&, const NoDefault&) = delete;  // free-operator filter

// --- Virtual functions / trampoline (Tier 1: hand-written trampoline) ---

struct Shape {
    Shape() = default;
    virtual ~Shape() = default;
    virtual double area() const = 0;                      // pure virtual
    virtual std::string kind() const { return "shape"; }  // non-pure virtual
};

// Free functions that invoke the virtuals from C++ -- used to prove that a
// Python override is dispatched into when C++ calls through a base reference.
inline double call_area(const Shape& s) { return s.area(); }
inline std::string call_kind(const Shape& s) { return s.kind(); }

// --- Keyword-argument names (P3096 parameter names -> nb::arg) ---

struct Kw {
    Kw() {}
    int add(int a, int b) const { return a + b; }     // method kwargs
    static int smul(int x, int y) { return x * y; }   // static-method kwargs
};

inline int kw_sub(int a, int b) { return a - b; }     // free-function kwargs

struct KwCtor {                                       // constructor kwargs
    int i, j;
    KwCtor(int i, int j) : i(i), j(j) {}
};

// --- Class / enum docstrings (annotation on the type itself) ---

struct NB_FIXTURE_ANN(r::doc{"A documented class."}) DocClass {
    DocClass() {}
    int v() const { return 1; }
};

enum class NB_FIXTURE_ANN(r::doc{"A documented enum."}) DocEnum { X, Y };

// --- Free (namespace-scope) operators -> dunders ---

struct Vec {
    double x, y;
    Vec() : x(0), y(0) {}
    Vec(double x, double y) : x(x), y(y) {}
};

// Symmetric same-type operator -> forward __add__ only.
inline Vec operator+(const Vec& a, const Vec& b) { return Vec(a.x + b.x, a.y + b.y); }
// Scalar on the right -> forward __mul__ on Vec.
inline Vec operator*(const Vec& v, double s) { return Vec(v.x * s, v.y * s); }
// Scalar on the left -> reversed __rmul__ on Vec (the key reversed-dunder case).
inline Vec operator*(double s, const Vec& v) { return Vec(s * v.x, s * v.y); }
// Comparison free operator -> __eq__.
inline bool operator==(const Vec& a, const Vec& b) { return a.x == b.x && a.y == b.y; }
// Unary free operators -> __neg__ / __invert__ (binary-only was the old limit;
// absl::int128's negation is a free operator-(int128)).
inline Vec operator-(const Vec& v) { return Vec(-v.x, -v.y); }
inline Vec operator~(const Vec& v) { return Vec(v.y, v.x); }  // arbitrary, observable: swap

// --- Array data members are skipped (BINDER-0006); scalar siblings still bind. ---

struct WithArray {
    int arr[3];          // C-array member -> skipped (not def_rw-able)
    int scalar;          // ordinary member -> bound
    WithArray() : arr{1, 2, 3}, scalar(7) {}
};

// --- Properties from getter/setter pairs ([[=r::property]]) ---

struct Thermo {
    double c_;
    Thermo() : c_(0) {}
    // Read-write property via an overloaded accessor name.
    NB_FIXTURE_ANN(r::property{"celsius"}) double celsius() const { return c_; }   // getter
    NB_FIXTURE_ANN(r::property{"celsius"}) void   celsius(double v) { c_ = v; }    // setter
    // Read-only property, with a C++ accessor name unrelated to the property name.
    NB_FIXTURE_ANN(r::property{"fahrenheit"}) double to_f() const { return c_ * 9.0 / 5.0 + 32.0; }
};

} // namespace reflect_test

// An unbound middle link between a reflected derived class and a reflected
// ancestor: reflect_test::Base IS in the bind set, so it becomes the real
// Python base of ChainThroughUnbound, looked up THROUGH the unbound link
// (class_<T, Grandbase> works across an unambiguous public chain); the link's
// own members flatten onto the derived class.
namespace external_bases {
struct MidToBase : reflect_test::Base {
    int mtb;
    MidToBase() : mtb(0) {}
    int mtb_method() const { return mtb + 1; }
};
} // namespace external_bases

namespace reflect_test {
struct ChainThroughUnbound : external_bases::MidToBase {
    int cu;
    ChainThroughUnbound() : cu(0) {}
};
} // namespace reflect_test

// --- Member function templates: the all-defaulted shape binds via its default
//     instantiation (heterogeneous-lookup APIs: template <class K = int> bool
//     contains(const K&)); packs and non-defaulted params stay skipped. Mirrors
//     absl::flat_hash_map's query surface (which lives on a flattened base).
namespace member_template_detail {

// NOT reflected: HetMap's base. Its defaulted member template must reach HetMap
// through the flattening path (like raw_hash_set's query APIs).
struct HetBase {
    std::vector<int> keys;
    HetBase() {}
    template <class K = int>
    bool contains(const K& k) const {
        for (int v : keys) if (v == k) return true;
        return false;
    }
};

} // namespace member_template_detail

namespace member_template_test {

struct HetMap : member_template_detail::HetBase {
    HetMap() {}
    // Public-base re-export: the flattening pass already exposes contains();
    // the proxy this using-declaration creates must NOT bind a duplicate.
    using member_template_detail::HetBase::contains;
    void add(int k) { keys.push_back(k); }
    template <class K = int, class P = double>
    int at(const K& k) const { return k * 2; }                  // multi-defaulted
    template <class K = int>
    std::size_t erase(const K& k) { return k > 0 ? 1u : 0u; }
    // Const/non-const same-named default-instantiable pair (ankerl
    // unordered_dense's heterogeneous at()): the dispatcher instantiations
    // for the two siblings must get DISTINCT symbols (GCC-8; the
    // member_tmpl_mangle_hint disambiguator).
    template <class K = int>
    int hat(const K& k) { return k + 1; }
    template <class K = int>
    int hat(const K& k) const { return k + 1; }
    template <class K = int>
    int operator[](const K& k) const { return k + 10; }         // -> __getitem__
    // SFINAE-false pack sibling of operator[] (TC-0004 trigger shape: mirrors absl
    // raw_hash_map's lifetimebound pair). Never instantiable, so it must be skipped --
    // and it must not disturb the sibling's binding (same-named function-template
    // reflections as NTTPs used to mangle identically and fold at codegen).
    template <class K = int, class P = double, int&...,
              std::enable_if_t<(sizeof(K) == 0), int> = 0>
    int operator[](const K& k) const;
    template <class K = int>
    static int sdouble(const K& k) { return k * 2; }            // static template
    template <class K>
    void needs_explicit(K) {}                                    // no default: skipped
    template <class... Args>
    void emplace(Args&&...) {}                                   // pure pack: skipped
    template <class K = int, class... Args>
    void try_emplace(K, Args&&...) {}                            // pack tail: skipped
};

} // namespace member_template_test

// --- Streamable: free operator<<(ostream&, T) -> __str__ (BINDER-0007), while a genuine
//     operator<<(T, int) shift still maps to __lshift__. Reflected as a TYPE (^^stream_test::
//     Streamable), mirroring how a real streamable library value (e.g. absl::int128) is bound.
namespace stream_test {

struct Streamable {
    int v;
    Streamable() : v(0) {}
    explicit Streamable(int v) : v(v) {}
};

// Stream-insertion: surfaced as Python __str__ (NOT bound as a dunder; operand is a stream).
inline std::ostream& operator<<(std::ostream& os, const Streamable& s) { return os << "S(" << s.v << ")"; }
// A genuine left-shift on the value type -> __lshift__ (operand types are both bindable).
inline Streamable operator<<(const Streamable& s, int n) { return Streamable(s.v << n); }

} // namespace stream_test

// --- Templates: class-template specializations (roadmap #6) ---
//
// Templates themselves are not bindable; their *specializations* are. reflect_
// auto-discovers every user class-template specialization reachable from the
// reflected set's signatures (recursively), and additional ones can be listed
// explicitly. Python names are CamelCase: Box<int> -> BoxInt, Pair<int,double> ->
// PairIntDouble, Array<int,3> -> ArrayInt3, Box<Box<int>> -> BoxBoxInt.
namespace template_test {

template <class T>
struct Box {
    T value;
    Box() : value{} {}
    explicit Box(T v) : value(v) {}
    T get() const { return value; }
    void set(T v) { value = v; }
    // Dependent noexcept-specifier (the nlohmann basic_json::swap shape):
    // GCC 16 keeps it deferred for the instantiated member, and matching the
    // spliced function type against the method-binder matrix ICE'd until the
    // binder forced resolution via nb_fn_type_of (GCC-5).
    void swap_with(Box& other) noexcept(noexcept(T(static_cast<T&&>(other.value)))) {
        T tmp = static_cast<T&&>(value);
        value = static_cast<T&&>(other.value);
        other.value = static_cast<T&&>(tmp);
    }
};

template <class A, class B>
struct Pair {
    A first;
    B second;
    Pair() : first{}, second{} {}
    Pair(A a, B b) : first(a), second(b) {}
};

template <class T, int N>
struct Array {
    T head;
    Array() : head{} {}
    int size() const { return N; }
};

template <class T>
struct Wrap {
    Box<T> inner;
};

// A namespace-scope DEDUCTION GUIDE: enumerated by members_of like any other
// member, never bindable, and (pre-TC-0008) unmangleable as a reflection NTTP
// -- the binder's namespace walks must strip it before their
// define_static_array lifts (namespace_members_for_binding). The field shape
// is TartanLlama/expected's `unexpected(E) -> unexpected<E>`. The module
// compiling at all is the assertion.
template <class T> Wrap(T) -> Wrap<T>;

// A "policy" template argument: Pol<int> appears ONLY as a template argument of
// Cont (never in a callable signature), so the reachability rule keeps it out of
// the bind set -- mirroring hash-map Hash/Eq/Alloc/Policy args. Cont's genuine
// interface (T) still binds.
template <class T>
struct Pol {};

template <class T, class P>
struct Cont {
    T v;
    Cont() : v() {}
    T get() const { return v; }
};

// A free FUNCTION template. Templates can't be bound, only instantiations; a
// specialization is bound only when listed explicitly (it appears in no signature,
// and explicit instantiation definitions are not enumerable via reflection). The
// Python name is CamelCase like classes: identity<int> -> identityInt.
template <class T>
T identity(T x) { return x; }

// A plain (non-template) class whose signatures reference specializations: every
// one below is discovered and bound without being listed explicitly.
struct UsesBoxes {
    Box<int> bi;                              // data member          -> BoxInt
    Box<Box<int>> nested;                     // nested type arg      -> BoxBoxInt (+ BoxInt)
    Wrap<int> wrapped;                        // transitive (fixpoint)-> WrapInt surfaces BoxInt
    Pair<int, double> pid;                    // multiple type args   -> PairIntDouble
    Array<int, 3> arr;                        // non-type arg         -> ArrayInt3
    Cont<int, Pol<int>> cp;                   // spec bound; its Pol<int> arg is NOT
    UsesBoxes() = default;
    Box<double> make_bd() const { return Box<double>(2.5); }   // return type -> BoxDouble
    void take(const Box<int>& b) { bi = b; }                  // param type (dup) -> BoxInt
    // A stdlib-INTERNAL class in a signature: std::vector's iterator dealiases
    // to an implementation-detail class template -- libc++'s std::__wrap_iter
    // (under std) or libstdc++'s __gnu_cxx::__normal_iterator (a reserved
    // namespace OUTSIDE std). is_in_std must classify BOTH as stdlib
    // implementation so the user-spec discovery fixpoint never drags the
    // iterator into the bind set (unordered_dense's table::begin/find field
    // shape on libstdc++); the method itself binds, with an unregistered
    // return type. Asserted via tt_has_spec in test_reflect.cpp.
    std::vector<int>::iterator first_of(std::vector<int>& v) const {
        return v.begin();
    }
};

} // namespace template_test

// --- nb::exclude_: call-site exclusions for code you do not own ---
//
// exclude_test models the expression-template shape (Eigen): Vec's methods
// return Expr<...> specializations whose OWN members mint ever-deeper specs
// (Expr<T>::deeper() -> Expr<Expr<T>>), so unrestricted discovery would
// DIVERGE (caught by the reflect_discovery_diverged guard). Excluding the
// ^^Expr template (every specialization), the ^^detail namespace, the concrete
// ^^Opaque type, and the ^^ExBase base makes Vec bindable: any member whose
// signature mentions an excluded entity is skipped, an excluded base is not
// flattened, and everything else binds normally.
namespace exclude_test {

template <class T> struct Expr {
    Expr<Expr<T>> deeper() const { return {}; }    // divergent without exclusion
    int rank() const { return 1; }
};

namespace detail {
struct Helper { int h = 0; };
}  // namespace detail

struct Opaque { int o = 0; };

struct ExBase {
    int from_base() const { return 9; }
};

struct XVec : ExBase {
    int len_ = 3;
    Expr<int> field;                                // excluded member type -> skipped
    XVec() = default;
    explicit XVec(int n) : len_(n) {}
    XVec(const Opaque&) {}                           // excluded ctor param  -> skipped
    int len() const { return len_; }                // binds
    int doomed() const { return 1; }                // listed by REFLECTION -> skipped
                                                    // (the per-member escape hatch for
                                                    // lazily-ill-formed bodies in
                                                    // unowned code; Eigen's sized ctors)
    int dot(const Expr<int>&) const { return 7; }   // excluded param       -> skipped
    Expr<int> expr() const { return {}; }           // excluded return      -> skipped
    detail::Helper helper() const { return {}; }    // excluded namespace   -> skipped
    Opaque opaque() const { return {}; }            // excluded type        -> skipped
};

inline int free_len(const XVec& v) { return v.len(); }     // binds
inline Expr<int> free_expr(const XVec&) { return {}; }     // excluded return -> skipped

} // namespace exclude_test

// BINDER-0015 (unbindable parameter/return shapes skip gracefully) +
// BINDER-0019 (pointer to incomplete plain class skips like a non-completable
// spec). Every poison member here used to be a TU-wide hard error.
namespace unbindable_shapes {

struct Opaque;                                       // pImpl idiom: never defined

struct Gadget {
    int v = 1;
    Gadget() = default;
    char** mangle(char** argv) { return argv; }      // ptr-to-ptr param + return
    bool peek(Gadget*& out) noexcept { out = this; return true; }  // T*& out-param
    void on_event(void (*cb)(int)) { cb(v); }        // function-pointer param
    Opaque* impl() const { return nullptr; }         // incomplete-pointee return
    const void* blob() const { return &v; }          // cv void* return (BINDER-0023)
    int ok() const { return v; }                     // binds
};

} // namespace unbindable_shapes

// BINDER-0022: same-named enums in sibling scopes used to clobber one module
// attribute (yaml-cpp's NodeType::value vs EmitterStyle::value); the second
// now binds parent-qualified. BINDER-0024: a static shadowed by a same-named
// instance method used to ABORT nanobind at import; it now skips.
// BINDER-0028: a namespace-alias member is not followed by the walks.
namespace collide_a { enum class shade { LIGHT = 1, DARK = 2 }; }
namespace collide_b { enum class shade { RED = 10, BLUE = 20 }; }

namespace shadow_test {
struct Conn {
    int id = 3;
    int info() const { return id; }                  // binds
    static int info(int x) { return x * 2; }         // shadowed: skipped
    static int probe() { return 17; }                // unshadowed static binds
};
} // namespace shadow_test

namespace alias_leak_target {
struct LeakedInner { int z = 1; };
} // namespace alias_leak_target

namespace alias_fixture {
namespace shorthand = alias_leak_target;             // NOT followed (BINDER-0028)
inline int forty_two() { return 42; }
} // namespace alias_fixture

// BINDER-0025: T& class returns borrow (reference_internal), not copy.
namespace ref_return_test {
struct Cell {
    int v = 7;
    Cell() = default;
    Cell(const Cell&) = delete;                      // copy policy would abort
    int get() const { return v; }
    void set(int x) { v = x; }
};
struct CellHolder {
    Cell cell;
    CellHolder() = default;
    CellHolder(const CellHolder&) = delete;
    Cell& edit() { return cell; }                    // borrowed reference
};
} // namespace ref_return_test

// BINDER-0026: reflected ctors construct with PARENS -- an initializer_list
// ctor must not hijack a braced init (immer::vector's (size_type, T) fill
// ctor narrowed and hard-errored; non-narrowing shapes silently ran the
// wrong ctor).
namespace parens_init_test {
struct FillVec {
    std::vector<int> data;
    FillVec(std::initializer_list<int> il) : data(il) {}
    FillVec(std::size_t n, int v) : data(n, v) {}    // must bind AND run as fill
    int size() const { return (int) data.size(); }
    int sum() const { int s = 0; for (int x : data) s += x; return s; }
};
} // namespace parens_init_test

// BINDER-0017: a bare class-pointer return defaults to a BORROWING policy
// (reference_internal on methods, reference on statics/free functions), not
// nanobind's automatic/take_ownership -- the fluent-builder/accessor pattern
// (CLI11's add_option) double-freed under the old default.
namespace ownership_test {

struct Item {
    int v = 7;
    int get() const { return v; }
};

class Registry {
    std::vector<std::unique_ptr<Item>> items_;
public:
    Registry() = default;
    Registry(const Registry&) = delete;
    Item* add() {                                    // borrowed pointer into owned storage
        items_.push_back(std::make_unique<Item>());
        return items_.back().get();
    }
    Registry* self() { return this; }                // fluent self-return
    int count() const { return (int) items_.size(); }
    int sum() const {
        int s = 0;
        for (auto& i : items_) s += i->v;
        return s;
    }
    static Item* shared_item() {                     // static -> rv_policy::reference
        static Item it{99};
        return &it;
    }
};

inline Item* free_shared_item() {                    // free fn -> rv_policy::reference
    static Item it{55};
    return &it;
}

} // namespace ownership_test

// BINDER-0018: C-style `typedef struct {...} name_t;` declares an ANONYMOUS
// record; the binder used to hard-error computing its name via identifier_of.
// It now binds under the typedef name for linkage (tinyobjloader's idiom).
namespace anon_typedef_test {

typedef struct {     // C-compatible (data only), exactly the tinyobjloader idiom
    int x;
    int y;
} point_t;

typedef enum { ANON_RED = 1, ANON_GREEN = 2 } color_t;

} // namespace anon_typedef_test

// BINDER-0020: an in-class-initialized `static const` with no out-of-line
// definition used to bind by address (&[:mem:]), ODR-using it -> undefined
// symbol at link (moodycamel::ConcurrentQueue's config constants). Constant-
// readable statics now bind by value.
namespace static_const_test {

struct Config {
    static const int BLOCK = 7;                  // in-class init, NO definition
    static const long long BIG = 1LL << 40;      // ditto
    static constexpr double RATIO = 2.5;         // constexpr (inline) value path
    static int counter;                          // mutable: address path
    int id = 0;
};
inline int Config::counter = 3;

} // namespace static_const_test
