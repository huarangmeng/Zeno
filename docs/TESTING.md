# Zeno 测试计划

在编译器出现之前，本仓库的测试是可执行规格。每个 `.zn` 文件记录预期行为或预期诊断。

## 1. 测试类别

`tests/spec/compile-pass`：

- 必须通过类型检查和安全分析的程序。
- 除非标记 hosted，否则不应依赖 OS 服务。
- 应保持小而聚焦。

`tests/spec/compile-fail`：

- 必须被拒绝的程序。
- 在出错行附近使用 `// expected-error:` 注释。
- 每个测试应聚焦一个主要失败规则。

`tests/spec/manifest-pass`：

- 必须通过 manifest 校验的 `Zeno.toml` 片段。
- 用于覆盖 hosted、freestanding、kernel / embedded 和自定义 target 策略。

`tests/spec/manifest-fail`：

- 必须被拒绝的 manifest。
- 在出错字段附近使用 `# expected-error:` 注释。
- 每个测试应聚焦一个主要配置错误。

`tests/spec/module-pass`：

- 必须通过包图、模块图、导入解析和可见性检查的多文件包夹具。
- 每个测试目录包含自己的 `Zeno.toml` 和 `src/`。

`tests/spec/module-fail`：

- 必须在模块路径、依赖、导入或可见性阶段被拒绝的多文件包夹具。
- 在出错源码行或 manifest 字段附近使用 `expected-error` 注释。

`tests/spec/package-pass`：

- 必须通过依赖解析、workspace 展开和 lockfile 校验的包管理夹具。
- 每个测试目录可以包含 `Zeno.toml`、`Zeno.lock` 和多个 member 包。

`tests/spec/package-fail`：

- 必须在依赖解析、版本、lockfile 或 trust 审计阶段被拒绝的包管理夹具。
- 在出错 manifest 或 lockfile 行附近使用 `expected-error` 注释。

`tests/spec/incremental-pass`：

- 必须通过增量失效、缓存 key 和诊断重放规则的场景夹具。
- v1 使用 `case.toml` 描述 before/after fingerprint 和期望失效范围。

`tests/spec/incremental-fail`：

- 必须被增量缓存或诊断稳定性规则拒绝的场景夹具。
- 在出错字段附近使用 `expected-error` 注释。

`tests/spec/codegen-pass`：

- 必须满足 HIR、MIR、LLVM 或汇编级降级不变量的场景。
- v1 使用 `case.toml` 描述源码片段、期望 MIR 事实、期望 LLVM 属性和禁止出现的隐藏成本。
- 编译器实现后可以升级为文本 MIR 断言、LLVM FileCheck 风格断言或目标汇编断言。

`tests/spec/codegen-fail`：

- 必须被 codegen verifier 或 IR verifier 拒绝的场景。
- 用于防止错误优化，例如不合法 `noalias`、缺失 cleanup edge、隐藏闭包分配、把 `try` 降低成异常。

未来类别：

- `run-pass`：能编译并产生预期输出的程序。
- `perf`：微基准和 codegen 不变量。
- `stdlib`：最小 core 边界和语言语义所需库 API 的一致性测试；完整标准库 API 测试在语言冻结后单独扩展。

## 2. MVP 与完整规格门禁

测试分两层：

- MVP 门禁：stage0 第一批必须通过的测试，范围见 [V01_SUBSET.md](V01_SUBSET.md)。
- 完整规格门禁：完整 v0.1 设计最终必须通过的测试。

覆盖延后能力的测试仍然保留，例如完整 `Shared<T>` runtime、`Shared<Interface>`、async lowering、scoped 并发、跨 package opaque return metadata、registry/git package manager。这些测试不应删除，只是不进入 stage0 MVP 发布门禁。

测试 runner 出现后，测试可以用头部注释或 `case.toml` 标记：

```text
stage = "mvp"
stage = "full-spec"
feature = "async"
feature = "shared"
```

没有显式标记时，默认按完整规格测试处理。进入 stage0 发布门禁前，再把第一批必须通过的测试补充为 `stage = "mvp"`。

## 3. 必要 compile-pass 覆盖

完整 v0.1 规格测试套件必须包含：

