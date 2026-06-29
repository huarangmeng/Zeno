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
    top-level `match`, or covered `if (value is Pattern)` branch expression;
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
    generic parameters, interface generic parameters used by interface method
    signatures, nested generic function constraints, builtin declaration
    packages, and imported pub type declarations;
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
- Build metadata now has two front-end fact sources: the legacy
  `ParsedAstNode` path still feeds AST-derived HIR/MIR/LLVM preview facts, and
  the first token-cursor syntax spine emits independent `syntax.file`,
  `syntax.module`, `syntax.import`, `syntax.item`, `syntax.block`,
  `syntax.field`, `syntax.variant`, `syntax.generic-param`, `syntax.param`,
  `syntax.type`, `syntax.pattern`, `syntax.arg`, and `syntax.stmt` facts plus
  structured expression facts such as `syntax.binary-expr`,
  `syntax.literal-expr`, `syntax.call`, `syntax.field-access`,
  `syntax.struct-literal`, `syntax.try-expr`, and `syntax.await-expr` for the
  covered grammar surface. The
  legacy path covers module declarations, imports, attributes, structs, enums,
  interfaces, functions, extern declarations, impl headers, trust-block
  expressions, and covered function-body statements including `return`, `val` /
  `var` local bindings, simple call statements, assignment, local `const`,
  `break`, `continue`, `if`, `while`, `for` with read/mut/move iteration modes,
  statement `match` with read/move/mut access modes, match arms with optional
  guards, or-pattern/range-pattern text, `else`, if-pattern / while-pattern
  with read/move/mut pattern modes, pattern destructuring local bindings,
  standalone `try`, top-level `const`, top-level `static`, type aliases,
  struct fields, enum variant payloads, interface parent lists, generic
  interface headers and methods, generic interface impl methods, impl consts,
  destroy blocks, generic / const-generic declaration headers including nested
  generic constraints such as `fn staticConsumer<T: Consumer<U32>>`, top-level
  function/extern/method signatures, trust extern declarations, trust-block
  expressions, C ABI export attributes, struct literal expressions,
  method-call expressions, tuple/unit/char/cast/index/type-static call
  expressions, closure expressions including move-capturing and block closure
  forms, `try` expressions, match expressions including `match move`, and
  simple binary/comparison-expression facts. The new syntax spine parses
  token streams with balanced item/member/function blocks and records
  file/item/block/field/variant/generic-param/param/type/pattern/argument/
  statement/expression structure for functions, impl/interface members, local
  bindings, assignment, if/while pattern conditions using `is`, for loops,
  standalone and return-position `match`, guarded match arms, block arms,
  `try`, break/continue, calls, receiver/parameter modes, generic constraints,
  const-generic parameters, impl target types, enum payload types, local type
  annotations, and binary/comparison expressions.
  The resulting `astNodes` and `astFingerprint` enter `.zmeta`, preview
  MIR/LLVM emit files, cache-key inputs, and the build fingerprint. Module
  symbol collection and build declaration summaries still consume the legacy
  parsed AST nodes, including top-level `const` / `static`, so nested
  interface/impl methods do not leak into package declaration fingerprints or
  public/package API summaries.
