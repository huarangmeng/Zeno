# Zeno v0.1 / stage0 MVP 子集

本文定义第一版 C++20 + LLVM 21 stage0 编译器必须实现的最小语言与库边界。主语言规范仍然描述完整 v0.1 设计；本文只规定第一批实现门禁，防止 stage0 被运行时、包管理器和高级并发一次性拖住。

原则：

- 先实现决定语言语义和性能模型的能力。
- 先实现能支撑自举前端的能力。
- 运行时成本较重、需要大量库工程或跨包元数据的能力可以保留在规范中，但从 stage0 MVP 门禁中移出。
- 编译器可以先保留关键字、语法和标准库声明，再对未实现能力给出明确 staged diagnostic。

开发输入：

- C++20 + LLVM 21 + CMake/Ninja。
- 首批 host：macOS arm64、Linux x86_64。
- 首批 target：`aarch64-apple-darwin`、`x86_64-unknown-linux-gnu`。
- 首批 CLI：`zeno check`、`zeno build`、`zeno test`。
- 首批产物：application executable、library static archive、`.zmeta` 编译器元数据。
- LLVM 22+ 只作为后续显式后端升级，不进入 stage0 MVP 基线。
- 实现目录与里程碑见 [BOOTSTRAP.md](BOOTSTRAP.md)：`compiler/stage0`、`lib/zeno/{core,alloc,std}`、`runtime/stage0`，M0-M9 分批交付。

## 1. P1 决策

| 问题 | stage0 MVP 决策 | 原因 |
| --- | --- | --- |
| `Shared<T>` 是否完整进入第一批 | 不进入完整 runtime；保留规范和类型语义，完整引用计数、`Shared<Interface>` 和跨线程释放进入第二批 | `Shared` 需要控制块、原子引用计数、allocator 绑定和并发释放，工程量高；先用 `Box<T>` 建立拥有式分配和接口对象模型 |
| async 是否进入第一批 | 语法和安全规则保留；stage0 MVP 可以解析后拒绝 `async fn`、`async { ... }` / `async move { ... }`、Future block 实参和 `await`，不实现 executor 和 future lowering | async 的正确实现需要 future 状态机、drop cleanup、跨 `await` 逃逸检查和 runtime 边界，不能半成品进入 |
| `Thread.scope` / `splitDisjoint` 是否进入第一批 | 普通 `Thread.spawn` 类型检查进入第一批；scoped 并发和 disjoint API 证明进入第二批 | OS 线程所有权转移是核心模型；scoped 借入并发需要更复杂的访问证明 |
| `pub fn -> Interface` 跨 package | 语言允许；stage0 MVP 先支持同 package 静态接口返回，跨 package opaque return metadata 进入第二批 | 同包可在编译单元内统一具体返回类型；跨包需要稳定公开元数据和增量失效规则 |
| `Array<T>.clone` / `Vector<T>.clone` | 第一批只支持 `T: Copy`；非 `Copy` 深拷贝接口延后 | 避免在核心库早期引入复杂 `Clone` 语义；复制成本仍显式可见 |
| package manager | 第一批只支持本地 package、workspace 和 builtin core；registry、git fetch、发布协议延后 | 自举编译器需要可复现本地构建，不需要一开始联网解析依赖 |
| 标准库实现方式 | 第一批使用 builtin + 声明包；真实 `core` / `alloc` 实现逐步替换 | 避免标准库工程拖慢 parser、Sema、MIR 和 LLVM 降级；声明包仍能验证语言语义和成本模型 |

这些决策不删除完整 v0.1 能力，只定义 stage0 第一批必须通过的范围。

## 2. 必须实现

### 2.1 前端与诊断

- 词法器、解析器和 AST。
- 带源码 span 的稳定诊断。
- `module`、默认 `src/` 源码根、文件路径到模块路径的推断。
- 同 package 直接可见、`import`、外部包根解析。
- 默认 package-visible、`pub` 外部可见、`private` 文件私有。
- 函数和方法重载解析。

### 2.2 类型与控制流

