import pytest

# Every test takes the bound module via the `t` fixture (tests/conftest.py),
# parameterized over BOTH backends: test_reflect_ext (constexpr reflect_) and
# test_reflect_emit_ext (the emit backend's generated source, compiled without
# reflection). One behavioral suite, two binders.


def test01_enum(t):
    assert t.Enum.A.name == 'A'
    assert t.Enum.B.name == 'B'
    assert t.Enum.C.name == 'C'
    assert t.Enum.A.value == 0
    assert t.Enum.B.value == 1
    assert t.Enum.C.value == 2


def test02_default_ctor(t):
    s = t.Struct()
    assert s.i == 0
    assert s.d == 0.0
    assert s.s == ''


def test03_param_ctor(t):
    s = t.Struct(3, 1.5)
    assert s.i == 3
    assert s.d == 1.5


def test04_data_member_rw(t):
    s = t.Struct()
    s.i = 7
    s.d = 2.5
    s.s = 'hello'
    assert s.i == 7
    assert s.d == 2.5
    assert s.s == 'hello'


def test05_const_method(t):
    s = t.Struct(3, 1.5)
    assert s.get_i() == 3
    assert abs(s.sum() - 4.5) < 1e-10


def test06_nonconst_method(t):
    s = t.Struct()
    s.set_i(10)
    assert s.get_i() == 10


def test07_method_overload(t):
    s = t.Struct(0, 1.0)
    assert abs(s.overloaded(2.0) - 3.0) < 1e-10
    assert abs(s.overloaded(2, 3) - 6.0) < 1e-10


def test08_static_const(t):
    assert t.Struct.static_const == 42


def test09_static_mutable(t):
    before = t.Struct.static_mut
    t.Struct()
    assert t.Struct.static_mut == before + 1


def test10_static_method(t):
    assert t.Struct.create_count() >= 0


def test11_ns_class(t):
    a = t.A()
    a.x = 1
    a.y = 2.0
    assert a.x == 1
    assert a.y == 2.0

    b = t.B()
    b.s = 'test'
    assert b.s == 'test'


def test12_ns_enum(t):
    assert t.E.X.name == 'X'
    assert t.E.Z.name == 'Z'
    assert t.E.X.value == 0
    assert t.E.Z.value == 2


def test13_free_fn(t):
    assert abs(t.free_fn(1.0, 2.0) - 3.0) < 1e-10


def test14_free_fn_overload(t):
    assert abs(t.free_fn(1.0, 2.0, 3.0) - 6.0) < 1e-10


def test15_string_member(t):
    n = t.Nested()
    n.name = 'abc'
    assert n.name == 'abc'


def test16_vector_member(t):
    n = t.Nested()
    n.items = [1, 2, 3]
    assert n.items == [1, 2, 3]


def test17_nested_type(t):
    n = t.Nested()
    s = t.Struct(5, 1.0)
    n.inner = s
    assert n.inner.i == 5
    assert n.inner.d == 1.0


def test18_private_members_hidden(t):
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


def test19_single_inheritance(t):
    d = t.Derived()
    d.b = 3   # inherited data member
    d.d = 4   # own data member
    assert d.b == 3
    assert d.d == 4
    assert d.base_method() == 3       # inherited method
    assert d.derived_method() == 4    # own method
    assert issubclass(t.Derived, t.Base)
    assert isinstance(d, t.Base)


def test20_multilevel_chain(t):
    x = t.L2()
    x.v0 = 1
    x.v1 = 2
    x.v2 = 3
    assert (x.m0(), x.m1(), x.m2()) == (1, 2, 3)
    assert issubclass(t.L2, t.L1)
    assert issubclass(t.L1, t.L0)
    assert issubclass(t.L2, t.L0)
    assert isinstance(x, t.L0)


def test21_external_base_flattened(t):
    # ExtBase lives outside reflect_test and was never passed to reflect_<...>,
    # so it is NOT in the bind set: being a base does not surface a type
    # (reachability rule). Its public members are flattened onto UsesExtBase
    # instead, and UsesExtBase has no Python base.
    assert not hasattr(t, 'ExtBase')
    u = t.UsesExtBase()
    u.eb = 5   # from the external base, flattened
    u.ub = 6   # own member
    assert u.eb == 5
    assert u.ub == 6
    assert u.ext_method() == 5
    assert t.UsesExtBase.__bases__ == (object,)


