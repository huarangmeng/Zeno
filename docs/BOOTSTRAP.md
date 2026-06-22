# Zeno 自举计划

完成规范里程碑后，第一版实现是基于 C++20 和 LLVM 的 stage0 编译器。

## 1. stage0 目标

stage0 必须做到：

- 解析 `.zn` 源文件。
- 生成带源码 span 的可用诊断。
- 解析模块、名字、类型和接口。
- 检查 `Copy`、move、初始化和访问规则。
- 单态化泛型函数和类型。
- 把检查后的 HIR 降低为 MIR，再把优化后的 MIR 降低到 LLVM IR。
- 编译足够多的 `core`，以运行 compile-pass、compile-fail 和基础 run-pass 测试。

stage0 暂不需要：

- 一开始就自举。
- 最细粒度的高级增量编译；但数据结构必须支持 package 级增量、并行 parse/codegen 和安全缓存 key。
- 完整 IDE 协议支持。
- 稳定 ABI。
- 宏展开。

## 2. 推荐架构

```text
source
  -> 词法器
  -> 解析器
  -> AST
  -> name resolution
  -> type/interface checking
  -> ownership + initialization checking
  -> access + escape checking
  -> typed HIR
  -> monomorphization
  -> MIR
  -> LLVM IR
  -> object / executable
```

AST、HIR 和 MIR 应与 LLVM 解耦，方便未来自举前端复用同一套概念管线。

编译模型、并行调度、增量缓存和诊断稳定性见 [COMPILATION.md](COMPILATION.md)。
HIR、MIR、LLVM 降级和 codegen 优化不变量见 [IR.md](IR.md)。
别名、布局、泛型、边界检查、分配器和并发性能契约见 [PERFORMANCE.md](PERFORMANCE.md)。

## 3. stage0 MVP 语言子集

第一批实现门禁见 [V01_SUBSET.md](V01_SUBSET.md)。本节给出自举路线视角下的摘要。

最小子集：

- `Zeno.toml` manifest 解析和校验，至少支持单包构建、workspace、默认 `src/` 源码根、builtin core、path dependency、`Zeno.lock` frozen 校验、target triple、profile、allocator、panic/OOM 和 trust 字段。
- 模块与导入。
- 文件路径到模块路径的推断、可选 `module` 校验、同包直接可见、包依赖图构建、外部导入解析和 `pub` / `private` 可见性检查。
- 函数和编译期重载解析。
- `val`、`var`、块、`if`、`while`、`for`。
- `for` 的只读、可写、消耗遍历、整数半开区间 lowering，以及具体 `Iterator<T>` 拥有式遍历。
- 基础 `match`、pattern 穷尽性、只读 / `mut` / `move` enum payload 访问。
- 基础类型。
- 结构体和枚举，包括默认 Auto layout、`@layout(Source)`、`@layout(C)`、`@layout(Packed(N))`、单字段零成本包装和基础 enum niche 优化。
- `Option` 和 `Result`。
- `try`，包括 `Result` / `Option` 限定传播、`okOr` / `okOrElse` / `unwrapOrElse` / `mapErr` 检查。
- `Never`、`panic(message) -> Never`、`PanicInfo` 调用点注入、惰性调用栈遍历、`oom(layout) -> Never`，以及 profile 对 abort / trap / unwind / handler 的选择。
- `Copy`、move 和确定性销毁。
- 基础闭包语法和 `Fn` / `MutFn` / `OnceFn` 能力推导。
- `Array`、`Vector`、`ArraySlice`、`mut` 访问和 `move` 所有权转移。
- `Hash`、`HashKey`、`Map`、`Set` 和关联集合的默认哈希表实现。
- `StringSlice` literal、`String.from` / `String.fromIn` 拥有构造、默认 allocator、`tryReserve` 可恢复 OOM 入口与字符串访问规则。
- `EscapingAllocator`、scoped allocator owner 的 allocation region 逃逸检查。
- 整数 wrapping 运算、checked 运算、无损 `as` 转换检查、`fromChecked` / `truncate` / `saturate` 转换 API。
- `trust` 边界、`trust extern` 和信任报告的基础记录。
- `@export(..., abi: C)`、C-compatible 签名检查、导出符号唯一性和 panic ABI 边界检查。
- 带接口约束的泛型函数。
- 静态接口派发、匿名接口参数展开、泛型接口参数推断和静态接口返回一致性检查。
- `Box<T>` 和最小 `Box<Interface>` 擦除、动态派发、销毁 lowering。
- `Send` / `Sync` 自动推导和 `trust impl` 记录。
- `Thread.spawn` 的所有权转移与 `Send` 检查。

