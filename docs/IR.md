# Zeno HIR、MIR 与 LLVM 降级策略

Zeno 的高性能目标不能只依赖后端优化。前端必须把所有权、访问模式、布局、泛型、闭包和错误传播都降低成后端能直接利用的事实，同时不能把语言安全语义丢给 LLVM 猜。

本文件定义 stage0 和自举编译器都必须遵守的 IR 分层。

## 1. 目标

IR 设计目标：

- 把源码语义和机器优化分层，避免前端被 LLVM 绑定死。
- 在 HIR 中保留语言级安全事实，例如 move、`mut`、销毁、接口约束和 `trust` 边界。
- 在 MIR 中显式化控制流、清理边、边界检查、drop flag 和接口派发。
- 在 LLVM IR 中只表达已经证明成立的事实，例如 `noalias`、`readonly`、`nonnull`、`noreturn`。
- 让高性能承诺可测试：无隐藏分配、无隐藏异常、无隐藏动态派发、无不必要边界检查。

Zeno 借鉴 Rust MIR 的核心思路：使用控制流图、显式类型、简单表达式和数据流分析来服务安全检查、优化和 codegen。Zeno 也借鉴 LLVM 的低层能力：target data layout、函数/参数属性、指针 alias/capture 信息、volatile/atomic 内存模型等。但 Zeno 不把这些后端细节暴露成日常源码负担。

## 2. 编译管线

推荐管线：

```text
AST
  -> resolved HIR
  -> typed HIR
  -> checked HIR
  -> mono HIR
  -> MIR
  -> optimized MIR
  -> LLVM IR
  -> object / executable
```

职责边界：

- AST 只表达语法结构和源码 span。
- resolved HIR 完成名字、模块、可见性和重载候选解析。
- typed HIR 完成类型、接口约束、方法接收者和访问模式推导。
- checked HIR 完成所有权、初始化、逃逸、`Send` / `Sync`、`trust` 能力和 layout 检查。
- mono HIR 把泛型函数、泛型类型和静态接口约束实例化为具体类型。
- MIR 表达显式控制流和低层语义，是主要优化 IR。
- LLVM IR 只接收已经证明安全和优化合法的低层事实。

## 3. HIR

HIR 必须保留源码级语义，不允许过早丢信息。

HIR 节点至少包含：

- stable node id。
- 源码 span。
- 所属 package、module 和可见性。
- 解析后的声明目标。
- 显式类型和推导类型。
- 参数访问模式：只读、`mut`、`move`。
- `self`、`mut self`、`move self` 接收者语义。
- `Copy` / 非 `Copy` / 带 `destroy` 的资源分类。
- `Send` / `Sync` 自动推导结果和 `trust impl` 来源。
- layout id、size、align、字段源码顺序和字段实际布局顺序。
- `@layout(Source)`、`@layout(C)`、`@layout(Packed(N))` 和 `@export` 信息。
- `trust` span、能力类别和 manifest 授权结果。
- 闭包捕获模式和能力：`Fn`、`MutFn`、`OnceFn`。
- pattern binding graph：每个 binding 的来源 place、访问模式、是否 refutable、是否来自 or pattern。
- allocation region、allocator kind 和 owner 是否可逃逸。
- `@noAlloc`、`@noPanic` 和 profile 影响。

HIR 中可以保留复杂表达式，因为它仍然服务诊断和源码语义。不要在 HIR 中做后端专用重写。

## 4. Mono HIR

泛型默认单态化。

每个 mono item key 必须包含：

- 泛型定义 stable id。
- 类型实参的 type fingerprint。
- 类型实参的 layout shape fingerprint。
- 接口约束满足证明。
- `Send` / `Sync` 能力证明。
- target triple、cpu、features。
- profile、panic/OOM、allocator 和优化配置。

单态化规则：