- Package-pass fixtures can opt into `build-artifact` validation; the runner
  invokes `zeno build` for those directory cases and checks the produced
  native object artifact, host-native executable when the target matches the
  current host, or deterministic static archive header plus the native object
  member, as well as `.zmeta` package/kind/artifact facts, including
  fixture-requested required and forbidden `.zmeta` substrings; package
  fixtures can also request `--emit mir` or `--emit llvm-ir` and assert
  required or forbidden substrings in the generated preview file. Host-native
  application fixtures can declare `run-exit-code`, causing the runner to
  execute the produced binary and fail if the process exit code differs.
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
  - every build writes `mir/<package-name>.mir`, `ir/<package-name>.ll`, and a
    real native object file at `obj/<package-name>.o`;
  - the first native lowering path supports covered `I32` functions with `I32`
    parameters, integer literals, top-level `I32 const` values, local `const`,
    top-level simple `I32 static` globals loaded through LLVM `load`, local
    `val` / `var` bindings, local-name returns, direct calls to successfully
    lowered `I32` functions, simple `+`, `-`, `*`, and `/`
    integer expressions, simple struct literals lowered through LLVM named
    aggregate types plus `insertvalue`, and struct field reads lowered through
    `extractvalue`; simple struct-returning functions can now return LLVM
    aggregates and callers can bind `call %Struct` results, pass `%Struct`
    arguments to covered callees, and extract fields from struct parameters.
    The original all-`I32` path still covers the first C ABI aggregate thunk,
    while the native Auto-layout struct path now also supports mixed primitive
    scalar fields such as LLVM `{ i8, i16, i64, float, double }`, with field
    extraction preserving `U8`, `U16`, `I64`, `F32`, and `F64` call signatures;
    covered struct parameters can also be returned again as LLVM aggregates, a minimal
    `if (<i32 comparison>) { return ... }` plus fallthrough-return control-flow
    shape, simple `if (<i32 comparison>) { return ... } else { return ... }`
    double-return control flow, plus simple
    `while (<i32 comparison>) { var = expr }` loops over mutable `var` locals
    lowered through `alloca` / `load` / `store`,
    producing valid LLVM IR
    definitions such as `define i32 @main()` / `define i32 @addOne(i32 %value)`
    plus `icmp` / `br` blocks and loop back-edges, and compiling them with the
    local `clang` backend;
  - the native lowering path now also covers simple `Bool` functions with
    `I32` parameters, lowered as LLVM `i1`, and `I32` `if` / `while`
    conditions can call successfully lowered `Bool` functions. This produces
    real IR such as `define i1 @isPositive(i32 %value)`, `ret i1 %t0`, and
    `call i1 @isPositive(...)` feeding `br i1`, then compiles and links through
    the same native object/executable path;
  - the same native path now lowers covered no-payload enum values and
    enum-returning functions as `i32` discriminants; callers can bind enum
    call results, compare enum values with `==`, and feed them into statement
    `match` or `return match` arms lowered as real LLVM ordered branch chains, e.g.
    `define i32 @chooseMode()` with `ret i32 1`,
    `define i1 @isHot(i32 %mode)` with `icmp eq i32 %mode, 1`,
    `define i32 @score(i32 %mode)`, and
    `define i32 @statementScore(i32 %mode)` with `Mode.Cold` / `Mode.Hot` /
    `Mode.Done` cases branching to native return blocks. The first payload
    enum ABI path now lowers covered all-`I32` payload aggregates too:
    `WorkState` is emitted as `%WorkState = type { i32, i32 }` and `Event`
    as `%Event = type { i32, i32, i32 }`; constructors insert the tag and
    payload slots, calls pass/return the LLVM aggregate, and `match` extracts
    the tag plus selected tuple/record payload slots before returning native
    `I32`. Guarded arms, closed `I32` payload range arms, or-pattern arms over
    same-layout payload variants, and unit variants inside payload enums now
    lower through the same native ordered branch path. The payload enum ABI
    path now also covers the first mixed primitive scalar payload layout:
    `Packet` can be emitted as `%Packet = type { i32, i8, i16, i64, float, double }`
    while carrying either a mixed-scalar `%Header = type { i8, i16, i64, float, double }`
    struct payload or a record payload with `U8`, `U16`, `I64`, `F32`, and
    `F64` fields; constructors insert typed slots, `match` arms extract or
    reconstruct typed payloads, and helper calls preserve `i8`, `i16`, `i64`,
    `float`, and `double` signatures. Covered payload enum
    `if (expr is Pattern)` branches now lower to native tag tests and payload
    binding too, including the `if (state is mut WorkState.Ready(job))` fixture.
    Covered payload enum `while (expr is Pattern)` loops also lower to native loop
    headers, aggregate-returning scrutinee calls, tag tests, payload binding,
    body blocks, exit blocks, and loop back-edges over mutable locals, including
    `while (keyUntil(limit, i) is Event.Key(value))` with `acc` / `i` stores.
    The first generic payload enum instantiation path now lowers
    `Maybe<I32>`, `Maybe<Job>`, multi-field `Maybe<Task>`, and mixed-scalar
    `Maybe<Header>` as named LLVM
    aggregates `%Maybe_I32 = type { i32, i32 }`,
    `%Maybe_Job = type { i32, i32 }`, and
    `%Maybe_Task = type { i32, i32, i32 }`, plus
    `%Maybe_Header = type { i32, i8, i16, i64, float, double }`, including
    `Maybe.Some(value)` / `Maybe.None` constructors, struct payload
    reconstruction for `Maybe.Some(Job { ... })` and
    `Maybe.Some(Task { ... })`, and
    `if (maybe is Maybe.Some(value))` / `if (maybe is Maybe.Some(job))` tag tests
    plus `return match maybe` over covered generic instantiations. The
    `Maybe<I32>` producer and `while (maybeUntil(limit, i) is Maybe.Some(value))`
    loop now also lower to real native LLVM with aggregate-returning loop
    scrutinee calls, tag extraction, payload binding, mutable local stores,
    and a loop back-edge; broader generic payload forms with incompatible
    per-variant slot layouts remain preview-backed
    until the native monomorphization path is widened;
  - the first native `for` lowering path now covers I32 range iteration:
    `for i in start..end` lowers to a real `for.cond` header with `icmp slt`,
    body block, `for.step` increment, mutable accumulator stores, and a loop
    back-edge; `for i in start..=end` uses the same shape with `icmp sle`.
    The concentrated fixture covers parameter bounds, empty half-open ranges,
    single-element closed ranges, object code, host executable output, and
    runner-checked exit code;
  - the first `Result<I32, I32>` native path now instantiates the two-parameter
    generic payload enum as `%Result_I32_I32 = type { i32, i32 }`, lowers
    `Result.Ok(value)` and `Result.Err(code)` constructors, lowers
    `val value = try producer(...)` to ordinary LLVM `extractvalue` tag tests
    and `br i1` blocks, returns the Err aggregate directly on the error edge,
    binds the Ok payload on the continuation edge, lowers standalone
    `try producer(...)` statements to the same Err early-return / Ok continue
    branch shape, and composes with native `return match result` extraction
    and executable output. This is a real no-exception control-flow path for
    the covered all-`I32` Result shape;
  - the covered native `I32` path also lowers minimal
    `@export("symbol", abi: C)` functions as real LLVM C ABI thunks. The
    source function remains available for same-package calls, and the exported
    thunk calls it under the requested symbol, e.g. `define i32 @zeno_add(...)`
    calling `@add(...)`. Covered thunks can now carry all-`I32`
    `@layout(C)` struct parameters as LLVM named aggregates, e.g.
    `define i32 @zeno_consume_pair(%ExportPair %pair)` calling
    `@exportedConsumePair(%ExportPair %pair)`; the build-artifact runner can
    assert required or forbidden byte strings in the native object, so this path is checked
    against object contents rather than only `.zmeta` comments;
  - the same native `I32` path now lowers simple `trust extern "C"` functions
    with `I32` parameters/return as real LLVM declarations and can return a
    direct call from a `trust { ... }` block, producing object files with
    defined wrapper functions and an unresolved external C symbol for the
    linker to satisfy;
  - applications whose target matches the current host produce a real native
    executable at `bin/<package-name>`; cross-target applications stop at the
    real object artifact instead of emitting a fake launcher;
  - libraries produce `lib/lib<package-name>.a` as a deterministic ar archive
    with a real `zeno-stage0.o/` native object member plus `.zmeta`;
  - `.zmeta` records package identity, kind, entry, compiler/LLVM identity,
    target/profile, manifest target/profile, builtin core marker, artifact
    format, stable source, AST, HIR, MIR, and LLVM-preview fingerprints, the
    effective local or ancestor `Zeno.lock` root/fingerprint when present, a
    stable build fingerprint over source plus AST/HIR/MIR/LLVM-preview facts,
    lockfile, effective target/profile/allocator/panic/OOM/trust
    /dependency policy, dependency package identity/content facts,
    interface/ABI/trust/cost metadata, Send/Sync facts, and builtin
    declaration package hashes, AST, AST-derived HIR/MIR/LLVM-preview facts
    for top-level declarations plus covered return, local binding, local
    `const`, call statement, assignment, `if`, `while`, `for` with iteration
    mode, statement
    `match` with access mode, match-arm with optional guard, or-pattern and
    range-pattern arm text, if-pattern / while-pattern pattern mode, `break`,
    `continue`, standalone `try`, top-level
    `const` / `static`, type alias, struct field, enum variant payload,
    generic interface and interface/impl method, generic interface impl
    method, impl const, destroy, generic header including nested generic
    function constraints, trust extern declaration, trust block, C ABI export
    metadata, closure expression including move-capturing and block closure
    forms,
    tuple/unit/char/cast/index/type-static-call expression, simple
    binary/comparison expression, try-expression, and match-expression body
    shapes including `match move`,
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
  - `--emit mir` and `--emit llvm-ir` select the intermediate file that
    fixture metadata should inspect; build now writes both intermediates by
    default. The MIR/LLVM files carry package kind, entry, target, source, AST,
    HIR, MIR, and LLVM-preview fingerprints, AST-derived pipeline facts,
    declaration/layout/drop/SendSync/interface/ABI/trust/dependency/
    dependency-package/cost/runtime/linked-runtime fingerprints, and build
    fingerprint; `--emit mir` now also includes the LLVM-preview node set and
    the full `source->ast->hir->mir->llvm-ir->object->artifact` pipeline marker
    so the debug artifact visibly carries the end-to-end chain;
    the build-artifact verifier now exercises requested emit
    files for the base application and library artifact fixtures.
  - primitive `I8`, `I16`, `I64`, `ISize`, `U8`, `U16`, `U32`, `U64`,
    `USize`, `F32`, and `F64` now participate in the covered native scalar
    path: annotated integer locals and integer literals typed by context lower
    as LLVM `i8`, `i16`, `i32`, or `i64`, signed integer scalars use `sdiv`
    and `icmp s*`, unsigned integer scalars use `udiv` and `icmp u*`, while
    annotated float locals and float literal arguments are lowered as LLVM
    `float` / `double` with `fadd` / `fsub` / `fmul` / `fdiv` and ordered
    `fcmp`; `Bool` condition calls preserve the scalar type in their LLVM call
    signatures. The concentrated `I8`, `I16`, `I64`, `ISize`, `U8`, `U16`,
    `U32`, `U64`, `USize`, `F32`, and `F64` fixtures produce real IR, native
    object files, host executables, and runner/direct-checked exit codes.

## Preview-Backed Gaps

The runner now treats `expected-error` comments as expectations only; it no
longer injects diagnostics from those comments for source, manifest, module,
package, codegen, or incremental fail cases. Ordinary `zeno check` reports only
diagnostics produced by implemented stage0 logic, and `case.toml` codegen /
incremental cases use preview fact/plan verifiers. Those checks are still
lightweight: many facts do not yet come from real HIR/MIR/LLVM artifacts or a
persistent incremental cache.

The following V01 areas are still preview-backed or placeholder-backed today:

