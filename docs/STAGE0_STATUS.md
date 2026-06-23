# Stage0 V01 implementation status

This file tracks implementation evidence for the requirements in
`docs/V01_SUBSET.md`. It is intentionally conservative: a green spec runner
does not by itself mean a language feature is fully implemented when stage0 is
using lightweight scans, preview facts, or placeholder artifacts for future
semantic phases.

## Implemented as executable behavior

- C++20 stage0 command line entry point with `zeno --version`, `zeno check`,
  `zeno build`, and `zeno test`; bare `zeno test` defaults to the MVP gate.
- CLI option validation for the first closed sets: `--diagnostic-format` must
  be `human` or `json`; `zeno test --stage` must be `mvp` or `full-spec`;
  `zeno test --milestone` must be `M0`-`M9`; and `zeno build --emit` must be
  `mir` or `llvm-ir`.
- CLI and manifest target/profile validation for the stage0 MVP baselines:
  targets are limited to `aarch64-apple-darwin` and
  `x86_64-unknown-linux-gnu`; profiles are limited to `hosted`,
  `freestanding`, `kernel`, and `embedded`.
- Manifest closed-set validation for `package.kind`, `panic.strategy`, and
  `oom.strategy`, with `panic.strategy = "unwind"` kept as a staged
  diagnostic rather than a silently accepted implementation.
- Manifest `[trust]` validation rejects unknown trust-section fields while
  accepting the V01 trust capability categories plus `dependencyTrust`,
  `requireReport`, and `allowedPackages` policy fields.
- Manifest package identity validation requires `[package].name` when a package
  section is present and requires package names to be legal Zeno identifiers;
  workspace-only root manifests remain valid without package identity.
- Manifest kind/entry validation rejects `package.entry` on library packages.
- Human and JSON Lines diagnostics with stable code, primary span, notes/help,
  and `isStaged` / `feature` for staged diagnostics; the spec runner rejects
  `E9000-E9099` diagnostics that omit staged metadata.
- Source loading, byte-offset line mapping, ASCII lexer checks, delimiter
  recovery, removed-keyword diagnostics, and staged async diagnostics.
- Lightweight source semantic checks for the first MVP gates:
  - move-parameter call sites requiring explicit `move`;
  - repeated move use of the same local binding inside a function;
  - `val` reassignment and uninitialized `var` field reads;
  - overlapping read plus `mut` access in the same call;
  - local array owner escaping as `ArraySlice`;
  - `mut self` calls through immutable `val` bindings;
  - `@noAlloc` calls to known allocating APIs and covered local functions that
    can reach them, including format, string construction/growth/clone,
    collection allocation/insertion, and Box/Shared construction;
  - lossy `as U8`, `try U8.fromChecked` without `okOr` / `okOrElse`,
    owned `String` initialization from a literal, and freestanding default
    allocator use;
  - raw `extern "C"` declarations without `trust extern`;
  - top-level executable statements are rejected so imports cannot imply
    module initialization work;
  - runtime work in `static` initializers, including trust/extern execution,
    direct extern calls, runtime I/O, default allocator materialization,
    `String.from`, owner types requiring exit-time global destruction,
    `Thread.spawn`, and task runtime spawning.
- Lightweight declaration and ABI boundary checks:
  - duplicate top-level overload keys and return-type-only overloads;
  - `@layout` only on structs and at most one layout strategy per struct;
  - `@layout(C)` structs treated as C-compatible signature types;
  - `@export` only on `pub` top-level non-generic functions;
  - exported symbol uniqueness;
  - C ABI export/import signature compatibility;
  - generic `extern "C"` rejection;
  - minimal `bridge: C` compatibility for `StringSlice`, `Option`, and
    `Result` signatures.
- Lightweight field, layout, and interface-object checks:
  - known local struct constructors and field access must name declared
    fields, local struct constructors must initialize every declared field,
    duplicate field initializers are rejected, and obvious single-line or
    multiline field initializer type mismatches are diagnosed;
  - known local enum-qualified constructor/pattern uses must name declared
    variants and match declared payload shape;
  - `ArraySlice` and `StringSlice` cannot be stored in struct fields;
  - bare interface names cannot be used as fields or collection element types,
    while `Box<Interface>` remains explicit and allowed;
  - `@layout(C)` fields must be C-compatible and C-layout types cannot define
    `destroy`;
  - `@layout(Packed(N))` validates supported alignments, rejects non-Copy-like
    fields, and rejects passing packed fields as `mut` access.
- Lightweight destroy, trust, and CTFE-safety checks:
  - `destroy` rejects `try`, `await`, `panic`, known allocating calls, covered
    local functions that can reach panic/allocation, direct user calls, and
    Result-returning cleanup calls;
  - manual `Send` / `Sync` impls require `trust impl`, while `trust impl` is
    limited to compiler-recognized marker interfaces;
  - raw memory operations and trust extern calls require explicit `trust`
    blocks;
  - CTFE rejects calls into functions that require trust or extern execution.
  - `static` initializers reject dynamic initialization, extern execution,
    runtime I/O, hidden runtime owner allocation/destruction, static owner
    types requiring exit-time global destruction, thread startup, and task
    runtime work.
- Lightweight callable and pattern checks:
  - `MutFn` calls require mutable callable access;
  - `OnceFn` callables are consumed by the first call;
  - mutating or consuming closures cannot be passed where `Fn` is required;
  - refutable constructor patterns are rejected in plain `val` bindings;
  - obvious or-pattern binding mismatches, string patterns, wildcard-covered
    unreachable branches, and two common non-exhaustive match shapes are
    diagnosed.
- Lightweight collection and dynamic interface checks:
  - `ArraySlice` cannot be stored in `Box`, `Shared`, or `Vector`;
  - `Box<Interface>` rejects dynamic dispatch of methods returning `Self`,
    method-level generics, and `move self` methods;
  - `Shared<Interface>` rejects `mut self` interface method calls;
  - `Box<Interface>` crossing `Thread.spawn` requires a `Send` interface, and
    `Shared<T>` crossing `Thread.spawn` requires `T: Send + Sync` for known
    resource-owning types;
  - moving non-Copy values out through indexed access and `get`/`Map.get` on
    non-Copy element/value types are diagnosed for the covered collection
    shapes;
  - `Array`, `Vector`, `Map`, and `Set` clone calls reject known non-Copy
    element, key, or value types, matching the first-stage Copy-only clone
    declaration-package boundary for the covered local shapes;
  - `Map` and `Set` construction, signatures, and covered mutating calls reject
    known local key/value types that do not implement `HashKey`, while explicit
    local `impl HashKey for T` enables those collection shapes;
  - `MapEntry` is declared as the short-lived entry view for `Map.entry`, local
    entry use is accepted, and storing/returning `MapEntry` through covered
    field, static, collection, `Box`, or `Shared` shapes is rejected.
