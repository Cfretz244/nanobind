.. _reflection:

C++26 Static Reflection
========================

nanobind supports automatic binding generation using C++26 static reflection
(P2996). Instead of manually writing ``.def()`` calls for every member, you can
reflect entire types and namespaces in a single line.

Requirements
------------

A C++ compiler with `P2996 <https://wg21.link/p2996>`_ static reflection
support. Known compatible compilers include:

- **GCC 16+** with ``-std=c++26 -freflection``
- **Bloomberg clang-p2996** fork

The header is guarded by ``__cpp_reflection`` / ``__cpp_impl_reflection`` and
compiles to nothing on compilers without reflection support, so it is safe to
include unconditionally.

Basic usage
-----------

Include ``nanobind/nb_reflect.h`` and use ``nb::reflect_``:

.. code-block:: cpp

   #include <nanobind/nb_reflect.h>

   namespace nb = nanobind;

   struct Point {
       double x, y, z;

       Point() : x(0), y(0), z(0) {}
       Point(double x, double y, double z) : x(x), y(y), z(z) {}

       double length() const;
       void scale(double factor);
       static Point origin() { return {0, 0, 0}; }
   };

   enum class Color { Red, Green, Blue };

   NB_MODULE(my_module, m) {
       nb::reflect_<^^Color, ^^Point>(m);
   }

This single call is equivalent to:

.. code-block:: cpp

   nb::enum_<Color>(m, "Color")
       .value("Red", Color::Red)
       .value("Green", Color::Green)
       .value("Blue", Color::Blue);

   nb::class_<Point>(m, "Point")
       .def(nb::init<>())
       .def(nb::init<double, double, double>())
       .def_rw("x", &Point::x)
       .def_rw("y", &Point::y)
       .def_rw("z", &Point::z)
       .def("length", &Point::length)
       .def("scale", &Point::scale)
       .def_static("origin", &Point::origin);

What gets bound
---------------

For **classes**, ``reflect_`` automatically binds:

- All public non-copy/move constructors (including default and parameterized)
- All public nonstatic data members (read-write; read-only if ``const``)
- All public static data members (read-only if ``const``, read-write otherwise)
- All public nonstatic methods (including overloads, and ``const``, ``noexcept``,
  and lvalue-ref-qualified (``&``) methods)
- All public static methods
- Overloaded operators, mapped to Python dunders (see `Operators`_)
- Conversion functions to ``bool``/integral/floating types (``__bool__`` /
  ``__int__`` / ``__float__``)
- A single public base class (see `Inheritance`_)

For **enums**, all enumerators are bound by name.

Names are derived from the C++ identifiers automatically -- no strings needed.

Namespace reflection
--------------------

You can reflect an entire namespace at once. All classes, enums, and free
functions within it are bound:

.. code-block:: cpp

   namespace game {
       struct Enemy { std::string type; int hp; };
       struct Weapon { std::string name; int damage; };
       enum class Rarity { Common, Rare, Legendary };
       double damage_per_second(double damage, double speed);
   }

   NB_MODULE(my_module, m) {
       nb::reflect_<^^game>(m);
   }

Mixing types and namespaces
---------------------------

``reflect_`` accepts any mix of reflected types, enums, namespaces, and free
functions:

.. code-block:: cpp

   nb::reflect_<^^Color, ^^Point, ^^Player, ^^game, ^^some_free_function>(m);

Overloaded functions
--------------------

Overloaded methods and free functions are handled automatically. Each overload
is registered as a separate binding, and nanobind's dispatch resolves them at
call time based on argument types:

.. code-block:: cpp

   struct Point {
       double distance(const Point& other) const;
       double distance(double x, double y, double z) const;
   };

.. code-block:: python

   p1 = my_module.Point(1, 2, 3)
   p2 = my_module.Point(4, 5, 6)
   p1.distance(p2)           # calls Point::distance(const Point&)
   p1.distance(4.0, 5.0, 6.0)  # calls Point::distance(double, double, double)

Keyword arguments
-----------------

Parameter names are recovered via P3096 parameter reflection and emitted as
``nb::arg("name")``, so callers can use Python keyword arguments on methods,
static methods, free functions, and constructors:

.. code-block:: cpp

   struct Rect {
       Rect(int width, int height);
       int area(int scale) const;
   };

.. code-block:: python

   r = my_module.Rect(width=3, height=4)   # constructor keywords
   r.area(scale=2)                         # method keyword

A parameter that has no name in the C++ declaration is bound as an unnamed
positional argument; if *no* parameter of a function is named, the function is
bound exactly as before (positional only).

