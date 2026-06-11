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
- **Deleted functions are filtered on every path** (BINDER-0012, found via tl::expected's
  `unexpected() = delete;`): ctors, methods/operators/conversions, member-template default
  instantiations (checked on the substituted SPEC — `is_deleted` is silently false on a
  Template reflection), flattening,
  free functions/operators, properties, the `__int__` widest-conversion contest, and the
  caster/spec discovery walks. A class with only deleted ctors binds with no `__init__`
  (Python TypeError, the abstract-class contract).
- **Python-side copy construction** (BINDER-0013): `init<const T&>` binds when T is
  publicly copy-constructible and has no trampoline. Move ctors never bind.
- **Call-site exclusions + completeness gates (BINDER-0014, the eigen run's
  feature)**: `nb::exclude_<^^Entity...>` passed in the `reflect_` pack makes
  the listed class templates (all specializations), concrete types, namespaces
  (transitively), or individual MEMBER reflections (the only handle on
  members whose BODIES are lazily ill-formed for a bound spec, e.g. Eigen's
  size-asserting `Matrix(x,y,z)` on a 3x3 -- bodies are not reflectable)
  opaque on every path: never bound/discovered/walked/flattened, no caster
  demands, and members whose signatures mention one (transitively through
  template args; signatures are DEALIASED first) are skipped. This is what
  makes expression-template libraries bindable at all (Eigen's facades mint
  Transpose<Transpose<...>> forever -- discovery diverges; a >1024-spec
  worklist now fails through the pointed non-constexpr
  `reflect_discovery_diverged` diagnostic). Independent of exclusions, a spec
  that cannot be COMPLETED in the TU (template forward-declared, definition
  in a never-included header: Eigen's SparseView under <Eigen/Dense>) is
  neither discovered nor bound (`is_complete_type` gates; sugar-blind on
  unpatched toolchains, TC-0012). The completeness probes cost constexpr
  steps: several corpus runs needed a raised `-fconstexpr-steps`.
- **Deduction guides are stripped from the namespace walks** before the
  `define_static_array` lift (`namespace_members_for_binding`): a guide is never bindable,
  and pre-TC-0008 toolchains ICE mangling a guide reflection ("Can't mangle a deduction
  guide name!" — tl's `unexpected(E) -> unexpected<E>` was the field shape).
- **Using-redeclarations**: public-base re-exports are covered by inheritance/flattening.
  Private-base re-exports (StatusOr's `value()`) do NOT bind — the entity-proxy feature
  (clang fork's `-fentity-proxy-reflection`) was REMOVED after P3687R1 deferred
  shadow-declaration reflection past C++26 (the binder targets standard C++26 / GCC 16).

- **Wave-1 parallel-corpus hardening (BINDER-0015..0020, one commit each):**
  unrepresentable parameter/return shapes (ptr-to-ptr, ptr-to-function, `T*&`
  out-params) gracefully skip on every path incl. the caster walk (0015,
  cli11/toml++); raw class-pointer returns default to BORROWING policies —
  `reference_internal` on methods, `reference` on statics/free functions,
  annotation wins — instead of `automatic`/take_ownership's double-free (0017,
  cli11's fluent API); `typedef struct {...} name_t;` binds under the typedef
  name threaded down from the walk (`reached_entity_name`; truly anonymous
  types skip; 0018, tinyobjloader); forward-declared plain classes are opaque
  like non-completable specs (signatures AND namespace walks; 0019, pugixml's
  pImpl); constant-readable `static const` members bind by VALUE (no ODR-use →
  no undefined symbol; fixed-NTTP probe because an `auto`-NTTP probe ICEs the
  toolchain, TC-0013; 0020, moodycamel); `is_exclude_marker` dealiases (0016
  investigation — per-overload exclusion itself NOT reproduced, repros under
  `corpus/findings/repros/BINDER-0016/`).

- **Wave-2 parallel-corpus hardening (BINDER-0022..0028):** module name
  collisions bind parent-qualified instead of clobbering (`yamlcpp`'s
  NodeType::value vs EmitterStyle::value; reflect_enum also gained the
  is_valid idempotence guard); cv-qualified `void*` joins the unbindable
  shapes (sqlitecpp's getBlob); a static shadowed by a same-named instance
  method skips instead of aborting nanobind at import (sqlitecpp's
  getHeaderInfo); `T&` class returns borrow like `T*`
  (returns_borrowed_class_indirection; taskflow's accessors aborted under
  policy=copy); reflected ctors construct with PARENS via reflect_init
  (immer's initializer_list hijack/narrowing); namespace-alias members are
  not followed by any walk (simdjson's fixture alias bound the world);
  0027 (exclusions vs std::function arg types) recorded OPEN. Wave 2 also
  drove four toolchain fixes (TC-0014 DescriptionOf builtin templates,
  TC-0015 guide-spec mangling, TC-0016 linkage-spec walk truncation -- what
  hid every post-Python.h global from reflection -- and TC-0017 NEON
  Long-element mangling).

- **The emit backend (full source codegen; Phase 4's deployability story)**:
  `nb::write_bindings<Rs...>(path, module_name, preamble)` (include/nanobind/
  nb_reflect_emit.h) walks the SAME metadata through the SAME shared
  classifiers (the `classify_*`/`*_route`/`ctor_binds`/`plan_free_operator`
  consteval value-form predicates factored into nb_reflect.h -- the single
  source of truth for every WHAT-to-bind decision in both backends) and
  renders them as ONE self-contained binding TU of plain C++17/20 nanobind
  code (no reflection constructs); a production compiler builds the module.
  Type text comes from include/nanobind/nb_reflect_spell.h (full recursive
  grammar renderer, fully qualified, inline-std-namespace skipping,
  typedef-for-linkage recovery; unspellable signatures skip with a
  `// skipped:` comment -- the emit-only gate). Trampolines are inline and
  OPT-IN via `nb::trampoline_<^^Cls...>` / `nb::trampoline_all_` pack markers
  (inert in the constexpr lane) so the two backends present identical
  surfaces; compiler-answered decisions (the BINDER-0020 static-const value
  probe, `__str__` streamability) are EMITTED as identical probes, which is
  why the generated bind functions are templates over a defaulted `Self`.
  Internals: per-entity text memoized by WORKLIST INDEX (never by entity
  reflection: deep specs' reflection-NTTP manglings blow the linker's
  ~128K symbol cap) in 8K static chunks (define_static_string miscompiles
  >=32K, TC-0018; char-pack symbols mangle ~7 bytes/char) and streamed at the
  generator's runtime. Tests: tests/test_reflect.py runs against BOTH
  backends (conftest `t` fixture; the emit module builds at c++20 with no
  reflection flags in its own NB_DOMAIN), test_reflect_emit.py owns the
  recursive surface diff. The corpus validates three-way per run (oracle /
  constexpr / emit + surface diff; corpus/lib/run_gates.py).

- **Emit-mode spelling-probe TU**: `nb::write_spelling_probe<Rs...>(path,
  preamble)` renders a second generated TU (plain C++, no nanobind) that
  re-states every spelled signature as an overload-exact cast
  (`static_cast<Ret (Cls::*)(Args...) cv ref noexcept>(&Cls::f)`), a
  `decltype(Cls::x)` identity assert, a ctor function-type alias, or an
  enumerator mention -- the round-trip oracle for nb_reflect_spell.h: a wrong
  spelling is a COMPILE error against the library headers alone. Wired into
  the unit build as the `test_reflect_spelling_probe` OBJECT library
  (c++20, no reflection flags).

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
- **Entity proxies were removed** (P3687R1 deferred shadow-declaration reflection past
  C++26): the binder no longer passes `-fentity-proxy-reflection` and has no proxy
  paths. `using` re-exports from private bases simply do not bind.

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
$TC/bin/clang++ -std=c++26 -freflection-latest -stdlib=libc++ \
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