- 基础整数、浮点、`Bool`、`Char`、`Unit`、`Never`。
- `struct`、`enum`、`interface`、`impl`。
- 默认 Auto layout、`@layout(Source)`、`@layout(C)`、`@layout(Packed(N))`。
- `const` 项、局部 `const`、`static` CTFE 初始化、常量泛型参数和布局查询。
- CTFE 必须能执行普通函数、方法、循环、`match`、泛型和静态接口派发；不能只实现字面量表达式。
- `val`、`var`、块、`if`、`while`、`for`、基础 `match`。
- 基础 pattern、穷尽性检查、只读 / `mut` / `move` enum payload 访问。
- 闭包语法：`(params) -> T { ... }` 和 `(params) => expr`。
- `Fn` / `MutFn` / `OnceFn` 能力推导。

### 2.3 所有权、访问和销毁

- `Copy` 标记、非 `Copy` 默认移动。
- `move` 参数、`move self`、调用点 `move value`、`match move`、`for move` 和 `move` 闭包捕获；赋值、绑定、返回和字段初始化保留自动移动。
- 默认只读访问、`mut` 唯一可写访问。
- `self`、`mut self`、`move self` 接收者。
- 初始化检查、move 后不可用、部分初始化 drop flags。
- RAII 销毁 lowering。
- `destroy` 语义：编译器调用、不可直接调用、不能 `try`、不能移出非 `Copy` 字段。
- 访问逃逸检查，包括视图不能长期保存。

### 2.4 核心库子集

stage0 MVP 的核心库先以编译器发行包的内建声明包为准。声明包必须提供类型签名、接口约束、layout/drop 事实、`Send` / `Sync` 事实、intrinsic 绑定和必要 runtime symbol；它们可以没有完整 Zeno 函数体。第一批实现只需要足够支持类型检查、安全检查、MIR 降级、codegen 符号引用和少量基础 run-pass。

第一批必须有声明和语义检查：

- `Option<T>`、`Result<T, E>` 和 `try`。
- `Array<T>`：拥有、连续、固定长度。
- `Vector<T>`：拥有、连续、可增长。
- `ArraySlice<T>`：非拥有连续访问值，不能保存到长期位置。
- `Hash`、`HashKey`、`Map<K, V>` 和 `Set<T>` 的基础 API。
- `Map` / `Set` 的默认哈希表实现：无稳定顺序、无每元素分配、支持 `tryReserve*`。
- `StringSlice` literal 和只读文本访问。
- `String.from`、`String.fromIn`、`String.tryReserve*`、`String.clone` 的第一批语义。
- `Vector.tryReserve*` 可恢复 OOM 入口。
- `Array<T>.clone`、`Vector<T>.clone` 第一批只对 `T: Copy` 开放。
- `Map<K, V>.clone` 第一批只对 `K: Copy` 且 `V: Copy` 开放；`Set<T>.clone` 第一批只对 `T: Copy` 开放。
- `Box<T>` 和最小 `Box<Interface>` 擦除、动态派发、销毁 lowering。
- `Allocator`、`EscapingAllocator`、profile 默认 allocator 和 `In` 后缀 allocator API。

### 2.5 泛型与接口

- 默认单态化泛型函数和泛型类型。
- 一个泛型参数只允许一个直接约束。
- 多能力通过命名组合接口表达，例如 `interface SortKey: Ord, Copy {}`。
- `interface Consumer<T> { ... }` 泛型接口。
- `consumer: Consumer<T>` 匿名静态接口参数展开。
- 泛型接口参数推断。
- 多个裸接口参数默认是独立隐藏具体类型。
- 需要同一具体类型时使用显式泛型名，例如 `W: Writer`。
- `-> Interface` 同 package 静态接口返回一致性检查。
- `Box<Interface>` 只支持动态可派发方法：`self` / `mut self`，无方法级泛型、无 `Self` 普通参数或返回、无 `move self`。

### 2.6 错误、panic 和 OOM

