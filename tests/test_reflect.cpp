#include <nanobind/nb_reflect.h>
#include <nanobind/nb_reflect_annotations.h>
#include <nanobind/trampoline.h>
#include <nanobind/stl/vector.h>
#include <nanobind/stl/string.h>
#include <cmath>
#include <string>
#include <vector>

namespace nb = nanobind;
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

int Struct::static_mut = 0;

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

double free_fn(double a, double b) { return a + b; }
double free_fn(double a, double b, double c) { return a + b + c; }

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

double free_ne(double a) noexcept { return a * 2; }         // free noexcept -> bound

// --- Annotation-driven control (skip / rename / doc / lifetime) ---

struct [[=r::skip{}]] HiddenClass {
    int x;
    HiddenClass() : x(0) {}
};

struct Annotated {
    int kept;
    [[=r::skip{}]] int secret;                    // skipped data member
    Annotated() : kept(0), secret(0) {}

    int visible() const { return kept; }
    [[=r::skip{}]] int hidden_method() const { return 1; }        // skipped
    [[=r::rename{"renamed"}]] int original_name() const { return 7; }
    [[=r::doc{"documented method"}]] int documented() const { return 9; }
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
    [[=r::reference_internal]] Inner& get_inner() { return inner; }
};

struct KA {
    int total;
    KA() : total(0) {}
    // keep_alive{1,2}: nurse = self (1), patient = the argument (2).
    [[=r::keep_alive{1, 2}]] void absorb(KA& other) { total += other.total + 1; }
};

[[=r::skip{}]] int hidden_free() { return 1; }
[[=r::rename{"renamed_free"}]] int original_free() { return 3; }

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

struct ManyConv {
    long long v;
    ManyConv() : v(0) {}
    explicit ManyConv(long long n) : v(n) {}
    explicit operator char() const { return (char) v; }       // narrow -- skipped
    explicit operator long long() const { return v; }         // widest -> __int__
    explicit operator int() const { return (int) v; }         // declared last -- skipped
    explicit operator bool() const { return v != 0; }         // __bool__ unaffected
};

// --- Virtual functions / trampoline (Tier 1: hand-written trampoline) ---

struct Shape {
    Shape() = default;
    virtual ~Shape() = default;
    virtual double area() const = 0;                      // pure virtual
    virtual std::string kind() const { return "shape"; }  // non-pure virtual
};

// Free functions that invoke the virtuals from C++ -- used to prove that a
// Python override is dispatched into when C++ calls through a base reference.
double call_area(const Shape& s) { return s.area(); }
std::string call_kind(const Shape& s) { return s.kind(); }

// --- Keyword-argument names (P3096 parameter names -> nb::arg) ---

struct Kw {
    Kw() {}
    int add(int a, int b) const { return a + b; }     // method kwargs
    static int smul(int x, int y) { return x * y; }   // static-method kwargs
};

int kw_sub(int a, int b) { return a - b; }            // free-function kwargs

struct KwCtor {                                       // constructor kwargs
    int i, j;
    KwCtor(int i, int j) : i(i), j(j) {}
};

// --- Class / enum docstrings (annotation on the type itself) ---

struct [[=r::doc{"A documented class."}]] DocClass {
    DocClass() {}
    int v() const { return 1; }
};

enum class [[=r::doc{"A documented enum."}]] DocEnum { X, Y };

// --- Free (namespace-scope) operators -> dunders ---

struct Vec {
    double x, y;
    Vec() : x(0), y(0) {}
    Vec(double x, double y) : x(x), y(y) {}
};

