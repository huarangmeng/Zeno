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

未来类别：

- `run-pass`：能编译并产生预期输出的程序。
- `codegen`：检查生成 IR 或汇编的测试。
- `perf`：微基准和 codegen 不变量。
- `stdlib`：core 和 std API 一致性测试。

## 2. 必要 compile-pass 覆盖

第一版编译器测试套件必须包含：

- hosted `Zeno.toml` manifest 可以启用默认 allocator、abort panic 和符号化调用栈。
- freestanding `Zeno.toml` manifest 可以禁用默认 allocator，并把 panic/OOM 降低为 trap。
- kernel / embedded `Zeno.toml` manifest 可以指定 `panic.handler`、`oom.handler`、地址级调用栈和硬件 `trust` 能力。
- `Copy` 值正常复制。
- 普通整数 `+`、`-`、`*` 使用 wrapping 语义。
- `checked*` 整数方法返回 `Option<T>`。
- `saturating*` 和 `overflowing*` 方法语义明确。
- 无损整数转换可以使用 `as`。
- 检查窄化使用 `T.fromChecked(value)`，明确截断使用 `T.truncate(value)`，钳制转换使用 `T.saturate(value)`。
- 非 `Copy` 资源 move 后源绑定失效。
- 非 `Copy` 拥有者普通赋值必须被拒绝，除非显式 `move`。
- `Array<T>` / `Vector<T>` 的 `clone` 显式表达深拷贝；指定 allocator 时使用 `cloneIn`。
- `String.clone()` 显式复制拥有文本；`@noAlloc` 中必须被拒绝。
- `Shared<T>.clone` 只表达引用计数复制，成本通过类型名可见。
- 默认参数只读访问非 `Copy` 资源后，调用方仍然拥有该资源。
- `mut` 参数唯一可写访问后，调用方仍然拥有该资源。
- `move` 参数必须由调用点显式 `move` 传入。
- `self`、`mut self` 和 `move self` 方法接收者语义。
- 从 `move self` 中移出字段后只销毁剩余字段。
- 带 `destroy` 的类型可以在自己的 `move self` 方法中提供官方拆解 API。
- 从参数派生的 `ArraySlice<T>` 可以返回。
- 活跃视图结束后，`Vector<T>` 可以继续结构性修改。
- RAII 在所有正常退出路径上销毁。
- 资源类型通过显式 `close`、`flush` 或 `finish` 返回清理错误。
- 部分初始化和清理。
- 通过接口约束静态泛型派发。
- 泛型和接口继承的多约束使用逗号，例如 `T: Ord, Copy` 和 `interface Worker: Send, Sync`。
- 结构体通过泛型字段保存具体接口实现，保持静态派发。
- `Self` 返回或参数形式的接口要求可以通过静态泛型约束调用。
- 通过接口类型参数进行接口派发。
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
- `try` 提前返回会执行离开作用域的 `defer` 和 RAII 销毁。
- `panic(message) -> Never` 和 `oom(layout) -> Never` 可以出现在任意返回类型位置。
- profile `panicHandler(info: PanicInfo) -> Never` 可以读取 panic 消息、调用点位置和惰性调用栈。
- `PanicInfo.stack()` 可以通过 `for move frame in move frames` 无分配遍历 `StackFrame`。
- 默认分配 API 和 `In` 后缀 API 失败时调用 profile 的 `oom(layout)`，普通调用点不写 `try`。
- `for item in data` 只读遍历连续集合，不移动非 `Copy` 元素。
- `for mut item in mut data` 可写遍历连续集合。
- `for move item in move data` 消耗遍历拥有集合。
- `for i in 0..data.len` 整数半开区间遍历。
- `Iterator<T>` 可以通过 `for move item in move iterator` 做拥有式遍历。
- 字符串 literal 的类型是 `StringSlice`，不分配。
- 拥有字符串必须显式使用 `String.from(text)` 或 `String.fromIn(text, mut allocator)`。
- 默认分配 API 使用 profile 默认 allocator；显式 allocator API 使用 `In` 后缀。
- `Vector.tryReserve` 和 `String.tryReserve` 是可恢复 OOM 的容量预留入口；成功后普通 `push` 不返回 `Result`。
- `tryReserve` 返回 `Result<Unit, AllocError>`，失败时不调用 `oom`，集合内容保持不变。
- `String.asBytes()` 和 `StringSlice.asBytes()` 可得到只读 `ArraySlice<U8>`。
- `StringSlice.chars()` 返回具体 `Iterator<Char>`。
- 线程所有权转移。
- 普通纯数据结构自动满足 `Send`，可以 move 进线程。
- `Mutex<T>` 和 `Shared<Mutex<T>>` 允许跨线程共享可变状态，前提是 `T: Send`。
- `Box<ThreadInterface>` 可以跨线程移动，其中 `ThreadInterface` 继承目标接口和 `Send`。
- `Shared<SharedInterface>` 可以跨线程共享只读接口，其中 `SharedInterface` 继承目标接口、`Send` 和 `Sync`。
- 带 `destroy` 的线程安全资源可以通过 `trust impl Send` 显式声明。
- `trust extern` 和 `trust` 块可以表达显式底层边界。
- 可选任务运行时的所有权转移。
- async future 状态所有权。
- 共享可变状态只能通过同步类型访问。