- 泛型定义先按约束检查一次，不能等到具体实例才发现约束外操作。
- 使用点请求具体 mono item。
- 静态接口约束调用在 mono HIR 中解析为具体函数或可内联调用。
- 函数参数位置的裸接口名在 HIR 中展开成匿名静态泛型参数，并进入 mono item key；每个裸接口参数引入一个独立 hidden type parameter。
- 函数返回位置的裸接口名在 HIR 中记录为静态接口返回；类型检查必须推断出一个具体返回类型表达式，并让所有返回路径统一到该类型。`pub` 静态接口返回必须在包元数据中记录 opaque return identity、layout fingerprint、drop glue 和接口满足证明。
- 只有源码类型显式使用接口拥有者 `Box<Interface>` 或 `Shared<Interface>` 时，mono HIR 才保留接口表派发。
- 共享 mono cache 必须以完整 key 校验，不能跨 target 或跨 profile 误用实例。
- 未被代码生成实际使用的类型参数不进入 codegen key。
- 只依赖 layout shape 的内部实现可以共享机器代码，但不能引入隐藏动态派发或隐藏分配。
- cold block、panic/OOM 路径和错误格式化路径应在 mono 后 outline，避免泛型热路径膨胀。

## 5. MIR 基本结构

MIR 函数由以下部分组成：

```text
MirFunction {
  signature
  locals
  basicBlocks
  cleanupBlocks
  dropFlags
  debugSpans
  optimizationFacts
}
```

MIR 是控制流图：

- 每个 basic block 由若干 statement 和一个 terminator 组成。
- statement 不能跳转。
- terminator 决定控制流。
- 表达式不再嵌套，复杂表达式拆成 locals 和 temporaries。
- 所有 place、operand、rvalue 都有类型。

核心概念：

```text
Place   = local | field(place, fieldId) | index(place, value) | deref(place)
Operand = copy(place) | move(place) | const(value)
Rvalue  = use(operand)
        | binary(op, lhs, rhs)
        | checkedBinary(op, lhs, rhs)
        | aggregate(kind, operands)
        | discriminant(place)
        | len(place)
        | cast(kind, operand)
        | callValue(callee, args)
```

`copy(place)` 只允许用于 `Copy` 值。非 `Copy` 值只能通过 `move(place)` 消耗。

## 6. MIR 语句与终结符

建议 statement：

- `assign(place, rvalue)`
- `storageLive(local)`
- `storageDead(local)`
- `setDropFlag(local, bool)`
- `assume(fact)`
- `boundsCheck(index, len, onFail)`
- `debugValue(local, span)`
- `poison(place)`，用于 move 后不可用建模

建议 terminator：

- `goto(block)`
- `switch(value, cases, otherwise)`
- `return`
- `call(destination, callee, args, normalBlock, unwindBlock?)`
- `drop(place, normalBlock, unwindBlock?)`
- `panic(info, strategy)`
- `trap`
- `unreachable`

`try`、`match`、`if`、`for` 和 RAII 都必须降低为普通控制流，不使用异常作为常规错误流。

## 7. RAII 与清理边

Zeno 的销毁必须确定发生。

MIR lowering 规则：

- 每个需要销毁的 local 有 drop flag。
- 完全初始化后设置 drop flag。
- move 出去后清除 drop flag。
- 部分初始化聚合为每个需要销毁的字段维护状态。
- 正常离开作用域按源码声明逆序销毁；未初始化或已移动的 local 由 drop flag 跳过。
- `try` 的提前返回边必须连接到当前作用域清理链。
- `panic.strategy = "abort"` 或 `"trap"` 时，panic 冷路径不生成 unwind 清理。
- `panic.strategy = "unwind"` 时，可能 panic 的 call 必须有 unwind cleanup edge。

赋值 lowering：