Default-argument **values** are *not* bound. This is a limitation of the C++26
standard itself, not of this binder: P3096 exposes only ``has_default_argument``
(a ``bool``) and provides no way to read a default argument's value or
expression -- a default argument is an arbitrary expression evaluated in the
caller's context, not a reflectable entity. A bound function therefore still
requires every argument to be supplied (by position or keyword), even those that
have a C++ default.

Inheritance
-----------

Single inheritance is handled automatically. A derived class is bound as
``nb::class_<Derived, Base>`` so that, on the Python side, it is a subclass of
its base and transparently exposes the base's (already bound) members:

.. code-block:: cpp

   struct Animal { std::string name; std::string sound() const; };
   struct Dog : Animal { int good_boy_points; };

   NB_MODULE(my_module, m) {
       nb::reflect_<^^Dog>(m);   // Animal is bound automatically, too
   }

.. code-block:: python

   d = my_module.Dog()
   d.name = "Rex"            # inherited from Animal
   d.sound()                 # inherited method
   issubclass(my_module.Dog, my_module.Animal)   # True

The base class is bound **transitively**: if a derived class's base was not
itself passed to ``reflect_`` (for example, it lives in another namespace or a
library), it is bound on demand so the inheritance relationship still works.
Binding is idempotent -- a type reached more than once is bound only the first
time -- so ordering of types within ``reflect_`` does not matter.

Multiple inheritance
~~~~~~~~~~~~~~~~~~~~~~

nanobind models a single C++ base per Python type. When a class has more than
one public base, the **first** public base becomes the real Python base class,
and the public members of every **additional** base (and of that base's own
bases) are *flattened* directly onto the derived type -- so they remain fully
accessible from Python:

.. code-block:: cpp

   struct Drawable { void draw() const; };
   struct Serializable { std::string serialize() const; };
   struct Widget : Drawable, Serializable { int id; };

.. code-block:: python

   w = my_module.Widget()
   w.draw()            # via the real base (Drawable)
   w.serialize()       # flattened from the secondary base (Serializable)
   w.id
   issubclass(my_module.Widget, my_module.Drawable)      # True
   issubclass(my_module.Widget, my_module.Serializable)  # False (see below)

The single thing lost for the secondary bases is the *type relationship*:
``isinstance``/``issubclass`` against them is ``False``, and a ``Widget`` cannot
be passed where a bound function expects a ``Serializable&``. Member access,
however, is preserved. Diamond hierarchies are handled without binding any
member twice (a base reached through the primary chain is not also flattened).

Operators
---------

Member ``operator@`` overloads are mapped to the corresponding Python dunder
methods automatically -- arithmetic and bitwise (``__add__``, ``__mul__``,
``__lshift__``, …), comparisons (``__eq__``, ``__lt__``, …), in-place
(``__iadd__``, … -- these preserve object identity), ``operator()`` →
``__call__``, ``operator[]`` → ``__getitem__``, unary ``-``/``+``/``~`` →
``__neg__``/``__pos__``/``__invert__``, and conversion functions to
``bool``/integral/floating → ``__bool__``/``__int__``/``__float__``. Operators are
bound with ``nb::is_operator()``, so calling one with an incompatible type yields
``NotImplemented`` (letting Python try the reflected operand) rather than raising.

Binary **free (namespace-scope) operators** are mapped too. A ``operator@(L, R)``
is attached as a dunder to its operand's class: the forward dunder (``__add__``,
…) on ``L``, and the reversed dunder on ``R`` (``__radd__``, …; comparisons use
the swapped operator). The reversed form is what lets a scalar-on-the-left
expression such as ``2.0 * vec`` work when only the right operand is a bound type:

.. code-block:: cpp

   struct Vec { double x, y; };
   Vec  operator*(const Vec& v, double s);   // -> Vec.__mul__
   Vec  operator*(double s, const Vec& v);   // -> Vec.__rmul__
   Vec  operator+(const Vec& a, const Vec& b);  // -> Vec.__add__
   bool operator==(const Vec& a, const Vec& b); // -> Vec.__eq__

.. code-block:: python

   v * 2.0     # operator*(Vec, double)
   2.0 * v     # operator*(double, Vec), via __rmul__
   a + b       # operator+(Vec, Vec)

Skipped: ``operator<=>``, ``++``/``--``, ``operator->``, logical ``&&``/``||``/``!``,
and assignment ``operator=``.

Virtual functions (overriding from Python)
------------------------------------------

Letting a *Python* subclass override a C++ ``virtual`` (so that C++ code calling
through a base pointer dispatches into the Python override) requires nanobind's
*trampoline*: a class derived from the bound type that overrides each virtual to
forward into Python. A trampoline cannot be synthesized in-language (P2996 has no
way to inject member functions), so reflection handles virtuals in **two tiers**.