- Full parser/AST construction is now started but not complete: stage0 has a
  token-cursor syntax spine for the covered syntax surface, including
  item/block/field/variant/generic-param/param/type/pattern/argument/statement/
  expression node facts, emitted as `syntax.*` facts alongside the legacy
  top-level/body AST node parser used by module symbols, declaration
  summaries, artifact metadata, cache keys, and AST-derived HIR/MIR/LLVM
  preview facts. Expression facts in the syntax spine are now classified
  directly from token ranges rather than by joining source text back through
  the legacy expression fallback; generic argument type/value classification,
  field-access bases, method-call receivers, and top-level / impl `const` /
  `static` initializers now carry token-derived expression facts on
  `syntax.*` nodes before they are bridged into AST-derived HIR/MIR facts.
  AST expression nodes now also preserve a token-derived `exprFact` field for
  calls, names, literals, access expressions, casts, prefix/binary/range
  expressions, closures, control expressions, and match expressions; MIR
  operand/rvalue facts consume that field first, with the older node fields
  only used as compatibility fallbacks inside stage0. Expression AST nodes now
  also have a structured field table for call callees, field/method receivers,
  binary/range operands, casts, type-static calls, and enum constructors; the
  corresponding HIR/MIR facts consume those fields before falling back to the
  older generic/params/returnType slots. Statement and
  control-flow AST nodes now carry the same token-derived `exprFact` for
  return/local/const/assign/pattern bindings, if/while/for/match/try/break,
  and expression statements, and their MIR expression facts consume that field
  before falling back to legacy parameter fields. Statement AST nodes now also
  carry structured fields for local names, declared types, return expectations,
  assignment targets/operators, for-loop items/iterables, and statement-order
  subject/expr data; statement HIR/MIR lowering consumes these fields before
  falling back to the older slots. Pattern/control AST nodes now
  also carry structured `pattern`, `scrutinee`, and `guard` fields; pattern
  locals, if-pattern / while-pattern, statement and expression match arms, and
  pattern-condition HIR/MIR facts consume those AST fields before falling back
  to older node-name or parameter-derived facts. Declaration/type/generic/
  parameter AST nodes now also carry structured `kind`, `generic`, `params`,
  `return`, `layout`, `abi`, `target`, `parent`, `text`, `name`, `type`,
  `mode`, and `index` fields where applicable; declaration collection,
  overload/interface/layout/generic indexes, and HIR/MIR facts consume those
  fields first for function signatures, extern ABI declarations, type aliases,
  const generic params, generic arguments, path/type refs, and value params.
  The native LLVM subset now also reads those structured fields directly for
  decl fields, top-level const/static materialization, function/extern
  signatures, C ABI extern declarations, generic payload instance discovery,
  native parameter tables, and aggregate-return callee discovery. The first
  native body lowering fast paths are now AST-field driven for the covered
  `nativeApp.addIfPositive` if-return shape, `nativeApp.sumTo` var/while/assign
  loop shape, `nativeIfElseApp.choosePositive` if/else-return shape, and
  no-payload enum `nativeEnumMatchApp.chooseMode` return shape, payload enum
  `return match` shapes for `nativeEnumMatchApp.matchMaybe` / `matchJob` /
  `matchTask`, payload enum if-pattern return shapes for
  `nativeEnumMatchApp.unwrapMaybe` / `unwrapJob`, payload enum while-pattern
  return/assign shapes for `nativeEnumMatchApp.firstKey` / `sumKeys` /
  `sumMaybe`, and `Result<I32, I32>` constructor, try-binding, standalone
  try, and return-match shapes for `nativeTryResultApp`, plus straight-line
  `nativeApp.main`/`nativeIfElseApp.main` shapes (`val` call bindings followed
  by `return`); their
  `.zmeta` records `nativeBodySources = ["addIfPositive:ast-fields-if-return",
  "main:ast-fields", "sumTo:ast-fields-while-assign"]` for 020 and
  `nativeBodySources = ["choosePositive:ast-fields-if-else-return",
  "main:ast-fields"]` for 023, and `nativeBodySources =
  ["chooseMode:ast-fields-enum-return",
  "firstKey:ast-fields-payload-while-return",
  "matchJob:ast-fields-payload-return-match",
  "matchMaybe:ast-fields-payload-return-match",
  "matchTask:ast-fields-payload-return-match",
  "sumKeys:ast-fields-payload-while-assign",
  "sumMaybe:ast-fields-payload-while-assign",
  "unwrapJob:ast-fields-payload-if-else-return",
  "unwrapMaybe:ast-fields-payload-if-else-return"]` for 025, and
  `nativeBodySources = ["checkErr:ast-fields-result-try-stmt",
  "checkOk:ast-fields-result-try-stmt",
  "errValue:ast-fields-payload-constructor-return",
  "fail:ast-fields-result-try-binding",
  "okValue:ast-fields-payload-constructor-return",
  "step:ast-fields-result-try-binding",
  "unwrapOrCode:ast-fields-payload-return-match"]` for 026 while the emitted
  LLVM IR still builds real host executables whose runners verify
  `run-exit-code: 7` / `114` / `34`.
  HIR/MIR/LLVM lowering still mostly derives from the legacy
  `ParsedAstNode` facts rather than the new syntax spine. Full syntax-tree
  ownership, parse errors/recovery, scope-aware name
  binding, CFG-shaped MIR, MIR verification, pattern binding in match arms,
  monomorphization, and LLVM lowering remain preview-backed beyond the minimal
  native `I32`
  parameter/local/binary-expression/direct-call, if-return, if/else-return,
  var/while-loop, simple `Bool`/`i1` condition-call path, all-`I32` struct
  aggregate path, no-payload enum switch path, first all-`I32` tuple/record
  payload enum aggregate path, first mixed primitive scalar struct and payload
  enum aggregate paths, and covered `Result<I32, I32>` try-binding
  branch/early-return path, plus the covered `Char`, `I8`, `I16`, `I64`,
  `ISize`, `U8`, `U16`, `U32`, `U64`, `USize`, `F32`, and `F64` native scalar
  paths. The
  next implementation direction is to make the V01 syntax surface complete in
  AST artifacts first, then derive HIR/MIR/LLVM facts from those nodes instead
  of adding more source-text-only scans.
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
- Real MIR, MIR verifier, monomorphization, full LLVM lowering, full linker
  integration, runtime shim linking, and performance gates beyond the covered
  native build-artifact paths and first `case.toml` preview fact checks.
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
  -> zeno test: 146 passed, 0 failed (stage mvp)
build/stage0/zeno test --feature diagnostic-order
  -> zeno test: 1 passed, 0 failed