## 3. 必要 compile-fail 覆盖

第一版编译器测试套件必须拒绝：

- 未知 manifest 字段。
- 缺少 `target.profile` 或未知 profile。
- `panic.strategy = "handler"` 但没有 `panic.handler`。
- `oom.strategy = "handler"` 但没有 `oom.handler`。
- `allocator.default = true` 但没有可解析默认 allocator。
- hosted manifest 启用硬件、inline asm 或中断能力。
- use after move。
- double move。
- 非 `Copy` 拥有者普通赋值造成隐式复制。
- Freestanding profile 没有默认 allocator 时调用无 `In` 后缀的分配 API。
- 把默认只读参数当成拥有值返回或保存。
- 在不可写接收者上调用 `mut self` 方法。
- 调用 `move self` 方法后继续使用接收者。
- 从 `self` 或 `mut self` 中移出字段。
- 从带 `destroy` 块的类型外部移出字段。
- 移出字段后继续读取该字段或把原对象当作完整值使用。
- `destroy` 中使用 `try`。
- `destroy` 调用返回 `Result` 的失败 API。
- 读取未初始化值。
- 返回局部存储的访问值。
- `ArraySlice<T>` 作为结构体字段。
- `ArraySlice<T>` 进入 `Box<T>`、`Shared<T>`、`Array<T>`、`Vector<T>`、`static`、逃逸闭包、线程、任务或 async future。
- 活跃 `ArraySlice<T>` 存在时，对底层 `Vector<T>` 做结构性修改。
- 活跃 `ArraySlice<T>` 存在时，对底层 `Vector<T>` 调用 `reserve`、`reserveExact`、`tryReserve` 或 `tryReserveExact`。
- `for mut item in data` 缺少右侧 `mut` 可写访问。
- `for item in mut data` 中修改只读循环元素。
- `for move item in move slice` 试图消耗遍历非拥有 `ArraySlice<T>`。
- 消耗遍历后继续使用被 `move` 的集合。
- 对 `Iterator<T>` 使用非消耗 `for item in iterator` 遍历。
- 命名非 `Copy` 迭代器作为 `for move` 源时缺少右侧 `move`。
- `let text: String = "literal"` 这类通过类型标注隐式构造拥有字符串。
- `StringSlice` 作为结构体字段、集合元素、`static`、逃逸闭包捕获、线程任务捕获或 async future 状态。
- `@noAlloc` 中调用 `String.from`、`String.fromIn`、`String.push`、`String.reserve`、`String.tryReserve`、`String.clone`、`cloneIn`、集合/Box/Shared 分配 API 或会分配的格式化 API。
- `@noPanic` 中直接或间接调用 `panic`。
- 当前 profile 把 OOM 配置成 panic 时，`@noPanic` 中调用可能分配的 API。
- 把 `PanicInfo`、`StackFrames` 或其他 panic 诊断访问值保存到结构体、集合、`static`、逃逸闭包、线程、任务或 async future 中。
- 可写访问与只读访问重叠。
- 可写访问逃逸到非 scoped 线程。
- 裸接口访问类型作为结构体字段、枚举载荷、元组字段、`static` 或集合元素长期保存。
- 裸接口访问类型被逃逸闭包、线程、任务或 async future 捕获。
- 把 `OnceFn` 传给要求 `Fn` 或 `MutFn` 的 API。
- 把 `MutFn` 传给要求 `Fn` 的 API。
- 通过只读访问调用 `MutFn`。
- 调用 `OnceFn` 后继续使用该闭包。
- `move writer: Interface` 这类接收裸接口访问所有权的参数。
- 通过 `Shared<Interface>` 调用 `mut self` 接口方法。
- 把非 `Send` 类型移动进 `Thread.spawn`、多线程任务或可跨线程 future。
- `Box<Interface>` 在缺少 `Send` 能力时跨线程移动。
- `Shared<T>` 在 `T` 不是 `Send, Sync` 时跨线程移动或共享。
- 普通 `impl Send for T {}` 或 `impl Sync for T {}`，没有 `trust` 边界。
- 通过接口访问动态调用返回 `Self` 或接收 `Self` 普通参数的方法。
- 通过接口访问动态调用带方法级泛型参数的方法。
- 通过接口访问动态调用 `move self` 方法。
- 没有 `trust` 的裸 FFI 声明或底层操作。
- manifest 未允许对应能力时使用 `trust extern`、硬件访问、inline asm 或中断入口。
- `unsafe` 语法。
- no-allocation 上下文中的隐藏堆分配。
- 常量越界索引。
- 常量除零、取模零和非法移位。
- 用 `as` 执行可能截断或改变符号含义的整数转换。
- 在返回 `Result` 的函数中直接 `try Option<T>`。
- 对错误类型不同的 `Result<T, E>` 直接使用 `try`。
- `trust` 边界中违反普通所有权、初始化或类型规则。