- 对已经初始化的 place，先把右侧 lowering 成临时值；右侧所有失败边都跳到当前清理链，不能提前销毁左侧旧值。
- 右侧成功后，若左侧旧值需要销毁，先运行旧值 drop，再把临时值 move/store 到左侧。
- 对 `maybe-init` 的 `var` place，右侧成功后根据 drop flag 条件销毁旧值，再写入新值并设置 drop flag；右侧失败边保持原 drop flag。
- 对未初始化 place，赋值只设置初始化状态，不运行旧值 drop。
- 对 `val` place，HIR 必须证明赋值目标是确定未初始化且当前路径没有成功初始化过；`maybe-init`、`init` 或 `moved` 的 `val` 赋值必须在 HIR 阶段拒绝。
- 对 `Copy` 或空 drop 类型，旧值 drop 可被消除，最终降低为 store、memcpy 或标量写入。
- 赋值目标存在活动只读访问或可写访问时，HIR/MIR 访问检查必须拒绝，不能靠 codegen 猜测。
- `replaceAt` 这类替换 API 在 MIR 中必须建模为移动出旧元素并返回，不能等价降低为“drop 旧元素再写新元素”。

部分初始化 lowering：

- `val object: Struct; object.field = expr;` / `var object: Struct; object.field = expr;` 在对象未完整初始化时是字段初始化写入。
- 每个需要销毁的字段有独立 drop flag；字段初始化成功后设置该字段 flag。
- 全部字段初始化后，对象变成完整初始化；若类型有 `destroy` 块，之后退出作用域按完整对象销毁。
- 任何字段初始化失败边只清理已经初始化的字段，不运行完整对象的 `destroy` 块。
- 完整对象被移动后，必须清除对象或字段 drop flag，避免部分字段 double drop。

enum / match lowering：

- `match value` 对 scrutinee 建立只读访问，根据 tag 分支。payload pattern binding 是 payload place 的只读 projection。
- `match mut value` 对 scrutinee 建立唯一可写访问，根据 tag 分支。payload binding 是 payload place 的 mutable projection，但不是 owning move。
- `match move value` 先消耗 scrutinee place，根据 tag 分支。被选中 payload binding 是 owning place；未选中 variants 不存在可销毁 payload。
- `if val` 降低为 refutable pattern test 加 then / else 分支；`while val` 降低为循环头 pattern test 加 body / exit 分支。
- `val pattern = expr` 只接受 irrefutable pattern；HIR 阶段应拒绝 refutable pattern。
- struct pattern 降低为字段 projection；tuple pattern 降低为 index projection；嵌套 pattern 递归生成 projection。
- literal pattern 降低为整数、bool、char 或 unit variant 比较。
- range pattern 降低为边界比较，不创建 range 对象。
- or pattern 降低为多个测试路径汇入同一个分支 block；汇入前必须创建同名、同类型、同访问模式的 binding。
- guard 降低为 pattern 成功后的额外条件分支。guard 为 false 时继续尝试后续 pattern。
- `match move` 分支中 move 出 payload 后必须清除对应 drop flag；分支退出 cleanup 只销毁仍拥有的 payload。
- `match move` 分支若没有移动出 payload，离开分支时必须销毁该 payload。
- `match` expression 的 join block 只允许兼容类型；发散分支 `Never` 可以并入其他分支类型。
- 对普通 enum，MIR 应是 tag switch 或等价 branch，不需要堆分配、接口派发或隐藏临时 owner。
- pattern lowering 不能调用用户代码，不能分配，不能做动态派发。字符串、regex、downcast、extractor、collection tail pattern 在 v1 HIR 阶段直接拒绝。

带 `destroy` 的类型：

- `destroy` 本身不能 `try`、不能 `await`。
- `destroy` 按隐式 `@noPanic` 和 `@noAlloc` 检查；可能 panic、可能 OOM 或会分配的可达调用必须在 HIR 阶段拒绝。
- `destroy` 中不能调用返回 `Result` 的失败 API。
- `destroy` 不是 `move self`，不能移动字段，不能创建逃逸访问。
- 完整对象 drop 顺序必须是：运行该类型的 `destroy` body，然后按源码声明逆序销毁字段；Auto layout 的字段重排不能改变该顺序。
- 通过 HIR 检查的 `destroy` body 可以在 LLVM 降级中标记为 no-unwind / no-allocation；字段 drop 是否 no-unwind 由字段类型的销毁事实决定。
- 部分初始化失败路径只清理已经初始化的字段，不运行完整对象的 `destroy` body。
- 带 `destroy` 的类型进入 `move self` 方法时，方法体中的 `self` 仍有普通 drop obligation；除非整个 `self` 被移动给另一个拥有者，否则退出方法时仍运行 `destroy` 和字段销毁。