build/stage0/zeno test --feature build-artifact
  -> zeno test: 34 passed, 0 failed; artifact fixtures assert top-level AST node facts plus token-cursor syntax spine facts for file/item/block/field/variant/generic-param/param/type/pattern/argument/statement/expression structure, covered function-body return/local-binding/pattern-binding/call-statement/assignment/if/else/if-pattern/while/while-pattern/for/match/match-arm/try/local-const/break/continue AST and HIR/MIR/LLVM-preview facts, top-level const/static/type-alias/field/enum-variant/interface/impl/destroy AST and declaration facts, struct-literal/method-call/closure/try/match/tuple/unit/char/cast/index/type-static-call expression facts, simple binary/comparison-expression facts including top-level binary calls, ast/hir/mir/llvm cache inputs, brace-aware top-level declaration collection, declaration stableNodeId/module/visibility/span facts, declaration/runtime/layout/drop/SendSync/interface/ABI/trust/dependency/dependency-package/cost/linked-runtime fingerprints, cacheKeyInputs, emitted MIR/LLVM preview facts, real LLVM IR for the minimal I32 parameter/local/binary-expression/direct-call/if-return/if-else-return/while-loop native path plus simple Bool/i1 functions and Bool-call branch conditions, Char parameter/local/literal/comparison lowering as internal i32 scalars, I8/I16 parameter/local/literal/arithmetic/comparison lowering as internal i8/i16 scalars with signed divide and signed comparisons, U8/U16 parameter/local/literal/arithmetic/comparison lowering as internal i8/i16 scalars with unsigned divide and unsigned comparisons, I64/ISize parameter/local/literal/arithmetic/comparison lowering as internal i64 scalars with signed divide and signed comparisons, U32 parameter/local/literal/arithmetic/comparison lowering as internal i32 scalars with unsigned divide and unsigned comparisons, U64/USize parameter/local/literal/arithmetic/comparison lowering as internal i64 scalars with unsigned divide and unsigned comparisons, F32 parameter/local/literal/arithmetic/comparison lowering as internal float scalars, F64 parameter/local/literal/arithmetic/comparison lowering as internal double scalars, simple all-I32 struct aggregate `insertvalue` / `extractvalue` lowering plus mixed primitive scalar struct aggregate lowering as LLVM `{ i8, i16, i64, float, double }`, no-payload, all-I32 payload, first mixed primitive scalar payload enum discriminants, and generic mixed-scalar `Maybe<Header>` payload instantiations lowered through ordered LLVM branch chains including guarded, range, or-pattern, unit payload-enum arms, mixed scalar payload slot extraction/reconstruction, generic `Maybe<I32>` / `Maybe<Job>` / `Maybe<Task>` aggregate constructors, generic payload match expressions, `Maybe<I32>` while-pattern loops, `Result<I32, I32>` try-binding plus standalone-try early-return branches, and I32 range `for` loops with real loop headers/steps/backedges, C ABI export thunks carrying all-I32 `@layout(C)` struct parameters as LLVM aggregates, real native object files, host-native executable output plus runner-checked `run-exit-code`, hosted profile with empty linkRuntimeNeeds, reachable Thread.spawn with linkRuntimeNeeds = ["thread"], reachable allocation/panic/OOM with linkRuntimeNeeds = ["allocator", "oom", "panic"], C ABI exports with linkRuntimeNeeds = ["c-abi-boundary"], C bridge exports recorded as bridge=C, trust report capability metadata, trusted and automatic Send/Sync metadata, interface/impl/static-return metadata, transitive path dependency package fingerprints plus dependency runtime propagation, and layout/drop glue cache metadata
build/stage0/zeno test --stage mvp --feature build-artifact
  -> zeno test: 21 passed, 0 failed (stage mvp)
build/stage0/zeno test --stage full-spec --feature build-artifact
  -> zeno test: 34 passed, 0 failed
build/stage0/zeno test --feature nope
  -> zeno test: 0 passed, 0 failed (no tests selected)
build/stage0/zeno build tests/spec/package-pass/004_application_artifact --emit mir
  -> target/.../mir/artifactApp.mir records declarationFingerprint, buildFingerprint, source->ast->hir->mir->llvm-ir->object->artifact pipeline facts, and `return 0` AST/HIR/MIR/LLVM-preview facts; target/.../ir/artifactApp.ll contains `define i32 @main()` and target/.../obj/artifactApp.o is an ELF x86-64 relocatable object; cross-target application finalArtifact is the native object rather than a fake executable
build/stage0/zeno build tests/spec/package-pass/005_library_artifact --emit llvm-ir
  -> target/.../ir/artifactLib.ll records declarationFingerprint and buildFingerprint; obj/artifactLib.o is a native object and lib/libartifactLib.a is a deterministic ar archive with zeno-stage0.o/ native object member and .zmeta archiveFormat = "ar-native-object"
build/stage0/zeno build tests/spec/package-pass/006_hosted_no_hidden_runtime --emit mir
  -> target/.../mir/hostedQuiet.mir records runtimeFingerprint and buildFingerprint
build/stage0/zeno build tests/spec/package-pass/007_hosted_thread_runtime --emit llvm-ir
  -> target/.../ir/hostedThread.ll records runtimeFingerprint and buildFingerprint; .zmeta records runtimeNeeds/linkRuntimeNeeds = ["thread"]
build/stage0/zeno build tests/spec/package-pass/008_hosted_allocator_panic_oom_runtime --emit mir
  -> target/.../mir/hostedRuntime.mir records runtimeFingerprint and buildFingerprint; .zmeta records runtimeNeeds/linkRuntimeNeeds = ["allocator", "oom", "panic"]
build/stage0/zeno build tests/spec/package-pass/009_c_abi_export_runtime --emit llvm-ir
  -> target/.../ir/cAbiRuntime.ll records runtimeFingerprint and buildFingerprint; .zmeta records exports and runtimeNeeds/linkRuntimeNeeds = ["c-abi-boundary"]; the lowerable `@export("zeno_add", abi: C)` function emits `define i32 @add(i32 %a, i32 %b)`, `define i32 @zeno_add(i32 %a, i32 %b)`, and `%ret = call i32 @add(i32 %a, i32 %b)`; target/.../obj/cAbiRuntime.o is an ELF x86-64 relocatable object whose symbol table contains `T zeno_add`, and the fixture now checks the native object bytes contain `zeno_add`
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
build/stage0/zeno build tests/spec/package-pass/020_native_application_artifact --emit llvm-ir
  -> target/aarch64-apple-darwin/hosted/ir/nativeApp.ll contains real LLVM IR for `addIfPositive(value: I32, delta: I32) -> I32`, `if (value > 0) { return value + delta }`, `sumTo(limit: I32)`, `var i`, `var acc`, `while (i < limit)`, and `val result = addIfPositive(sumTo(4), 1)`, including `define i32 @addIfPositive(i32 %value, i32 %delta)`, `%t0 = icmp sgt i32 %value, 0`, `br i1 %t0, label %if.then.1, label %if.cont.1`, `define i32 @sumTo(i32 %limit)`, `%i.addr = alloca i32`, `while.cond.0:`, `br i1 %t2, label %while.body.0, label %while.exit.0`, `store i32 %t5, ptr %acc.addr`, `br label %while.cond.0`, `%t0 = call i32 @sumTo(i32 4)`, and `%t1 = call i32 @addIfPositive(i32 %total, i32 1)`; .zmeta records `nativeBodySources = ["addIfPositive:ast-fields-if-return", "main:ast-fields", "sumTo:ast-fields-while-assign"]` for the AST-field-driven if-return, while/assign loop, and straight-line bodies, and .zmeta/.mir also record `stmt.while`, `stmt.assign`, `hir.while`, `hir.assign`, `mir.loop`, `mir.branch kind=while`, `mir.store`, `llvm.loop`, and `llvm.store ... source=mir.store`; target/.../obj/nativeApp.o is `Mach-O 64-bit object arm64`; target/.../bin/nativeApp is `Mach-O 64-bit executable arm64`; `run-exit-code: 7` makes the build-artifact runner execute the binary and verify exit code 7
build/stage0/zeno build tests/spec/package-pass/021_native_bool_condition_artifact --emit llvm-ir
  -> target/aarch64-apple-darwin/hosted/ir/nativeBoolApp.ll contains real LLVM IR for `isPositive(value: I32) -> Bool`, `pick(value: I32) -> I32`, and `main() -> I32`, and `.zmeta` records `nativeBodySources = ["isPositive:ast-fields-bool-return", "main:ast-fields", "pick:ast-fields-scalar-if-return"]` for the AST-field-driven Bool return, scalar if-return, and straight-line bodies, including `define i1 @isPositive(i32 %value)`, `%t0 = icmp sgt i32 %value, 0`, `ret i1 %t0`, `define i32 @pick(i32 %value)`, `%t0 = call i1 @isPositive(i32 %value)`, `br i1 %t0, label %if.then.1, label %if.cont.1`, and `%t0 = call i32 @pick(i32 3)`; target/.../obj/nativeBoolApp.o is `Mach-O 64-bit object arm64`; target/.../bin/nativeBoolApp is `Mach-O 64-bit executable arm64`; executing it exits with code 7 and the build-artifact runner verifies `run-exit-code: 7`