- Lightweight constant/profile/diagnostic-access checks:
  - constant out-of-bounds indexing for covered fixed-length `Array.filledIn`
    locals and constant divide-by-zero are diagnosed;
  - `static ArraySlice`, `static StringSlice`, const `String` runtime
    materialization, simple const initialization cycles, and const generic
    `String` parameters are rejected;
  - `@noPanic` rejects direct `panic`, covered local functions that can reach
    `panic`, and OOM-as-panic allocation paths for the covered profile shape;
  - `PanicInfo`, `StackFrames`, and `TaskContext` are prevented from escaping
    into covered stored fields, statics, storage containers, Box/Shared owners,
    or task returns, and hidden current-task cancellation queries are rejected.
- Lightweight local ownership and access checks:
  - read parameters cannot be returned or boxed as owned resource values;
  - `move self` receiver consumption, read/mut self field moves, destroy-type
    field moves, partial field moves, and non-Copy assignment moves are
    diagnosed for the covered local shapes;
  - active `ArraySlice` views block covered vector structural mutations,
    owner overwrite, element replacement, and thread escape until a local block
    ends;
  - `for mut`, read item assignment, `for move` over slices, consumed for-loop
    sources, and non-Copy iterator traversal without `for move` are diagnosed.
- Lightweight task and ABI bridge checks:
  - `runtime.spawn*` task handles must be settled by `await`, `blockOn`,
    `cancel`, or `detach`, and consuming task APIs require explicit `move`;
  - `spawnBlocking` rejects named jobs without `move` and obvious non-`Send`
    destroy-type captures;
  - ad-hoc comma-separated generic constraints, C++ bridge exports, exported
    C ABI under unwind panic profile, non-pub C-layout parameters in exports,
    and covered `Result` bridge C payload/error-code constraints are
    diagnosed.
- Additional lightweight semantic checks now covered by ordinary `zeno check`:
  - manifest-denied `trust.ffi`, `trust.rawMemory`, `trust.hardware`,
    `trust.inlineAsm`, `trust.interrupts`, and `trust.allowedPackages` now gate
    package-source trusted FFI, raw pointer, MMIO/volatile-style hardware,
    inline-asm, interrupt, and trust-boundary markers, with single-file
    fixtures still using header comments for manifest-shaped metadata;
  - `trust.dependencyTrust = false` now rejects trust boundaries discovered in
    local path dependency source packages as well as lockfile trust metadata;
  - `try` rejects the covered `Result` error-type mismatch shape;
  - ambiguous integer-literal numeric overloads are diagnosed for the covered
    overload shape;
  - obvious single-line return literal/value plus explicit local initializer,
    local assignment, field assignment, and uniquely known local function-call
    argument count/type plus result type mismatches are diagnosed for Bool,
    Char, Unit-returning calls, integer, float, owned String, and known local
    struct/enum result shapes;
  - `Never`-producing calls from local functions, `panic`, and `oom` satisfy
    explicit return/value contexts, while ordinary returns from explicit
    `Never` functions are rejected for the covered simple shapes;
  - explicit value-returning functions reject covered fallthrough shapes when
    the outermost body does not end in a terminal `return`, `panic`, `oom`,
    top-level `match`, or covered `if val` branch expression;
  - obvious arithmetic operator operands reject known non-numeric Bool/String/
    struct/enum values in local initializers, assignments, struct fields,
    call arguments, and returns while chained or otherwise complex arithmetic
    remains outside the lightweight checker;
  - obvious comparison operand mismatches are rejected for known equality and
    ordering expressions in local initializers, assignments, struct fields,
    call arguments, returns, and `if` / `while` conditions;
  - obvious known non-Bool `if` / `while` conditions are rejected while pattern
    conditions and broader expression typing remain outside the lightweight
    checker;
  - obvious unknown bare value identifiers are rejected in typed and inferred
    initializers, assignment targets and RHS expressions, struct field
    initializers, call arguments, conditions, and returns, while known local
    bindings, parameters, top-level constants/statics, enum variants, receiver
    `self`, and const-generic value parameters are accepted for the covered
    simple shapes;
  - delayed `val` maybe-initialized/maybe-uninitialized cases and covered
    partial aggregate field initialization are diagnosed;
  - scoped allocator owner returns, generic allocator owner returns requiring
    `EscapingAllocator`, scoped thread indexed mutable access without a
    disjoint proof, match read/mut payload mode mistakes, destroy-type
    `Thread.spawn` captures without trusted `Send`, match-move owner reuse,
    async access/future escape shapes, TaskGroup settlement shapes, and
    static/generic interface consistency shapes are diagnosed.
- Basic manifest parsing for package name, target profile, allocator, panic,
  OOM, dependency keys, relative path dependencies, path/git/registry staged
  checks with JSON staged metadata, hosted low-level trust, handler
  requirements, kernel/embedded handler-strategy requirements, and
  kernel/embedded trust-report requirements.
- Builtin declaration packages:
  - `lib/zeno/core`, `lib/zeno/alloc`, and `lib/zeno/std` now contain
    declaration-package `Zeno.toml` files and `.zn` declarations for the first
    core library boundary;
  - declaration packages cover stage0 signatures and facts for `Option`,
    `Result`, base marker interfaces, allocator interfaces and allocator
    types, layout queries, panic/OOM declarations, FFI bridge contracts,
    slices/string slices, owning collections, `Box`, `Shared`, hosted threads,
    tasks, and sync/atomic placeholders;
  - builtin package module names use package-qualified paths such as
    `core.result`, `alloc.collections`, and `std.thread`, and the module/path
    checker accepts that prefix only for compiler builtin packages;
  - `std` imports are rejected outside hosted profiles unless an explicit
    hosted-capable builtin dependency is declared.