可以推迟到后续 stage0 迭代：

- 完整 `Shared<T>` runtime、`Shared<Interface>` 动态派发和跨线程引用计数释放。
- scoped 并发、`Thread.scope` 和 `splitDisjoint` 这类 disjoint API 的不重叠证明记录。
- 完整 async lowering、future 状态机、任务运行时和多线程 executor；如果后续 stage0 迭代实现 async 子集，必须同时实现 future drop cleanup、禁止访问值逃逸跨 `await` 和非逃逸 future 无分配。
- 跨 package `pub fn -> Interface` opaque return metadata。
- 高级逃逸闭包对象化和 callable interface owner。
- 自定义 hasher policy、有序关联集合和持久化稳定哈希。
- hosted 文件系统 API。
- registry 下载、git fetch 和发布协议；stage0 MVP 只依赖本地包源和 lockfile。
- 高级 C ABI 包装生成器。
- 高级模式匹配。

## 4. C++20 实现建议

推荐组件：

- 手写词法器和解析器，以获得控制力和诊断质量。
- 编译器 AST/HIR 节点使用 arena 分配。
- intern 标识符。
- 持久源码映射。
- 接口求解使用约束图。
- `Send` / `Sync` 推导作为接口求解的一部分处理，并在并发边界生成专门诊断。
- 初始化和 move 状态使用数据流分析。
- inferred access scope 使用 region-like 内部表示。
- 类型布局计算在 HIR 类型完成后执行；Auto layout 的字段重排结果必须进入后续访问检查、销毁 lowering 和 codegen，但不改变源码语义顺序。
- MIR 使用显式 CFG、locals、places、operands、rvalues、drop flags 和 cleanup edges；不要让复杂源码表达式直接进入 LLVM lowering。
- `mut` / `move` 的别名事实先在 MIR 中证明，再选择性发出 LLVM `noalias`、`nocapture`、`readonly` 等属性。
- provenance、range facts、capacity facts 和 escape state 必须作为 MIR verifier 可检查的数据，而不是只存在于优化 pass 的临时推断里。
- 编译数据库必须记录 stable node id、阶段 fingerprint、缓存 key 和可重放诊断。
- 早期测试可以使用 LLVM ORC 或普通 object emission。

不要让 C++ 实现便利性反过来限制语言设计。

## 5. IR 策略

完整策略见 [IR.md](IR.md)。本节只列 stage0 必须满足的摘要。

HIR 应保留语言语义：

- 所有权转移。
- 销毁点。
- 源码字段顺序、实际布局字段顺序、字段 offset、size 和 align。
- 访问区域。
- 整数转换检查结果和钳制转换。
- 泛型约束。
- `Send` / `Sync` 自动推导结果和 `trust impl` 来源。
- 模式穷尽性。
- `trust` 边界、manifest 能力授权、能力类别和公开 API 影响范围。
- `@noAlloc`、`@noPanic` 和 profile 影响。

MIR 应显式化 lowering 细节：

- 控制流图。
- locals、places、operands、rvalues 和类 SSA 临时值。
- `try` 的成功边、提前返回边和清理边。
- 部分初始化 drop flag。
- 边界检查。
- range facts 和容量 facts。
- provenance、access path、disjoint proof 和 escape state。
- allocation region、allocator kind 和 owner 逃逸状态。
- 单态化函数。
- 非逃逸闭包环境。
- 必要的接口派发调用。
- packed 字段的 unaligned load/store。