- `try` 降低为普通分支和 cleanup，不使用异常。
- `panic(message) -> Never`。
- `PanicInfo` 调用点注入、`SourceLocation` 和无分配 stack frame 地址遍历。
- `oom(layout) -> Never`。
- 默认分配 API 失败进入 OOM 策略。
- `tryReserve*` 失败返回 `Result<Unit, AllocError>`，不调用 `oom`。
- hosted 默认 `panic.strategy = "abort"`、`oom.strategy = "abort"`；freestanding 默认 `panic.strategy = "trap"`、`oom.strategy = "trap"`。
- `panic.strategy = "unwind"` 第一批只解析并给 staged diagnostic，不生成半成品 unwind。
- `@noPanic` 和 `@noAlloc` 使用保守可达调用图检查。

### 2.7 trust、FFI 和 ABI 边界

- `trust` 块和 `trust impl`。
- `trust extern`。
- manifest trust 能力检查和 trust 报告基础记录，包括 `ffi`、`rawMemory`、`hardware`、`inlineAsm`、`interrupts` 和 `threadSafety` 类别。
- `@export("symbol", abi: C)`。
- C-compatible 签名检查。
- `@export("symbol", bridge: C)` 的最小 C bridge：C-compatible 类型、`ArraySlice<T>` / `mut ArraySlice<T>`、`StringSlice`、`Option<T>` 返回、`Result<T, E>` 返回和 C thunk 生成。
- `zeno bindgen c` 可以作为工具链能力实现；完整 C++ bindgen 不属于 v1 门禁，但缓存和包元数据不能排斥后续 `cxx` 前端。
- 导出符号唯一性。
- panic 跨 C ABI 边界检查。

### 2.8 并发最小子集

- `Send` / `Sync` 自动推导。
- `trust impl Send` / `trust impl Sync` 记录。
- `Thread.spawn` 的所有权转移与 `Send` 检查。
- `Box<Interface>` 跨线程时必须通过包含 `Send` 的命名接口证明。
- `Mutex<T>` 和原子类型可以先作为核心库接口和类型检查目标；完整 hosted 实现可随标准库推进。

### 2.9 HIR、MIR 和 LLVM lowering

- SourceManager、`FileId`、`Span` 和稳定诊断映射。
- 诊断错误码分段、human 输出和 JSON Lines 输出。
- staged diagnostic 使用 `E9000-E9099`，必须带 `isStaged` 和 `feature`。
- Lexer、Parser、AST 和语法错误恢复。
- Declaration Collection 收集顶层声明、模块路径、可见性、重载入口和 stable node id。
- Name / Module Resolution 解析同包直接可见、外部 import、模块限定名和可见性。
- HIR 保留所有权、访问区域、布局、泛型约束、trust 边界、profile 信息和源码 span。
- Type / Interface / Ownership Sema 完成类型检查、重载、接口约束、初始化、move、RAII、访问逃逸和 `Send` / `Sync`。
- MIR 使用显式 CFG、locals、places、operands、rvalues、drop flags 和 cleanup edges。
- `try`、RAII、panic/OOM 终点都必须进入 MIR verifier。
- 单态化后再进入 LLVM lowering。
- `mut` / `move` 的别名事实必须由 MIR 证明后才能发 LLVM `noalias`、`nocapture`、`readonly`。
- 连续集合遍历、非逃逸闭包、静态接口参数和静态接口返回不得生成隐藏堆分配或接口表派发。

### 2.10 包与构建

- `Zeno.toml` 解析。
- `[package]` 最小字段：`name`、可选 `version`、可选 `kind`、可选 `entry`。
- `kind = "application"` 默认入口为 `src/main.zn` 中的 `main`；`kind = "library"` 没有入口，`src/lib.zn` 只是推荐组织文件。
- 固定 `src/` 源码根，文件路径推导模块路径，`module` 声明只做路径校验。
- 同 package 直接可见；外部 package 和内建包才需要 `import`。
- 单包构建。
- workspace member。
- builtin core。
- builtin `core` / `alloc` / 最小 `std` 声明包。
- path dependency。
- `core` 自动可用，不需要依赖声明；`std` 只在 hosted 或支持 hosted 能力的 profile 中解析。
- `Zeno.lock` 本地 frozen 校验，第一批支持 `path` 和 `builtin` 来源。
- registry、git fetch、版本范围求解和发布协议延后，遇到时给 staged diagnostic。
- target triple、profile、allocator、panic/OOM 和 trust 字段。
- CLI 行为：`zeno check` 不 codegen，`zeno build` 生成产物，`zeno test` 默认运行 MVP 规格测试。
- `--diagnostic-format human|json`，JSON 输出为一行一个 object。
- application 输出 `bin/<package-name>`；library 输出 `lib/lib<package-name>.a` 和 `meta/<package-name>.zmeta`。
- 并行 parse / body check / monomorphization / codegen 的调度结构。
- package 级增量缓存 key、stable node id 和可重放诊断。