- Basic package/module checks:
  - explicit `module` path versus `src/` path validation;
  - dependency root existence for imports;
  - path dependency and builtin imports now validate imported module existence
    plus grouped/single named imports against exported top-level declarations
    and public enum variants;
  - package-level type-name resolution checks function parameter/return
    signatures and struct fields against primitive types, local declarations,
    generic parameters, builtin declaration packages, and imported pub type
    declarations;
  - local path dependencies must point at an existing package root;
  - dependency key conflict with same-package source roots;
  - private top-level declaration use across files;
  - package-visible type exposure through `pub` API;
  - duplicate workspace package names;
  - application packages without an explicit `package.entry` must provide
    `src/main.zn` for the default `main.main` entry;
  - explicit application `package.entry = "module.function"` values must map
    to an existing source file and top-level function declaration;
  - manifest-named panic and OOM handlers in package source must return
    `Never`;
  - path dependencies that export `pub fn -> Interface` now receive staged
    `E9004` diagnostics with `feature = "cross-package-interface-return"`,
    while same-package static interface returns remain accepted;
  - lockfile dependency trust denied by root manifest;
  - existing `Zeno.lock` files are audited for compiler identity, relative
    `path:` sources, workspace member path sources, path dependency edges,
    builtin dependency edges, `builtin:<name>` sources, and builtin compiler
    package hashes for the covered local forms;
  - frozen path-package lock entries validate local `manifestHash` and
    `contentHash` values against current stage0 package fingerprints;
  - pinned git dependencies and exact registry dependencies are checked against
    matching lockfile dependency edges and exact `source` strings for the
    covered offline forms;
  - exact git/registry dependencies without a pre-resolved `Zeno.lock` now
    produce staged `E9002` diagnostics instead of implying network resolution;
  - `--update-lock` writes local stage0 lockfiles for single-package,
    workspace-member, path-dependency, implicit `core`, and explicit builtin
    dependency graphs using stage0 fingerprints and builtin compiler package
    hashes, and refuses git/registry dependencies with staged `E9002` before
    writing a partial lockfile;
  - frozen `zeno build` package directories now require a local or ancestor
    `Zeno.lock`, audit ancestor workspace locks before artifact emission, and
    can be unblocked by `zeno build --update-lock`; single-source builds and
    compiler builtin declaration packages remain lock-free.
- Spec runner discovery for compile, manifest, module, package, incremental,
  and codegen cases with `--stage`, `--milestone`, `--feature`, and `--target`
  filters; fail-category tests now fail the runner unless they contain at
  least one explicit `expected-error` expectation, and MVP fail tests must use
  stable coded expectations such as `expected-error[E0401]`. Feature-only runs
  such as `zeno test --feature build-artifact` search across stages unless
  `--stage` is explicitly supplied, while bare `zeno test` and milestone-only
  runs keep the default MVP gate; filtered runs that select no tests say so
  explicitly in the summary line.
- Spec runner metadata validation rejects unknown stage, milestone, profile,
  or target metadata before filtering; real manifest TOML files only use header
  comments for test metadata, while `case.toml` keeps structured metadata
  fields, including `[target].triple` as target metadata for ABI/codegen
  filtering.
- Diagnostic emission and spec-runner diagnostic consumption now use the same
  stable ordering by source file, byte span, error code, message, stage, and
  category before JSON/human output or runner failure matching.
- Build metadata now uses a top-level AST node parser for module declarations,
  imports, attributes, structs, enums, interfaces, functions, extern
  declarations, impl headers, trust-block expressions, and covered function
  body statements including `return`, `val` / `var` local bindings, simple
  call statements, and simple binary-expression facts; the resulting
  `astNodes` and `astFingerprint` enter `.zmeta`, preview MIR/LLVM emit files,
  cache-key inputs, and the build fingerprint. Module symbol collection and
  build declaration summaries now consume the same parsed AST nodes, so nested
  interface/impl methods do not leak into package declaration fingerprints or
  public/package API summaries.
- Package-pass fixtures can opt into `build-artifact` validation; the runner
  invokes `zeno build` for those directory cases and checks the produced
  deterministic application executable launcher metadata or deterministic
  static archive header plus the stage0 preview object member, as well as
  `.zmeta` package/kind/artifact facts, including fixture-requested required
  and forbidden `.zmeta` substrings; package fixtures can also request
  `--emit mir` or `--emit llvm-ir` and assert required or forbidden substrings
  in the generated preview file.
- `case.toml` verifier coverage for the first non-source gates:
  - `codegen-pass` cases generate stage0 preview HIR/MIR/layout/LLVM facts from
    their source snippets and verify `[expect.*].contains`,
    `[expect.*].paramAttrs`, and `[expect.*].forbid` arrays, including
    before/after optimization sections;
  - `codegen-fail` cases are rejected from their supplied `bad_codegen`,
    `bad_mir`, `bad_hir`, or `bad_layout` artifacts instead of constructing a
    diagnostic solely from `[expect] error`;
  - `incremental-pass` cases derive a preview invalidation plan from
    `before` / `after` fingerprints and dependency graph sections, then verify
    `recheck`, `recodegen`, `invalidateDownstream`, `keepValid`,
    `diagnosticOrderStable`, and invalidation `reason` expectations, including
    a focused `diagnostic-order` feature fixture;
  - `incremental-fail` cases check missing trust configuration in cache keys
    and unstable observed diagnostic ordering from their input sections;
  - case files with `expected-error` comments no longer receive oracle
    diagnostics when the case verifier itself produces no error;
  - `stage = "mvp"` / `stage: mvp` fail fixtures are rejected by the runner
    unless each expected diagnostic includes a stable error code;
  - all current `codegen-fail` / `incremental-fail` `case.toml` fixtures carry
    explicit `expected-error` markers as well as structured `[expect]` data.
- Stage0 preview build outputs:
  - manifest `package.name`, `version`, optional `kind`, and optional `entry`
    are read for build artifacts;
  - manifest `[target]` `triple` / `profile` are used when CLI flags do not
    override them;
  - manifest allocator, panic, OOM, trust, and dependency policy fields are
    parsed into build-policy facts;
  - omitted `kind` is inferred from `src/main.zn`, with applications defaulting
    to `entry = "main.main"`;
  - applications produce an executable `bin/<package-name>` launcher carrying
    package, target, and build fingerprint metadata plus `.zmeta`;
  - libraries produce `lib/lib<package-name>.a` as a deterministic ar archive
    with a `zeno-stage0.o/` preview object member plus `.zmeta`;
  - `.zmeta` records package identity, kind, entry, compiler/LLVM identity,
    target/profile, manifest target/profile, builtin core marker, artifact
    format, stable source, AST, HIR, MIR, and LLVM-preview fingerprints, the
    effective local or ancestor `Zeno.lock` root/fingerprint when present, a
    stable build fingerprint over source plus AST/HIR/MIR/LLVM-preview facts,
    lockfile, effective target/profile/allocator/panic/OOM/trust
    /dependency policy, dependency package identity/content facts,
    interface/ABI/trust/cost metadata, Send/Sync facts, and builtin
    declaration package hashes, AST, AST-derived HIR/MIR/LLVM-preview facts
    for top-level declarations plus covered return, local binding, call
    statement, and simple binary-expression body shapes,
    declaration, layout, drop, Send/Sync, interface, ABI, trust, dependency,
    dependency-package, cost, runtime, and linked-runtime phase fingerprints
    included in the build fingerprint and cache-key inputs,
    top-level declaration stable-node ids with relative source spans,
    public/package-visible declaration summaries, layout/drop summaries,
    trusted and automatic `Send` / `Sync` facts, interfaces
    and impls, generic signatures, static interface returns, C ABI / bridge
    exports, trust capability categories, manifest dependency summaries,
    dependency roots, transitive local path-dependency runtime needs, reachable
    builtin package hashes, builtin public API / layout / runtime summaries,
    root runtime needs, explicit linked runtime needs, and cost-explanation
    inputs inferred from stage0 source scans;
  - `--emit mir` and `--emit llvm-ir` write structured stage0 preview debug
    files carrying package kind, entry, target, source, AST, HIR, MIR, and
    LLVM-preview fingerprints, AST-derived pipeline facts,
    declaration/layout/drop/SendSync/interface/ABI/trust/dependency/
    dependency-package/cost/runtime/linked-runtime fingerprints, and build
    fingerprint; `--emit mir` now also includes the LLVM-preview node set so
    the debug artifact visibly carries the `ast->hir->mir->llvm` chain;
    the build-artifact verifier now exercises requested emit
    files for the base application and library artifact fixtures.