def test21b_two_level_unbound_chain_flattened(t):
    # MidUnbound and DeepUnbound (both outside the bind set) flatten transitively.
    x = t.UsesDeepChain()
    x.own = 1
    x.mid = 2
    x.deep = 3
    assert x.mid_method() == 200
    assert x.deep_method() == 30
    assert not hasattr(t, 'MidUnbound') and not hasattr(t, 'DeepUnbound')
    assert t.UsesDeepChain.__bases__ == (object,)


def test21c_python_base_through_unbound_link(t):
    # ChainThroughUnbound : MidToBase(unbound) : Base(bound). The in-set ancestor
    # is wired as the real Python base across the unbound link, whose own members
    # flatten onto the derived class.
    c = t.ChainThroughUnbound()
    c.cu = 1
    c.mtb = 2     # from the unbound link, flattened
    c.b = 3       # from Base, via the Python base
    assert c.mtb_method() == 3
    assert c.base_method() == 3
    assert issubclass(t.ChainThroughUnbound, t.Base)
    assert not hasattr(t, 'MidToBase')


def test22_multiple_bases_flattened(t):
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


def test23_secondary_subtree_flattened(t):
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


def test24_diamond_no_double_bind(t):
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


def test28_annotation_skip_rename_doc(t):
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


def test29_annotation_lifetime(t):
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


def test27_operators(t):
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


def test27b_widest_int_conversion(t):
    # Multiple integral conversion operators: only the widest (long long) binds
    # __int__. 5e9 exceeds int32, so the previously-last-bound `operator int`
    # would have truncated it.
    big = 5_000_000_000
    m = t.ManyConv(big)
    assert int(m) == big
    assert bool(m) and not bool(t.ManyConv(0))  # __bool__ unaffected


def test27c_move_only_by_value_param_skipped(t):
    # An overload taking a move-only class type BY VALUE is skipped (the class
    # caster cannot produce it); sibling overloads and the class still bind.
    s = t.Sink()
    s.put(5)
    assert s.get() == 5
    with pytest.raises(TypeError):
        s.put(t.MoveOnlyBuf())   # only the int overload exists


def test27d_abstract_class_no_ctor(t):
    # An abstract class binds (so derived classes get a real Python base) but gets
    # NO constructor (BINDER-0011): instantiating it from Python raises TypeError.
    # Its skipped unique_ptr methods must not have demanded the unique_ptr caster
    # (this module does not include it) -- covered by the module compiling at all.
    with pytest.raises(TypeError):
        t.AbstractIface()
    c = t.ConcreteImpl()
    assert c.compute(4) == 9
    assert isinstance(c, t.AbstractIface)
    assert issubclass(t.ConcreteImpl, t.AbstractIface)
    assert not hasattr(t.AbstractIface, "hidden")


def test26_function_qualifiers(t):
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


def test25_virtual_override_from_python(t):
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


def test30_keyword_arguments(t):
    # Parameter names from C++ (P3096 reflection) become Python keyword args.
    k = t.Kw()

    # Method: positional still works; keywords and reordered keywords work too.
    assert k.add(2, 3) == 5
    assert k.add(a=2, b=3) == 5
    assert k.add(b=3, a=2) == 5

    # Static method.
    assert t.Kw.smul(3, 4) == 12
    assert t.Kw.smul(x=3, y=4) == 12
    assert t.Kw.smul(y=4, x=3) == 12

    # Free function.
    assert t.kw_sub(10, 4) == 6
    assert t.kw_sub(a=10, b=4) == 6
    assert t.kw_sub(b=4, a=10) == 6

    # Constructor.
    c = t.KwCtor(j=2, i=1)
    assert c.i == 1 and c.j == 2

    # An unknown keyword is rejected (proves the names are actually bound).
    with pytest.raises(TypeError):
        k.add(a=2, c=3)


def test31_class_enum_docstrings(t):
    # A [[=r::doc{...}]] annotation on a class or enum sets its Python __doc__.
    assert t.DocClass.__doc__ == "A documented class."
    assert t.DocEnum.__doc__ == "A documented enum."