LLVM IR 应接收已经检查过的内存安全操作。不能指望 LLVM 在 lowering 后恢复 Zeno 的安全语义。
LLVM 属性只能来自 MIR 中已证明的事实，例如唯一 `mut` 访问才能发 `noalias`，不逃逸访问才能发 `nocapture`。

## 6. 自举路径

阶段 A：规范和示例。

阶段 B：C++20 stage0 编译器能编译小程序。

阶段 C：stage0 编译 `core` 和规格测试套件。

阶段 D：用受限 Zeno 子集编写词法器、解析器和诊断库。

阶段 E：stage0 编译 Zeno 前端。

阶段 F：Zeno 前端 + C++/LLVM 后端编译自身。

阶段 G：逐步替换后端组件，或保留 LLVM 作为生产后端。

当 Zeno 编写的编译器能够从源码编译出下一个等价 Zeno 编译器时，认为自举完成。

## 7. 性能验收

编译器最终应证明：

- 泛型抽象能像 C++ template 一样单态化和内联。
- 泛型代码体积通过 cold outlining、shape sharing、identical code folding、LTO 和 PGO 控制，不牺牲默认静态派发。
- 非逃逸闭包不分配。
- MIR 中非逃逸闭包应降低为局部环境或标量，不出现 `Box`、`Shared` 或 allocator call。
- `okOrElse`、`unwrapOrElse` 和 `mapErr` 的 `OnceFn` 非逃逸闭包内联为分支，不污染成功热路径。
- 核心连续集合的 `for` 遍历直接 lowering 为循环，不分配 iterator，不走接口派发。
- 具体 `Iterator<T>` 的 `for move` 遍历静态派发 `next`，可内联。
- 访问检查除了无法证明的边界检查外都在编译期完成。
- 典型 `ArraySlice<T>` / `Array<T>` / `Vector<T>` 循环中的冗余边界检查被消除。
- `Map<K, V>` / `Set<T>` 默认实现不做每元素堆分配，预留容量成功后的批量插入不重复触发 rehash 慢路径。
- `mut` 参数只有在唯一且不逃逸时才发 LLVM `noalias`，避免为性能制造未定义行为。
- allocator 泛型调用可以静态派发并内联，零大小 allocator 不增加拥有者大小。
- scoped allocator owner 不能逃逸 allocator region。
- 非逃逸 future 不分配；逃逸 future 的分配必须由 task handle、runtime spawn 或 `Box<Future<...>>` 显式表达。
- future drop 必须按当前状态执行 RAII cleanup。
- `CachePadded<T>` 提供显式 cache line padding，普通 Auto layout 不偷偷插入同级 padding。
- 普通整数 `+`、`-`、`*` 不生成隐藏溢出检查。
- `checkedAdd` 等方法使用目标支持的 overflow intrinsic 或等价高效 lowering。
- `truncate` 降低为截断，`saturate` 降低为比较/选择或目标饱和指令。
- RAII 销毁降低为直接调用，没有隐藏引用计数。
- 接口派发只在显式接口拥有者中出现，例如 `Box<Writer>`、`Shared<Writer>`；`writer: Writer` 和 `W: Writer` 都是静态接口约束，保持静态派发。
- `Box<T>` 到 `Box<Interface>` 的擦除转换不重新分配；`Shared<T>` 到 `Shared<Interface>` 的擦除转换不增加引用计数。

## 8. trust 交付与审计

第一版编译器应支持用户包中的 `trust` 边界。编译器发行包仍可包含 intrinsic 声明，但不再是用户写底层代码的唯一入口。

构建应记录：

- 编译器版本。
- 编译器发行包 hash。
- `Zeno.toml` 路径和内容 hash。
- 目标 triple。
- 启用 profile：`freestanding`、`hosted` 或自定义。
- 分配器、panic 策略和 OOM 策略。
- 每个 `trust` 边界的位置、能力类别和公开 API 影响范围。