build/stage0/zeno build tests/spec/package-pass/022_ast_syntax_surface_artifact --emit mir
  -> target/.../meta/astSyntaxSurface.zmeta and target/.../mir/astSyntaxSurface.mir record token-cursor `syntax.*` spine facts for file/item/block/field/variant/generic-param/param/type/pattern/argument/statement/expression structure, including `fn sumValues`, its body block, struct fields, enum variants and payload types, const generic parameters, generic interface bounds, receiver/move parameters, local type annotations, call arguments, `for value in values`, `if (maybe is Some(value))`, `while (nextMaybe(total) is Some(value))`, guarded return-position match arms, or-pattern/range-pattern facts, and `return a + b`, plus AST expression `exprFact` payloads such as `expr.call ... exprFact=call:parseDigit`, `expr.name ... exprFact=name:LIMIT`, `expr.try ... exprFact=try:call:parseDigit`, `expr.access ... exprFact=mut:method-call:consume`, structured expression fields such as `expr.call ... calleeFact=parseDigit`, `expr.method-call ... receiverFact=counter methodFact=consume`, `expr.field-access ... baseFact=counter fieldFact=current`, `expr.binary ... leftFact=name:a rightFact=name:b`, and `expr.cast ... sourceExprFact=int-literal:1 targetTypeFact=U32`, statement `exprFact` payloads such as `stmt.const ... exprFact=name:LIMIT`, `stmt.for ... exprFact=name:values`, `stmt.val ... exprFact=try:call:parseDigit`, `stmt.assign ... exprFact=call:mutateValue`, and `stmt.expr ... exprFact=mut:method-call:consume`, structured statement fields such as `stmt.val ... nameFact=parsed`, `stmt.const ... typeFact=U32`, `stmt.for ... itemFact=value iterableFact=name:values`, `stmt.assign ... targetFact=total opFact==`, and `stmt.return ... expectedFact=I32`, structured pattern/control payloads such as `stmt.if-pattern ... patternFact=Some(value) scrutineeFact=name:maybe`, `stmt.while-pattern ... patternFact=Some(value) scrutineeFact=call:nextMaybe`, `stmt.match-arm ... patternFact=Some(value) guardFact=compare:>`, field/method receiver facts, declaration/type/generic/parameter field facts such as `decl.fn exportedAdd ... paramsFact=a: I32, b: I32 returnFact=I32`, `decl.type ByteBuffer ... targetFact=Buffer<U8, 4>`, `generic-param.const Buffer.const Capacity: USize ... nameFact=Capacity typeFact=USize`, `generic-arg.expr ByteBuffer.alias-target.1 ... textFact=4 parentFact=Buffer<U8, 4>`, `param.value accessModes.value ... indexFact=1 typeFact=U32`, `type.path-generic staticConsumer.generic-bound ... textFact=Consumer<U32>`, and HIR/MIR consumers such as `hir.fn-signature exportedAdd ... return=I32`, `hir.type-alias ByteBuffer target=Buffer<U8, 4>`, `mir.type-alias ByteBuffer target=Buffer<U8, 4>`, and `mir.generic-input Buffer params=<T, const Capacity: USize>`; the same fixture also covers top-level `const LIMIT`, `static Seed`, enum variant payload `Code(U32)`, `struct Point: Copy`, `@layout(Source) struct Buffer<T, const Capacity: USize>`, struct fields, `type ByteBuffer = Buffer<U8, 4>`, `interface Writer: Send, Sync`, interface method signatures, `impl Writer for Buffer<U8, 4>`, impl consts, impl methods, `destroy`, generic `interface Consumer<T>`, `impl Consumer<U32> for Counter`, `fn genericIdentity<T: Copy>`, nested generic constraint `fn staticConsumer<T: Consumer<U32>>`, `trust extern "C" fn hostRead`, `return trust { ... }`, `@export("zeno_surface_add", abi: C) pub fn exportedAdd`, local `const`, `static NativeSeed`, `fn nativeStaticAdd`, struct/tuple/unit/char/cast/index/type-static-call expressions, statement and expression `match`, `try`, closures, access modes, HIR/MIR/LLVM-preview facts, real LLVM declarations/definitions for the native subset, and a native object whose symbol table contains `NativeSeed`, `nativeConstAdd`, `nativeStaticAdd`, `trustedHostRead`, `zeno_surface_add`, and unresolved external `hostRead`
build/stage0/zeno build tests/spec/package-pass/023_native_if_else_artifact --emit llvm-ir
  -> target/aarch64-apple-darwin/hosted/ir/nativeIfElseApp.ll contains real LLVM IR for `choosePositive(value: I32) -> I32`, including `%t0 = icmp sgt i32 %value, 0`, `br i1 %t0, label %if.then.1, label %if.cont.1`, `if.then.1:`, `ret i32 %value`, `if.cont.1:`, and `ret i32 7`; .zmeta records `nativeBodySources = ["choosePositive:ast-fields-if-else-return", "main:ast-fields"]` for the AST-field-driven if/else-return and straight-line bodies; target/.../obj/nativeIfElseApp.o is a native object, target/.../bin/nativeIfElseApp is a host executable, and the build-artifact runner executes it and verifies `run-exit-code: 7`
build/stage0/zeno build tests/spec/package-pass/024_native_struct_artifact --emit llvm-ir
  -> target/aarch64-apple-darwin/hosted/ir/nativeStructApp.ll is now generated through AST-field aggregate lowering (`nativeBodySources = ["consumePair:ast-fields-aggregate-i32", "exportedConsumePair:ast-fields-aggregate-i32", "identityPair:ast-fields-aggregate-return", "main:ast-fields-aggregate-i32", "makePair:ast-fields-aggregate-return", "sumPair:ast-fields-aggregate-i32"]`) for `struct Pair { left: I32, right: I32 }`, `fn makePair(a: I32, b: I32) -> Pair`, `val pair = makePair(a, b)`, `fn identityPair(pair: Pair) -> Pair`, `val normalized = identityPair(pair)`, `fn consumePair(pair: Pair) -> I32`, `@layout(C) pub struct ExportPair { left: I32, right: I32 }`, and `@export("zeno_consume_pair", abi: C) pub fn exportedConsumePair(pair: ExportPair) -> I32`, including `%Pair = type { i32, i32 }`, `%ExportPair = type { i32, i32 }`, `define %Pair @makePair(i32 %a, i32 %b)`, `%t0 = insertvalue %Pair undef, i32 %a, 0`, `%t1 = insertvalue %Pair %t0, i32 %b, 1`, `ret %Pair %t1`, `define %Pair @identityPair(%Pair %pair)`, `ret %Pair %pair`, `define i32 @consumePair(%Pair %pair)`, `%t0 = extractvalue %Pair %pair, 0`, `%t1 = extractvalue %Pair %pair, 1`, `%t2 = add i32 %t0, %t1`, `%t0 = call %Pair @makePair(i32 %a, i32 %b)`, `%t1 = call %Pair @identityPair(%Pair %t0)`, `%t2 = call i32 @consumePair(%Pair %t1)`, `%t0 = call i32 @sumPair(i32 3, i32 4)`, `define i32 @exportedConsumePair(%ExportPair %pair)`, `extractvalue %ExportPair %pair`, `define i32 @zeno_consume_pair(%ExportPair %pair)`, and `%ret = call i32 @exportedConsumePair(%ExportPair %pair)`; target/.../obj/nativeStructApp.o contains `_consumePair` / `_exportedConsumePair` / `_identityPair` / `_makePair` / `_sumPair` / `_zeno_consume_pair` / `_main`, target/.../bin/nativeStructApp is a host executable, and the build-artifact runner executes it and verifies `run-exit-code: 7`
