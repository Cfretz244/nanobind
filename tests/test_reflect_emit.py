"""Equivalence tests for the emit (full source codegen) backend.

test_reflect_emit_ext is built from GENERATED source (test_reflect_emit_gen
renders the binding TU via reflection; the module itself compiles at c++20
with no reflection flags) over the same fixture + TEST_REFLECT_ARGS pack as
test_reflect_ext. The two backends must present an identical Python surface:
same classes/enums/functions, same attribute kinds, same overload signatures
(nanobind embeds them in __doc__), same base wiring, same enum members.
Behavior is covered by running the full test_reflect.py suite against both
modules (see conftest/parameterization); this file owns the surface diff and
a few emit-specific spot checks for runtime-decided paths.
"""

import pytest

import test_reflect_ext as ref
import test_reflect_emit_ext as emit

# Dunders the binder synthesizes (operators, conversions, __str__, init);
# other dunders are interpreter noise and excluded from the diff.
SYNTH_DUNDERS = {
    "__init__", "__eq__", "__ne__", "__lt__", "__gt__", "__le__", "__ge__",
    "__add__", "__radd__", "__sub__", "__rsub__", "__mul__", "__rmul__",
    "__truediv__", "__rtruediv__", "__mod__", "__rmod__", "__xor__",
    "__rxor__", "__and__", "__rand__", "__or__", "__ror__", "__lshift__",
    "__rlshift__", "__rshift__", "__rrshift__", "__iadd__", "__isub__",
    "__imul__", "__itruediv__", "__imod__", "__ixor__", "__iand__",
    "__ior__", "__ilshift__", "__irshift__", "__neg__", "__pos__",
    "__invert__", "__call__", "__getitem__", "__bool__", "__int__",
    "__float__", "__str__", "__hash__", "__iter__", "__doc__",
}


def _interesting(name):
    if name.startswith("__") and name.endswith("__"):
        return name in SYNTH_DUNDERS
    return True


def _describe_attr(owner_name, name, attr):
    kind = type(attr).__name__
    doc = getattr(attr, "__doc__", None)
    info = {"kind": kind}
    if callable(attr) or kind in ("property", "nb_func", "nb_method"):
        info["doc"] = doc
    if kind == "property":
        info["writable"] = attr.fset is not None
        info["doc"] = doc
    # Enum members / class-level constants: capture a value where safely
    # comparable across modules.
    if isinstance(attr, (int, float, str, bool)):
        info["value"] = attr
    try:
        info["int"] = int(attr)  # nanobind enum members convert to int
    except (TypeError, ValueError):
        pass
    return info


def _describe_class(name, cls, seen):
    if name in seen:
        return {"recursive": True}
    seen = seen | {name}
    desc = {
        "bases": [b.__name__ for b in cls.__bases__],
        "doc": cls.__doc__,
        "attrs": {},
    }
    for aname, attr in sorted(cls.__dict__.items()):
        if not _interesting(aname):
            continue
        if aname == "__doc__":
            continue
        if isinstance(attr, type):
            desc["attrs"][aname] = _describe_class(aname, attr, seen)
        else:
            desc["attrs"][aname] = _describe_attr(name, aname, attr)
    return desc


def describe_module(mod):
    out = {}
    for name in sorted(dir(mod)):
        if name.startswith("__"):
            continue
        attr = getattr(mod, name)
        if isinstance(attr, type):
            out[name] = _describe_class(name, attr, frozenset())
        else:
            out[name] = _describe_attr(mod.__name__, name, attr)
    return out


def _diff(a, b, path, out):
    if isinstance(a, dict) and isinstance(b, dict):
        for k in sorted(set(a) | set(b)):
            if k not in a:
                out.append(f"{path}.{k}: only in emit")
            elif k not in b:
                out.append(f"{path}.{k}: only in constexpr")
            else:
                _diff(a[k], b[k], f"{path}.{k}", out)
    elif a != b:
        out.append(f"{path}: constexpr={a!r} emit={b!r}")


def test_surface_identical():
    ref_desc = describe_module(ref)
    emit_desc = describe_module(emit)
    mismatches = []
    _diff(ref_desc, emit_desc, "", mismatches)
    assert not mismatches, "surface diverged:\n" + "\n".join(mismatches)


# --- Emit-specific spot checks: paths whose DECISION is taken by the
# --- production compiler inside the generated source ---

def test_static_const_by_value():
    # BINDER-0020: in-class-init static const binds by VALUE via the emitted
    # value_probe (an undefined symbol at link otherwise -- so importing at
    # all is half the test).
    assert emit.Config.BLOCK == 7
    assert emit.Config.BIG == (1 << 40)
    assert emit.Config.RATIO == 2.5
    emit.Config.counter = 5
    assert emit.Config.counter == 5
    emit.Config.counter = 3

def test_streamable_str():
    # The emitted streamability probe: Streamable has operator<<.
    assert str(emit.Streamable(7)) == "S(7)"

def test_trampoline_override():
    # The trampoline_<^^Shape> marker: a generated trampoline must dispatch
    # Python overrides when C++ calls through the base.
    class Sq(emit.Shape):
        def __init__(self, s):
            super().__init__()
            self.s = s
        def area(self):
            return self.s * self.s
        def kind(self):
            return "square"
    sq = Sq(3.0)
    assert emit.call_area(sq) == 9.0
    assert emit.call_kind(sq) == "square"
    # Non-overridden non-pure virtual falls through to the C++ default.
    class Plain(emit.Shape):
        def area(self):
            return 1.0
    assert emit.call_kind(Plain()) == "shape"

def test_collision_naming():
    # BINDER-0022: second same-named enum binds parent-qualified -- decided
    # at module init by the emitted hasattr check.
    first = emit.shade
    second = getattr(emit, "collide_a_shade", None) or getattr(
        emit, "collide_b_shade")
    assert first is not second

def test_parens_init():
    # BINDER-0026 via the emitted reflect_init: (n, v) must FILL, not hijack
    # to the initializer_list ctor.
    fv = emit.FillVec(3, 7)
    assert fv.size() == 3
    assert fv.sum() == 21

def test_inplace_identity():
    a = emit.Ops(2)
    before = id(a)
    a += emit.Ops(3)
    assert id(a) == before and a.x == 5