## Preview-Backed Gaps

The runner now treats `expected-error` comments as expectations only; it no
longer injects diagnostics from those comments for source, manifest, module,
package, codegen, or incremental fail cases. Ordinary `zeno check` reports only
diagnostics produced by implemented stage0 logic, and `case.toml` codegen /
incremental cases use preview fact/plan verifiers. Those checks are still
lightweight: many facts do not yet come from real HIR/MIR/LLVM artifacts or a
persistent incremental cache.

The following V01 areas are still preview-backed or placeholder-backed today:

- Full parser/AST construction remains incomplete beyond the top-level AST
  node parser used by module symbols, declaration summaries, artifact metadata,
  cache keys, and the covered function-body `return`, `val` / `var`, call
  statement, and simple binary-expression shapes. HIR/MIR/LLVM lowering now
  has AST-derived phase facts and fingerprints for those nodes, but full
  syntax-tree construction, scope-aware name binding, CFG-shaped MIR, MIR
  verification, monomorphization, and LLVM object lowering remain
  preview-backed beyond those facts.
- Full name resolution and overload resolution beyond the lightweight
  top-level declaration, dependency/builtin import, and type-name checks listed
  above.
- Full type checking for primitives, structs, enums, interfaces, impls,
  generics, static interface dispatch, and `Box<Interface>` beyond the
  signature/field name resolution and field/layout/interface-object checks
  listed above.
- Complete ownership, move, access, initialization, RAII, destroy, view escape,
  and `Send` / `Sync` checks beyond the lightweight local scans listed above.
- CTFE, `static` materialization, constant generics, layout queries, and full
  pattern exhaustiveness beyond the lightweight pattern checks listed above.
- Full `.zmeta` facts produced by checked HIR/type/layout/drop/interface
  analysis, including exact ABI/layout/drop fingerprints and core library
  declaration package facts beyond the current declaration-package hash and
  source-summary facts.
- Real MIR, MIR verifier, monomorphization, LLVM lowering, object emission,
  real object-backed archive generation, linker integration, runtime shim
  linking, and performance gates beyond the first `case.toml` preview fact
  checks and stage0 preview artifact layout.
- Real incremental cache keys, stable node ids, parallel scheduling, persistent
  cache reuse, and replayable diagnostics beyond the preview invalidation-plan
  verifier and stage0 build-policy fingerprint.
- Lockfile solving remains partial: stage0 can generate and audit local
  path/builtin lockfiles, but does not update registry/git packages, signatures,
  or full publish-grade lock metadata.

## Current verification commands

```text
tools/build_stage0.sh
build/stage0/zeno test
build/stage0/zeno test --stage mvp
build/stage0/zeno test --stage full-spec
build/stage0/zeno check lib/zeno/core
build/stage0/zeno check lib/zeno/alloc
build/stage0/zeno check lib/zeno/std
build/stage0/zeno check <temp-copy-of-package> --update-lock
build/stage0/zeno build <temp-copy-of-package-without-lock>
build/stage0/zeno build <temp-copy-of-package> --update-lock
build/stage0/zeno build tests/spec/package-pass/001_workspace_lock/app
build/stage0/zeno test --feature build-artifact
build/stage0/zeno test --stage full-spec --feature build-artifact
```

Current local results after the latest update:

