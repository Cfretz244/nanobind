import pytest

try:
    import test_reflect_ext as t
    def needs_reflect(x):
        return x
except ImportError:
    needs_reflect = pytest.mark.skip(reason="C++26 reflection support required")


@needs_reflect
def test01_enum():
    assert t.Enum.A.name == 'A'
    assert t.Enum.B.name == 'B'
    assert t.Enum.C.name == 'C'
    assert t.Enum.A.value == 0
    assert t.Enum.B.value == 1
    assert t.Enum.C.value == 2


@needs_reflect
def test02_default_ctor():
    s = t.Struct()
    assert s.i == 0
    assert s.d == 0.0
    assert s.s == ''


@needs_reflect
def test03_param_ctor():
    s = t.Struct(3, 1.5)
    assert s.i == 3
    assert s.d == 1.5


@needs_reflect
def test04_data_member_rw():
    s = t.Struct()
    s.i = 7
    s.d = 2.5
    s.s = 'hello'
    assert s.i == 7
    assert s.d == 2.5
    assert s.s == 'hello'


@needs_reflect
def test05_const_method():
    s = t.Struct(3, 1.5)
    assert s.get_i() == 3
    assert abs(s.sum() - 4.5) < 1e-10


@needs_reflect
def test06_nonconst_method():
    s = t.Struct()
    s.set_i(10)
    assert s.get_i() == 10


@needs_reflect
def test07_method_overload():
    s = t.Struct(0, 1.0)
    assert abs(s.overloaded(2.0) - 3.0) < 1e-10
    assert abs(s.overloaded(2, 3) - 6.0) < 1e-10


@needs_reflect
def test08_static_const():
    assert t.Struct.static_const == 42


@needs_reflect
def test09_static_mutable():
    before = t.Struct.static_mut
    t.Struct()
    assert t.Struct.static_mut == before + 1


@needs_reflect
def test10_static_method():
    assert t.Struct.create_count() >= 0


@needs_reflect
def test11_ns_class():
    a = t.A()
    a.x = 1
    a.y = 2.0
    assert a.x == 1
    assert a.y == 2.0

    b = t.B()
    b.s = 'test'
    assert b.s == 'test'


@needs_reflect
def test12_ns_enum():
    assert t.E.X.name == 'X'
    assert t.E.Z.name == 'Z'
    assert t.E.X.value == 0
    assert t.E.Z.value == 2


@needs_reflect
def test13_free_fn():
    assert abs(t.free_fn(1.0, 2.0) - 3.0) < 1e-10


@needs_reflect
def test14_free_fn_overload():
    assert abs(t.free_fn(1.0, 2.0, 3.0) - 6.0) < 1e-10


@needs_reflect
def test15_string_member():
    n = t.Nested()
    n.name = 'abc'
    assert n.name == 'abc'


@needs_reflect
def test16_vector_member():
    n = t.Nested()
    n.items = [1, 2, 3]
    assert n.items == [1, 2, 3]


@needs_reflect
def test17_nested_type():
    n = t.Nested()
    s = t.Struct(5, 1.0)
    n.inner = s
    assert n.inner.i == 5
    assert n.inner.d == 1.0


@needs_reflect
def test18_private_members_hidden():
    m = t.Mixed()
    m.pub_i = 7
    m.pub_s = 'hi'
    assert m.pub_i == 7
    assert m.pub_s == 'hi'
    assert m.get_pub() == 7

    assert not hasattr(m, 'priv_i')
    assert not hasattr(m, 'priv_d')
    assert not hasattr(m, 'priv_method')
    assert not hasattr(m, 'prot_i')


@needs_reflect
def test19_single_inheritance():
    d = t.Derived()
    d.b = 3   # inherited data member
    d.d = 4   # own data member
    assert d.b == 3
    assert d.d == 4
    assert d.base_method() == 3       # inherited method
    assert d.derived_method() == 4    # own method
    assert issubclass(t.Derived, t.Base)
    assert isinstance(d, t.Base)


@needs_reflect
def test20_multilevel_chain():
    x = t.L2()
    x.v0 = 1
    x.v1 = 2
    x.v2 = 3
    assert (x.m0(), x.m1(), x.m2()) == (1, 2, 3)
    assert issubclass(t.L2, t.L1)
    assert issubclass(t.L1, t.L0)
    assert issubclass(t.L2, t.L0)
    assert isinstance(x, t.L0)


@needs_reflect
def test21_external_base_transitive():
    # ExtBase lives outside reflect_test and was never passed to reflect_<...>;
    # it must have been bound transitively because UsesExtBase derives from it.
    assert hasattr(t, 'ExtBase')
    u = t.UsesExtBase()
    u.eb = 5   # inherited from the external base
    u.ub = 6   # own member
    assert u.eb == 5
    assert u.ub == 6
    assert u.ext_method() == 5
    assert issubclass(t.UsesExtBase, t.ExtBase)


@needs_reflect
def test22_multiple_bases_flattened():
    # MultiDerived : MixinA, MixinB -> MixinA is the real nanobind base, and
    # MixinB's members are flattened directly onto MultiDerived.
    md = t.MultiDerived()
    md.md = 9
    md.a = 1            # from the first base (MixinA), via the Python base
    md.b2 = 2           # from the second base (MixinB), flattened
    assert md.md == 9
    assert md.a == 1
    assert md.b2 == 2
    assert md.from_a() == 1
    assert md.from_b() == 2     # flattened method works
    # Only the first base is a real Python base.
    assert issubclass(t.MultiDerived, t.MixinA)
    assert isinstance(md, t.MixinA)
    # The secondary base relationship is NOT modeled (single-base limitation).
    assert not issubclass(t.MultiDerived, t.MixinB)
    assert not isinstance(md, t.MixinB)
    # MixinB is still bound on its own (it is a member of the namespace).
    assert t.MixinB().from_b() == 0