build/stage0/zeno build tests/spec/package-pass/025_native_enum_match_artifact --emit llvm-ir
  -> target/aarch64-apple-darwin/hosted/ir/nativeEnumMatchApp.ll contains real LLVM lowering for no-payload enum discriminants plus tuple/record all-I32 payload enum ABI paths: `.zmeta` now records expanded AST-field sources for enum returns, payload constructors, payload match/if-pattern, and payload while lowering; `define i32 @chooseMode()` returns `ret i32 1`, `define i1 @isHot(i32 %mode)` emits `%t0 = icmp eq i32 %mode, 1`, `define i32 @score(i32 %mode)` branches over `Mode` discriminants, `%WorkState = type { i32, i32 }`, `%Event = type { i32, i32, i32 }`, `%Maybe_I32 = type { i32, i32 }`, `%Maybe_Job = type { i32, i32 }`, and `%Maybe_Task = type { i32, i32, i32 }`; `define %WorkState @makeReady(i32 %priority)` constructs `WorkState.Ready(Job)` with `insertvalue`, `define %Maybe_I32 @makeMaybe(i32 %value)` constructs `Maybe.Some(value)`, `define %Maybe_I32 @makeNone()` constructs `Maybe.None`, `define i32 @unwrapMaybe(%Maybe_I32 %maybe)` extracts the generic enum tag and payload for `if (maybe is Maybe.Some(value))`, `define %Maybe_Job @makeMaybeJob(i32 %priority)` constructs `Maybe.Some(Job { priority })`, `define %Maybe_Job @makeNoJob()` constructs `Maybe.None`, `define i32 @unwrapJob(%Maybe_Job %maybe)` extracts the tag, reconstructs `Job`, and reads `job.priority`, `define %Maybe_Task @makeMaybeTask(i32 %priority, i32 %cost)` constructs a two-slot `Maybe.Some(Task { priority, cost })`, `define %Maybe_Task @makeNoTask()` constructs `Maybe.None`, `define i32 @matchMaybe(%Maybe_I32 %maybe)` lowers `return match maybe` over `Maybe.Some(value)` / `Maybe.None`, `define i32 @matchJob(%Maybe_Job %maybe)` lowers `return match maybe` over `Maybe.Some(job)` / `Maybe.None`, and `define i32 @matchTask(%Maybe_Task %maybe)` reconstructs `Task`, reads `task.priority` and `task.cost`, and returns their sum; `define %Maybe_I32 @maybeUntil(i32 %limit, i32 %current)` lowers `if (current < limit)` to `icmp slt`, `br i1`, `Maybe.Some(current)`, and `Maybe.None`, while `define i32 @sumMaybe(i32 %limit)` lowers `while (maybeUntil(limit, i) is Maybe.Some(value))` to an aggregate-returning scrutinee call in the loop header, generic enum tag extraction, payload binding, `store` updates to `%acc.addr` / `%i.addr`, and a loop back-edge before returning `acc`; `define %Event @makeKey(i32 %code)` constructs `Event.Key` with tag 0 plus payload slot 1, `define %Event @makeCode(i32 %code)` constructs `Event.Code` with tag 1 plus payload slot 1, `define %Event @makePair(i32 %left, i32 %right)` constructs record-shaped `Event.Pair` with tag 2 plus payload slots 1 and 2, and `define %Event @makeFinished()` constructs the unit `Event.Finished` tag. `define i32 @eventCode(%Event %event)` extracts the tag with `extractvalue`, lowers the guarded `Event.Key(code) if code < 10` arm, closed range `Event.Code(10..=20)` arm, `Event.Key(code) | Event.Code(code)` or-pattern arm, record payload arm, and unit arm to ordered LLVM `icmp` / `br` blocks before returning native `I32`; `define i32 @bump(%WorkState %state)` lowers `if (state is mut WorkState.Ready(job))` to a tag test, reconstructs the `Job` payload, reads `job.priority`, and returns `job.priority + 1` or `0`; `define i32 @firstKey(i32 %code)` lowers `while (makeKey(code) is Event.Key(value))` to a native loop; `define %Event @keyUntil(i32 %limit, i32 %current)` lowers an `if (current < limit)` aggregate return path to `icmp slt`, `br i1`, `Event.Key(current)`, and `Event.Finished`; `define i32 @sumKeys(i32 %limit)` lowers `while (keyUntil(limit, i) is Event.Key(value))` to an aggregate-returning scrutinee call in the loop header, tag extraction, payload binding, `store` updates to `%acc.addr` / `%i.addr`, and a loop back-edge before returning `acc`; `main` calls `@makeReady`, `@makeMaybe`, `@makeNone`, `@makeMaybeJob`, `@makeNoJob`, `@makeMaybeTask`, `@makeNoTask`, `@unwrapMaybe`, `@unwrapJob`, `@matchMaybe`, `@matchJob`, `@matchTask`, `@firstKey`, `@sumKeys`, `@sumMaybe`, and `@eventCode` for each event value, target/.../obj/nativeEnumMatchApp.o contains `_chooseMode` / `_isHot` / `_makeReady` / `_makeMaybe` / `_makeNone` / `_makeMaybeJob` / `_makeNoJob` / `_makeMaybeTask` / `_makeNoTask` / `_unwrapMaybe` / `_unwrapJob` / `_matchMaybe` / `_matchJob` / `_matchTask` / `_firstKey` / `_keyUntil` / `_sumKeys` / `_maybeUntil` / `_sumMaybe` / `_eventCode` / `_main`, target/.../bin/nativeEnumMatchApp is a host executable, and the build-artifact runner executes it and verifies `run-exit-code: 114`
build/stage0/zeno build tests/spec/package-pass/026_native_try_result_artifact --emit llvm-ir
  -> target/aarch64-apple-darwin/hosted/ir/nativeTryResultApp.ll contains real LLVM lowering for `Result<I32, I32>`: `.zmeta` records `nativeBodySources = ["checkErr:ast-fields-result-try-stmt", "checkOk:ast-fields-result-try-stmt", "errValue:ast-fields-payload-constructor-return", "fail:ast-fields-result-try-binding", "okValue:ast-fields-payload-constructor-return", "step:ast-fields-result-try-binding", "unwrapOrCode:ast-fields-payload-return-match"]`; `%Result_I32_I32 = type { i32, i32 }`, `define %Result_I32_I32 @okValue(i32 %value)` builds `Result.Ok` with tag 0, `define %Result_I32_I32 @errValue(i32 %code)` builds `Result.Err` with tag 1, `define %Result_I32_I32 @step(i32 %value)` lowers two `val ... = try okValue(...)` bindings to aggregate-returning calls, tag `extractvalue`, `icmp eq ... 1`, `br i1` to `try.err.*` / `try.ok.*`, direct Err aggregate `ret`, Ok payload extraction, and final `Result.Ok`; `define %Result_I32_I32 @fail(i32 %value)` lowers the second `try errValue(...)` to an early `ret %Result_I32_I32 %t6`; `define %Result_I32_I32 @checkOk(i32 %value)` lowers standalone `try okValue(value);` to Err early-return and Ok continuation before returning `Result.Ok(value + 2)`, `define %Result_I32_I32 @checkErr(i32 %value)` lowers standalone `try errValue(value + 9);` to an early `ret %Result_I32_I32 %t1`; `define i32 @unwrapOrCode(%Result_I32_I32 %result)` lowers `return match result` over `Result.Ok(value)` / `Result.Err(code)` to ordered tag branches and payload extraction; `main` calls `@step`, `@fail`, `@checkOk`, `@checkErr`, and `@unwrapOrCode`, target/.../obj/nativeTryResultApp.o contains `_okValue` / `_errValue` / `_step` / `_fail` / `_checkOk` / `_checkErr` / `_unwrapOrCode` / `_main`, target/.../bin/nativeTryResultApp is a host executable, and the build-artifact runner executes it and verifies `run-exit-code: 34`