- hosted `Zeno.toml` manifest 可以启用默认 allocator、abort panic 和符号化调用栈。
- freestanding `Zeno.toml` manifest 可以禁用默认 allocator，并把 panic/OOM 降低为 trap。
- kernel / embedded `Zeno.toml` manifest 可以指定 `panic.handler`、`oom.handler`、地址级调用栈和硬件 `trust` 能力。
- `src/net/http.zn` 可以推断为模块路径 `net.http`，显式 `module net.http` 只能作为校验。
- 同 package 内声明不需要 import，可以直接使用。
- 同 package 内同名声明需要用 `net.http.Client` 这类模块限定名消歧。
- `private` 顶层项和字段只在当前文件可见。
- 默认顶层项、类型和字段是 package-visible，外部 package 只能访问 `pub` 项。
- 函数和方法可以按参数数量、参数类型和 `mut` 访问模式形成重载集；只读 / `move` 参数不能单独区分同名重载。
- 重载解析优先选择唯一精确匹配，泛型重载按使用到的具体类型单态化。
- `import core.result.{Result, Ok}` 可以从模块导入多个公开项。
- `import std.io` 可以导入模块绑定，并通过 `io.Error` 访问公开项。
- path dependency 可以提供新的 import 根。
- git dependency 必须指定 commit `rev`，registry dependency 必须指定精确 `version`。
- workspace members 可以共享一个 `Zeno.lock`，同 workspace 依赖优先解析到本地 member。
- `Zeno.lock` 记录 path、git、registry、builtin 依赖的精确解析结果、内容 hash 和 trust 能力摘要。
- frozen 构建可以校验 lockfile，没有隐式更新依赖解析结果。
- 修改非 `pub` 函数体不会使下游 package 失效。
- 修改 `pub` 签名、`@layout(C)` 或 `@export` ABI 会使依赖方失效。
- 并行编译和缓存命中不会改变诊断顺序或错误码。
- 同 package 文件可以在签名中互相引用，且不会执行代码。
- 普通 `struct` 使用默认 Auto layout，`sizeOf`、`alignOf` 和 `offsetOf` 可在编译期求值。
- `const` 可以通过完整 CTFE 执行普通函数、循环、`match`、泛型和静态接口派发。
- 局部 `const` 是编译期绑定，不产生运行期存储。
- `static` 初始化默认通过 CTFE 物化，不执行隐式动态初始化。
- 常量泛型参数可以参与类型、函数和 impl 的单态化。
- `@layout(Source)` 保留源码字段顺序和自然对齐。
- `@layout(C)` 接受 C-compatible 字段、拒绝自定义 `destroy`，并与目标 C ABI 对齐。
- `@layout(Packed(N))` 接受 `U16`、`U32` 等普通 Copy 字段，并通过 unaligned load/store 访问字段。
- 单真实存储字段结构体是零成本包装。
- `Option<Box<T>>`、`Option<Shared<T>>` 和 core/std 句柄类型使用 niche 优化，不增加大小。
- 协议类型通过安全 API 暴露普通整数，端序转换不泄露为用户字段类型。
- `Copy` 值正常复制。
- 普通整数 `+`、`-`、`*` 使用 wrapping 语义。
- `checked*` 整数方法返回 `Option<T>`。
- `saturating*` 和 `overflowing*` 方法语义明确。
- 无损整数转换可以使用 `as`。
- 检查窄化使用 `T.fromChecked(value)`，明确截断使用 `T.truncate(value)`，钳制转换使用 `T.saturate(value)`。
- 非 `Copy` 资源 move 后源绑定失效。
- 非 `Copy` 拥有者普通赋值会移动右侧值；移动后继续使用源绑定必须被拒绝。
- 延迟 `val` 可以在所有路径上恰好初始化一次，初始化后不可重写。
- 延迟 `var` 可以在 move 后重新初始化，也可以覆盖 `maybe-init` 状态；覆盖时旧值条件销毁。
- 聚合值可以通过字段逐步初始化；完整使用前必须所有字段都初始化。
- 对已初始化位置重新赋值会销毁旧值；右侧先求值，失败时左侧保持不变。
- `replaceAt` 返回旧值所有权，不销毁旧值；普通索引赋值销毁旧元素并写入新元素。
- `Array<T>` / `Vector<T>` 的 `clone` 显式表达深拷贝；指定 allocator 时使用 `cloneIn`。
- `Array<T>` / `Vector<T>` / `ArraySlice<T>` 的 `collection[index]` 是元素访问；`Copy` 元素可复制，非 `Copy` 元素可短期读取字段或通过 `mut collection[index]` 修改。
- `Vector<T>.removeAt` 保持顺序并返回被移除元素，`Vector<T>.swapRemove` 不保持顺序但可 O(1) 移除，`replaceAt` 替换元素并返回旧元素。
- `Map<K, V>` 可以用 `HashKey` key 插入、查找、替换和移除 value。
- `Set<T>` 可以用 `HashKey` value 插入、查找、移除和 `take`。
- `Map.tryReserve` 和 `Set.tryReserve` 是可恢复 OOM 的容量预留入口；成功后批量 `insert` 不应重复触发 rehash 慢路径。
- `Map.get(key)` 只为 `V: Copy` 返回 `Option<V>`；非 `Copy` value 使用 checked index 或 `entry` API 做短期访问 / 更新。
- `Map.entry(move key)` 可以单次探测后插入、替换或移除，`MapEntry<K, V>` 不能逃逸；`Copy` key 调用点可以省略 `move`。
- `match value` 只读匹配 enum payload，不移动非 `Copy` payload。
- `match move value` 消耗 enum 并允许移动出被选中 payload。
- `match mut value` 原地可写匹配 enum payload，不取得 payload 所有权。
- `if val` / `while val` 支持只读、`move` 和 `mut` pattern 模式。
- `val` 支持不可失败 tuple / struct 解构。
- pattern 支持 enum、struct、tuple、literal、range、or、guard 和 nested pattern。
- guard 分支不参与穷尽证明。
- `String.clone()` 显式复制拥有文本；`@noAlloc` 中必须被拒绝。
- `Shared<T>.clone` 只表达引用计数复制，成本通过类型名可见。
- 默认参数只读访问非 `Copy` 资源后，调用方仍然拥有该资源。
- `mut` 参数唯一可写访问后，调用方仍然拥有该资源。
- `move` 参数在声明处接收所有权；从已有命名位置传入时调用点必须写 `move`，调用后源绑定不可再用；函数体内的 `move` 参数可以作为 `mut` 接收者或 `mut` 实参使用。
- `self`、`mut self` 和 `move self` 方法接收者语义。
- 从 `move self` 中移出字段后只销毁剩余字段。
- 带 `destroy` 的类型可以用 `close`、`flush` 或 `finish` 这类 `move self` 方法显式完成资源，并通过状态让后续 `destroy` no-op。
- `destroy` 不是 `move self` 方法，不能由用户直接调用。
- `destroy` 先于字段销毁运行，字段按源码声明顺序的逆序销毁。
- `destroy` 隐式满足 `@noPanic`、`@noAlloc`，并且不能 `await`。
- 从参数派生的 `ArraySlice<T>` 可以返回。
- 活跃视图结束后，`Vector<T>` 可以继续结构性修改或元素替换。
- RAII 在所有正常退出路径上销毁。
- 资源类型通过显式 `close`、`flush` 或 `finish` 返回清理错误。
- 部分初始化和清理。
- 通过接口约束静态泛型派发。
- 泛型参数只允许一个直接接口约束；多能力约束必须先定义命名组合接口，例如 `interface SortKey: Ord, Copy {}` 后写 `T: SortKey`。
- 结构体通过泛型字段保存具体接口实现，保持静态派发。
- `Self` 返回或参数形式的接口要求可以通过静态泛型约束调用。
- 接口类型参数例如 `writer: Writer` 是匿名静态接口参数，不生成接口表派发。
- 泛型接口参数例如 `consumer: Consumer<T>` 可以从普通参数和接口实现共同推断 `T`。
- 同一具体类型可以实现同一泛型接口的不同类型实参，例如 `Consumer<Event>` 和 `Consumer<Message>`。
- 多个裸接口参数默认是独立隐藏具体类型；需要同一个具体类型时使用显式 `W: Interface`。
- 接口返回类型例如 `-> Writer` 是静态接口返回，所有返回路径必须统一到同一个具体实现类型。
- 拥有异构接口值必须通过 `Box<Interface>` 或 `Shared<Interface>` 显式表达。
- `Box<T>` 可以移动转换成 `Box<Interface>`，不重新分配。
- `Shared<T>` 可以移动转换成 `Shared<Interface>`，不增加引用计数。
- `Shared<Interface>` 可以调用只读 `self` 接口方法。
- 块闭包使用 `(params) -> T { ... }` 语法。
- 短表达式闭包使用 `(params) => expr` 语法。
- 只读捕获闭包满足 `Fn`，可重复调用。
- 可变捕获闭包满足 `MutFn`，调用时需要 `mut` 访问。
- 消耗捕获闭包满足 `OnceFn`，只能调用一次。
- `Fn` 可以传给要求 `MutFn` 或 `OnceFn` 的 API；`MutFn` 可以传给要求 `OnceFn` 的 API。
- 非逃逸闭包不分配。
- 逃逸闭包显式分配。
- `Result` 通过 `try` 传播。
- `Option` 可以在返回 `Option` 的函数中通过 `try` 传播。
- `Option` 进入 `Result` 错误流必须显式使用 `okOr` 或 `okOrElse`。
- 昂贵错误构造通过 `okOrElse` 延迟到 `None` 路径。
- 昂贵 fallback 通过 `unwrapOrElse` 延迟到 `None` 路径。
- 错误类型转换必须显式使用 `mapErr` 或等价 API。
- `mapErr` 只在 `Err` 路径执行转换。
- `try` 提前返回会执行离开作用域的 RAII 销毁。
- `panic(message) -> Never` 和 `oom(layout) -> Never` 可以出现在任意返回类型位置。
- profile `panicHandler(info: PanicInfo) -> Never` 可以读取 panic 消息、调用点位置和惰性调用栈。
- `PanicInfo.stack()` 可以通过 `for move frame in frames` 无分配遍历 `StackFrame`。
- 默认分配 API 和 `In` 后缀 API 失败时调用 profile 的 `oom(layout)`，普通调用点不写 `try`。
- `for item in data` 只读遍历连续集合，不移动非 `Copy` 元素。
- `for mut item in mut data` 可写遍历连续集合。
- `for move item in data` 消耗遍历拥有集合。
- `for i in 0..data.len` 整数半开区间遍历。
- `Iterator<T>` 可以通过 `for move item in iterator` 做拥有式遍历。
- 字符串 literal 的类型是 `StringSlice`，不分配。
- 拥有字符串必须显式使用 `String.from(text)` 或 `String.fromIn(text, mut allocator)`。
- 默认分配 API 使用 profile 默认 allocator；显式 allocator API 使用 `In` 后缀。
- `Vector.tryReserve`、`Map.tryReserve`、`Set.tryReserve` 和 `String.tryReserve` 是可恢复 OOM 的容量预留入口；成功后普通 `push` / `insert` 不返回 `Result`。
- `tryReserve` 返回 `Result<Unit, AllocError>`，失败时不调用 `oom`，集合内容保持不变。
- scoped allocator 创建的拥有者可以在 allocator 活跃期间局部使用。
- `EscapingAllocator` 创建的拥有者可以按普通所有权规则返回。
- `String.asBytes()` 和 `StringSlice.asBytes()` 可得到只读 `ArraySlice<U8>`。
- `StringSlice.chars()` 返回具体 `Iterator<Char>`。
- 线程所有权转移。
- scoped 线程可以接收来自 `splitDisjoint` 的短期不重叠 `mut` 访问。
- 普通纯数据结构自动满足 `Send`，可以 move 进线程。
- `Mutex<T>` 和 `Shared<Mutex<T>>` 允许跨线程共享可变状态，前提是 `T: Send`。
- `Box<ThreadInterface>` 可以跨线程移动，其中 `ThreadInterface` 继承目标接口和 `Send`。
- `Shared<SharedInterface>` 可以跨线程共享只读接口，其中 `SharedInterface` 继承目标接口、`Send` 和 `Sync`。
- 带 `destroy` 的线程安全资源可以通过 `trust impl Send` 显式声明。
- `trust extern` 和 `trust` 块可以表达显式底层边界。
- `trust` 能力分类包括 `ffi`、`rawMemory`、`hardware`、`inlineAsm`、`interrupts` 和 `threadSafety`，并进入信任报告。
- `@export("symbol", abi: C)` 可以把非泛型顶层 `pub fn` 导出为 C ABI 符号。
- 重载函数可以通过不同 `@export` 符号分别导出。
- C ABI 导入和导出的签名只接受 C-compatible 类型。
- `@export("symbol", bridge: C)` 可以把 bridge-compatible Zeno 签名导出为生成的 C ABI thunk。
- C bridge 支持 C-compatible 类型、`ArraySlice<T>` / `mut ArraySlice<T>`、`StringSlice`、`Option<T>` 返回和 `Result<T, E>` 返回。
- C bridge 的 `Result<T, E>` 错误类型必须显式映射为 C 错误码。
- 导出函数在 abort / trap profile 下不会让 panic unwind 穿过 C ABI。
- 可选任务运行时通过 `Runtime.spawn(future)` 消费 async future，并返回 `Task<T>`。
- 命名 future 传给 `Runtime.spawn` 时必须写 `move future`；临时 async 调用可以直接传入。
- `Runtime.spawnBlocking` 把同步阻塞工作路由到独立 blocking pool，并返回 `Task<T>`。
- `Runtime.spawnWithContext` 和 `Runtime.spawnBlockingWithContext` 能显式传入 `TaskContext`。
- `TaskGroup<T>` 可以启动多个 future，并通过 `joinAll` / `tryJoinAll` 结构化收尾。
- `TaskGroup.next` / `tryNext` 可以按完成顺序流式消费结果。
- `await task` 消费 `Task<T>` 句柄并返回任务结果。
- 同步入口可以通过 `mut runtime.blockOn(move task)` 或 `mut executor.blockOn(move future)` 显式阻塞等待。
- `Task.cancel` 和 `Task.detach` 显式消费并收尾任务句柄。
- 对 `spawnBlocking` 返回的任务调用 `cancel` 只表达协作取消，不隐藏强杀或阻塞等待。
- 声明 `TaskContext` 参数的任务可以调用 `ctx.isCancellationRequested()` 检查取消请求。
- `spawnBlocking` 任务可以在阻塞调用之间检查 `TaskContext`，但不能把检查伪装成强杀阻塞调用。
- async future 状态所有权。
- future drop 会清理当前状态中已初始化字段和跨 `await` 的 RAII guard。
- 立即 `await mut receiver.method()` 可以用于 future 拥有的接收者。
- 共享可变状态只能通过同步类型访问。