## 8. 访问、别名与逃逸

Zeno 不使用 Rust 式显式生命周期语法，但 MIR 必须有内部访问区域。

MIR 必须建立内部 provenance 模型：

- `MemoryObject`：拥有存储身份的对象，例如 local、Array buffer、Vector buffer、Map table、Set table、Box allocation。
- `Provenance`：访问值从哪个 `MemoryObject` 派生。
- `AccessPath`：对象内路径，例如字段、索引或 slice range。
- `DisjointProof`：两个访问路径是否可证明不重叠。
- `EscapeState`：`noEscape`、`scopeEscape`、`threadEscape` 或 `globalEscape`。

访问事实：

- 只读访问允许共享读。
- `mut` 访问表示唯一可写访问。
- `move` 表示所有权转移。
- 视图类型不能长期保存，不能进入结构体、集合、`static`、逃逸闭包、线程、任务或 async future。
- 活跃只读视图和可写访问不能冲突。
- 活跃 `mut` 访问和任何其他访问不能冲突。

MIR 中应记录：

- `AccessRegion`
- `readonly`
- `unique`
- `owned`
- `nocapture`
- `mayEscape`
- `provenance`
- `accessPath`
- `disjointProof`
- `escapeState`
- `allocationRegion`
- `allocatorCanEscape`

LLVM 降级时只能在已证明条件下发属性：

- `mut` 参数可证明唯一且不逃逸时，才可以发 `noalias`。
- 只读访问可证明不写且不逃逸时，才可以发 `readonly` / `nocapture`。
- `move` 参数可用所有权事实做 drop 消除、store 消除和别名优化。
- 不能因为源码写了 `mut` 就无条件给 LLVM `noalias`。
- 独立 field、独立 range 和不同 `MemoryObject` 的不重叠事实应进入 alias scope 或等价后端信息。

## 9. 边界检查与范围事实

索引访问先在 MIR 中显式表示：

```text
boundsCheck(index, len, onFail)
value = load(data[index])
```

优化器负责消除冗余检查。

MIR 必须记录：

- `0 <= i`
- `i < len`
- 半开区间 `start..end`
- 循环 induction variable。
- 集合长度是否在循环内不变。
- `tryReserve` 成功后的容量事实。
- `Map` / `Set` 的 bucket capacity、load limit 和 entry probe 事实。
- slice lineage、slice offset 和 slice len。
- `start <= end <= len` 这类 range 包含关系。

典型形式：

```zn
for i in 0..data.len {
    sum = sum + data[i];
}
```

优化后不应保留循环体内的冗余边界检查。无法证明时才保留检查，并按 profile 降低到 panic、trap 或 handler。

## 10. Result、Option 与 Never

`try` 不生成异常。

`Result<T, E>` lowering：

```text
tmp = call mayFail()
switch discriminant(tmp):
  Ok  -> use payload
  Err -> cleanup current scopes; return Err(payload)
```

`Option<T>` lowering 同理。`Option` 进入 `Result` 错误流必须显式使用 `okOr` 或 `okOrElse`，因此 MIR 不需要隐式错误构造。

`Never` 表示不会返回：

- `panic(message) -> Never`
- `oom(layout) -> Never`
- `trap -> Never`
- 无限循环且无 `break` 的表达式可以是 `Never`

`Never` 可以流入任意期望类型，但 MIR control flow 必须以 `panic`、`trap`、`unreachable` 或等价 noreturn call 结束。

## 11. 闭包

闭包 lowering 目标是让默认情况不分配。

非逃逸闭包：

- 捕获环境降低为局部 aggregate 或被标量替换。
- 调用降低为直接调用或内联。
- 不能生成 `Box`、`Shared` 或 heap allocation。
- 不能生成接口表派发，除非源码显式使用接口拥有者。

逃逸闭包：

