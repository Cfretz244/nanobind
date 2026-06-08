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
def test22_multiple_bases_first_only():
    # MultiDerived : MixinA, MixinB -> only MixinA becomes the nanobind base.
    md = t.MultiDerived()
    md.md = 9
    md.a = 1            # from the first base (MixinA)
    assert md.md == 9
    assert md.a == 1
    assert md.from_a() == 1
    assert issubclass(t.MultiDerived, t.MixinA)
    # MixinB's members are NOT exposed on MultiDerived (single-base limitation).
    assert not issubclass(t.MultiDerived, t.MixinB)
    assert not hasattr(md, 'b2')
    assert not hasattr(md, 'from_b')
    # MixinB is still bound on its own (it is a member of the namespace).
    assert t.MixinB().from_b() == 0