@needs_reflect
def test23_secondary_subtree_flattened():
    # Combo : PrimaryX, Mid; Mid : SecBase. PrimaryX is the real base; Mid AND
    # its base SecBase are flattened onto Combo.
    c = t.Combo()
    c.cm = 1
    c.px = 2            # PrimaryX (real base)
    c.mid = 3           # Mid (flattened)
    c.sb = 4            # SecBase, base of the secondary base (flattened)
    assert (c.cm, c.px, c.mid, c.sb) == (1, 2, 3, 4)
    assert c.sec_method() == 4
    assert issubclass(t.Combo, t.PrimaryX)
    assert not issubclass(t.Combo, t.Mid)
    assert not issubclass(t.Combo, t.SecBase)


@needs_reflect
def test24_diamond_no_double_bind():
    # Dia : DiaL, DiaR; both DiaL and DiaR : DiaTop. DiaTop is reached through the
    # primary chain (DiaL), so it is a real ancestor and is not flattened twice.
    d = t.Dia()
    d.db = 1
    d.dl = 2            # via DiaL (real base)
    d.dt = 3            # via DiaTop (real ancestor through DiaL)
    d.dr = 4            # DiaR's own member, flattened
    assert (d.db, d.dl, d.dt, d.dr) == (1, 2, 3, 4)
    assert d.top_method() == 3
    assert issubclass(t.Dia, t.DiaL)
    assert issubclass(t.Dia, t.DiaTop)   # via the primary chain / MRO
    assert not issubclass(t.Dia, t.DiaR)


@needs_reflect
def test28_annotation_skip_rename_doc():
    # skip: class, method, data member, and free function are absent.
    assert not hasattr(t, 'HiddenClass')
    assert not hasattr(t, 'hidden_free')
    a = t.Annotated()
    assert hasattr(a, 'kept') and hasattr(a, 'visible')
    assert not hasattr(a, 'secret')
    assert not hasattr(a, 'hidden_method')
    # rename: present under the new name, absent under the old.
    assert a.renamed() == 7
    assert not hasattr(a, 'original_name')
    assert t.renamed_free() == 3
    assert not hasattr(t, 'original_free')
    # doc: docstring is attached.
    assert a.documented() == 9
    assert t.Annotated.documented.__doc__ is not None
    assert 'documented method' in t.Annotated.documented.__doc__


@needs_reflect
def test29_annotation_lifetime():
    # reference_internal: repeated access returns the SAME Python object (proof the
    # return-value policy was applied; a default ref return would copy).
    h = t.Holder()
    assert h.get_inner() is h.get_inner()
    # the returned view stays valid while the parent lives
    h.get_inner().v = 5
    assert h.get_inner().v == 5
    # keep_alive-annotated method binds and works.
    x = t.KA()
    y = t.KA()
    x.absorb(y)
    assert x.total == 1


@needs_reflect
def test27_operators():
    a = t.Ops(2)
    b = t.Ops(3)
    assert (a + b).x == 5          # __add__
    assert (-a).x == -2            # __neg__
    assert (a == t.Ops(2))         # __eq__
    assert (a != b)                # __ne__ derived from __eq__ by Python
    assert (a < b)                 # __lt__
    assert t.Ops(4)(3) == 12       # __call__
    assert t.Ops(4)[1] == 5        # __getitem__
    assert bool(t.Ops(1)) and not bool(t.Ops(0))   # __bool__
    # __iadd__ mutates in place, preserving object identity.
    c = t.Ops(2)
    cid = id(c)
    c += b
    assert c.x == 5 and id(c) == cid
    # is_operator(): mismatched type yields NotImplemented, not TypeError.
    assert a.__add__("nope") is NotImplemented


@needs_reflect
def test26_function_qualifiers():
    q = t.Quals()
    # noexcept / const noexcept / lvalue-ref-qualified methods are bound.
    assert q.plain(1) == 2
    assert q.plain_ne(1) == 3
    assert q.c_ne(1) == 1
    assert q.lref() == 10
    # static noexcept and free noexcept are bound.
    assert t.Quals.sfn_ne() == 99
    assert t.free_ne(2.5) == 5.0
    # rvalue-ref-qualified, volatile, and C-variadic shapes are skipped (not bound)
    # rather than breaking the build.
    assert not hasattr(q, 'rref')
    assert not hasattr(q, 'vol')
    assert not hasattr(q, 'va')


@needs_reflect
def test25_virtual_override_from_python():
    # A Python subclass overrides a C++ virtual; C++ code calling through a base
    # reference must dispatch into the Python override (this is what the
    # trampoline provides).
    class MyShape(t.Shape):
        def area(self):
            return 42.0
        # kind() intentionally NOT overridden -> should fall back to C++

    s = MyShape()
    assert s.area() == 42.0                  # direct Python call
    assert t.call_area(s) == 42.0            # C++ -> Python override dispatch
    assert s.kind() == 'shape'               # inherited C++ implementation
    assert t.call_kind(s) == 'shape'         # C++ -> C++ base fallback