- 必须在源码中通过显式拥有者表达成本，例如 `Box<Fn<...>>`、任务或线程 API。
- 捕获的 `move` 值进入闭包环境所有权。
- 捕获 `mut` 值时必须满足逃逸和并发规则。

能力推导：

- 只读捕获满足 `Fn`。
- 可写捕获满足 `MutFn`。
- 消耗捕获满足 `OnceFn`。
- `Fn` 可以传给要求 `MutFn` 或 `OnceFn` 的 API；`MutFn` 可以传给要求 `OnceFn` 的 API。

## 12. Future 状态机

`async fn` 降低为拥有状态机。MIR 必须显式表示：

- future state enum。
- 每个状态已初始化的 locals 和字段。
- 跨 `await` 存活的 RAII guard 和其他拥有资源。
- poll 入口、完成状态、取消 drop 状态。
- 已经交给 runtime 的 escaped future。

future drop lowering：

- 丢弃未完成 future 时，销毁当前状态中已经初始化且仍拥有的字段。
- future drop 不能 `await`，不能调用异步清理。
- 非拥有访问值不能作为独立字段跨 `await` 存入 future 状态。
- 立即 `await mut receiver.method(...)` 的 async `mut self` 调用应在 MIR 中表示为“future 拥有者字段 + 暂停点内活动可写访问”，不能生成可逃逸的访问 future。
- `blockOn(move future)` 在 future 不逃逸时应 lowered 为当前函数内的直接 poll 循环或等价内联状态机，不能引入隐藏 heap allocation。`blockOn(move task)` lowered 为显式 runtime wait/park 边界。
- `Runtime.spawn` / `Runtime.spawnBlocking` 的 `OnceFn<TaskContext, T>` 任务应 lowered 为显式任务上下文参数；上下文指向 runtime 任务控制块中的取消状态。
- `TaskContext.isCancellationRequested()` 应 lowered 为轻量取消状态读取，不能隐式调用全局当前任务查询、分配、加锁或执行系统调用。
- `TaskContext` 可以保存在同一任务 future 状态中，但逃逸分析必须拒绝返回、长期存储、线程捕获、子任务捕获和其他逃逸闭包捕获。

禁止访问值逃逸跨 `await` 后，v1 不需要用户可见 `Pin`。future 在未交给 runtime 前按普通 move 规则移动；交给 runtime 后，用户移动的是 task handle。

## 13. 接口派发

Zeno 的默认接口约束是静态派发。

静态泛型：

```zn
fn writeAll<W: Writer>(mut writer: W, data: ArraySlice<U8>) { ... }
```

mono 后应变成具体 `W` 的直接调用，可内联。

匿名静态接口参数：

```zn
fn log(mut writer: Writer, data: ArraySlice<U8>) { ... }
```

HIR 应把它展开成隐藏类型参数：

```zn
fn log<W: Writer>(mut writer: W, data: ArraySlice<U8>) { ... }
```

mono 后也必须是具体类型的直接调用，不能包含接口表间接调用。

泛型接口参数：

```zn
fn feed<T>(mut consumer: Consumer<T>, move item: T) { ... }
```

HIR 应展开为等价的 hidden type parameter：

```zn
fn feed<T, C: Consumer<T>>(mut consumer: C, move item: T) { ... }
```

调用点求解必须同时使用普通参数约束和接口实现约束。若 `consumer` 的具体类型实现多个 `Consumer<X>` 且其他参数无法唯一决定 `T`，HIR 类型检查报调用不明确，不能生成多个候选再留给 codegen。

静态接口返回：

```zn
fn makeWriter() -> Writer {
    return FileWriter.openDefault();
}
```

HIR 应记录隐藏返回类型 `R: Writer`，并把 `R` 推断为 `FileWriter`。不同返回路径推断出不同具体类型时必须在 HIR 类型检查期报错，而不是退化成接口拥有者。

如果静态接口返回来自接口参数，HIR 应把隐藏参数类型和隐藏返回类型统一：

```zn
fn identity(value: Writer) -> Writer {
    return value;
}
```

