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
- All public nonstatic methods (including overloads)
- All public static methods
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

Limitations
-----------

- Requires a compiler with P2996 support. As of March 2026, mainline Clang
  and MSVC do not implement the proposal.
- Private and protected members are skipped.
- Operator overloads (``operator+``, etc.) are not yet mapped to Python
  dunder methods (as of March 2026).
- **Multiple inheritance**: nanobind supports a single base class. The first
  public base is the real Python base; additional bases are flattened (their
  members are exposed on the derived type, but ``isinstance``/``issubclass``
  against them is ``False`` and the derived type cannot be passed where those
  bases are expected at the binding boundary). See `Multiple inheritance`_.
- Virtual functions are not bound with trampolines, so overriding C++ virtual
  methods from Python is not yet supported. Virtual (and diamond) inheritance
  layouts are untested.
