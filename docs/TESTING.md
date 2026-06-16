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

未来类别：

- `run-pass`：能编译并产生预期输出的程序。
- `codegen`：检查生成 IR 或汇编的测试。
- `perf`：微基准和 codegen 不变量。
- `stdlib`：core 和 std API 一致性测试。

## 2. 必要 compile-pass 覆盖

第一版编译器测试套件必须包含：

- `Copy` 值正常复制。
- 普通整数 `+`、`-`、`*` 使用 wrapping 语义。
- `checked*` 整数方法返回 `Option<T>`。
- `saturating*` 和 `overflowing*` 方法语义明确。
- 无损整数转换可以使用 `as`。
- 检查窄化使用 `T.fromChecked(value)`，明确截断使用 `T.truncate(value)`，钳制转换使用 `T.saturate(value)`。
- 非 `Copy` 资源 move 后源绑定失效。
- 非 `Copy` 拥有者普通赋值必须被拒绝，除非显式 `move`。
- `Array<T>` / `Vector<T>` 的 `clone` 显式接收 allocator 并返回 `Result`。
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
- 非逃逸闭包不分配。
- 逃逸闭包显式分配。
- `Result` 通过 `try` 传播。
- `Option` 可以在返回 `Option` 的函数中通过 `try` 传播。
- `Option` 进入 `Result` 错误流必须显式使用 `okOr`。
- 错误类型转换必须显式使用 `mapErr` 或等价 API。
- `try` 提前返回会执行离开作用域的 `defer` 和 RAII 销毁。
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

- use after move。
- double move。
- 非 `Copy` 拥有者普通赋值造成隐式复制。
- 会分配的 clone 缺少 allocator 或不返回 `Result`。
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
- 可写访问与只读访问重叠。
- 可写访问逃逸到非 scoped 线程。
- 裸接口访问类型作为结构体字段、枚举载荷、元组字段、`static` 或集合元素长期保存。
- 裸接口访问类型被逃逸闭包、线程、任务或 async future 捕获。
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

compile-fail 主错误：

```zn
// expected-error: use of moved value
```

测试 harness 应把这些注释当作断言。
