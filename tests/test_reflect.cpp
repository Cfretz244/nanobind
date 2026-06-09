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

// A base class that lives OUTSIDE the reflected namespace and is never passed to
// reflect_<...>. It must still be bound transitively so the derived class works.
namespace external_bases {

struct ExtBase {
    int eb;
    ExtBase() : eb(0) {}
    int ext_method() const { return eb; }
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

// Derived from a base outside the reflected namespace (bound transitively).
struct UsesExtBase : external_bases::ExtBase {
    int ub;
    UsesExtBase() : ub(0) {}
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

NB_MODULE(test_reflect_ext, m) {
    // Box<float> is referenced by no signature; it is bound only because it is listed
    // explicitly here (the explicit opt-in for specializations the walk can't reach).
    // identity<int> is a free-function-template specialization, also explicit-only.
    nb::reflect_<^^reflect_test, ^^template_test,
                 ^^template_test::Box<float>,
                 ^^template_test::identity<int>>(m);
}
