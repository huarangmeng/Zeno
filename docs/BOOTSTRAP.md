# Zeno 自举计划

完成规范里程碑后，第一版实现是基于 C++20 和 LLVM 21 的 stage0 编译器。

## 0. 开发输入冻结

stage0 第一批开发输入固定为：

- 实现语言：C++20。
- 后端：LLVM 21。
- 构建系统：CMake + Ninja。
- 首批 host：macOS arm64 和 Linux x86_64。
- 首批 target：`aarch64-apple-darwin` 和 `x86_64-unknown-linux-gnu`。
- 首批 CLI：`zeno check`、`zeno build`、`zeno test`。
- 首批产物：application executable、library static archive 和 `.zmeta` 编译器元数据。

LLVM 21 是 stage0 的稳定开发基线。LLVM 22+ 不作为第一批默认后端；后续升级必须作为独立后端升级任务处理，并重新跑 HIR/MIR/LLVM 降级、target data layout、ABI fingerprint、缓存 key 和 codegen 规格测试。

### 0.1 仓库目录冻结

stage0 实现目录冻结为：

```text
compiler/stage0/
  CMakeLists.txt
  cmake/
  include/zeno/
    base/
    source/
    diag/
    lex/
    parse/
    ast/
    package/
    names/
    hir/
    sema/
    mir/
    mono/
    codegen/
    driver/
  src/
    base/
    source/
    diag/
    lex/
    parse/
    ast/
    package/
    names/
    hir/
    sema/
    mir/
    mono/
    codegen/
    driver/
    main.cpp
  tests/unit/

lib/zeno/
  core/
  alloc/
  std/

runtime/stage0/
  panic/
  alloc/
  thread/
  ffi/
```

目录职责：

- `compiler/stage0` 是 C++20 stage0 编译器，不放语言标准库实现。
- `compiler/stage0/include/zeno` 只放稳定的内部 C++ 头文件；`src` 放实现。
- `base` 放 arena、intern、stable id、fingerprint、小型容器封装和通用工具。
- `source` 放 SourceManager、FileId、Span、LineTable 和源码缓存。
- `diag` 放错误码、DiagnosticEngine、human / JSON Lines emitter 和可重放诊断。
- `lex` / `parse` / `ast` 只处理语法，不依赖 HIR、Sema、MIR 或 LLVM。
- `package` 放 manifest、lockfile、workspace、builtin/path dependency 和 package graph。
- `names` 放声明收集、模块路径、符号表、导入解析、可见性和重载入口。
- `hir` 放源码语义 IR、HIR builder 和 HIR dump。
- `sema` 放类型、接口、布局、CTFE、所有权、访问、RAII、`Send` / `Sync` 和 staged diagnostic。
- `mir` 放 MIR 数据结构、builder、verifier、文本输出和目标无关优化。
- `mono` 放单态化计划、实例 key、代码膨胀控制和 mono item 分区。
- `codegen` 放 LLVM target 初始化、LLVM IR lowering、object/archive/link 和 `.zmeta` 输出。
- `driver` 放 CLI、编译 pipeline、缓存调度、测试 runner 和 profile/target 配置。
- `lib/zeno/core`、`lib/zeno/alloc`、`lib/zeno/std` 是编译器发行包里的内建声明包；它们看起来应尽量像普通 Zeno package。
- `runtime/stage0` 只放必要 C++ runtime shim，例如 panic/OOM 入口、默认 allocator、线程启动桥和 C bridge helper，不承载语言语义。

依赖方向冻结为：

```text
base
  -> source
  -> diag
  -> lex -> parse -> ast
  -> package -> names -> hir -> sema -> mir -> mono -> codegen
  -> driver
```

实际 C++ 依赖可以让 `diag` 读取 `source` 的行列信息，`driver` 可以依赖所有模块。除此之外，低层模块不能反向依赖高层模块，`ast` / `hir` / `mir` 不能依赖 LLVM 头文件。

stage0 C++ 实现不使用 C++ exception 作为编译流程控制；错误通过诊断、`Expected` / `Result` 风格返回值和显式状态传播。AST、HIR 和 MIR 节点使用 arena 分配；跨缓存、跨线程或跨 package 边界只保存稳定 id、fingerprint 和可序列化摘要。

### 0.2 实现里程碑冻结

第一批实现拆成以下里程碑：

| 里程碑 | 目标 | 完成标准 |
| --- | --- | --- |
| M0 scaffold | CMake/Ninja、LLVM 21 探测、`zeno --version`、基础单元测试框架 | macOS arm64 和 Linux x86_64 可构建空编译器 |
| M1 source/diag/lexer/parser | SourceManager、稳定错误码、human / JSON Lines 诊断、lexer、parser、AST dump | `zeno check --parse` 能解析语法测试并稳定报错 |
| M2 package/names | `Zeno.toml`、`Zeno.lock`、固定 `src/`、workspace、builtin/path dependency、声明收集、模块和可见性 | module/package/manifest 测试可运行 |
| M3 HIR/type/basic sema | HIR、基础类型、控制流、重载、接口骨架、布局骨架、内建声明包加载 | 基础 compile-pass/fail 能进入类型检查 |
| M4 ownership/RAII/access | `val`/`var`、move、初始化、drop flags、`destroy`、访问逃逸、视图规则 | use-after-move、悬垂视图、非法可写共享等测试稳定失败 |
| M5 generics/interface/CTFE | 单态化计划、接口求解、泛型接口参数、同包静态接口返回、完整 CTFE 第一版 | 泛型/接口/const-generic 规格测试可 check |
| M6 MIR | MIR builder、verifier、cleanup edge、bounds check、alias/provenance/allocation region facts、文本 MIR | codegen-pass 的 MIR 不变量可检查 |
| M7 LLVM/codegen | LLVM IR lowering、目标 data layout、object/archive/link、`.zmeta`、基础 runtime shim | `zeno build` 能产出 application / library 第一版产物 |
| M8 spec runner | `zeno test --stage mvp`、expected-error、JSON diagnostic 消费、稳定排序 | MVP 门禁测试可自动跑并复现 |
| M9 performance gate | no hidden allocation/dynamic dispatch/exception lowering、基础边界检查消除、缓存 key 验证 | 第一批 codegen / incremental 性能规格通过 |

