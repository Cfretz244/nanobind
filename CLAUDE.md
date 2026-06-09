# CLAUDE.md — nanobind reflection binder (prove-out)

This file orients Claude Code (claude.ai/code) when working in **this** checkout of
nanobind. It is part of an ongoing prove-out, not upstream nanobind; see the umbrella
repo `~/git/cpp26-reflect-nanobind` for the whole picture.

## What this checkout is

A fork of [`wjakob/nanobind`](https://github.com/wjakob/nanobind) used to build an
**automatic Python-bindings generator driven by C++26 static reflection (WG21 P2996)**.
The work lives on branch **`mk-reflect`**. In *this* checkout the only remote is
**`origin` = `git@github.com:Cfretz244/nanobind.git`** (the user's fork) — push
`mk-reflect` there. Upstream `wjakob/nanobind` is **not** configured as a remote here;
never add it as a push target.

The starting point was a community proof-of-concept by *matthewkolbe* (`nb::reflect_<^^ns>(m)`,
fetched from his `reflect` branch). It has since been substantially extended and hardened.

Core idea: `nb::reflect_<^^some_namespace>(m)` walks the namespace's types, members,
functions, and enums with reflection and emits ordinary `nb::class_<T>().def(...)` calls
*via splices inside `template for` loops* — no codegen text on the common path. The one
thing that cannot be expressed in-language (a virtual-override **trampoline**) has a
**text-codegen fallback**.

> Requires a C++26/P2996 compiler. Build everything with the from-source clang-p2996
> toolchain at `~/llvm-toolchain` (see "Building & testing" — these are exact, this-laptop
> instructions).

## Where the implementation lives

- `include/nanobind/nb_reflect.h` — the binder. `reflect_<Rs...>` → `reflect_dispatch`
  → `reflect_class` / `reflect_enum` / `reflect_free_function`. Contains the member/method/
  static/free binders, the cv/ref/noexcept specialization matrix, operator→dunder mapping,
  inheritance (single + multiple-base flattening), the trampoline hook, and the annotation
  reading/application helpers.
- `include/nanobind/nb_reflect_annotations.h` — **dependency-free** vocabulary
  (`namespace nanobind::reflect`) users put on their *own* library code:
  `skip`, `rename{"..."}`, `doc{"..."}`, `return_policy{...}` (+ shorthands
  `take_ownership`/`copy`/`move`/`reference`/`reference_internal`/`take_nothing`),
  `keep_alive{nurse, patient}`.
- `include/nanobind/nb_reflect_codegen.h` — Tier-2 codegen fallback:
  `emit_trampolines<^^ns...>()` returns C++ source — the required `<nanobind/stl/*.h>`
  caster `#include`s (see STL casters below) plus trampoline structs +
  `NB_REFLECT_TRAMPOLINE` registrations for every class with overridable virtuals;
  `write_trampolines(path, src)` dumps it. Run by a small generator program at build time.
- `tests/test_reflect.{cpp,py}` — the main suite. `tests/test_reflect_codegen.{h,cpp,py}`
  + `tests/test_reflect_codegen_gen.cpp` — the codegen (two-stage build) test, wired in
  `tests/CMakeLists.txt` (search for `NB_REFLECT` / `test_reflect_codegen`).
- `docs/reflection.rst` — user-facing documentation of every feature + limitations.

## What works today (each landed as its own commit on `mk-reflect`)

- Classes: public constructors, data members (`def_rw`/`def_ro` via pointer-to-member),
  static data, methods, static methods — including overloads.
- **Function-type matching**: `const`, `noexcept`, and lvalue-ref-qualified (`&`) methods.
  `volatile`, rvalue-ref (`&&`), and C-variadic functions are gracefully skipped.
- **Operators → Python dunders** (`__add__`, `__eq__`, `__call__`, `__getitem__`,
  `__neg__`, in-place with identity preserved, …); conversion ops → `__bool__`/`__int__`/
  `__float__`. Bound with `nb::is_operator()`. **Binary free operators** too: forward dunder on
  the left operand's class, reversed dunder (`__radd__` …, swapped comparison) on the right —
  so `2.0 * vec` works (`bind_free_operators` scans `parent_of(^^T)` during class binding).
- **Enums** → `nb::enum_` with all values.
- **Inheritance**: first public base → real Python base (`class_<T, Base>`, bound
  transitively + idempotently); additional bases' members **flattened** onto the derived
  type (diamonds handled without double-binding).
- **Virtual functions** (Python overrides C++): two-tier — a `reflect_trampoline<T>` hook
  wires in a hand-written *or* generated trampoline as nanobind's `Alias`.
- **Annotations**: skip / rename / doc / return-value policy / keep-alive. `doc` now applies to
  classes and enums too (the annotation follows the `struct`/`enum class` keyword), not just
  functions/methods/data members.
- **Keyword-argument names**: P3096 parameter names → `nb::arg("name")` on methods, static
  methods, free functions, and constructors. (Default-argument *values* are not bound — a
  C++26 standard gap, not a binder one: P3096 exposes only `has_default_argument`, no value.)
- **STL type-caster coverage**: reflection detects which std types appear in bound signatures
  (recursively; `stl_caster_header`/`required_stl_types` in `nb_reflect.h`) and maps them to
  `<nanobind/stl/*.h>`. Codegen *emits* those `#include`s (truly automatic); the header-only
  `reflect_` path can't inject includes so it *static_asserts* with the missing header name
  (`check_stl_casters`, P2741 message). `#include` can't be emitted from template code, so the
  header-only path is detect-and-diagnose only.

Roadmap / not yet: templates (need explicit instantiation lists — naturally
annotation-driven), per-argument ownership-transfer annotations.

## Key gotchas (clang-p2996 @ the pinned toolchain commit)

- **Mangler crash**: a lambda whose *signature* names a spliced type (`[:type_of(x):]` as a
  parameter/return type), passed to a dependent `cls.def*(...)` call, crashes the compiler
  (`UNREACHABLE … mangling a placeholder type`). The binder avoids this everywhere: data
  members use pointer-to-member; method lambdas keep real `Ret`/`Args...` template params;
  conversion lambdas use fixed concrete return types and cast.
- **Annotation values must be valid template arguments**: a `const char*` member is not, so
  `rename`/`doc` store text in a `fixed_string<N>` (char array) via CTAD.
- `def_rw`/`def_ro` default to `rv_policy::reference_internal`; the binder only passes a
  policy when one is annotated, so it never clobbers that default.

## Building & testing (exact, this laptop)

Toolchain: `~/llvm-toolchain` (the from-source clang-p2996, built from `~/git/llvm-project`).
Python: Homebrew `python3.12` (system `/usr/bin/python3` lacks dev headers). A venv with
pytest lives at `/tmp/nbvenv`; the CMake build tree at `/tmp/nbbuild` (both under `/tmp` —
recreate if cleared, see below).

Fast front-end check (no build/link):

```bash
TC=~/llvm-toolchain
PYINC=$(/opt/homebrew/bin/python3.12 -c 'import sysconfig;print(sysconfig.get_path("include"))')
$TC/bin/clang++ -std=c++26 -freflection-latest -stdlib=libc++ \
  -isysroot "$(xcrun --show-sdk-path)" -nostdinc++ -isystem $TC/include/c++/v1 \
  -I "$PYINC" -I include -fsyntax-only tests/test_reflect.cpp
```

Recreate the venv + build tree if `/tmp` was cleared:

```bash
git submodule update --init ext/robin_map
/opt/homebrew/bin/python3.12 -m venv /tmp/nbvenv && /tmp/nbvenv/bin/pip -q install pytest
TC=~/llvm-toolchain
cmake -S . -B /tmp/nbbuild -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=$TC/bin/clang -DCMAKE_CXX_COMPILER=$TC/bin/clang++ \
  -DCMAKE_OSX_SYSROOT="$(xcrun --show-sdk-path)" \
  -DPython_EXECUTABLE=/tmp/nbvenv/bin/python \
  -DNB_TEST=ON -DNB_TEST_FREE_THREADED=OFF -DNB_TEST_STABLE_ABI=OFF \
  -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-rpath,$TC/lib" \
  -DCMAKE_MODULE_LINKER_FLAGS="-Wl,-rpath,$TC/lib"
```
(Configure prints `NB_HAS_REFLECTION_BLOOMBERG - Success` when the toolchain is detected.)

Build + run the reflection tests:

```bash
ninja -C /tmp/nbbuild test_reflect_ext test_reflect_codegen_ext
DYLD_LIBRARY_PATH=~/llvm-toolchain/lib PYTHONPATH=/tmp/nbbuild/tests \
  /tmp/nbvenv/bin/python -m pytest tests/test_reflect.py tests/test_reflect_codegen.py \
  -W error::RuntimeWarning
```
All reflection tests pass (`-W error::RuntimeWarning` turns nanobind's double-registration
warning into a failure). Only `test_reflect*` targets are reflection-related; the rest of
nanobind's suite is upstream and not the focus here.

## Contribution workflow

Commit reflection work on `mk-reflect`, push to `origin` (the fork). Keep `docs/reflection.rst` and this
file in sync with behavior. The umbrella repo `~/git/cpp26-reflect-nanobind` pins the exact
commit of this checkout as a submodule.