```text
build/stage0/zeno check lib/zeno/core
build/stage0/zeno check lib/zeno/alloc
build/stage0/zeno check lib/zeno/std
build/stage0/zeno check /private/tmp/.../pkg --update-lock
build/stage0/zeno check /private/tmp/.../pkg
build/stage0/zeno build /private/tmp/.../pkg
  -> error[E1007]: frozen build requires Zeno.lock
build/stage0/zeno build /private/tmp/.../pkg --update-lock
build/stage0/zeno build /private/tmp/.../pkg
build/stage0/zeno build tests/spec/package-pass/001_workspace_lock/app
build/stage0/zeno check tests/spec/package-fail/001_git_missing_rev --diagnostic-format json
  -> "feature":"git-dependency","isStaged":true
build/stage0/zeno check tests/spec/package-fail/002_registry_version_range --diagnostic-format json
  -> "feature":"registry-dependency","isStaged":true
build/stage0/zeno check tests/spec/module-fail/009_cross_package_interface_return --diagnostic-format json
  -> "feature":"cross-package-interface-return","isStaged":true
build/stage0/zeno check tests/spec/package-fail/009_remote_without_lock --diagnostic-format json
  -> git/registry dependency requires a pre-resolved Zeno.lock, "isStaged":true
build/stage0/zeno check /private/tmp/.../remote-pkg --update-lock --diagnostic-format json
  -> --update-lock cannot resolve git/registry dependency, no Zeno.lock written
build/stage0/zeno build /private/tmp/.../remote-pkg --update-lock --diagnostic-format json
  -> --update-lock cannot resolve git/registry dependency, no Zeno.lock written
build/stage0/zeno build /private/tmp/.../workspace/app
  -> .zmeta records ancestor lockRoot/lockFingerprint; mutating Zeno.lock changes buildFingerprint
build/stage0/zeno build tests/spec/compile-pass/001_move_raii.zn
  -> .zmeta records lockFingerprint = "none"
build/stage0/zeno check tests/spec/compile-pass/001_move_raii.zn --diagnostic-format xml
  -> zeno: --diagnostic-format must be human or json
build/stage0/zeno test --stage smoke
  -> zeno: --stage must be mvp or full-spec
build/stage0/zeno test --milestone M10
  -> zeno: --milestone must be M0-M9
build/stage0/zeno build tests/spec/compile-pass/001_move_raii.zn --emit asm
  -> zeno: --emit must be mir or llvm-ir
build/stage0/zeno build tests/spec/compile-pass/001_move_raii.zn --emit mir
  -> target/.../mir/zeno-package.mir written
build/stage0/zeno build tests/spec/compile-pass/001_move_raii.zn --emit llvm-ir
  -> target/.../ir/zeno-package.ll written
build/stage0/zeno check tests/spec/manifest-fail/009_unknown_target.toml --diagnostic-format json
  -> error[E1002]: unsupported target triple
build/stage0/zeno build tests/spec/compile-pass/001_move_raii.zn --target wasm32-unknown-unknown
  -> zeno: --target must be aarch64-apple-darwin or x86_64-unknown-linux-gnu
build/stage0/zeno build tests/spec/compile-pass/001_move_raii.zn --profile server
  -> zeno: --profile must be hosted, freestanding, kernel, or embedded
build/stage0/zeno check tests/spec/manifest-fail/010_unknown_package_kind.toml --diagnostic-format json
  -> error[E1002]: package.kind must be application or library
build/stage0/zeno check tests/spec/manifest-fail/011_unknown_panic_strategy.toml --diagnostic-format json
  -> error[E1002]: unknown panic strategy
build/stage0/zeno check tests/spec/manifest-fail/018_panic_unwind_staged.toml --diagnostic-format json
  -> error[E9003]: panic unwind is not implemented in stage0, "feature":"panic-unwind", "isStaged":true
build/stage0/zeno check tests/spec/manifest-fail/019_kernel_requires_handler_strategies.toml --diagnostic-format json
  -> error[E1002]: kernel profile requires handler panic strategy; error[E1002]: kernel profile requires handler oom strategy
build/stage0/zeno check tests/spec/manifest-fail/020_kernel_requires_trust_report.toml --diagnostic-format json
  -> error[E1002]: kernel profile requires trust.requireReport = true
build/stage0/zeno check tests/spec/manifest-fail/012_unknown_oom_strategy.toml --diagnostic-format json
  -> error[E1002]: unknown oom strategy
build/stage0/zeno check tests/spec/manifest-fail/013_unknown_trust_capability.toml --diagnostic-format json
  -> error[E1001]: unknown trust capability field
build/stage0/zeno check tests/spec/manifest-fail/014_missing_package_name.toml --diagnostic-format json
  -> error[E1003]: package.name is required
build/stage0/zeno check tests/spec/manifest-fail/015_invalid_package_name.toml --diagnostic-format json
  -> error[E1003]: package.name must be a legal Zeno identifier
build/stage0/zeno check tests/spec/manifest-fail/016_library_entry.toml --diagnostic-format json
  -> error[E1003]: library packages cannot declare package.entry
build/stage0/zeno check tests/spec/manifest-fail/017_absolute_path_dependency.toml --diagnostic-format json
  -> error[E1004]: dependency path must be relative, not absolute
build/stage0/zeno check tests/spec/package-fail/010_application_missing_main --diagnostic-format json
  -> error[E1003]: application default entry requires src/main.zn or explicit package.entry
build/stage0/zeno check tests/spec/package-fail/011_application_bad_entry --diagnostic-format json
  -> error[E1003]: application entry function is not declared
build/stage0/zeno check tests/spec/package-fail/012_application_malformed_entry --diagnostic-format json
  -> error[E1003]: application entry must be module.function
build/stage0/zeno check tests/spec/package-fail/013_application_missing_entry_file --diagnostic-format json
  -> error[E1003]: application entry source file does not exist
build/stage0/zeno check tests/spec/package-fail/014_path_dependency_missing --diagnostic-format json
  -> error[E1004]: dependency path does not exist
build/stage0/zeno check tests/spec/package-fail/015_std_import_freestanding --diagnostic-format json
  -> error[E0201]: std is available only in hosted profile or an explicit hosted-capable builtin dependency
build/stage0/zeno check tests/spec/package-fail/016_handler_return_never --diagnostic-format json
  -> error[E1002]: panic.handler must return Never; error[E1002]: oom.handler must return Never
build/stage0/zeno check tests/spec/package-fail/017_manifest_trust_capability_denied --diagnostic-format json
  -> error[E0714]: manifest does not allow FFI trust capability; error[E0711]: manifest does not allow rawMemory trust capability
build/stage0/zeno check tests/spec/package-fail/018_trust_allowed_packages --diagnostic-format json
  -> error[E1006]: package driver is not allowed to contain trust boundaries
build/stage0/zeno check tests/spec/package-fail/019_manifest_low_level_trust_denied --diagnostic-format json
  -> error[E0712]: manifest does not allow hardware trust capability; error[E0713]: manifest does not allow inlineAsm trust capability; error[E0715]: manifest does not allow interrupts trust capability
build/stage0/zeno check tests/spec/package-fail/020_dependency_trust_denied --diagnostic-format json
  -> error[E1006]: dependency package platform contains trust boundaries but dependencyTrust is false
build/stage0/zeno check tests/spec/package-pass/017_std_builtin_freestanding --diagnostic-format json
  -> ok; lockfile records std and core builtin compiler package hashes
build/stage0/zeno check tests/spec/module-pass/005_external_named_import --diagnostic-format json
  -> ok; grouped path dependency imports resolve pub struct/function declarations
build/stage0/zeno check tests/spec/module-fail/010_missing_imported_item --diagnostic-format json
  -> error[E0201]: imported item missingPid is not declared in module platform.os
build/stage0/zeno check tests/spec/module-fail/008_external_package_visible_not_exported --diagnostic-format json
  -> error[E0203]: hidden is package-visible, not pub
build/stage0/zeno check tests/spec/module-fail/011_unknown_type_name --diagnostic-format json
  -> error[E0201]: type MissingType is not declared; error[E0201]: type MissingArg is not declared
build/stage0/zeno check tests/spec/compile-fail/167_unknown_struct_field.zn --diagnostic-format json
  -> error[E0201]: field missing is not declared in struct Packet (constructor and field access)
build/stage0/zeno check tests/spec/compile-fail/168_missing_struct_field_initializer.zn --diagnostic-format json
  -> error[E0404]: field length is not initialized in struct Header
build/stage0/zeno check tests/spec/compile-fail/169_duplicate_struct_field_initializer.zn --diagnostic-format json
  -> error[E0404]: field tag is initialized more than once in struct Header
build/stage0/zeno check tests/spec/compile-fail/170_unknown_enum_variant.zn --diagnostic-format json
  -> error[E0201]: variant Missing is not declared in enum State
build/stage0/zeno check tests/spec/compile-fail/171_enum_variant_payload_shape.zn --diagnostic-format json
  -> error[E0201]: variant Some requires 1 payload value; error[E0201]: variant None does not take payload values; error[E0201]: variant Some expects 1 payload value but got 2
build/stage0/zeno check tests/spec/compile-fail/172_return_literal_type_mismatch.zn --diagnostic-format json
  -> error[E0302]: return type U32 does not match Bool expression; error[E0302]: return type Bool does not match integer expression; error[E0301]: string literal is StringSlice; use String.from("hello") for owned String
build/stage0/zeno check tests/spec/compile-fail/173_return_variable_type_mismatch.zn --diagnostic-format json
  -> error[E0302]: return type U32 does not match Bool expression; error[E0302]: return type Bool does not match U32 expression; error[E0302]: return type Packet does not match Header expression
build/stage0/zeno check tests/spec/compile-fail/174_initializer_type_mismatch.zn --diagnostic-format json
  -> error[E0302]: initializer type U32 does not match Bool expression; error[E0302]: initializer type Bool does not match U32 expression; error[E0302]: initializer type Packet does not match Header expression; error[E0302]: initializer type State does not match Maybe expression
build/stage0/zeno check tests/spec/compile-fail/175_struct_field_initializer_type_mismatch.zn --diagnostic-format json
  -> error[E0302]: field count type U32 does not match Bool expression; error[E0302]: field enabled type Bool does not match U32 expression; error[E0302]: field header type Header does not match Footer expression; error[E0302]: field state type State does not match Maybe expression
build/stage0/zeno check tests/spec/compile-fail/176_assignment_type_mismatch.zn --diagnostic-format json
  -> error[E0302]: assignment type U32 does not match Bool expression; error[E0302]: assignment type Bool does not match U32 expression; error[E0302]: assignment type Packet does not match Header expression; error[E0302]: assignment type State does not match Maybe expression
build/stage0/zeno check tests/spec/compile-fail/177_field_assignment_type_mismatch.zn --diagnostic-format json
  -> error[E0302]: field assignment count type U32 does not match Bool expression; error[E0302]: field assignment enabled type Bool does not match U32 expression; error[E0302]: field assignment header type Header does not match Packet expression; error[E0302]: field assignment state type State does not match Maybe expression
build/stage0/zeno check tests/spec/compile-fail/178_function_argument_type_mismatch.zn --diagnostic-format json
  -> error[E0302]: argument 1 of needCount expects U32 but got Bool; error[E0302]: argument 1 of needFlag expects Bool but got U32; error[E0302]: argument 1 of needPacket expects Packet but got Header; error[E0302]: argument 1 of needState expects State but got Maybe
build/stage0/zeno check tests/spec/compile-fail/179_function_argument_arity_mismatch.zn --diagnostic-format json
  -> error[E0302]: function needPair expects 2 arguments but got 1; error[E0302]: function needPair expects 2 arguments but got 3; error[E0302]: function needNone expects 0 arguments but got 1
build/stage0/zeno check tests/spec/compile-fail/180_function_call_result_type_mismatch.zn --diagnostic-format json
  -> error[E0302]: initializer type U32 does not match Bool expression; error[E0302]: assignment type Bool does not match U32 expression; error[E0302]: field assignment header type Header does not match Packet expression; error[E0302]: argument 1 of needCount expects U32 but got Bool; error[E0302]: return type U32 does not match Bool expression
build/stage0/zeno check tests/spec/compile-fail/181_condition_type_mismatch.zn --diagnostic-format json
  -> error[E0302]: if condition must be Bool, got U32; error[E0302]: if condition must be Bool, got integer expression; error[E0302]: while condition must be Bool, got U32
build/stage0/zeno check tests/spec/compile-fail/182_binary_operand_type_mismatch.zn --diagnostic-format json
  -> error[E0302]: operator + requires numeric operands, got Bool and U32; error[E0302]: operator - requires numeric operands, got Bool and U32; error[E0302]: operator * requires numeric operands, got U32 and Bool; error[E0302]: operator / requires numeric operands, got Bool and U32; error[E0302]: operator + requires numeric operands, got Bool and U32; error[E0302]: operator - requires numeric operands, got Bool and U32; error[E0302]: operator * requires numeric operands, got Bool and U32
build/stage0/zeno check tests/spec/compile-fail/183_comparison_operand_type_mismatch.zn --diagnostic-format json
  -> error[E0302]: operator == requires operands of the same comparable type, got U32 and Bool; error[E0302]: operator < requires numeric operands, got Bool and U32; error[E0302]: operator <= requires numeric operands, got U32 and Bool; error[E0302]: operator != requires operands of the same comparable type, got U32 and Bool; error[E0302]: operator > requires numeric operands, got U32 and Bool; error[E0302]: operator >= requires numeric operands, got Bool and U32; error[E0302]: operator == requires operands of the same comparable type, got U32 and Bool; error[E0302]: operator < requires numeric operands, got U32 and Bool; error[E0302]: operator > requires numeric operands, got Bool and U32
build/stage0/zeno check tests/spec/compile-pass/083_char_literals.zn
  -> no diagnostics
build/stage0/zeno check tests/spec/compile-fail/184_char_literal_type_mismatch.zn --diagnostic-format json
  -> error[E0302]: initializer type U32 does not match Char expression; error[E0302]: initializer type Bool does not match Char expression; error[E0302]: field assignment kind type Char does not match U32 expression; error[E0302]: field kind type Char does not match Bool expression; error[E0302]: argument 1 of needChar expects Char but got U32; error[E0302]: if condition must be Bool, got Char; error[E0302]: return type Char does not match Bool expression
build/stage0/zeno check tests/spec/compile-pass/084_float_literals.zn
  -> no diagnostics
build/stage0/zeno check tests/spec/compile-fail/185_float_literal_type_mismatch.zn --diagnostic-format json
  -> error[E0302]: initializer type U32 does not match float expression; error[E0302]: initializer type Bool does not match float expression; error[E0302]: field assignment x type F32 does not match Bool expression; error[E0302]: field x type F32 does not match Bool expression; error[E0302]: argument 1 of needCount expects U32 but got float expression; error[E0302]: if condition must be Bool, got float expression; error[E0302]: return type Bool does not match float expression
build/stage0/zeno check tests/spec/compile-pass/085_unit_return_flow.zn
  -> no diagnostics
build/stage0/zeno check tests/spec/compile-fail/186_unit_result_type_mismatch.zn --diagnostic-format json
  -> error[E0302]: initializer type U32 does not match Unit expression; error[E0302]: initializer type Bool does not match Unit expression; error[E0302]: assignment type U32 does not match Unit expression; error[E0302]: field value type U32 does not match Unit expression; error[E0302]: argument 1 of needCount expects U32 but got Unit; error[E0302]: if condition must be Bool, got Unit; error[E0302]: return type U32 does not match Unit expression; error[E0302]: Unit function cannot return integer expression
build/stage0/zeno check tests/spec/compile-pass/086_never_return_flow.zn
  -> no diagnostics
build/stage0/zeno check tests/spec/compile-pass/043_panic_oom_never.zn
  -> no diagnostics
build/stage0/zeno check tests/spec/compile-fail/187_never_return_type_mismatch.zn --diagnostic-format json
  -> error[E0302]: Never function cannot return integer expression; error[E0302]: Never function cannot return Bool expression; error[E0302]: Never function cannot return Unit expression
build/stage0/zeno check tests/spec/compile-pass/087_explicit_return_paths.zn
  -> no diagnostics
build/stage0/zeno check tests/spec/compile-fail/188_missing_return_path.zn --diagnostic-format json
  -> error[E0302]: function missingReturn must return U32 on all paths; error[E0302]: function conditionalOnly must return U32 on all paths
build/stage0/zeno check tests/spec/compile-pass/088_known_identifier_flow.zn
  -> no diagnostics
build/stage0/zeno check tests/spec/compile-fail/189_unknown_identifier.zn --diagnostic-format json
  -> error[E0201]: value missingInit is not declared; error[E0201]: value missingInferred is not declared; error[E0201]: value missingAssign is not declared; error[E0201]: value missingTarget is not declared; error[E0201]: value missingField is not declared; error[E0201]: value missingArg is not declared; error[E0201]: value missingCondition is not declared; error[E0201]: value missingReturn is not declared
build/stage0/zeno check tests/spec/compile-pass/089_collection_clone_copy_bounds.zn
  -> no diagnostics
build/stage0/zeno check tests/spec/compile-fail/190_collection_clone_requires_copy.zn --diagnostic-format json
  -> error[E0513]: Array.clone requires T: Copy; error[E0513]: Vector.clone requires T: Copy; error[E0513]: Map.clone requires K: Copy and V: Copy; error[E0513]: Map.clone requires K: Copy and V: Copy; error[E0513]: Set.clone requires T: Copy
build/stage0/zeno check tests/spec/compile-pass/090_map_set_hashkey_bounds.zn
  -> no diagnostics
build/stage0/zeno check tests/spec/compile-fail/191_map_set_requires_hashkey.zn --diagnostic-format json
  -> error[E0601]: Map key type Packet must implement HashKey; error[E0601]: Set value type Packet must implement HashKey; error[E0601]: Map key type Packet must implement HashKey; error[E0601]: Set value type Packet must implement HashKey
build/stage0/zeno check tests/spec/compile-pass/091_map_entry_local_flow.zn
  -> no diagnostics
build/stage0/zeno check tests/spec/compile-fail/192_map_entry_cannot_escape.zn --diagnostic-format json
  -> error[E0503]: MapEntry cannot be stored in structure fields; error[E0503]: MapEntry cannot be stored inside collection fields; error[E0503]: MapEntry cannot be stored in static storage; error[E0503]: MapEntry cannot be returned from a function; error[E0503]: MapEntry cannot be stored inside collections; error[E0503]: MapEntry cannot be stored in Box; error[E0503]: MapEntry cannot be stored in Shared
build/stage0/zeno check tests/spec/compile-fail/193_no_alloc_rejects_allocating_apis.zn --diagnostic-format json
  -> error[E0305]: push may allocate in no-allocation context; error[E0305]: reserve may allocate in no-allocation context; error[E0305]: String.from may allocate in no-allocation context; error[E0305]: clone may allocate in no-allocation context; error[E0305]: collection allocation is not allowed in no-allocation context; error[E0305]: collection insertion may allocate in no-allocation context; error[E0305]: Box/Shared allocation is not allowed in no-allocation context
build/stage0/zeno check tests/spec/compile-fail/194_no_panic_reachable_calls.zn --diagnostic-format json
  -> error[E0807]: noPanic function cannot call function that may panic; error[E0807]: allocation failure would call panic in this profile
build/stage0/zeno check tests/spec/compile-fail/195_effect_guards_reject_reachable_allocations.zn --diagnostic-format json
  -> error[E0305]: noAlloc function cannot call function that may allocate; error[E0807]: allocation failure would call panic in this profile; error[E0405]: destroy cannot call panic; error[E0405]: destroy cannot allocate
build/stage0/zeno check tests/spec/compile-fail/196_panic_diagnostic_storage_escape.zn --diagnostic-format json
  -> error[E0809]: PanicInfo is a diagnostic access value and cannot be stored; error[E0809]: StackFrames is valid only inside the panic handler path
build/stage0/zeno test
  -> zeno test: 127 passed, 0 failed (stage mvp)
build/stage0/zeno test --feature diagnostic-order
  -> zeno test: 1 passed, 0 failed
build/stage0/zeno test --feature build-artifact
  -> zeno test: 15 passed, 0 failed; artifact fixtures assert top-level AST node facts, covered function-body return/local-binding/call-statement AST/lowering facts, simple binary-expression facts, AST-derived HIR/MIR/LLVM-preview facts, ast/hir/mir/llvm cache inputs, brace-aware top-level declaration collection, declaration stableNodeId/module/visibility/span facts, declaration/runtime/layout/drop/SendSync/interface/ABI/trust/dependency/dependency-package/cost/linked-runtime fingerprints, cacheKeyInputs, emitted MIR/LLVM preview facts, hosted profile with empty linkRuntimeNeeds, reachable Thread.spawn with linkRuntimeNeeds = ["thread"], reachable allocation/panic/OOM with linkRuntimeNeeds = ["allocator", "oom", "panic"], C ABI exports with linkRuntimeNeeds = ["c-abi-boundary"], C bridge exports recorded as bridge=C, trust report capability metadata, trusted and automatic Send/Sync metadata, interface/impl/static-return metadata, transitive path dependency package fingerprints plus dependency runtime propagation, and layout/drop glue cache metadata
build/stage0/zeno test --stage mvp --feature build-artifact
  -> zeno test: 2 passed, 0 failed (stage mvp)
build/stage0/zeno test --stage full-spec --feature build-artifact
  -> zeno test: 15 passed, 0 failed
build/stage0/zeno test --feature nope
  -> zeno test: 0 passed, 0 failed (no tests selected)
build/stage0/zeno build tests/spec/package-pass/004_application_artifact --emit mir
  -> target/.../mir/artifactApp.mir records declarationFingerprint, buildFingerprint, ast->hir->mir->llvm pipeline facts, and `return 0` AST/HIR/MIR/LLVM-preview facts; bin/artifactApp carries package/target/buildFingerprint launcher metadata and .zmeta executableFormat = "sh-stage0-preview-launcher"
build/stage0/zeno build tests/spec/package-pass/005_library_artifact --emit llvm-ir
  -> target/.../ir/artifactLib.ll records declarationFingerprint and buildFingerprint; lib/libartifactLib.a is a deterministic ar archive with zeno-stage0.o/ preview object and .zmeta archiveFormat = "ar-stage0-preview-object"
build/stage0/zeno build tests/spec/package-pass/006_hosted_no_hidden_runtime --emit mir
  -> target/.../mir/hostedQuiet.mir records runtimeFingerprint and buildFingerprint
build/stage0/zeno build tests/spec/package-pass/007_hosted_thread_runtime --emit llvm-ir
  -> target/.../ir/hostedThread.ll records runtimeFingerprint and buildFingerprint; .zmeta records runtimeNeeds/linkRuntimeNeeds = ["thread"]
build/stage0/zeno build tests/spec/package-pass/008_hosted_allocator_panic_oom_runtime --emit mir
  -> target/.../mir/hostedRuntime.mir records runtimeFingerprint and buildFingerprint; .zmeta records runtimeNeeds/linkRuntimeNeeds = ["allocator", "oom", "panic"]
build/stage0/zeno build tests/spec/package-pass/009_c_abi_export_runtime --emit llvm-ir
  -> target/.../ir/cAbiRuntime.ll records runtimeFingerprint and buildFingerprint; .zmeta records exports and runtimeNeeds/linkRuntimeNeeds = ["c-abi-boundary"]
build/stage0/zeno build tests/spec/package-pass/010_c_bridge_export_runtime --emit mir
  -> target/.../mir/cBridgeRuntime.mir records runtimeFingerprint and buildFingerprint; .zmeta records bridge=C exports and runtimeNeeds/linkRuntimeNeeds = ["c-abi-boundary"]
build/stage0/zeno build tests/spec/package-pass/011_trust_report_artifact --emit llvm-ir
  -> target/.../ir/trustReport.ll records runtimeFingerprint and buildFingerprint; .zmeta records manifestTrust/buildPolicy/trustCapabilities for ffi/rawMemory/trust-block
build/stage0/zeno build tests/spec/package-pass/012_trust_impl_send_sync_artifact --emit mir
  -> target/.../mir/trustedSendSync.mir records runtimeFingerprint and buildFingerprint; .zmeta records sendSync trusted Send/Sync facts and threadSafety trust capability without runtime links
build/stage0/zeno build tests/spec/package-pass/013_layout_drop_artifact --emit llvm-ir
  -> target/.../ir/layoutDrop.ll records layoutFingerprint, dropFingerprint, runtimeFingerprint, and buildFingerprint; .zmeta records C/Packed/Auto layout facts, Resource drop glue, and layouts/drop cacheKeyInputs
build/stage0/zeno build tests/spec/package-pass/014_auto_send_sync_artifact --emit mir
  -> target/.../mir/autoSendSync.mir records declaration/layout/drop/runtime/build fingerprints; .zmeta records Job automatic Send/Sync facts without runtime links
build/stage0/zeno build tests/spec/package-pass/015_interface_abi_cache_artifact --emit llvm-ir
  -> target/.../ir/interfaceAbiCache.ll records SendSync/interface/ABI/trust/dependency/cost fingerprints; .zmeta records those fingerprints in cacheKeyInputs plus interface, impl, generic signature, static return, export, trust, and cost facts
build/stage0/zeno build tests/spec/package-pass/016_path_dependency_artifact --emit mir
  -> target/.../mir/pathDepArtifact.mir records dependencyPackageFingerprint and linkRuntimeFingerprint; .zmeta records transitive platform and platform/driver dependency package names, relative paths, manifest/content hashes, dependencyPackages/linkRuntime cacheKeyInputs, dependencyRuntimeNeeds = ["platform/driver:thread", "platform:allocator", "platform:oom", "platform:panic"], root runtimeNeeds = [], and linkRuntimeNeeds = ["allocator", "oom", "panic", "thread"]
build/stage0/zeno build tests/spec/package-pass/018_declaration_collection_artifact --update-lock
  -> target/.../meta/declarationCollection.zmeta records only top-level Worker/Job/makeJob declarations and forbids nested run/helper method declaration facts
build/stage0/zeno build tests/spec/package-pass/019_ast_artifact --emit mir
  -> target/.../meta/astArtifact.zmeta records astFingerprint, astNodes for module/import/attribute/decl/trust/return/local-binding/call-statement nodes, AST-derived hirNodes/mirNodes/llvmNodes including return, local, call, and simple binary-expression lowering, ast/hir/mir/llvm cacheKeyInputs, and preview MIR ast->hir->mir->llvm pipeline fingerprints with llvmNodes emitted in the MIR debug file
build/stage0/zeno test --stage mvp --milestone M2
  -> zeno test: 61 passed, 0 failed (stage mvp, M2)
build/stage0/zeno test --stage mvp --milestone M3
  -> zeno test: 10 passed, 0 failed (stage mvp, M3)
build/stage0/zeno test --stage mvp --milestone M5
  -> zeno test: 23 passed, 0 failed (stage mvp, M5)
build/stage0/zeno test --milestone M4
  -> zeno test: 25 passed, 0 failed (stage mvp, M4)
build/stage0/zeno test --workspace /private/tmp/zeno-metadata-check
  -> invalid test milestone metadata ... M22
build/stage0/zeno test --workspace /private/tmp/zeno-target-metadata
  -> invalid test target metadata ... wasm32-unknown-unknown
build/stage0/zeno test --stage full-spec
  -> zeno test: 409 passed, 0 failed
build/stage0/zeno test --stage full-spec --target x86_64-unknown-linux-gnu
  -> zeno test: 409 passed, 0 failed
build/stage0/zeno test --stage full-spec --target aarch64-apple-darwin
  -> zeno test: 391 passed, 0 failed
zeno test: 127 passed, 0 failed (stage mvp)
zeno test: 409 passed, 0 failed
```

At this point, every `tests/spec/compile-fail/*.zn` file is rejected by ordinary
`zeno check` without relying on the spec runner's `expected-error` oracle. The
manifest, module, and package fail fixtures are likewise rejected by real
`zeno check` diagnostics, and `case.toml` fail fixtures are rejected by their
preview verifiers.
This is still not full V01 completion: many broader requirements remain
preview-backed or placeholder-backed outside single-file source checking,
especially real HIR/MIR construction, type/ownership proof infrastructure,
monomorphization, LLVM object/codegen output, archives/linking, runtime shims,
incremental cache keys, and performance gates.