## 3. 明确延后

stage0 MVP 不要求实现：

- 完整 `Shared<T>` runtime。
- `Shared<Interface>` 动态接口对象。
- async lowering、future 状态机、executor、task runtime 和 `TaskGroup<T>`。
- `Thread.scope`、scoped thread、`splitDisjoint` 的不重叠证明。
- 跨 package `pub fn -> Interface` opaque return metadata。
- registry 解析、git fetch、发布协议。
- 完整 IDE / LSP。
- 高级 C ABI 包装生成器。
- 自定义 hasher policy、有序 `OrderedMap` / `OrderedSet` 和持久化稳定哈希。
- 高级增量缓存策略。
- 宏系统。
- 完整反射。
- downcast。
- 稳定外部二进制包 ABI。
- 动态库和发布包格式。

编译器遇到延后能力时应优先给出阶段性诊断，不能静默生成错误代码。比如解析到 `async fn`、`spawn({ ... })` Future block 实参或 `await` 后可以报“当前 stage0 MVP 尚未实现 async lowering”；解析到跨 package `pub fn -> Interface` 后可以报“当前 stage0 MVP 尚未生成跨包 opaque return metadata”。

## 4. 测试门禁

规格测试分两层：

- MVP 门禁：stage0 第一批必须通过的测试。
- 完整规格门禁：完整 v0.1 设计最终必须通过的测试。

测试 runner 路径冻结为：

```text
tests/spec/compile-pass/*.zn
tests/spec/compile-fail/*.zn
tests/spec/manifest-pass/*.toml
tests/spec/manifest-fail/*.toml
tests/spec/module-pass/*/
tests/spec/module-fail/*/
tests/spec/package-pass/*/
tests/spec/package-fail/*/
tests/spec/incremental-pass/*/case.toml
tests/spec/incremental-fail/*/case.toml
tests/spec/codegen-pass/*/case.toml
tests/spec/codegen-fail/*/case.toml
```

现有测试文件如果覆盖延后能力，应保留为完整规格门禁，不删除。`.zn` 和真实 manifest TOML 用头部注释标记 `stage` / `feature` / `profile` / `target`；`codegen-*` 和 `incremental-*` 的 `case.toml` 用结构化字段标记。没有显式 `stage` 时，默认按完整规格测试处理；进入发布门禁前再批量补齐 `stage: mvp` 或 `stage = "mvp"`。

runner 命令：

- `zeno test --stage mvp` 只跑 stage0 MVP 门禁。
- `zeno test --stage full-spec` 跑完整规格门禁。
- `zeno test --feature async` 按 feature 过滤。
- `zeno test --target x86_64-unknown-linux-gnu` 按目标过滤 codegen / ABI 相关测试。

所有失败类测试必须包含 `expected-error`。runner 输出必须按 category、test path、package、source file、byte offset 和 error code 稳定排序；并行执行和缓存命中不能改变诊断顺序。

第一批 MVP 门禁的性能底线：

- 泛型和静态接口参数必须单态化。
- `writer: Writer` 不生成接口表派发。
- `-> Writer` 静态接口返回不生成 `Box` 或 allocator call。
- `Box<T>` 到 `Box<Interface>` 不重新分配。
- 非逃逸闭包不分配。
- `try` 不使用异常。
- `Array` / `Vector` / `ArraySlice` 核心循环应允许消除冗余边界检查。
- `Map` / `Set` 默认实现不得做每元素堆分配；预留容量成功后的批量插入不应重复触发 rehash 慢路径。
- 普通整数运算不生成隐藏溢出检查。
- RAII 销毁降低为直接 cleanup 调用，没有隐藏引用计数。