In both tiers, ``reflect_`` consults a trait: when a trampoline is registered for a
class (via ``NB_REFLECT_TRAMPOLINE(Type, Trampoline)``) it is passed to nanobind as
the ``class_`` *Alias*. The two tiers differ only in who writes the trampoline.

**Tier 1 -- hand-written trampoline.** Write the trampoline as you would for plain
nanobind and register it; ``reflect_`` wires it in and still auto-binds everything
else:

.. code-block:: cpp

   struct Shape { virtual double area() const = 0; virtual ~Shape() = default; };

   struct PyShape : Shape {                 // outside the reflected namespace
       NB_TRAMPOLINE(Shape, 1);
       double area() const override { NB_OVERRIDE_PURE(area); }
   };
   NB_REFLECT_TRAMPOLINE(Shape, PyShape);

**Tier 2 -- generated trampoline (codegen fallback).** ``<nanobind/nb_reflect_codegen.h>``
provides ``emit_trampolines<^^ns...>()``, which returns C++ source text containing the
required ``<nanobind/stl/*.h>`` caster includes (see `STL type casters`_) plus a
trampoline (and its ``NB_REFLECT_TRAMPOLINE``) for every class with overridable
virtuals. A small generator program writes it to a header that the bindings TU
includes before calling ``reflect_``. The build runs the same three steps whether or
not any trampoline is needed (the generated header may be empty):

.. code-block:: cpp

   // generator.cpp
   #include <nanobind/nb_reflect_codegen.h>
   #include "my_types.h"
   int main(int argc, char** argv) {
       return nanobind::write_trampolines(
           argv[1], nanobind::emit_trampolines<^^my_namespace>()) ? 0 : 1;
   }

.. code-block:: cmake

   add_executable(gen generator.cpp)
   target_compile_options(gen PRIVATE ${REFLECT_FLAGS})
   add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/trampolines.gen.h
                      COMMAND gen ${CMAKE_CURRENT_BINARY_DIR}/trampolines.gen.h
                      DEPENDS gen)
   nanobind_add_module(my_ext bindings.cpp ${CMAKE_CURRENT_BINARY_DIR}/trampolines.gen.h)
   target_include_directories(my_ext PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

.. code-block:: cpp

   // bindings.cpp
   #include "my_types.h"
   #include "trampolines.gen.h"     // generated; may be empty
   NB_MODULE(my_ext, m) { nanobind::reflect_<^^my_namespace>(m); }

The generated code refers to each virtual's return and parameter types via splices
off the method's reflection, so it needs no C++ type-name printer and resolves
overloaded virtuals correctly. Inherited virtuals are included; the destructor and
private virtuals are skipped.

STL type casters
----------------

Binding a signature that uses a std type (``std::vector``, ``std::map``,
``std::optional``, ``std::shared_ptr``, …) requires the matching
``<nanobind/stl/*.h>`` type-caster header. The binder detects, via reflection, which
std types appear in a namespace's bound signatures (recursively, e.g.
``std::map<std::string, std::vector<int>>``) and maps each to its caster header.
Because ``#include`` cannot be emitted from template code into the current
translation unit, this is handled differently on the two paths:

- **Codegen path** -- ``emit_trampolines<^^ns...>()`` *emits* the required
  ``#include <nanobind/stl/...>`` lines into the generated header. Since that header
  is included before ``reflect_``, a generated module needs **no** hand-listed stl
  casters. (``std::string`` is always available -- ``nb_reflect.h`` includes its
  caster.)

- **Header-only path** -- ``reflect_<^^ns>(m)`` in a single TU cannot inject includes,
  so it instead *diagnoses*: if a std type's caster is missing, compilation fails with
  a message naming the exact header, e.g.::

     nb::reflect_: a bound signature uses the std type 'vector<int, allocator<int>>',
     whose nanobind type caster is not included in this translation unit.
     Add: #include <nanobind/stl/vector.h>

  ``nb::detail::required_stl_headers<^^ns>()`` returns the full list programmatically.

Only the common std types are recognized (string/string_view/wstring,
vector/list/array/set/unordered_set, map/unordered_map, pair/tuple, optional/variant,
shared_ptr/unique_ptr, function, complex). Container "policy" parameters (allocator,
comparator, hash, …) are ignored. Std types used only by an *external* base class not
itself in the reflected set are not detected.

Controlling the bindings with annotations
-----------------------------------------

You can tag declarations in your own C++ code to control what ``reflect_`` binds and
how, using P3394 annotation-attributes (``[[=value]]``). The vocabulary lives in a
dependency-free header so it can be included by *library* code without pulling in
nanobind:

.. code-block:: cpp

   #include <nanobind/nb_reflect_annotations.h>
   namespace r = nanobind::reflect;

   struct [[=r::skip{}]] Internal { ... };        // never bound

   struct Widget {
       int width;
       [[=r::skip{}]] int cache;                   // data member omitted

       [[=r::rename{"size"}]] int get_size() const;     // bound as "size"
       [[=r::doc{"Reset to defaults."}]] void reset();   // docstring

       // Return-value lifetime/ownership policy (maps to nanobind's rv_policy):
       [[=r::reference_internal]] Buffer& buffer();
       [[=r::reference]] Manager* manager() const;
   };

   // Tie lifetimes: keep argument 1 (the parent) alive as long as the return lives.
   [[=r::keep_alive{0, 1}]] Child* make_child(Parent& parent);

Available annotations:

- ``skip`` — exclude a class, enum, member, data member, constructor, or free function.
- ``rename{"name"}`` — bind under a different Python name.
- ``doc{"text"}`` — attach a docstring (functions, methods, data members, classes, enums).
  On a class or enum the annotation follows the ``struct``/``enum class`` keyword, e.g.
  ``struct [[=r::doc{"..."}]] Widget { ... };`` and
  ``enum class [[=r::doc{"..."}]] Color { ... };``.
- ``return_policy{...}`` / the shorthands ``take_ownership``, ``copy``, ``move``,
  ``reference``, ``reference_internal``, ``take_nothing`` — set the return-value policy on
  a function or method.
- ``keep_alive{nurse, patient}`` — tie one argument's/return's lifetime to another
  (index 0 = return, 1 = ``self`` / first argument, ...; see nanobind's ``keep_alive``).
- ``property{"name"}`` — bind a getter/setter pair as a Python property (see `Properties`_).

Annotations are read at compile time; an unannotated entity uses the defaults, so adding
annotations is purely incremental.

Properties
----------

Annotate a getter/setter pair with the same ``property`` name to bind a Python
``@property`` instead of two methods. The parameter-less accessor is the getter and the
one-parameter accessor the setter; annotate only the getter for a read-only property.
The C++ accessor names are irrelevant (the property name comes from the annotation), and
the getter's return-policy / docstring annotations apply to the property:

.. code-block:: cpp

   struct Thermo {
       double c_;
       [[=r::property{"celsius"}]] double celsius() const { return c_; }    // getter
       [[=r::property{"celsius"}]] void   celsius(double v) { c_ = v; }     // setter
       [[=r::property{"fahrenheit"}]] double to_f() const { return c_*9/5+32; } // read-only
   };

.. code-block:: python

   t = Thermo()
   t.celsius = 100        # setter
   assert t.celsius == 100 and t.fahrenheit == 212
   t.fahrenheit = 0       # AttributeError -- read-only

The accessors are bound via pointer-to-member (so no spliced type enters a lambda
signature). Pairing is annotation-driven only; getter/setter *name-convention* sniffing
(``getX``/``setX``) is intentionally not done.

Limitations
-----------

- Requires a compiler with P2996 support. As of March 2026, mainline Clang
  and MSVC do not implement the proposal.
- Private and protected members are skipped.
- ``volatile`` methods, rvalue-ref-qualified (``&&``) methods, and C-variadic
  (``...``) functions are skipped (they cannot bind meaningfully to a persistent
  Python object); the rest of the class still binds.
- Member and binary free operators are mapped to Python dunders (see `Operators`_);
  ``operator<=>``, ``++``/``--``, and logical ``&&``/``||``/``!`` are skipped.
- **Multiple inheritance**: nanobind supports a single base class. The first
  public base is the real Python base; additional bases are flattened (their
  members are exposed on the derived type, but ``isinstance``/``issubclass``
  against them is ``False`` and the derived type cannot be passed where those
  bases are expected at the binding boundary). See `Multiple inheritance`_.
- **Virtual functions**: overriding from Python works via a trampoline that is
  hand-written or generated (see `Virtual functions (overriding from Python)`_),
  since trampolines cannot be synthesized in-language. Ref-qualified
  (``&``/``&&``) and ``final`` virtuals are not generated; virtual (diamond) base
  layouts are untested.
- **Annotations**: per-argument ownership transfer is not yet handled (see
  `Controlling the bindings with annotations`_).
- **Default-argument values** are not bound — a standard limitation, not a binder
  one (P3096 exposes only ``has_default_argument``). See `Keyword arguments`_.
- **STL casters**: the header-only path cannot inject ``#include``s (it diagnoses
  instead); only the common std types are recognized, and types used solely by an
  external base outside the reflected set are not detected. See `STL type casters`_.