这等价于 `fn identity<W: Writer>(value: W) -> W`。若返回路径来自两个独立裸接口参数，则它们是不同 hidden type parameter，除非源码用显式 `W: Writer` 约束把它们统一。

接口拥有者：

```zn
val writer: Box<Writer> = Box.from(FileWriter.open(path));
```

`Box<T>` 到 `Box<Interface>` 的擦除转换不能重新分配，也不能移动堆内值。MIR 应把它表示为指针加接口表元数据的重解释。

只有 `Box<Interface>` / `Shared<Interface>` 这类接口拥有者的方法调用才降低为接口表调用。

## 14. 布局与 packed 访问

MIR field projection 必须同时知道：

- 源码字段 id。
- 实际 layout 字段序号。
- offset。
- field size。
- field align。
- 是否 packed / unaligned。

`@layout(Packed(N))` 字段读取降低为 unaligned load 或等价字节序列；写入降低为 unaligned store。不能把 packed 字段作为长期 `mut` place 传出。

Auto layout 的字段重排只影响内存布局，不影响源码字段名、初始化语义、销毁顺序和诊断位置。

## 15. FFI 与导出

FFI lowering 规则：

- `@export("symbol", abi: C)` 只影响外部符号和调用 ABI，不改变 Zeno 源码可见性规则。
- `pub` 不等于导出符号。
- `trust extern "C"` 导入必须经过 C-compatible 签名检查。
- C ABI 边界不能让 panic unwind 穿过。
- `@layout(C)` 和 C-compatible 类型检查在 HIR 完成，MIR 不再接受不合法 C ABI。
- 导出函数不能是泛型函数，不能是方法，不能是闭包。
- extern 调用必须位于 `trust` 边界中，并且该边界必须记录 `ffi` 能力。
- 裸地址 / 裸指针构造、偏移、解引用和按位重解释必须记录 `rawMemory` 能力。
- MMIO、端口 I/O、volatile 硬件访问、物理地址映射和链接段控制必须记录 `hardware` 能力。
- inline asm 必须记录 `inlineAsm` 能力；中断入口和裸调用约定必须记录 `interrupts` 能力。
- `trust impl Send` / `trust impl Sync` 记录为 `threadSafety` 信任事实，但不能给普通内存访问添加 alias、lifetime 或 alignment 属性。

## 16. MIR 优化

stage0 至少要有一组简单但可靠的 MIR 优化，release 构建再交给 LLVM 做更深优化。

必做优化：

- simplify CFG。
- 常量传播和常量折叠。
- copy propagation。
- dead local / dead store elimination。
- drop flag 消除和空 drop 消除。
- 部分初始化路径的重复 cleanup 合并。
- `try` 成功路径热化，错误路径冷化。
- `Option` / `Result` 分支简化。
- 非逃逸闭包内联。
- 静态接口约束 direct call 化。
- 已知具体类型的接口拥有者调用可以去虚化。
- range facts 驱动的边界检查消除。
- `tryReserve` 成功后的容量慢路径消除。
- `Map.entry` 单次探测事实复用，以及 `Map` / `Set` 预留容量后的 rehash 慢路径消除。
- layout-aware scalar replacement。
- `mut` / `move` 驱动的别名事实推导。
- provenance 驱动的 alias scope / noalias 推导。
- allocation region 驱动的 owner 逃逸检查。
- future drop cleanup 合并和未初始化状态消除。
- cold block outlining，尤其是 panic、OOM、错误格式化和泛型冷路径。
- shape sharing 和 identical code folding，避免泛型代码膨胀。
- panic / oom 冷路径拆分。

不允许的优化：

- 不能把可能别名的 `mut` 访问错误标为 `noalias`。
- 不能删除有副作用或可能 panic 的操作。
- 不能跨 `trust` 边界假设未声明的内存事实。
- `trust` 边界本身不能产生 `noalias`、`nonnull`、`dereferenceable`、更强 alignment、初始化或线程安全事实。
- 从 `rawMemory` 产生的指针默认具有未知 provenance 和未知别名关系；除非受信 API 明确建模，否则优化器必须保守处理。
- 不能改变 RAII 或 cleanup 顺序。
- 不能让 C ABI 边界出现 Zeno 内部异常或 unwinding。

