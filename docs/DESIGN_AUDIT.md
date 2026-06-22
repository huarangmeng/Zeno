# Zeno v0.1 设计冻结审计

本文记录 P0 设计冻结审计结果。目标不是继续扩展语言，而是把已经确定的规则压成可以实现、可以测试、可以进入 stage0 的规格。

当前冻结顺序：

1. 先完成语言设计：语法、类型系统、所有权/访问、控制流、错误、并发语义、FFI/trust、模块/编译模型、IR 降级和诊断。
2. 标准库只保留验证语言设计所需的最小核心边界，例如 `Option` / `Result`、拥有容器、分配显式性、同步边界和任务句柄语义。
3. 完整 `std` API 不进入 P0 设计冻结范围；I/O、网络、路径、时间、格式化、序列化和生态便利 API 在语言冻结后单独推进。

## 1. 当前冻结结论

### 1.1 所有权与访问

状态：冻结。

- 默认参数是只读访问，不移动所有权。
- 局部绑定使用 `val` / `var`：`val` 表示初始化后不可重新赋值，`var` 表示可重新赋值；不使用 `let` 作为绑定关键字。
- 编译器跟踪 `uninit` / `partial` / `init` / `moved` / `maybe-init` 状态；读取要求确定已初始化，`val` 只允许每路径初始化一次，`var` 可以在 move 后或 `maybe-init` 状态下重新赋值。
- 局部绑定按源码声明顺序的逆序销毁；延迟初始化和重新赋值不改变 cleanup 位置，未初始化或已移动的位置跳过销毁。
- `mut` 表示唯一可写访问，不取得所有权。
- `move` 表示所有权接收模式；赋值、绑定、返回和字段初始化可以自动移动，函数 / 方法 `move` 参数从已有命名位置接收所有权时调用点必须写 `move value`，非 `Copy` 资源进入拥有位置后由编译器诊断后续误用。
- `self` / `mut self` / `move self` 分别表示只读、可写、消费接收者。
- 带 `destroy` 的类型不能在安全代码中移出非 `Copy` 字段；`move self` 方法也只能完成资源状态，不能拆字段。
- `destroy` 不是用户可调用方法，不能 `try`，不能 `await`，隐式 `@noPanic` / `@noAlloc`，不能调用可失败清理 API。
- 完整对象销毁时先运行类型自己的 `destroy`，再按源码字段声明逆序销毁字段；部分初始化失败路径不运行外层完整对象 `destroy`。
- v1 不提供 `defer` 关键字。一次性清理用 RAII guard 类型表达，资源兜底清理用 `destroy` 表达。

审计结果：SPEC、SAFETY、IR、TESTING 已一致。

### 1.2 容器与视图

状态：冻结。

- `Array<T>`：拥有、连续、长度固定。
- `Vector<T>`：拥有、连续、可增长。
- `ArraySlice<T>`：非拥有连续片段，不作为字段、集合元素、`static`、逃逸闭包、线程、任务或 async future 状态保存。
- `Map<K, V>`：拥有哈希表，保存唯一 key 到 value 的映射。
- `Set<T>`：拥有哈希表，保存唯一值集合。
- `String`：拥有 UTF-8 文本，可增长。
- `StringSlice`：非拥有 UTF-8 文本访问值。
- `collection[index]` 只是元素访问，不表示移动所有权；非 `Copy` 元素移出必须用 `removeAt`、`swapRemove` 或 `replaceAt`。
- 活跃 slice 存在时，底层 `Vector<T>` 不能结构性修改或替换相关元素。
- `Map` / `Set` 默认不保证遍历顺序，不做每元素堆分配；需要可恢复 OOM 时先 `tryReserve`。

审计结果：SPEC、STDLIB、SAFETY、TESTING 已一致。

### 1.3 泛型与接口

状态：冻结。

- `interface Consumer<T> { ... }` 是标准泛型接口写法。
- 函数参数 `consumer: Consumer<T>` 表示匿名静态接口参数，内部等价于隐藏的 `C: Consumer<T>`。
- 函数返回 `-> Writer` 表示静态接口返回，所有返回路径必须统一到一个具体实现类型。
- 多个裸接口参数默认是独立隐藏具体类型。
- 多个位置必须同一个具体类型时，使用显式泛型名，例如 `W: Writer`。
- 动态异构接口必须显式写 `Box<Writer>`；共享拥有时写 `Shared<Writer>`。
- 不引入 `dyn`，不引入 `some`。
- 一个泛型参数只允许一个直接约束；多能力用命名组合接口，例如 `interface SortKey: Ord, Copy {}`。

审计结果：SPEC、GRAMMAR、IR、STDLIB、SAFETY、TESTING 已一致。

### 1.4 错误、panic 与 OOM

状态：冻结。

- 可恢复错误使用 `Result<T, E>` 和 `Option<T>`。
- `try` 是普通分支和 cleanup，不是异常。
- `panic` 只表示 bug 或不可恢复条件，不用于常规错误流。
- 默认分配 API 失败进入 profile 的 `oom(layout) -> Never`。
- 可恢复 OOM 只在容量预留 API 上提供：`Vector.tryReserve*` 和 `String.tryReserve*`。
- `@noPanic` 和 `@noAlloc` 是成本/失败路径约束。

审计结果：SPEC、STDLIB、MANIFEST、IR、TESTING 已一致。

### 1.5 并发

