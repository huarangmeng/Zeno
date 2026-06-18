# Zeno v0.1 / stage0 MVP 子集

本文定义第一版 C++20 + LLVM stage0 编译器必须实现的最小语言与库边界。主语言规范仍然描述完整 v0.1 设计；本文只规定第一批实现门禁，防止 stage0 被运行时、包管理器和高级并发一次性拖住。

原则：

- 先实现决定语言语义和性能模型的能力。
- 先实现能支撑自举前端的能力。
- 运行时成本较重、需要大量库工程或跨包元数据的能力可以保留在规范中，但从 stage0 MVP 门禁中移出。
- 编译器可以先保留关键字、语法和标准库声明，再对未实现能力给出明确 staged diagnostic。

## 1. P1 决策

| 问题 | stage0 MVP 决策 | 原因 |
| --- | --- | --- |
| `Shared<T>` 是否完整进入第一批 | 不进入完整 runtime；保留规范和类型语义，完整引用计数、`Shared<Interface>` 和跨线程释放进入第二批 | `Shared` 需要控制块、原子引用计数、allocator 绑定和并发释放，工程量高；先用 `Box<T>` 建立拥有式分配和接口对象模型 |
| async 是否进入第一批 | 语法和安全规则保留；stage0 MVP 可以解析后拒绝 `async fn` / `await`，不实现 executor 和 future lowering | async 的正确实现需要 future 状态机、drop cleanup、跨 `await` 逃逸检查和 runtime 边界，不能半成品进入 |
| `Thread.scope` / `splitDisjoint` 是否进入第一批 | 普通 `Thread.spawn` 类型检查进入第一批；scoped 并发和 disjoint API 证明进入第二批 | OS 线程所有权转移是核心模型；scoped 借入并发需要更复杂的访问证明 |
| `pub fn -> Interface` 跨 package | 语言允许；stage0 MVP 先支持同 package 静态接口返回，跨 package opaque return metadata 进入第二批 | 同包可在编译单元内统一具体返回类型；跨包需要稳定公开元数据和增量失效规则 |
| `Array<T>.clone` / `Vector<T>.clone` | 第一批只支持 `T: Copy`；非 `Copy` 深拷贝接口延后 | 避免在核心库早期引入复杂 `Clone` 语义；复制成本仍显式可见 |
| package manager | 第一批只支持本地 package、workspace 和 builtin core；registry、git fetch、发布协议延后 | 自举编译器需要可复现本地构建，不需要一开始联网解析依赖 |

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
- `let`、`var`、块、`if`、`while`、`for`、基础 `match`。
- 基础 pattern、穷尽性检查、只读 / `mut` / `move` enum payload 访问。
- 闭包语法：`(params) -> T { ... }` 和 `(params) => expr`。
- `Fn` / `MutFn` / `OnceFn` 能力推导。

### 2.3 所有权、访问和销毁

- `Copy` 标记、非 `Copy` 默认移动。
- `move` 参数、`move self`、调用点 `move value`、`match move`、`for move` 和 `move` 闭包捕获；赋值、绑定、返回和字段初始化保留自动移动。
- 默认只读访问、`mut` 唯一可写访问。
- `self`、`mut self`、`move self` 接收者。
- 初始化检查、move 后不可用、部分初始化 drop flags。
- RAII 销毁 lowering、`defer` cleanup。
- `destroy` 语义：编译器调用、不可直接调用、不能 `try`、不能移出非 `Copy` 字段。
- 访问逃逸检查，包括视图不能长期保存。

### 2.4 核心库子集

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
- `PanicInfo` 调用点注入和 profile 选择。
- `oom(layout) -> Never`。
- 默认分配 API 失败进入 OOM 策略。
- `tryReserve*` 失败返回 `Result<Unit, AllocError>`，不调用 `oom`。
- `@noPanic` 和 `@noAlloc` 检查。

### 2.7 trust、FFI 和 ABI 边界

- `trust` 块和 `trust impl`。
- `trust extern`。
- manifest trust 能力检查和 trust 报告基础记录。
- `@export("symbol", abi: C)`。
- C-compatible 签名检查。
- 导出符号唯一性。
- panic 跨 C ABI 边界检查。

### 2.8 并发最小子集

- `Send` / `Sync` 自动推导。
- `trust impl Send` / `trust impl Sync` 记录。
- `Thread.spawn` 的所有权转移与 `Send` 检查。
- `Box<Interface>` 跨线程时必须通过包含 `Send` 的命名接口证明。
- `Mutex<T>` 和原子类型可以先作为核心库接口和类型检查目标；完整 hosted 实现可随标准库推进。

### 2.9 HIR、MIR 和 LLVM lowering

- HIR 保留所有权、访问区域、布局、泛型约束、trust 边界和 profile 信息。
- MIR 使用显式 CFG、locals、places、operands、rvalues、drop flags 和 cleanup edges。
- `try`、RAII、`defer`、panic/OOM 终点都必须进入 MIR verifier。
- 单态化后再进入 LLVM lowering。
- `mut` / `move` 的别名事实必须由 MIR 证明后才能发 LLVM `noalias`、`nocapture`、`readonly`。
- 连续集合遍历、非逃逸闭包、静态接口参数和静态接口返回不得生成隐藏堆分配或接口表派发。

### 2.10 包与构建

- `Zeno.toml` 解析。
- 单包构建。
- workspace member。
- builtin core。
- path dependency。
- `Zeno.lock` frozen 校验。
- target triple、profile、allocator、panic/OOM 和 trust 字段。
- 并行 parse / body check / monomorphization / codegen 的调度结构。
- package 级增量缓存 key、stable node id 和可重放诊断。

## 3. 明确延后

stage0 MVP 不要求实现：

- 完整 `Shared<T>` runtime。
- `Shared<Interface>` 动态接口对象。
- async lowering、future 状态机、executor 和 task runtime。
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

编译器遇到延后能力时应优先给出阶段性诊断，不能静默生成错误代码。比如解析到 `async fn` 后可以报“当前 stage0 MVP 尚未实现 async lowering”；解析到跨 package `pub fn -> Interface` 后可以报“当前 stage0 MVP 尚未生成跨包 opaque return metadata”。

## 4. 测试门禁

规格测试分两层：

- MVP 门禁：stage0 第一批必须通过的测试。
- 完整规格门禁：完整 v0.1 设计最终必须通过的测试。

现有测试文件如果覆盖延后能力，应保留为完整规格门禁，不删除。测试 runner 出现后，可以通过测试头部注释或 `case.toml` 标记 `stage = "mvp"`、`stage = "full-spec"`、`feature = "async"` 等信息。没有标记时，默认按完整规格测试处理；进入发布门禁前再批量补齐标记。

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