## 4. 必要 compile-fail 覆盖

完整 v0.1 规格测试套件必须拒绝：

- 未知 manifest 字段。
- 缺少 `target.profile` 或未知 profile。
- `panic.strategy = "handler"` 但没有 `panic.handler`。
- `oom.strategy = "handler"` 但没有 `oom.handler`。
- `allocator.default = true` 但没有可解析默认 allocator。
- hosted manifest 启用硬件、inline asm 或中断能力。
- `[dependencies]` key 不是合法 Zeno 标识符。
- dependency key 和当前包 `src/` 顶层目录冲突。
- 包依赖图成环。
- git dependency 缺少 commit `rev`，或只写 branch/tag/floating ref。
- registry dependency 使用开放版本范围而不是精确版本。
- lockfile 缺失、过期、内容 hash 不匹配或记录了机器绝对路径。
- lockfile 中依赖 trust 能力扩大但构建策略禁止依赖 trust。
- workspace 有同名 member。
- 缓存 key 缺少 manifest、lockfile、target、profile、panic/OOM、trust 或 compiler identity 这类安全输入。
- 文件路径和 `module` 声明不匹配。
- 导入未在 `[dependencies]` 中声明的外部包根。
- 从外部包导入非 `pub` 项。
- 在其他文件中使用 `private` 项。
- `private` 类型出现在非 `private` API 签名中。
- package-visible 类型出现在 `pub` API 签名中。
- 同 package 内多个声明同名时使用未限定名字。
- 两个导入在同一作用域引入不同定义的同名项。
- 两个函数只靠返回类型不同形成重载。
- 两个函数或方法拥有相同重载键。
- 两个重载只靠同一位置的只读参数和 `move` 参数区分。
- 重载调用没有唯一最佳候选，例如整数字面量同时匹配多个数值类型候选。
- 同一个结构体声明多个 layout 策略。
- `@layout(...)` 标在非结构体声明上。
- `@layout(C)` 结构体包含非 C-compatible 字段，例如 `Vector<T>`、`String`、接口拥有者或闭包。
- `@layout(C)` 结构体带自定义 `destroy`。
- `@export` 或 `trust extern` 签名使用 `Char`、普通 enum、`Option<T>`、`Result<T, E>`、普通 struct 或未标注 `@layout(C)` 的零成本包装。
- `@export(..., bridge: C)` 签名使用 `String`、`Vector<T>`、`Box<T>`、`Shared<T>`、接口拥有者、闭包或带 `destroy` 的资源拥有者。
- `@export(..., bridge: C)` 的 `Result<T, E>` 中 `E` 没有错误码映射，或 `T` 不是 bridge 可返回的 C-compatible 类型。
- `@export(..., bridge: C)` 需要生成隐藏分配、隐藏复制、隐藏释放或动态派发 thunk。
- v1 中直接请求 C++ bridge / C++ bindgen 语言语义；诊断应说明 C++ 路线预留但不属于 v1。
- `@layout(Packed(N))` 的 `N` 不是 1、2、4、8 或 16。
- `@layout(Packed(N))` 结构体包含非 `Copy` 字段、拥有资源字段或带 `destroy` 的字段。
- packed 字段作为 `mut` 访问或长期访问逃逸。
- use after move。
- double move。
- 非 `Copy` 拥有者普通赋值造成隐式复制。
- Freestanding profile 没有默认 allocator 时调用无 `In` 后缀的分配 API。
- scoped allocator 创建的拥有者从函数返回。
- scoped allocator 创建的拥有者保存到长期结构体字段、`static`、`Box`、`Shared`、逃逸闭包、非 scoped 线程、任务或 async future 状态。
- 泛型函数返回 allocator 创建的拥有者但只约束 `A: Allocator`，没有要求 `A: EscapingAllocator`。
- 把默认只读参数当成拥有值返回或保存。
- 在不可写接收者上调用 `mut self` 方法。
- 存在活动只读访问或可写访问时覆盖对应局部变量、字段或容器元素。
- 调用 `move self` 方法后继续使用接收者。
- 从 `self` 或 `mut self` 中移出字段。
- 从带 `destroy` 块的类型中移出非 `Copy` 字段，包括外部函数和类型自己的 `move self` 方法。
- 移出字段后继续读取该字段或把原对象当作完整值使用。
- 直接调用 `destroy`。
- `destroy` 中使用 `try`。
- `destroy` 中使用 `await`。
- `destroy` 中直接或间接调用 `panic`。
- `destroy` 中调用会分配或可能 OOM 的 API。
- `destroy` 调用返回 `Result` 的失败 API。
- 命名非 `Copy` owner 传给 `move` 参数时漏写调用点 `move`。
- 读取未初始化值。
- `val` 在可能已初始化、已经初始化或移动后重新赋值。
- `val` / `var` 只在部分控制流路径初始化后被读取或返回。
- 部分初始化聚合在缺少字段时被读取、返回或移动。
- 返回局部存储的访问值。
- `ArraySlice<T>` 作为结构体字段。
- `ArraySlice<T>` 进入 `Box<T>`、`Shared<T>`、`Array<T>`、`Vector<T>`、`static`、逃逸闭包、线程、任务或 async future。
- 活跃 `ArraySlice<T>` 存在时，对底层 `Vector<T>` 做结构性修改或元素替换。
- 活跃 `ArraySlice<T>` 存在时，对底层 `Vector<T>` 调用 `reserve`、`reserveExact`、`tryReserve` 或 `tryReserveExact`。
- 把非 `Copy` 的 `collection[index]` 放进拥有位置，试图通过索引移出元素。
- 对非 `Copy` 集合调用 `get(index) -> Option<T>`。
- 对 `Map<K, V>` 在 `V` 不是 `Copy` 时调用 `get(key) -> Option<V>`。
- `MapEntry<K, V>` 保存到结构体、集合、`static`、`Box`、`Shared`、逃逸闭包、线程、任务或 async future。
- 作为 `Map` key 或 `Set` value 的类型没有实现 `HashKey`。
- 非 `Copy` payload 在只读 `match` 中被移动、返回或逃逸。
- `match move` 后继续使用原 enum。
- `match mut` 作用于不可写 enum。
- `match` 非穷尽且没有 `_` 分支。
- 旧式 `let` 局部绑定语法；不可变绑定必须使用 `val`，可变绑定必须使用 `var`。
- `defer` 语句；作用域清理必须使用 RAII guard 或 `destroy`。
- `val` 使用可失败 pattern，例如多 variant enum 的 `Some(x)`。
- or pattern 两侧绑定名字、类型或访问模式不同。
- 带 guard 的分支被当成穷尽覆盖。
- 明显不可达的无 guard pattern 分支。
- 字符串、regex、downcast、extractor 或 collection tail pattern。
- 带 `destroy` 的类型通过 pattern 解构移动出非 `Copy` 字段或 payload。
- `for mut item in data` 缺少右侧 `mut` 可写访问。
- `for item in mut data` 中修改只读循环元素。
- `for move item in slice` 试图消耗遍历非拥有 `ArraySlice<T>`。
- 消耗遍历后继续使用被 `move` 的集合。
- 对 `Iterator<T>` 使用非消耗 `for item in iterator` 遍历。
- 命名非 `Copy` 迭代器作为 `for move` 源后继续使用原迭代器。
- `val text: String = "literal"` 这类通过类型标注隐式构造拥有字符串。
- `StringSlice` 作为结构体字段、集合元素、`static`、逃逸闭包捕获、线程任务捕获或 async future 状态。
- `@noAlloc` 中调用 `String.from`、`String.fromIn`、`String.push`、`String.reserve`、`String.tryReserve`、`String.clone`、`cloneIn`、集合/Box/Shared 分配 API 或会分配的格式化 API。
- `@noPanic` 中直接或间接调用 `panic`。
- 当前 profile 把 OOM 配置成 panic 时，`@noPanic` 中调用可能分配的 API。
- 把 `PanicInfo`、`StackFrames` 或其他 panic 诊断访问值保存到结构体、集合、`static`、逃逸闭包、线程、任务或 async future 中。
- 可写访问与只读访问重叠。
- 可写访问逃逸到非 scoped 线程。
- scoped 线程中通过普通运行期索引产生多个可写访问，且没有 disjoint API 证明不重叠。
- 裸接口类型作为结构体字段、枚举载荷、元组字段、`static` 或集合元素使用。
- 裸接口类型被逃逸闭包、线程、任务或 async future 捕获。
- 泛型接口参数只靠接口实现无法唯一推断类型参数。
- 重复或重叠的接口实现实例。
- 要求同一个显式泛型实现类型的位置传入了不同具体类型。
- `-> Interface` 静态接口返回的不同 `return` 路径产生不同具体实现类型。
- 把 `OnceFn` 传给要求 `Fn` 或 `MutFn` 的 API。
- 把 `MutFn` 传给要求 `Fn` 的 API。
- 通过只读访问调用 `MutFn`。
- 调用 `OnceFn` 后继续使用该闭包。
- 通过 `Shared<Interface>` 调用 `mut self` 接口方法。
- 把非 `Send` 类型移动进 `Thread.spawn`、多线程任务或可跨线程 future。
- `spawnBlocking` 调用漏写 `move job`。
- `spawnBlocking` 捕获或返回非 `Send` 类型。
- `Task.await()` 方法调用；任务等待必须写成 `await task`。
- `await task` 后继续使用同一个任务句柄。
- `blockOn` 调用漏写 `move task` / `move future`。
- async 上下文中调用 `blockOn`，应使用 `await`。
- `Task<T>` 句柄离开作用域前没有通过 `await`、`blockOn`、`cancel` 或 `detach` 显式收尾。
- `Task.cancel` 或 `Task.detach` 调用漏写 `move task`。
- `TaskGroup<T>` 离开作用域前没有通过 `joinAll`、`tryJoinAll`、`cancelRemaining` 或完整 drain 收尾。
- `TaskGroup.next` / `tryNext` 循环提前退出后没有 `cancelRemaining` 或消费 group。
- `TaskContext` 返回、保存进结构体/集合/`Box`/`Shared`/`static`，或被线程、子任务、逃逸闭包捕获。
- 使用隐藏全局当前任务查询来检查取消，例如 `Task.isCancellationRequested()`。
- `Box<Interface>` 在缺少 `Send` 能力时跨线程移动。
- `Shared<T>` 在 `T` 不是 `Send, Sync` 时跨线程移动或共享。
- 普通 `impl Send for T {}` 或 `impl Sync for T {}`，没有 `trust` 边界。
- 通过 `Box<Interface>` / `Shared<Interface>` 动态调用返回 `Self` 或接收 `Self` 普通参数的方法。
- 通过 `Box<Interface>` / `Shared<Interface>` 动态调用带方法级泛型参数的方法。
- 通过 `Box<Interface>` / `Shared<Interface>` 动态调用 `move self` 方法。
- 没有 `trust` 的裸 FFI 声明或底层操作。
- manifest 未允许对应能力时使用 `trust extern`、硬件访问、inline asm 或中断入口。
- manifest 未允许 `rawMemory` 时构造、偏移、解引用裸指针或执行按位重解释。
- `trust extern` 声明后，在普通代码中直接调用 extern 函数。
- `@export` 标在非 `pub` 函数、泛型函数、方法或非函数声明上。
- 两个导出使用同一个外部符号名。
- `@export(..., abi: C)` 签名包含非 C-compatible 类型，例如 `String`、`Vector<T>`、`ArraySlice<T>`、接口或闭包。
- `@export(..., abi: C)` 签名暴露非 `pub` 命名结构体类型。
- `@export(..., bridge: C)` 签名包含非 bridge-compatible 类型，或桥接 `Result` 的错误类型没有实现 `CErrorCode`。
- unwind profile 下导出函数可能 panic 且没有 `@noPanic` 或等价边界策略。
- `unsafe` 语法。
- no-allocation 上下文中的隐藏堆分配。
- 常量越界索引。
- 常量除零、取模零和非法移位。
- `const` 初始化环或 `static` 初始化环。
- CTFE 中调用 `trust`、extern、裸内存、硬件访问、inline asm、线程、任务、`await`、当前时间、随机数、环境变量、网络或未声明文件输入。
- 需要运行期拥有者时，直接把编译期 `String`、`Array<T>`、`Vector<T>`、`Map<K, V>` 或 `Set<T>` 当作运行期拥有者使用，隐藏分配或复制成本。
- 常量泛型实参无法得到稳定 fingerprint，例如含有运行期地址身份或非结构化拥有者。
- 隐式动态 `static` 初始化。
- 用 `as` 执行可能截断或改变符号含义的整数转换。
- 在返回 `Result` 的函数中直接 `try Option<T>`。
- 对错误类型不同的 `Result<T, E>` 直接使用 `try`。
- `trust` 边界中违反普通所有权、初始化或类型规则。
- 非拥有访问值作为独立值跨 `await` 进入 future 状态。
- async `mut self` 调用产生的访问 future 被保存、返回、传给 `spawn` 或跨另一个 `await`。
- 析构或 future drop cleanup 中依赖 `await` 完成异步清理。