def test32_free_operators(t):
    a = t.Vec(1.0, 2.0)
    b = t.Vec(3.0, 4.0)

    # Symmetric free operator+ -> __add__.
    c = a + b
    assert c.x == 4.0 and c.y == 6.0

    # operator*(Vec, double) -> forward __mul__ (scalar on the right).
    d = a * 2.0
    assert d.x == 2.0 and d.y == 4.0

    # operator*(double, Vec) -> reversed __rmul__ (scalar on the left). This is the
    # case that only works because the reversed dunder is bound on Vec.
    e = 2.0 * a
    assert e.x == 2.0 and e.y == 4.0

    # Free operator== -> __eq__.
    assert a == t.Vec(1.0, 2.0)
    assert not (a == b)

    # Unary free operator- / operator~ -> __neg__ / __invert__.
    n = -a
    assert n.x == -1.0 and n.y == -2.0
    w = ~a
    assert w.x == 2.0 and w.y == 1.0


def test32b_array_member_skipped(t):
    # BINDER-0006: a C-array data member has no def_rw-able form, so it is skipped while
    # ordinary scalar members still bind.
    w = t.WithArray()
    assert hasattr(w, 'scalar')
    assert w.scalar == 7
    assert not hasattr(w, 'arr')


def test32c_stream_operator_to_str(t):
    # BINDER-0007: a free operator<<(ostream&, T) is surfaced as Python __str__ (formatted via
    # std::ostringstream), NOT bound as a dunder; a genuine operator<<(T, int) shift still maps
    # to __lshift__.
    s = t.Streamable(5)
    assert str(s) == "S(5)"               # via the stream-insertion operator
    assert (s << 1).v == 10               # genuine left-shift -> __lshift__
    # The stream operator did not leak in as a left/right shift dunder taking a stream.
    assert not hasattr(s, '__rlshift__')


def test34_properties(t):
    th = t.Thermo()

    # Read-write property: get + set go through the C++ accessor pair.
    th.celsius = 100.0
    assert th.celsius == 100.0
    th.celsius = 0.0
    assert th.celsius == 0.0

    # Read-only property (getter only) reflects the underlying state.
    th.celsius = 100.0
    assert th.fahrenheit == 212.0
    with pytest.raises(AttributeError):
        th.fahrenheit = 0.0          # no setter -> read-only

    # The raw accessor methods are consumed by the property, not exposed as methods.
    assert not hasattr(th, "to_f")
    assert not callable(th.celsius)  # 'celsius' is now a value, not a method
    assert isinstance(type(th).celsius, property)
    assert isinstance(type(th).fahrenheit, property)


def test35_template_specializations(t):
    # Each instantiation is a distinct Python class, CamelCase-named.
    for name in ("BoxInt", "BoxDouble", "BoxFloat", "BoxBoxInt", "WrapInt",
                 "PairIntDouble", "ArrayInt3", "UsesBoxes"):
        assert hasattr(t, name), name
    assert t.BoxInt is not t.BoxDouble

    # Round-trip a couple of instantiations through their bound methods/ctor.
    b = t.BoxInt(5)
    assert b.get() == 5
    b.set(7)
    assert b.get() == 7
    assert b.value == 7
    assert t.BoxDouble(2.5).get() == 2.5
    # Explicitly-listed-only instantiation (no signature references it).
    assert t.BoxFloat().get() == 0.0


def test37_member_function_templates(t):
    # All-defaulted member function templates bind via their default
    # instantiation, under the TEMPLATE's name (the heterogeneous-lookup shape).
    h = t.HetMap()
    h.add(3)
    assert h.contains(3) and not h.contains(4)   # from the UNBOUND base, flattened
    assert not hasattr(t, 'HetBase')
    assert h.at(21) == 42                        # two defaulted params
    assert h.erase(1) == 1 and h.erase(-1) == 0
    assert h[5] == 15                            # operator[] template -> __getitem__
    assert t.HetMap.sdouble(8) == 16             # static member template
    # No default / parameter packs stay skipped.
    for absent in ('needs_explicit', 'emplace', 'try_emplace'):
        assert not hasattr(t.HetMap, absent), absent


def test38_private_base_using_reexports(t):
    # Members declared in a PRIVATE base and re-exported with using-declarations
    # bind as entity proxies through the derived class (BINDER-0009); the base
    # itself is neither bound nor flattened.
    u = t.UsesPrivateBase()
    assert u.own() == 1
    assert u.pmeth() == 7              # using ProxyImpl::pmeth (private base)
    assert u.padd(5) == 12             # with an argument
    assert t.UsesPrivateBase.psq(3) == 9   # static re-export
    assert not hasattr(t, 'ProxyImpl')