## 17. LLVM 降级

LLVM 降级应使用 target triple 和 data layout 作为所有大小、对齐和 ABI 决策的来源。

可以发出的属性和信息：

- `noalias`：只给已证明唯一且不逃逸的 `mut` / owned 间接参数。
- `readonly`：只给已证明不写的只读访问。
- `readnone`：只给纯计算且不读内存的函数。
- `nocapture`：只给已证明不逃逸的指针参数。
- `nonnull`：只给类型系统保证非空的指针。
- `dereferenceable(N)`：只给已证明可安全访问 N 字节的参数。
- `align(N)`：只给已证明对齐的指针。
- `noreturn`：给 `panic`、`oom`、`trap` 和其他 `Never` 函数。
- `cold`：给 panic/OOM/错误慢路径。
- `nounwind`：只在 profile 和函数体证明不会 unwind 时发出。
- lifetime start/end：用于局部存储范围，帮助栈槽复用和优化。

volatile 和 atomic：

- 普通 load/store 绝不自动变成 volatile。
- MMIO 只能通过 `trust` 封装 API 产生 volatile load/store。
- 原子操作只能通过 `Atomic<T>` API 产生，并携带明确 ordering。
- 锁和同步只能通过源码中显式出现的同步类型产生。

## 18. 比现有语言更激进的地方

Zeno 不承诺未验证地“天然更快”。它的设计目标是把高性能路径做成默认路径，并把隐藏成本变成编译错误或源码可见成本。

关键策略：

- 默认 Auto layout：普通结构体默认允许字段重排，优先减少 padding 和数组跨度。
- 默认单态化：泛型和静态接口约束优先生成具体代码。
- 泛型代码体积通过 cold outlining、shape sharing、ICF、LTO 和 PGO 控制，不牺牲默认静态派发。
- 默认无异常错误流：`try` 是分支和 cleanup，不是 exception。
- 默认无 GC 和强制运行时：分配、引用计数、锁、动态派发都必须在源码类型中显式出现。
- 默认非逃逸闭包不分配：闭包环境可以直接栈上或标量化。
- `mut` / `move` 给优化器提供别名和所有权事实，但用户不写生命周期。
- 视图类型不允许长期保存，减少复杂生命周期设计和别名不确定性。
- scoped allocator owner 不允许逃逸 allocator 作用域，避免 arena/bump/fixed buffer 产生悬垂释放路径。
- future drop 有显式状态清理，不需要隐藏运行时或用户可见 `Pin`。
- `panic`、OOM、调用栈符号化全部是冷路径和 profile 能力，不污染正常路径。

这些策略是否最终超过 Rust、Zig、Kotlin 或 C++，需要用同一目标、同一优化等级、同一算法和真实基准验证。规范层面要保证：Zeno 的抽象不会先天引入额外成本。

## 19. stage0 交付要求

stage0 至少实现：

- HIR 节点和 stable id。
- typed HIR 和 checked HIR。
- mono item key。
- MIR CFG、locals、places、operands、rvalues。
- drop flag 和 cleanup edge。
- `try` lowering。
- `for` lowering。
- `Array` / `Vector` / `ArraySlice` 索引和边界检查。
- 非逃逸闭包无分配 lowering。
- 如果实现 async 子集，必须同时实现 future 状态机和 drop cleanup lowering。
- allocation region 逃逸检查。
- 静态接口约束 direct call lowering。
- 接口拥有者的显式间接调用 lowering。
- packed unaligned load/store。
- `@export` C ABI lowering。
- 基础 MIR 优化和 LLVM 属性发射。
- codegen 规格测试中的 IR 断言。

stage0 可以先使用文本化 MIR 做测试断言。等编译器成熟后，再增加 LLVM IR FileCheck 风格断言和汇编级断言。

完整性能契约见 [PERFORMANCE.md](PERFORMANCE.md)。