## 5. 诊断格式

推荐诊断形状：

```text
error[E0201]: 使用了已移动的值 `file`
  --> tests/spec/compile-fail/001_use_after_move.zn:8:5
   |
 7 |     val owner = file;
   |                 ---- 值在这里被移动
 8 |     mut file.write("late");
   |     ^^^^ 移动后继续使用
help: 在移动前读取该值，或只移动一次
```

编译器出现后，诊断应使用稳定错误码。

## 6. 性能测试

性能验收应包含源码级和 codegen 级检查：

- 泛型 `max<T: SortKey>` 应为使用到的具体类型生成专门函数。
- 函数重载应在类型检查期解析为直接调用，不生成重载表、名字查找或动态派发。
- Auto layout 应减少典型结构体 padding；布局结果应可在编译期报告 size、align 和 offset。
- `@layout(Source)` 不应重排字段。
- 单字段包装在 Zeno 内部 ABI 中不应增加传参、返回或存储成本。
- `Option<Box<T>>`、`Option<Shared<T>>` 和 core/std 句柄类型不应额外存储 tag。
- 协议 API 的端序转换应降低为目标端序相关的 bswap 或 no-op，不分配。
- packed 字段读取应降低为目标支持的 unaligned load 或等价字节 load 序列，不产生未定义行为。
- 普通整数 `+`、`-`、`*` 不应生成隐藏溢出检查。
- `checkedAdd` 应降低为目标支持的 overflow intrinsic 或等价高效指令序列。
- `U8.truncate(value)` 应降低为截断，不生成范围检查。
- `U8.saturate(value)` 应降低为比较/选择或目标饱和指令，不分配、不 trap。
- `try` 应降低为普通分支和清理边，不生成异常或堆分配。
- `try` 的提前返回边必须执行离开作用域的 RAII cleanup。
- `panic` / `oom` 应降低为 `noreturn` 控制流终点；abort / trap profile 不应生成 unwind 清理路径。
- `panic` 调用点位置应由编译器注入；调用栈采集应只出现在 panic 冷路径，正常路径不分配、不登记栈帧对象。
- `StackFrames` 遍历应支持只输出 instruction pointer；符号化是 profile 诊断能力，不是语言运行时必需成本。
- panic-unwind profile 只在显式启用时生成栈展开和 RAII 清理路径。
- 完整对象 drop 必须先运行类型自己的 `destroy`，再按源码声明逆序 drop 字段；部分初始化失败路径不能运行外层完整对象的 `destroy`。
- `tryReserve` 应降低为普通 `Result` 分支；普通 `push` 在容量已证明足够时不应重复生成分配慢路径。
- `tryReserve` 成功后，在已证明容量范围内的循环 `push` 不应生成 per-iteration reserve / grow 慢路径。
- `Map` / `Set` 默认实现不应为每个元素生成单独 allocator call。
- `Map.tryReserve` / `Set.tryReserve` 成功后，在已证明容量范围内的循环 `insert` 不应生成 per-iteration rehash 慢路径。
- `Map.entry` 更新应只做一次查找 / 探测，不应降低为 `containsKey` 加 `insert` 的双重哈希。
- `Map` / `Set` 遍历不应分配 iterator 对象，也不应承诺稳定顺序。
- 已初始化非 `Copy` place 的赋值应先完成 RHS，再在成功边 drop 旧值并写入新值；RHS 失败边不能 drop 左侧旧值。
- `maybe-init` 的 `var` 赋值应在 RHS 成功边条件 drop 旧值，RHS 失败边保持原 drop flag。
- 延迟初始化聚合的字段 drop flag 应只清理已初始化字段；完整对象 `destroy` 不能在部分初始化失败边运行。
- `replaceAt` 应移动出旧元素并返回，不能生成旧元素 drop。
- `match` 应降低为 tag switch 或等价分支，不分配、不动态派发。
- `match move` 中已移动 payload 的 drop flag 必须清除，未移动 payload 必须在分支 cleanup 中销毁。
- `if val` / `while val` pattern 应降低为普通分支 / 循环头，不分配。
- literal、range 和 or pattern 应降低为比较、tag switch 或分支合并，不创建 pattern 对象。
- guard 应降低为 pattern 成功后的条件分支，guard 失败继续尝试后续分支。
- `okOrElse`、`unwrapOrElse` 和 `mapErr` 的 `OnceFn` 非逃逸闭包应内联成条件分支；成功路径不得构造失败路径值。
- `for item in data`、`for mut item in mut data` 和 `for move item in data` 应对核心连续集合降低为直接循环，不分配 iterator、不做接口派发。
- 具体 `Iterator<T>` 的 `for move` 遍历应静态派发并可内联 `next`；只有显式接口拥有者才允许动态派发。
- 字符串 literal 和 `StringSlice` 传参不应分配。
- `for i in 0..data.len` 形式的 `ArraySlice<T>` / `Array<T>` / `Vector<T>` 循环优化后不应包含冗余边界检查。
- 传给泛型函数的非逃逸闭包不应分配。
- 非逃逸闭包环境应栈上分配或被标量替换，不应生成 `Box`、`Shared` 或 allocator call。
- `mut` 参数只有在唯一且不逃逸时才可以生成 LLVM `noalias`；可逃逸或可能重叠时不能生成 `noalias`。
- 只读访问只有在不写且不逃逸时才可以生成 `readonly` / `nocapture`。
- 不同 `MemoryObject`、独立字段或可证明不重叠 range 应生成 alias scope 或等价后端信息。
- allocator 泛型调用应默认静态派发；零大小 allocator 不应增加拥有者大小。
- 非逃逸 future 不应分配；逃逸 future 的分配必须由 `Box`、task handle 或 runtime spawn 显式表达。
- future drop 应按状态销毁已初始化字段，不能遗漏 cleanup，也不能 drop 未初始化字段。
- scoped allocator owner 的 allocation region 应进入 HIR/MIR 逃逸检查，不能只靠 codegen 假设。
- `CachePadded<T>` 应增加显式 cache line padding；普通 Auto layout 不应隐藏插入同级 padding。
- `Writer` 接口类型参数调用不应包含接口表派发；它应像匿名 `W: Writer` 一样单态化。
- 显式 `W: Writer` 调用不应包含接口表派发。
- 泛型接口参数推断后应按具体实现单态化，不生成接口表或运行时类型查询。
- 静态接口返回应按具体返回类型直接返回，不生成 `Box`、allocator call 或接口表构造。
- 移动 `Box<T>` 应降低为指针转移，而不是深拷贝或引用计数。
- `Box<T>` 到 `Box<Interface>` 的移动转换不应重新分配，也不应移动堆内值。
- `Shared<T>` 到 `Shared<Interface>` 的移动转换不应增加引用计数。
- `Box<Interface>` 的销毁应为一次接口表销毁调用加一次 allocator deallocate。
- `Shared<Interface>.clone` 应为一次引用计数增加，不深拷贝具体值。
- `Send` / `Sync` 推导应在类型检查期完成，不需要运行时标记或查询。
- `Box<ThreadWriter>` 与 `Box<Writer>` 的运行时布局成本相同，只是类型系统保留了额外能力证明。
- `trust` 不能让优化器凭空推导 `noalias`、非空、对齐、初始化或生命周期事实；裸指针默认按未知 provenance / 未知别名关系处理。
- `@export(..., abi: C)` 应按目标 C ABI 生成外部符号，不使用 Zeno 内部 mangling。
- `@layout(C)` 的 field offsets、size、align 和 by-value aggregate passing ABI 必须来自目标 C ABI，并进入 ABI fingerprint。
- CTFE 结果应在 HIR/MIR 前稳定，可被布局、常量泛型、属性和 codegen 复用。
- CTFE 使用编译期 arena 不应产生运行期堆分配、引用计数或动态派发。
- 常量泛型 value fingerprint 必须进入 mono key、layout key 和增量缓存 key。
- `@export(..., bridge: C)` 应生成一个 C ABI thunk，原函数继续使用 Zeno ABI；slice 降为 pointer + length，`Result<T, E>` 降为 status + out 参数。
- C bridge thunk 不应包含堆分配、引用计数、动态派发、字符串复制或 Zeno 资源布局暴露。
- `bindgen c` 的 header hash、include path、宏定义、target triple、目标 C ABI、Clang 资源目录和生成器版本应进入增量缓存 key。
- `@layout(Packed(N))` 字段读取应在 MIR/LLVM 中标记为 unaligned load，不能伪造成自然对齐字段访问。
- 修改函数体的增量构建不应重新检查无关 package；修改 `pub` API 应只失效实际依赖该 API 的下游。
- 并行 parse、body check、monomorphization 和 codegen 不应改变最终诊断顺序。
- `Array<T>.clone` / `Vector<T>.clone` 的分配在源码中可见。
- 普通赋值非 `Copy` 拥有者不应生成深拷贝。

## 7. 测试文件约定

顶部注释：

```zn
// category: compile-pass
// profile: freestanding
// purpose: verifies non-Copy ownership transfer is diagnosed after move
```

测试注释可以模拟 manifest 字段：

```zn
// allocator.default: false
// panic.strategy: trap
// oom.strategy: trap
```

真实包构建只读取 `Zeno.toml` 或等价命令行参数，不读取源码注释。

compile-fail 主错误：

```zn
// expected-error: use of moved value
```

manifest-fail 主错误：

```toml
strategy = "handler" # expected-error: oom.handler is required
```

module-fail 主错误：

```zn
val client: Client = net.http.connect(); // expected-error: unqualified name Client is ambiguous
```

测试 harness 应把这些注释当作断言。

codegen-pass 主断言：

```toml
[expect.mir]
contains = ["boundsCheck", "cleanup"]
forbid = ["heapAlloc"]

[expect.llvm]
paramAttrs = ["noalias", "nocapture"]
forbid = ["invoke"]
```

codegen-fail 主错误：

```toml
[expect]
error = "noalias requires proven unique non-escaping access"
```