def test36b_policy_args_not_bound(t):
    # Cont<int, Pol<int>> is signature-reachable and bound; Pol<int> appears only
    # as its template argument (a "policy") and is not -- mirroring hash-map
    # Hash/Eq/Alloc/Policy args.
    c = t.ContIntPolInt()
    assert c.get() == 0
    assert not hasattr(t, 'PolInt')


def test36_template_nested_and_multi_arg(t):
    # Nested type arg: Box<Box<int>> -> BoxBoxInt, holding a BoxInt value.
    bb = t.BoxBoxInt()
    inner = t.BoxInt(9)
    bb.set(inner)
    assert bb.get().get() == 9

    # Multiple type args and a non-type (value) arg.
    p = t.PairIntDouble(3, 1.5)
    assert p.first == 3 and p.second == 1.5
    a = t.ArrayInt3()
    assert a.size() == 3


def test37_template_auto_discovery(t):
    # UsesBoxes references the specializations only via its signatures; they are
    # bound automatically (none were listed in reflect_<...>).
    u = t.UsesBoxes()
    u.bi = t.BoxInt(4)
    assert u.bi.get() == 4
    u.take(t.BoxInt(11))
    assert u.bi.get() == 11
    assert u.make_bd().get() == 2.5
    # Transitive (fixpoint) discovery: WrapInt surfaced via Wrap<int>'s own member.
    assert isinstance(u.wrapped, t.WrapInt)
    u.wrapped.inner = t.BoxInt(1)
    assert u.wrapped.inner.get() == 1


def test38_function_template_specialization(t):
    # A free-function-template specialization bound by explicit listing. Python name
    # is CamelCase (identity<int> -> identityInt); function templates cannot be bound,
    # only their instantiations, and only when listed explicitly.
    assert hasattr(t, "identityInt")
    assert t.identityInt(42) == 42


def test39_deleted_functions_not_bound(t):
    # Deleted functions are public, enumerable declarations; none may bind
    # (BINDER-0012). The module compiling at all is the primary regression --
    # pre-fix, each deleted fixture member was a TU-wide hard error.
    n = t.NoDefault(21)
    assert n.dbl() == 42
    # The deleted default ctor did not bind; the surviving int ctor did.
    with pytest.raises(TypeError):
        t.NoDefault()
    # Deleted method / member template / free function: simply absent.
    assert not hasattr(t.NoDefault, "poisoned")
    assert not hasattr(t.NoDefault, "tpoison")
    assert not hasattr(t, "free_deleted")
    # The deleted operator long long lost the __int__ contest to operator int.
    assert int(t.NoDefault(5)) == 5
    # The deleted free operator== did not bind a dunder (identity fallback only).
    assert t.NoDefault(1) != t.NoDefault(1)
    # All-deleted-ctors class binds with NO __init__: TypeError on instantiation
    # (the BINDER-0011 abstract-class contract); statics still callable.
    with pytest.raises(TypeError):
        t.OnlyDeletedCtors(1)
    with pytest.raises(TypeError):
        t.OnlyDeletedCtors()
    assert t.OnlyDeletedCtors.probe() == 7


def test40_copy_construction(t):
    # A publicly copy-constructible class binds init<const T&>: Python can copy
    # a bound instance (BINDER-0013, found via tl::expected's copy-ctor
    # differential). The copy is a distinct object with the same state.
    n = t.NoDefault(21)
    c = t.NoDefault(n)
    assert c is not n
    assert c.dbl() == 42


def test41_exclude_marker(t):
    # nb::exclude_<...> call-site exclusions (the out-of-line [[=r::skip]] for
    # code you do not own; what makes expression-template libraries bindable).
    # The clean surface binds normally.
    v = t.XVec(5)
    assert v.len() == 5
    assert t.free_len(v) == 5
    # Excluded entities are not bound under any name.
    for absent in ("ExprInt", "Opaque", "Helper", "ExBase"):
        assert not hasattr(t, absent), absent
    # Members whose signatures mention an excluded entity are skipped...
    for absent in ("expr", "dot", "helper", "opaque", "field", "doomed"):
        assert not hasattr(t.XVec, absent), absent
    # ...including free functions and constructors.
    assert not hasattr(t, "free_expr")
    with pytest.raises(TypeError):
        t.XVec(object())
    # An excluded base is opaque: nothing flattened from it.
    assert not hasattr(t.XVec, "from_base")


