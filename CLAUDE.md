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

> Requires a C++26/P2996 compiler. Build everything with the umbrella repo's repo-local
> from-source clang-p2996 toolchain at `../toolchain` (see "Building & testing" — these are
> exact, this-laptop instructions).

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
  static data, methods, static methods — including overloads. Unnamed (anonymous-union) and
  C-array (`T[N]`) data members are gracefully skipped (neither is `def_rw`-able).
- **Function-type matching**: `const`, `noexcept`, and lvalue-ref-qualified (`&`) methods.
  `volatile`, rvalue-ref (`&&`), and C-variadic functions are gracefully skipped.
- **Operators → Python dunders** (`__add__`, `__eq__`, `__call__`, `__getitem__`,
  `__neg__`, in-place with identity preserved, …); conversion ops → `__bool__`/`__int__`/
  `__float__`. Bound with `nb::is_operator()`. **Binary free operators** too: forward dunder on
  the left operand's class, reversed dunder (`__radd__` …, swapped comparison) on the right —
  so `2.0 * vec` works (`bind_free_operators` scans `parent_of(^^T)` during class binding). A
  free **stream-insertion** `operator<<(std::ostream&, T)` is surfaced as Python `__str__`
  (formatted via `std::ostringstream`, see `bind_stream_str`), not as a shift dunder; a genuine
  `operator<<(T, int)` shift still maps to `__lshift__` (the guard keys on the operand type).
- **Enums** → `nb::enum_` with all values.
- **Inheritance (reachability rule)**: a base becomes the real Python base
  (`class_<T, Base>`) only when it is independently in the bind set (reflected-namespace
  member, explicit `reflect_` argument, or signature-reachable spec); python_base_of looks
  through unbound links to the first in-set ancestor. Every other public base — secondary
  bases, unbound/internal facade chains (e.g. `flat_hash_map`'s `container_internal`
  ancestry) — has its public members **flattened** onto the derived type (diamonds handled
  without double-binding). A bound spec's own template args (Hash/Eq/Alloc policies) do
  NOT qualify types for binding; appearing in public member signatures does.
- **Virtual functions** (Python overrides C++): two-tier — a `reflect_trampoline<T>` hook
  wires in a hand-written *or* generated trampoline as nanobind's `Alias`.
- **Annotations**: skip / rename / doc / return-value policy / keep-alive / property. `doc` now
  applies to classes and enums too (the annotation follows the `struct`/`enum class` keyword), not
  just functions/methods/data members.
- **Properties**: a getter/setter pair sharing `[[=r::property{"name"}]]` (0-arg getter, 1-arg
  setter; getter-only ⇒ read-only) binds a Python property via `def_prop_rw`/`def_prop_ro`,
  using pointer-to-member (`&[:getter:]`). Annotation-driven only (no name-convention sniffing).
- **Keyword-argument names**: P3096 parameter names → `nb::arg("name")` on methods, static
  methods, free functions, and constructors. (Default-argument *values* are not bound — a
  C++26 standard gap, not a binder one: P3096 exposes only `has_default_argument`, no value.)
- **STL type-caster coverage**: reflection detects which std types appear in bound signatures
  (recursively; `stl_caster_header`/`required_stl_types` in `nb_reflect.h`) and maps them to
  `<nanobind/stl/*.h>`. Codegen *emits* those `#include`s (truly automatic); the header-only
  `reflect_` path can't inject includes so it *static_asserts* with the missing header name
  (`check_stl_casters`, P2741 message). `#include` can't be emitted from template code, so the
  header-only path is detect-and-diagnose only.
- **Templates**: binds *specializations* of user class templates. `reflect_` **auto-discovers**
  every user spec reachable from the reflected set's *public member signatures* (recursive
  fixpoint — `required_user_specs`/`collect_user_specs_from_type` in `nb_reflect.h`; a
  discovered spec's OWN template args do not qualify — policy args stay unbound), and
  additional ones (incl. free-function-template specs and unreferenced classes) are listed
  explicitly as `reflect_<^^ns, ^^Box<float>, ^^identity<int>>`. Python names are **CamelCase**
  (`spec_camel_name`: `Box<int>`→`BoxInt`, `Pair<int,double>`→`PairIntDouble`,
  `Array<int,3>`→`ArrayInt3`). The codegen path emits trampolines for spec'd templates with
  virtuals (`type_spelling` writes the qualified template-id; `emit_spec_classes`).
  `[[=r::instantiate]]` (infeasible — annotations can't carry types) is *not* supported;
  explicit instantiation defs aren't auto-detected (not enumerable).

- **Member function templates** with all-defaulted parameters bind via their default
  instantiation under the template's name (heterogeneous-lookup APIs: `contains`/`find`/
  `erase`/`operator[]` on hash/btree containers); packs (`emplace`) and
  explicit-argument templates are gracefully skipped.
- **Using-redeclarations** (`using Base::f;`, incl. from PRIVATE bases — StatusOr's
  `value()`) bind as entity proxies. Requires `-fentity-proxy-reflection` (NOT implied by
  `-freflection-latest`); template/data-member re-exports from inaccessible bases skipped.