build/stage0/zeno build tests/spec/package-pass/027_native_for_range_artifact --emit llvm-ir
  -> target/aarch64-apple-darwin/hosted/ir/nativeForRangeApp.ll contains real LLVM lowering for I32 range `for` loops and loop control, and `.zmeta` records `nativeBodySources = ["emptyHalfOpen:ast-fields-for-range-assign", "main:ast-fields", "singleClosed:ast-fields-for-range-assign", "sumClosed:ast-fields-for-range-assign", "sumForSkipBreak:ast-fields-for-range-control", "sumHalfOpen:ast-fields-for-range-assign", "sumWhileSkipBreak:ast-fields-while-control"]`: `define i32 @sumHalfOpen(i32 %limit)` lowers `for i in 0..limit` from AST fields to `%i.range.0.addr = alloca i32`, `store i32 0`, `for.cond.0:`, `load`, `icmp slt`, `br i1` to `for.body.0` / `for.exit.0`, body accumulator `store`, `for.step.0:`, increment, and a back-edge to `for.cond.0`; `define i32 @sumClosed(i32 %start, i32 %end)` lowers `for i in start..=end` with `icmp sle`; `define i32 @emptyHalfOpen(i32 %limit)` verifies an empty half-open range path, `define i32 @singleClosed(i32 %value)` verifies a one-element closed range path, and `define i32 @sumForSkipBreak(i32 %limit)` lowers AST `stmt.if` + `stmt.continue` + `stmt.break` inside a `stmt.for` range to real `if.then.*`, `if.cont.*`, `for.step.0`, and `for.exit.0` branches; `main` is rebuilt from AST call/binary fields after fixing argument facts to be emitted by call nodes instead of a whole-expression parenthesis scan. `define i32 @sumWhileSkipBreak(i32 %limit)` now lowers AST `stmt.while`, loop-body `stmt.assign`, `stmt.if`, `stmt.continue`, `stmt.break`, and final accumulator assignment to real `while.cond.0`, `while.body.0`, `if.then.*`, `if.cont.*`, and `while.exit.0` branches; target/.../obj/nativeForRangeApp.o contains `_sumHalfOpen` / `_sumClosed` / `_emptyHalfOpen` / `_singleClosed` / `_sumForSkipBreak` / `_sumWhileSkipBreak` / `_main`, target/.../bin/nativeForRangeApp is a host executable, and the build-artifact runner executes it and verifies `run-exit-code: 38`
build/stage0/zeno build tests/spec/package-pass/028_native_char_artifact --emit llvm-ir
  -> target/aarch64-apple-darwin/hosted/ir/nativeCharApp.ll contains AST-field-driven real LLVM lowering for `Char` as an internal i32 scalar, and `.zmeta` records `nativeBodySources = ["isAsciiA:ast-fields-bool-return", "main:ast-fields", "score:ast-fields-scalar-if-return"]`; `expr.literal` now carries structured `codepoint` fields so `define i1 @isAsciiA(i32 %ch)` compares `%ch` with literal codepoint `65`, `define i32 @score(i32 %ch)` materializes escaped newline as `10`, calls the Bool function, branches on Char equality/inequality, and `define i32 @main()` calls `@score` with local Char values and literal `'z'`; target/.../obj/nativeCharApp.o contains `_isAsciiA` / `_score` / `_main`, target/.../bin/nativeCharApp is a host executable, and direct execution verifies `run-exit-code: 43`
build/stage0/zeno build tests/spec/package-pass/029_native_f64_artifact --emit llvm-ir
  -> target/aarch64-apple-darwin/hosted/ir/nativeF64App.ll contains AST-field-driven real LLVM lowering for `F64` as an internal double scalar, and `.zmeta` records `nativeBodySources = ["classify:ast-fields-scalar-if-return", "isLarge:ast-fields-bool-return", "main:ast-fields"]`: `define i1 @isLarge(double %value)` compares with `fcmp ogt`, `define i32 @classify(double %value)` materializes F64 locals with `fadd`, multiplies with `fmul`, calls the Bool function, and branches on a second floating comparison, while `define i32 @main()` calls `@classify` with local F64 values; target/.../obj/nativeF64App.o contains `_classify` / `_isLarge` / `_main`, target/.../bin/nativeF64App is a host executable, and direct execution verifies `run-exit-code: 42`
build/stage0/zeno build tests/spec/package-pass/030_native_f32_artifact --emit llvm-ir
  -> target/aarch64-apple-darwin/hosted/ir/nativeF32App.ll contains AST-field-driven real LLVM lowering for `F32` as an internal float scalar, and `.zmeta` records `nativeBodySources = ["classify:ast-fields-scalar-if-return", "isWide:ast-fields-bool-return", "main:ast-fields"]`: `define i1 @isWide(float %value)` compares with `fcmp ogt`, `define i32 @classify(float %value)` materializes annotated F32 locals with `fadd`, multiplies with `fmul`, calls the Bool function, and branches on a second floating comparison, while `define i32 @main()` calls `@classify` with local F32 values; target/.../obj/nativeF32App.o contains `_classify` / `_isWide` / `_main`, target/.../bin/nativeF32App is a host executable, and direct execution verifies `run-exit-code: 35`
build/stage0/zeno build tests/spec/package-pass/031_native_u32_artifact --emit llvm-ir
  -> target/aarch64-apple-darwin/hosted/ir/nativeU32App.ll contains AST-field-driven real LLVM lowering for `U32` as an internal i32 scalar with unsigned semantics, and `.zmeta` records `nativeBodySources = ["classify:ast-fields-scalar-if-return", "isLarge:ast-fields-bool-return", "main:ast-fields"]`: `define i1 @isLarge(i32 %value)` compares with `icmp uge`, `define i32 @classify(i32 %value)` materializes annotated U32 locals with `add i32 0`, multiplies with `mul`, divides with `udiv`, calls the Bool function, and branches on `icmp ult`, while `define i32 @main()` calls `@classify` with local U32 values; target/.../obj/nativeU32App.o contains `_classify` / `_isLarge` / `_main`, target/.../bin/nativeU32App is a host executable, and direct execution verifies `run-exit-code: 34`
build/stage0/zeno build tests/spec/package-pass/032_native_u64_artifact --emit llvm-ir
  -> target/aarch64-apple-darwin/hosted/ir/nativeU64App.ll contains AST-field-driven real LLVM lowering for `U64` as an internal i64 scalar with unsigned semantics and a value wider than U32, and `.zmeta` records `nativeBodySources = ["classify:ast-fields-scalar-if-return", "isHuge:ast-fields-bool-return", "main:ast-fields"]`: `define i1 @isHuge(i64 %value)` compares with `icmp uge i64 %value, 4294967296`, `define i32 @classify(i64 %value)` materializes annotated U64 locals with `add i64 0`, multiplies with `mul`, divides with `udiv`, calls the Bool function, and branches on `icmp ult`, while `define i32 @main()` calls `@classify` with local U64 values; target/.../obj/nativeU64App.o contains `_classify` / `_isHuge` / `_main`, target/.../bin/nativeU64App is a host executable, and direct execution verifies `run-exit-code: 35`
build/stage0/zeno build tests/spec/package-pass/033_native_i64_artifact --emit llvm-ir
  -> target/aarch64-apple-darwin/hosted/ir/nativeI64App.ll contains AST-field-driven real LLVM lowering for `I64` as an internal i64 scalar with signed semantics and a value wider than I32, and `.zmeta` records `nativeBodySources = ["classify:ast-fields-scalar-if-return", "isHuge:ast-fields-bool-return", "main:ast-fields"]`: `define i1 @isHuge(i64 %value)` compares with `icmp sge i64 %value, 4294967296`, `define i32 @classify(i64 %value)` materializes annotated I64 locals with `add i64 0`, multiplies with `mul`, divides with `sdiv`, calls the Bool function, and branches on `icmp slt`, while `define i32 @main()` calls `@classify` with local I64 values; target/.../obj/nativeI64App.o contains `_classify` / `_isHuge` / `_main`, target/.../bin/nativeI64App is a host executable, and direct execution verifies `run-exit-code: 36`