def test42_unbindable_shapes_skip(t):
    # BINDER-0015 + BINDER-0019: ptr-to-ptr, T*& out-params, function-pointer
    # params, and pointers to incomplete plain classes skip gracefully (each
    # used to be a TU-wide hard compile error). The clean surface still binds.
    g = t.Gadget()
    assert g.ok() == 1
    for absent in ("mangle", "peek", "on_event", "impl"):
        assert not hasattr(t.Gadget, absent), absent


def test43_raw_pointer_return_borrows(t):
    # BINDER-0017: a bare class-pointer return is BORROWED by default. Under the
    # old automatic/take_ownership default, collecting `it` (or the discarded
    # add() return) double-freed and aborted the process (CLI11's field shape).
    import gc
    reg = t.Registry()
    it = reg.add()
    assert it.get() == 7
    reg.add()                       # discarded return: wrapper GC'd immediately
    del it
    gc.collect()
    assert reg.count() == 2         # C++ side still owns intact items
    assert reg.sum() == 14
    assert reg.self().count() == 2  # fluent self-return borrows too
    # Static and free raw-pointer returns borrow (rv_policy::reference).
    a = t.Registry.shared_item()
    b = t.Registry.shared_item()
    assert a.get() == 99 and b.get() == 99
    assert t.free_shared_item().get() == 55
    del a, b
    gc.collect()
    assert t.Registry.shared_item().get() == 99


def test44_anonymous_typedef_names(t):
    # BINDER-0018: `typedef struct {...} point_t;` binds under the typedef name
    # for linkage (the identifier_of route is ill-formed on the anonymous record).
    p = t.point_t()
    p.x = 3
    p.y = 4
    assert (p.x, p.y) == (3, 4)
    assert t.color_t.ANON_RED.value == 1
    assert t.color_t.ANON_GREEN.value == 2


def test46_unbindable_cv_void_ptr(t):
    # BINDER-0023: a cv-qualified void* return (SQLiteCpp's getBlob) skips
    # gracefully instead of hard-erroring in the void* caster.
    g = t.Gadget()
    assert g.ok() == 1
    assert not hasattr(t.Gadget, "blob")


def test47_enum_name_collision_qualifies(t):
    # BINDER-0022: same-named enums in sibling namespaces no longer clobber one
    # module attribute; the second binds parent-qualified. Bind order follows
    # the reflect_ pack (collide_a first).
    assert t.shade.LIGHT.value == 1 and t.shade.DARK.value == 2
    assert t.collide_b_shade.RED.value == 10 and t.collide_b_shade.BLUE.value == 20
    assert t.shade is not t.collide_b_shade


def test48_static_shadowed_by_instance_method(t):
    # BINDER-0024: a static method shadowed by a same-named instance method is
    # skipped (binding both under one name aborted nanobind at import -- this
    # module importing at all is most of the test).
    c = t.Conn()
    assert c.info() == 3
    assert t.Conn.probe() == 17       # unshadowed static still binds
    with pytest.raises(TypeError):
        t.Conn.info(5)                # the static overload is gone


def test49_namespace_alias_not_followed(t):
    # BINDER-0028: a namespace-alias member of a reflected namespace is a
    # shorthand, not contents -- the walk must not pull the aliased namespace
    # into the bind set (a fixture's `namespace sd = simdjson;` bound the world).
    assert t.forty_two() == 42
    assert not hasattr(t, "LeakedInner")


def test50_lvalue_ref_return_borrows(t):
    # BINDER-0025: a T& class return binds reference_internal (a live view),
    # not automatic (= COPY, which aborts for non-copyable T and silently
    # detaches for copyable T).
    h = t.CellHolder()
    view = h.edit()
    assert view.get() == 7
    view.set(41)
    assert h.edit().get() == 41       # mutation visible through the holder


def test51_ctor_parens_no_initializer_list_hijack(t):
    # BINDER-0026: reflected ctors construct with parens; the (size_t, int)
    # fill ctor must run as a fill, not brace-hijack to initializer_list.
    f = t.FillVec(3, 9)
    assert f.size() == 3
    assert f.sum() == 27


def test45_static_const_by_value(t):
    # BINDER-0020: constant-readable static const members bind by VALUE (no
    # ODR-use). In-class-initialized statics with no out-of-line definition
    # used to fail at link.
    assert t.Config.BLOCK == 7
    assert t.Config.BIG == (1 << 40)
    assert t.Config.RATIO == 2.5
    assert t.Config.counter == 3    # mutable static keeps the address path
    t.Config.counter = 5
    assert t.Config.counter == 5
    t.Config.counter = 3