Roadmap / not yet: per-argument ownership-transfer annotations; member function templates
needing explicit arguments; trampoline hardening for final/ref-qualified virtuals.

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
- **TC-0004 (fixed in the toolchain; workaround removed)**: same-named function-template
  reflections as NTTPs used to mangle identically, so the two
  `reflect_bind_member_template<T, tmpl>` instantiations for a sibling pair (raw_hash_map's
  `operator[]` + its SFINAE-false lifetimebound twin) were silently folded into one body at
  codegen — the operator never bound, no diagnostic. The toolchain mangler now appends an
  ODR hash of the template head + pattern; substitution happens inline in
  `reflect_bind_member_template` again, and HetMap's pack-sibling `operator[]` in the test
  suite keeps the trigger shape covered. Qualifier filtering uses the binder-spec
  completeness gate (`sizeof` on the undefined `reflect_method_binder` primary) — it is the
  volatile/`&&` matrix filter on every binding path, with no duplicated qualifier logic.
  (The "decl predicates misreport on proxy underlyings" caveat is RESOLVED: the real bug
  was `[[clang::lifetimebound]]` wrapping the method type in AttributedType sugar that
  blinded the qualifier predicates — proxies were incidental; fixed in the toolchain as
  TC-0005.)
- **Entity proxies need `-fentity-proxy-reflection`** (not implied by
  `-freflection-latest`), and proxy guards still precede kind predicates in `members_of`
  loops (`is_constructor` on a proxy was an ICE before the TC-0003 toolchain fix,
  upstreamed as bloomberg/clang-p2996#290 / PR #291; the ordering keeps the binder
  working on an unpatched compiler. Most type queries are still ill-formed on the proxy
  itself — use `proxy_underlying`/`underlying_entity_of`).

## Building & testing (exact, this laptop)

Everything is self-contained in the umbrella repo `~/git/cpp26-reflect-nanobind`, which pins
this checkout as its `nanobind/` submodule: the toolchain at `<umbrella>/toolchain` (the
from-source clang-p2996, built from the umbrella's `llvm-project/` submodule), the venv at
`<umbrella>/.venv` (Homebrew `python3.12` — system `/usr/bin/python3` lacks dev headers), and
the CMake build tree at `<umbrella>/build`. **Do not use the old `~/llvm-toolchain`,
`~/git/nanobind`, `~/git/llvm-project`, `/tmp/nbvenv`, or `/tmp/nbbuild`** — those predate the
self-contained umbrella repo.

Fast front-end check (no build/link; run from this directory):

```bash
TC=../toolchain
PYINC=$(/opt/homebrew/bin/python3.12 -c 'import sysconfig;print(sysconfig.get_path("include"))')
$TC/bin/clang++ -std=c++26 -freflection-latest -fentity-proxy-reflection -stdlib=libc++ \
  -isysroot "$(xcrun --show-sdk-path)" -nostdinc++ -isystem $TC/include/c++/v1 \
  -I "$PYINC" -I include -fsyntax-only tests/test_reflect.cpp
```

Recreate the venv + build tree (run from the umbrella root; `git submodule update --init
--recursive` there covers `ext/robin_map`):

```bash
cd ~/git/cpp26-reflect-nanobind
TC=$PWD/toolchain
python3.12 -m venv .venv && .venv/bin/pip -q install pytest
cmake -S nanobind -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=$TC/bin/clang -DCMAKE_CXX_COMPILER=$TC/bin/clang++ \
  -DCMAKE_OSX_SYSROOT="$(xcrun --show-sdk-path)" \
  -DPython_EXECUTABLE="$PWD/.venv/bin/python" \
  -DNB_TEST=ON -DNB_TEST_FREE_THREADED=OFF -DNB_TEST_STABLE_ABI=OFF \
  -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-rpath,$TC/lib" \
  -DCMAKE_MODULE_LINKER_FLAGS="-Wl,-rpath,$TC/lib"
```
(Configure prints `NB_HAS_REFLECTION_BLOOMBERG - Success` when the toolchain is detected.)

Build + run the reflection tests (from the umbrella root):

```bash
ninja -C build test_reflect_ext test_reflect_codegen_ext
DYLD_LIBRARY_PATH=$PWD/toolchain/lib PYTHONPATH=$PWD/build/tests \
  .venv/bin/python -m pytest nanobind/tests/test_reflect.py \
  nanobind/tests/test_reflect_codegen.py -W error::RuntimeWarning
```
All reflection tests pass (`-W error::RuntimeWarning` turns nanobind's double-registration
warning into a failure). Only `test_reflect*` targets are reflection-related; the rest of
nanobind's suite is upstream and not the focus here.

## Contribution workflow

Commit reflection work on `mk-reflect`, push to `origin` (the fork). Keep `docs/reflection.rst` and this
file in sync with behavior. The umbrella repo `~/git/cpp26-reflect-nanobind` pins the exact
commit of this checkout as a submodule.