build/stage0/zeno build tests/spec/package-pass/034_native_size_artifact --emit llvm-ir
  -> target/aarch64-apple-darwin/hosted/ir/nativeSizeApp.ll contains AST-field-driven real LLVM lowering for pointer-sized integer scalars as i64 on the first 64-bit targets, and `.zmeta` records `nativeBodySources = ["classifyOffset:ast-fields-scalar-if-return", "classifySize:ast-fields-scalar-if-return", "isWideOffset:ast-fields-bool-return", "isWideSize:ast-fields-bool-return", "main:ast-fields"]`: `USize` functions use `udiv i64` and `icmp uge` / `icmp ult`, while `ISize` functions use `sdiv i64` and `icmp sge` / `icmp slt`; the fixture includes `4294967296` typed as both `USize` and `ISize`, target/.../obj/nativeSizeApp.o contains `_classifySize` / `_isWideSize` / `_classifyOffset` / `_isWideOffset` / `_main`, target/.../bin/nativeSizeApp is a host executable, and direct execution verifies `run-exit-code: 85`
build/stage0/zeno build tests/spec/package-pass/035_native_narrow_int_artifact --emit llvm-ir
  -> target/aarch64-apple-darwin/hosted/ir/nativeNarrowIntApp.ll contains AST-field-driven real LLVM lowering for narrow integer scalars, and `.zmeta` records `nativeBodySources = ["classifySigned16:ast-fields-scalar-if-return", "classifySigned8:ast-fields-scalar-if-return", "classifyUnsigned16:ast-fields-scalar-if-return", "classifyUnsigned8:ast-fields-scalar-if-return", "isLargeSigned16:ast-fields-bool-return", "isLargeUnsigned16:ast-fields-bool-return", "isSmallSigned8:ast-fields-bool-return", "isSmallUnsigned8:ast-fields-bool-return", "main:ast-fields"]`: `I8`/`U8` use LLVM `i8`, `I16`/`U16` use LLVM `i16`, signed functions use `sdiv` and `icmp slt` / `icmp sge`, unsigned functions use `udiv` and `icmp ult` / `icmp uge`, and typed literals materialize as `add i8 0, 8` or `add i16 0, 300`; target/.../obj/nativeNarrowIntApp.o contains `_classifySigned8` / `_classifyUnsigned8` / `_classifySigned16` / `_classifyUnsigned16` / `_isSmallSigned8` / `_isSmallUnsigned8` / `_isLargeSigned16` / `_isLargeUnsigned16` / `_main`, target/.../bin/nativeNarrowIntApp is a host executable, and direct execution verifies `run-exit-code: 79`
build/stage0/zeno build tests/spec/package-pass/036_native_mixed_struct_artifact --emit llvm-ir
  -> target/aarch64-apple-darwin/hosted/ir/nativeMixedStructApp.ll is now generated through AST-field aggregate/scalar lowering (`nativeBodySources = ["consume:ast-fields-aggregate-i32", "identity:ast-fields-aggregate-return", "main:ast-fields-aggregate-i32", "makeHeader:ast-fields-aggregate-return", "scoreExact:ast-fields-scalar-if-return", "scoreLength:ast-fields-scalar-if-return", "scoreRatio:ast-fields-scalar-if-return", "scoreTag:ast-fields-scalar-if-return", "scoreWeight:ast-fields-scalar-if-return"]`) for mixed primitive scalar fields: `%Header = type { i8, i16, i64, float, double }`, `define %Header @makeHeader(i8 %tag, i16 %length, i64 %weight, float %ratio, double %exact)` inserts each field with its scalar type, `define i32 @consume(%Header %header)` extracts each field and calls typed helpers as `i8`, `i16`, `i64`, `float`, and `double`, target/.../obj/nativeMixedStructApp.o contains `_makeHeader` / `_identity` / `_consume` / `_scoreTag` / `_scoreLength` / `_scoreWeight` / `_scoreRatio` / `_scoreExact` / `_main`, target/.../bin/nativeMixedStructApp is a host executable, and direct execution verifies `run-exit-code: 49`
build/stage0/zeno build tests/spec/package-pass/037_native_mixed_enum_artifact --emit llvm-ir
  -> target/aarch64-apple-darwin/hosted/ir/nativeMixedEnumApp.ll contains real LLVM aggregate lowering for mixed primitive scalar enum payloads, with AST-field sources now recorded for `consumeHeader`, `consumeReading`, `inspect`, `main`, `makeEmpty`, `makeHeader`, `makeReading`, score helpers, and `unwrapHeader`; `%Header = type { i8, i16, i64, float, double }` and `%Packet = type { i32, i8, i16, i64, float, double }`; `define %Packet @makePacketHeader(i8 %tag, i16 %length, i64 %weight, float %ratio, double %exact)` builds a `Header`, extracts each typed field, and inserts `i8`, `i16`, `i64`, `float`, and `double` payload slots; `define %Packet @makeReading(...)` inserts the same typed record payload slots directly; `define i32 @inspect(%Packet %packet)` lowers `return match packet` to ordered tag branches, reconstructs `%Header` for `Packet.Header(header)`, and calls `@consumeReading(i8, i16, i64, float, double)` for `Packet.Reading`; `define i32 @unwrapHeader(%Packet %packet)` lowers `if (packet is Packet.Header(header))` to a tag test and typed payload reconstruction; numeric equality remains `==` and emits `icmp eq`, while `is` is only used for pattern matching. Remaining AST-source gap: `makePacketHeader` is still the tuple payload wrapper to move fully onto the AST helper. target/.../obj/nativeMixedEnumApp.o contains `_makePacketHeader` / `_makeReading` / `_makeEmpty` / `_inspect` / `_unwrapHeader` / `_consumeHeader` / `_consumeReading` / `_main`, target/.../bin/nativeMixedEnumApp is a host executable, and direct execution verifies `run-exit-code: 147`
build/stage0/zeno build tests/spec/package-pass/038_native_generic_mixed_enum_artifact --emit llvm-ir
  -> target/aarch64-apple-darwin/hosted/ir/nativeGenericMixedEnumApp.ll contains real LLVM monomorphization and aggregate lowering for `Maybe<Header>`, with AST-field sources now recorded for `consume`, `inspect`, `main`, `makeHeader`, `makeNone`, score helpers, and `unwrap`; `%Maybe_Header = type { i32, i8, i16, i64, float, double }`; `define %Maybe_Header @makeSome(i8 %tag, i16 %length, i64 %weight, float %ratio, double %exact)` calls `@makeHeader`, extracts each mixed scalar field, and inserts typed payload slots; `define %Maybe_Header @makeNone()` inserts the unit-variant tag; `define i32 @inspect(%Maybe_Header %maybe)` lowers `return match maybe` to ordered tag branches and reconstructs `%Header`; `define i32 @unwrap(%Maybe_Header %maybe)` lowers `if (maybe is Maybe.Some(header))` to a tag test and typed payload reconstruction. Remaining AST-source gap: `makeSome` is still the tuple payload wrapper to move fully onto the AST helper. target/.../obj/nativeGenericMixedEnumApp.o contains `_makeSome` / `_makeNone` / `_inspect` / `_unwrap` / `_consume` / `_main`, target/.../bin/nativeGenericMixedEnumApp is a host executable, and direct execution verifies `run-exit-code: 98`
build/stage0/zeno test --stage mvp --milestone M2
  -> zeno test: 80 passed, 0 failed (stage mvp, M2)
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
  -> zeno test: 428 passed, 0 failed
build/stage0/zeno test --stage full-spec --target x86_64-unknown-linux-gnu
  -> zeno test: 410 passed, 0 failed
build/stage0/zeno test --stage full-spec --target aarch64-apple-darwin
  -> zeno test: 408 passed, 0 failed
zeno test: 146 passed, 0 failed (stage mvp)
zeno test: 428 passed, 0 failed
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
