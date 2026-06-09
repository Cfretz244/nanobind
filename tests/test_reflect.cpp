#include <nanobind/nb_reflect.h>
#include <nanobind/trampoline.h>
#include <nanobind/stl/vector.h>
#include <nanobind/stl/string.h>
#include <cmath>
#include <string>
#include <vector>

namespace nb = nanobind;

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

NB_MODULE(test_reflect_ext, m) {
    nb::reflect_<^^reflect_test>(m);
}