实现顺序不能跳过 M1-M6 直接进入 LLVM。Zeno 的性能和安全事实必须先在 HIR/MIR 里建立，再交给 LLVM。

## 1. stage0 目标

stage0 必须做到：

- 解析 `.zn` 源文件。
- 生成带源码 span 的可用诊断。
- 解析模块、名字、类型和接口。
- 加载编译器发行包提供的 builtin `core` / `alloc` / 最小 `std` 声明包。
- 检查 `Copy`、move、初始化和访问规则。
- 单态化泛型函数和类型。
- 把检查后的 HIR 降低为 MIR，再把优化后的 MIR 降低到 LLVM IR。
- 编译足够多的 `core`，以运行 compile-pass、compile-fail 和基础 run-pass 测试。

stage0 暂不需要：

- 一开始就自举。
- 最细粒度的高级增量编译；但数据结构必须支持 package 级增量、并行 parse/codegen 和安全缓存 key。
- 完整 IDE 协议支持。
- 稳定 ABI。
- 动态库和发布包格式。
- 宏展开。

## 2. 推荐架构

```text
source files
  -> SourceManager / FileId / Span
  -> Lexer
  -> Parser
  -> AST
  -> Declaration Collection
  -> Name / Module Resolution
  -> HIR
  -> Type / Interface / Ownership Sema
  -> MIR
  -> Monomorphization
  -> LLVM IR
  -> object / link
```

AST、HIR 和 MIR 应与 LLVM 解耦，方便未来自举前端复用同一套概念管线。

职责冻结：

- SourceManager 负责源码文件、字节偏移、行列映射、include/import 位置和诊断 span；所有后续节点都必须能追溯到稳定 `FileId + Span`。
- Lexer 只产生 token、trivia 边界和 lexical diagnostic，不做名字或类型判断。
- Parser 产生 AST。AST 只表达源码结构、token/span 和语法错误恢复结果，不做复杂语义。
- Declaration Collection 收集顶层声明签名、模块路径、可见性、重载集合入口和 stable node id。
- Name / Module Resolution 建立 package symbol table，解析同包直接可见、外部 import、模块限定名和可见性。
- HIR 是源码语义 IR，保留名字解析结果、类型骨架、泛型、接口、`val` / `var`、`mut` / `move`、`trust`、属性和 span。
- Type / Interface / Ownership Sema 完成类型检查、重载决议、接口约束、初始化、move、RAII、访问逃逸、`Send` / `Sync` 和 staged diagnostics。
- MIR 是必须存在的中间层，负责显式 CFG、locals、places、operands、rvalues、drop flags、cleanup edge、`try` 降级、边界检查、别名事实和所有权事实。
- Monomorphization 在 MIR 前后都可以有计划阶段，但 LLVM 降级只接收具体实例和已经证明的接口/布局事实。
- LLVM IR 只接收 MIR verifier 已证明安全的事实，例如 `noalias`、`readonly`、`nocapture` 和 `noreturn`；不能把 Zeno 的语言安全语义交给 LLVM 猜。

编译模型、并行调度、增量缓存和诊断稳定性见 [COMPILATION.md](COMPILATION.md)。
HIR、MIR、LLVM 降级和 codegen 优化不变量见 [IR.md](IR.md)。
别名、布局、泛型、边界检查、分配器和并发性能契约见 [PERFORMANCE.md](PERFORMANCE.md)。

## 3. stage0 MVP 语言子集

第一批实现门禁见 [V01_SUBSET.md](V01_SUBSET.md)。本节给出自举路线视角下的摘要。

最小子集：

- `Zeno.toml` manifest 解析和校验，至少支持单包构建、workspace、默认 `src/` 源码根、builtin core、path dependency、`Zeno.lock` frozen 校验、target triple、profile、allocator、panic/OOM 和 trust 字段。
- 编译器发行包提供 builtin `core` / `alloc` / 最小 `std` 声明包；stage0 先依赖声明、intrinsic 绑定和必要 runtime shim，不要求完整标准库实现。
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
- 完整 CTFE 基础：`const`、`static` 初始化、常量泛型实参、属性参数、布局查询，以及普通函数 / 循环 / 泛型的编译期解释执行。
- `trust` 边界、`trust extern` 和信任报告的基础记录。
- `@export(..., abi: C)`、C-compatible 签名检查、导出符号唯一性和 panic ABI 边界检查。
- `@export(..., bridge: C)`、bridge-compatible 签名检查、C thunk 生成、头文件生成和无隐藏成本检查。
- `zeno bindgen c` 的 raw/safe 源码生成和缓存 key 记录。完整 C++ 绑定不进入 v1，但 bindgen 架构必须预留后续 `cxx` 前端。
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