状态：冻结核心模型，运行时 API 进入 v0.1 子集筛选。

- 没有默认虚拟线程，没有全局 executor。
- `Thread.spawn` 是 OS 线程，显式消耗 `OnceFn<T>`。
- `Runtime` 是可选任务运行时，不是语言强制运行时。
- `async fn` 降低为 future 状态机，创建 future 不自动运行。
- `Send` 表示所有权可跨线程/任务移动。
- `Sync` 表示只读共享可跨线程使用。
- 共享可变状态必须显式经过 `Mutex<T>`、原子类型或同步容器。

审计修复：`std.thread.scope` 命名已统一为 `std.thread.Thread.scope`。

### 1.6 trust 与 FFI

状态：冻结。

- 用户没有 `unsafe` 模式。
- 底层操作使用 `trust` 边界表达“编译器信任此处不变量由作者维护”。
- 裸 C ABI 声明必须写 `trust extern`，extern 调用也必须位于 `trust` 块或受信封装内。
- 普通所有权、初始化、类型、访问、layout 和 `Send` / `Sync` 检查在 `trust` 中仍然生效。
- Manifest 控制 `ffi`、`rawMemory`、`hardware`、`inlineAsm`、`interrupts` 和依赖 trust。
- `trust` 不给优化器额外 alias、非空、对齐、初始化、生命周期或线程安全事实；这些事实必须由显式 API 契约提供。
- `@export("symbol", abi: C)` 是唯一外部符号导出方式。
- 外部 C ABI 只允许 C-compatible 类型；静态接口参数/返回、接口拥有者、闭包和资源拥有者默认不能跨 C ABI。

审计结果：SPEC、SAFETY、FFI、MANIFEST、PACKAGE 已一致。

### 1.7 布局与成本模型

状态：冻结。

- 默认结构体使用 Auto layout，可重排字段以减少 padding。
- `@layout(Source)` 保留源码字段顺序。
- `@layout(C)` 遵守目标 C ABI。
- `@layout(Packed(N))` 用于字节级格式，packed 字段不能逃逸为 `mut` 访问。
- 不提供 `Transparent` / `Compact` / 语言级 endian 属性。
- `CachePadded<T>` 是显式缓存行 padding，普通 Auto layout 不偷偷 padding。

审计结果：LAYOUT、PERFORMANCE、IR、TESTING 已一致。

## 2. 本轮已修复问题

- 并发层级名称统一：`std.thread.Thread.scope`。
- 泛型接口推断、静态接口返回、opaque return metadata 已同步到 SPEC、GRAMMAR、IR、COMPILATION、TESTING。
- `some` / `dyn` 未进入语言表面语法。
- 裸接口参数不再被误判为动态接口对象；只有 `Box<Interface>` / `Shared<Interface>` 产生接口表派发。
- compile / codegen 规格测试头部元数据已检查通过。

## 3. P1 已拍板的问题

这些决策已经进入 [V01_SUBSET.md](V01_SUBSET.md)，作为 stage0 MVP 的第一批实现门禁。

1. `Shared<T>` 完整 runtime 不进入第一批。语言规范保留 `Shared<T>` / `Shared<Interface>` 语义；stage0 MVP 先实现 `Box<T>`、`Box<Interface>` 和静态接口。

2. async 不进入第一批 lowering。语法和安全规则保留；stage0 MVP 可以解析后拒绝 `async fn` / `await`，不实现 executor、task runtime 或 future 状态机。

3. scoped 并发不进入第一批。`Thread.spawn` 的 OS 线程所有权转移与 `Send` 检查进入第一批；`Thread.scope`、scoped thread 和 `splitDisjoint` 的不重叠证明进入第二批。

4. `pub fn -> Interface` 语言层允许。stage0 MVP 先支持同 package 静态接口返回；跨 package opaque return metadata 进入第二批。

5. `Array<T>.clone` / `Vector<T>.clone` 第一批只支持 `T: Copy`。非 `Copy` 深拷贝接口延后，避免过早引入复杂 `Clone` 语义。

6. stage0 package manager 只支持本地 package、workspace、builtin core、path dependency 和 frozen lockfile。registry、git fetch 和发布协议延后。

## 4. 冻结子集入口

stage0 MVP 的详细必做清单、明确延后项和测试门禁见 [V01_SUBSET.md](V01_SUBSET.md)。

核心边界：

- 必做：lexer/parser/AST、module/import/visibility、基础类型、`struct`/`enum`/`interface`/`impl`、所有权、RAII、`destroy`、`Result`/`Option`/`try`、静态泛型、泛型接口参数推断、同 package 静态接口返回、`Box<Interface>` 最小动态派发、核心集合、`String`/`StringSlice`、`trust extern`、`@export`、C-compatible 检查、HIR/MIR/LLVM lowering。
- 延后：完整 `Shared<T>` runtime、async lowering、scoped 并发、跨 package opaque return metadata、registry/git package manager、高级增量缓存、宏、反射、downcast、稳定外部 ABI。

## 5. 审计命令

本轮使用的轻量校验：

```text
git diff --check -- docs tests
python3 -c 'import pathlib, tomllib; files=list(pathlib.Path("tests/spec").glob("**/*.toml")); [tomllib.loads(p.read_text()) for p in files]; print(f"toml ok: {len(files)} files")'
```

结果：

- diff 空白检查通过。
- TOML 规格用例解析通过。
- compile/codegen 测试头部元数据检查通过。