## 4. 诊断格式

推荐诊断形状：

```text
error[E0201]: 使用了已移动的值 `file`
  --> tests/spec/compile-fail/001_use_after_move.zn:8:5
   |
 7 |     let owner = move file;
   |                 --------- 值在这里被移动
 8 |     file.write("late");
   |     ^^^^ 移动后继续使用
help: 在移动前读取该值，或只移动一次
```

编译器出现后，诊断应使用稳定错误码。

## 5. 性能测试

性能验收应包含源码级和 codegen 级检查：

- 泛型 `max<T: Ord, Copy>` 应为使用到的具体类型生成专门函数。
- 普通整数 `+`、`-`、`*` 不应生成隐藏溢出检查。
- `checkedAdd` 应降低为目标支持的 overflow intrinsic 或等价高效指令序列。
- `U8.truncate(value)` 应降低为截断，不生成范围检查。
- `U8.saturate(value)` 应降低为比较/选择或目标饱和指令，不分配、不 trap。
- `try` 应降低为普通分支和清理边，不生成异常或堆分配。
- `panic` / `oom` 应降低为 `noreturn` 控制流终点；abort / trap profile 不应生成 unwind 清理路径。
- `panic` 调用点位置应由编译器注入；调用栈采集应只出现在 panic 冷路径，正常路径不分配、不登记栈帧对象。
- `StackFrames` 遍历应支持只输出 instruction pointer；符号化是 profile 诊断能力，不是语言运行时必需成本。
- panic-unwind profile 只在显式启用时生成栈展开和 RAII 清理路径。
- `tryReserve` 应降低为普通 `Result` 分支；普通 `push` 在容量已证明足够时不应重复生成分配慢路径。
- `okOrElse`、`unwrapOrElse` 和 `mapErr` 的 `OnceFn` 非逃逸闭包应内联成条件分支；成功路径不得构造失败路径值。
- `for item in data`、`for mut item in mut data` 和 `for move item in move data` 应对核心连续集合降低为直接循环，不分配 iterator、不做接口派发。
- 具体 `Iterator<T>` 的 `for move` 遍历应静态派发并可内联 `next`；只有显式接口访问或接口拥有者才允许动态派发。
- 字符串 literal 和 `StringSlice` 传参不应分配。
- `for i in 0..data.len` 形式的 `ArraySlice<T>` / `Array<T>` / `Vector<T>` 循环优化后不应包含冗余边界检查。
- 传给泛型函数的非逃逸闭包不应分配。
- `Writer` 接口类型参数调用应包含一次类似 vtable 的间接调用。
- 静态 `W: Writer` 调用不应包含接口表派发。
- 移动 `Box<T>` 应降低为指针转移，而不是深拷贝或引用计数。
- `Box<T>` 到 `Box<Interface>` 的移动转换不应重新分配，也不应移动堆内值。
- `Shared<T>` 到 `Shared<Interface>` 的移动转换不应增加引用计数。
- `Box<Interface>` 的销毁应为一次接口表销毁调用加一次 allocator deallocate。
- `Shared<Interface>.clone` 应为一次引用计数增加，不深拷贝具体值。
- `Send` / `Sync` 推导应在类型检查期完成，不需要运行时标记或查询。
- `Box<ThreadWriter>` 与 `Box<Writer>` 的运行时布局成本相同，只是类型系统保留了额外能力证明。
- `Array<T>.clone` / `Vector<T>.clone` 的分配在源码中可见。
- 普通赋值非 `Copy` 拥有者不应生成深拷贝。

## 6. 测试文件约定

顶部注释：

```zn
// category: compile-pass
// profile: freestanding
// purpose: verifies non-Copy move is explicit
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

测试 harness 应把这些注释当作断言。