// Symmetric same-type operator -> forward __add__ only.
Vec operator+(const Vec& a, const Vec& b) { return Vec(a.x + b.x, a.y + b.y); }
// Scalar on the right -> forward __mul__ on Vec.
Vec operator*(const Vec& v, double s) { return Vec(v.x * s, v.y * s); }
// Scalar on the left -> reversed __rmul__ on Vec (the key reversed-dunder case).
Vec operator*(double s, const Vec& v) { return Vec(s * v.x, s * v.y); }
// Comparison free operator -> __eq__.
bool operator==(const Vec& a, const Vec& b) { return a.x == b.x && a.y == b.y; }
// Unary free operators -> __neg__ / __invert__ (binary-only was the old limit;
// absl::int128's negation is a free operator-(int128)).
Vec operator-(const Vec& v) { return Vec(-v.x, -v.y); }
Vec operator~(const Vec& v) { return Vec(v.y, v.x); }  // arbitrary, observable: swap

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
    [[=r::property{"celsius"}]] double celsius() const { return c_; }   // getter
    [[=r::property{"celsius"}]] void   celsius(double v) { c_ = v; }    // setter
    // Read-only property, with a C++ accessor name unrelated to the property name.
    [[=r::property{"fahrenheit"}]] double to_f() const { return c_ * 9.0 / 5.0 + 32.0; }
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

// The trampoline lives OUTSIDE the reflected namespace so reflect_ does not try
// to bind it as a class; it is wired in only via NB_REFLECT_TRAMPOLINE.
namespace reflect_test_tramp {
struct PyShape : reflect_test::Shape {
    NB_TRAMPOLINE(reflect_test::Shape, 2);
    double area() const override { NB_OVERRIDE_PURE(area); }
    std::string kind() const override { NB_OVERRIDE(kind); }
};
} // namespace reflect_test_tramp

NB_REFLECT_TRAMPOLINE(reflect_test::Shape, reflect_test_tramp::PyShape);

// --- Compile-time checks for the STL-caster detection core (roadmap #5) ---
// reflect_test binds std::string (Struct::s, ...) and std::vector<int>
// (Nested::items), and no other std container, so its required-caster set is
// exactly {string.h, vector.h}.
namespace {
consteval bool reflect_test_needs(std::string_view want) {
    for (const char* h : nb::detail::required_stl_headers<^^reflect_test>())
        if (std::string_view(h) == want)
            return true;
    return false;
}
}
static_assert(std::string_view(nb::detail::stl_caster_header(^^std::vector<int>)) ==
              "nanobind/stl/vector.h");
static_assert(nb::detail::stl_caster_header(^^int) == nullptr);
static_assert(reflect_test_needs("nanobind/stl/string.h"));
static_assert(reflect_test_needs("nanobind/stl/vector.h"));
static_assert(!reflect_test_needs("nanobind/stl/map.h"));

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
};

} // namespace template_test

// Discovery must find exactly the user specializations reachable from the signatures
// (and not pull in std types, which go to the caster path).
namespace {
consteval bool tt_has_spec(std::meta::info t) {
    for (auto s : nb::detail::required_user_specs(^^template_test))
        if (s == t)
            return true;
    return false;
}
}
static_assert(tt_has_spec(^^template_test::Box<int>));
static_assert(tt_has_spec(^^template_test::Box<double>));
static_assert(tt_has_spec(^^template_test::Box<template_test::Box<int>>));
static_assert(tt_has_spec(^^template_test::Wrap<int>));
static_assert(tt_has_spec(^^template_test::Pair<int, double>));
static_assert(tt_has_spec(^^template_test::Array<int, 3>));
static_assert(!tt_has_spec(^^std::vector<int>));     // std -> caster path, not bound
// Reachability: the spec itself is bound, its policy-only template arg is not.
static_assert(tt_has_spec(^^template_test::Cont<int, template_test::Pol<int>>));
static_assert(!tt_has_spec(^^template_test::Pol<int>));

NB_MODULE(test_reflect_ext, m) {
    // Box<float> is referenced by no signature; it is bound only because it is listed
    // explicitly here (the explicit opt-in for specializations the walk can't reach).
    // identity<int> is a free-function-template specialization, also explicit-only.
    nb::reflect_<^^reflect_test, ^^template_test,
                 ^^template_test::Box<float>,
                 ^^template_test::identity<int>,
                 ^^stream_test::Streamable>(m);
}
