# Zeno 安全模型

Zeno 的安全目标很直接，也很难实现：普通用户代码不应造成未定义行为、use-after-free、double-free、数据竞争或悬垂访问，同时仍然生成接近 C/C++ 的高性能代码。

语言通过所有权、确定性销毁、编译器检查的访问模式和显式 `trust` 信任边界来实现这个目标。

整数溢出不是未定义行为。普通 `+`、`-`、`*` 使用 wrapping 语义；需要检查时必须显式调用 `checked*` 方法。

`try` 提前返回不是异常路径。它会按普通作用域退出路径执行 `defer` 和 RAII 销毁，然后返回 `Err` 或 `None`。

## 1. trust 信任边界

Zeno 不暴露用户 `unsafe` 模式。和原始地址、C ABI、硬件交互相关的底层能力通过 `trust` 边界进入语言。

`trust` 的含义不是“这段代码不安全”，而是：

- 编译器无法独立证明这里的全部底层不变量。
- 作者声明这些不变量由自己负责保证。
- 编译器继续检查普通类型、所有权、初始化、析构和可证明的访问规则。

允许出现在 `trust` 边界中的底层能力包括：

- 裸 C ABI 声明和调用。
- 从整数构造裸地址或裸指针。
- 裸指针运算和解引用。
- volatile、MMIO、端口 I/O。
- inline asm。
- 链接段、启动入口、中断入口等平台 ABI 约定。

普通代码可以写驱动、内核模块和平台绑定，但底层操作必须被 `trust` 包住，并且包的 `Zeno.toml` manifest 必须允许对应能力。推荐把 `trust` 封装成安全 API，让调用方使用类型化句柄、能力对象和 `Result`。

## 2. 所有权不变量

对每个非 `Copy` 值：

- 任意时刻只有一个拥有者。
- 不能隐式复制。
- 所有权转移必须用 `move` 可见地表达；普通函数调用点写 `move value`，消费方法在签名中写 `move self`。
- 被移动后的绑定不可再用。
- 活跃拥有者会被销毁一次。
- 部分初始化状态会记录哪些字段需要销毁。

深拷贝必须通过显式 `clone` API 表达。`Array<T>` / `Vector<T>` / `String` 这类可能分配的 clone 失败时调用当前 profile 的 `oom(layout) -> Never`；指定 allocator 时使用 `cloneIn`。引用计数增加只能通过 `Shared<T>.clone` 等类型名显式暴露共享成本的 API 表达。

compile-fail 示例：

```zn
let a = File.open("log.txt")?;
let b = move a;
a.write("bad"); // expected-error: use of moved value
```

## 3. 初始化

值只有在所有字段都初始化后才能使用。

```zn
var header: Header;
header.magic = MAGIC;
header.version = 1;
// 所有字段初始化后 header 才可用
```

编译器需要按局部变量和字段跟踪初始化状态。

规则：

- 读取未初始化值会被拒绝。
- 销毁未初始化值是 no-op。
- 销毁部分初始化聚合值时，只销毁已初始化字段。
- 从拥有的结构体中移动字段会把该字段标记为未初始化，除非类型禁止字段移动。
- 从只读访问或唯一可写访问中不能移动字段；只有拥有整个对象时才能移出字段。
- 有自定义 `destroy` 块的类型默认禁止从外部移出字段；类型自己的 `move self` 方法可以作为显式拆解 API 移出 `self` 字段。

## 4. 访问模式

大多数参数是非拥有访问路径。源码中使用普通类型名表达，编译器负责检查。

参数规则是：默认读，`mut` 写，`move` 拿走。

只读访问：

- 是参数默认行为。
- 可以与其他只读访问共存。
- 不允许修改。
- 可以在被证明有效的存储生命周期内自由传递。
- 对非 `Copy` 资源不移动、不复制。
- 不能被当成拥有值返回、保存到拥有容器，或移动进 `Box<T>`、`Shared<T>`、线程、任务、future。

接口访问类型例如 `Writer` 也是非拥有访问值。它可以作为短期参数参与动态派发，但不能写成 `move writer: Writer`，也不能保存到字段或集合中。需要拥有异构实现时使用 `Box<Writer>`；需要共享拥有时使用 `Shared<Writer>`，但 `Shared<Writer>` 只提供只读接口访问。

唯一可写访问：

- 用 `mut` 表达。
- 排除对重叠存储的其他活动访问。
- 允许修改。
- 只能通过经过检查的库 API 缩短或拆分。
- 不取得所有权。

所有权接收：

- 用 `move` 参数表达。
- 普通函数调用点必须写 `move`。
- 被移动后的源绑定不可再用。
- 函数负责销毁该值，或继续把它移动给新的拥有者。

方法接收者遵守同一套规则：

- `self` 方法只读访问接收者。
- `mut self` 方法唯一可写访问接收者，不取得所有权；接收者必须是可写位置。
- `move self` 方法取得接收者所有权；方法调用后原接收者不可再用。
- `mut self` 方法调用点不写 `mut`，普通函数 `mut` 参数调用点仍然必须写 `mut`。

拒绝示例：

```zn
let s = String.from("abc");
let v = move s;
drop(move v);
use(s); // expected-error: use after move
```

```zn
fn readThenWrite(read: ArraySlice<U8>, mut write: ArraySlice<U8>) {}

var bytes = Array<U8>.filledIn(4, 0, mut allocator);
readThenWrite(bytes, mut bytes); // expected-error: writable access overlaps read-only access
```

```zn
struct Packet {
    bytes: Array<U8>,
}

fn badReturn(packet: Packet) -> Packet {
    return packet; // expected-error: read parameter is not owned
}
```

## 5. 逃逸分析

编译器必须拒绝逃逸出底层存储生命周期的访问值。

访问值在这些情况下会逃逸：

- 从函数返回，除非返回值只依赖调用方传入的访问参数。
- 存进结构体、枚举、元组、容器、堆对象、全局存储或其他拥有者。
- 被逃逸闭包捕获。
- 跨线程、任务或 future 边界移动。

允许：

```zn
fn firstByte(data: ArraySlice<U8>) -> Option<U8> {
    if data.len == 0 { return None; }
    return Some(data[0]);
}
```

拒绝：

```zn
fn bad(mut allocator: GlobalAllocator) -> Result<ArraySlice<U8>, AllocError> {
    let local = Array<U8>.filledIn(4, 0, mut allocator);
    return Ok(local); // expected-error: returns slice to local storage
}
```

`ArraySlice<T>` 不能作为结构体字段，也不能通过 `Array<ArraySlice<T>>`、`Vector<ArraySlice<T>>` 等形式间接保存。结构体如果需要保存数据，必须保存拥有者：

```zn
struct Packet {
    bytes: Array<U8>,
}
```

允许在底层存储仍然有效的范围内使用：

```zn
fn packetBody(bytes: ArraySlice<U8>) -> ArraySlice<U8> {
    return bytes.dropPrefix(4);
}

fn parsePacket(bytes: ArraySlice<U8>) -> Result<Token, ParseError> {
    let body = packetBody(bytes);
    return parseHeader(body);
}
```

`packetBody` 可以返回切片，因为返回值只依赖调用方传入的 `bytes`，不依赖函数内部创建的局部拥有者。

拒绝把切片放进字段：

```zn
struct BadPacket {
    bytes: ArraySlice<U8>, // expected-error: ArraySlice fields are not allowed
}
```

拒绝返回指向局部拥有者的切片：

```zn
fn badView(mut allocator: GlobalAllocator) -> Result<ArraySlice<U8>, AllocError> {
    let local = Array<U8>.filledIn(4, 0, mut allocator);
    return Ok(local.asSlice()); // expected-error: view outlives local storage
}
```

## 6. 活跃视图与拥有者修改

活跃 `ArraySlice<T>` 会让底层拥有者进入受限状态：

- 拥有者不能被移动或销毁。
- 对 `Array<T>`，元素修改仍按普通只读/可写访问冲突检查处理。
- 对 `Vector<T>`，改变长度或容量的结构性修改必须被拒绝。
- 只读查询可以继续执行。

拒绝：

```zn
fn badPush(mut allocator: GlobalAllocator) -> Result<USize, AllocError> {
    var bytes = Vector<U8>.withCapacityIn(4, mut allocator);
    let view = bytes.asSlice();
    bytes.push(1); // expected-error: vector structural mutation while view is live
    return Ok(view.len);
}
```

允许：

```zn
fn pushAfterView(mut allocator: GlobalAllocator) -> Result<Unit, AllocError> {
    var bytes = Vector<U8>.withCapacityIn(4, mut allocator);

    {
        let view = bytes.asSlice();
        parseHeader(view);
    }

    bytes.push(1);
    return Ok(());
}
```

这条规则完全静态检查，不需要运行时借用计数、隐藏引用计数或锁。

## 7. 边界与集合

`Array<T>` 是拥有、连续、长度固定的数组。`Vector<T>` 是拥有、连续、可增长的数组。`ArraySlice<T>` 是不拥有的连续数组片段，低层表示是基础能力加长度。

`Array<T>` 和 `Vector<T>` 可以被只读地看成 `ArraySlice<T>`，也可以作为 `mut ArraySlice<T>` 传入以修改元素。改变长度需要 `mut Vector<T>`。

索引必须保证边界安全：

- 常量越界索引是编译错误。
- 动态索引需要检查，除非编译器能证明范围安全。
- `Array<T>`、`Vector<T>` 或 `ArraySlice<T>` 上的典型范围循环应消除冗余边界检查。
- 拆分 API 必须证明可写访问不重叠。
- `for item in data` 不移动非 `Copy` 元素，只创建只读元素访问。
- `for mut item in mut data` 在循环体内持有当前元素的唯一可写访问；循环期间不能结构性修改同一个 `Vector<T>`。
- `for move item in move data` 只能用于拥有集合，提前退出时必须销毁尚未迭代的元素。

示例：

```zn
for i in 0..data.len {
    sum = sum + data[i]; // 边界检查应被消除
}
```

## 8. 销毁安全

销毁逻辑不能复活已移动值，也不能创建比对象活得更久的访问值。

规则：

- `destroy` 不返回值，不能使用 `try`，不能参与常规错误流。
- `destroy` 调用的清理 API 必须不可失败或 best-effort，返回 `Unit`。
- 需要报告清理错误时，资源类型必须提供显式 `close`、`flush`、`finish` 等 `move self` 方法返回 `Result<T, E>`。
- `destroy` 块以隐式 `self` 接收最终拥有者。
- `destroy` 块运行时可以访问字段。
- `destroy` 块之后，字段按源码声明顺序的逆序销毁；这属于语义顺序，不受 Auto layout 的内存字段重排影响。
- 如果对象被部分移动，只有仍初始化的字段会被销毁。
- 如果带 `destroy` 的类型在自己的 `move self` 方法中移出了字段，该对象的 `destroy` 块不会再自动运行；方法体必须先完成必要清理。
- 析构逻辑不能移动出另一个字段析构所依赖的字段，除非类型显式建模该依赖。

stage0 可以采用保守规则：`destroy` 块可以读取字段和调用方法，但不能移出字段。

## 9. 数据竞争自由

数据竞争指并发访问同一存储，至少一个访问是写入，并且缺少同步。普通 Zeno 代码不应能表达这种状态。

线程规则：

- 把拥有者移动到另一个线程，会让源线程中的绑定不可用。
- 被移动到另一个线程、任务或可跨线程 future 的值必须是 `Send`。
- `Copy` 值可以复制进线程。
- 共享不可变访问要求类型是 `Sync`。
- 共享可变访问必须经过同步类型。
- 可写访问不能跨越非 scoped 线程边界。

`Send` / `Sync` 是结构化自动推导的标记接口。普通纯数据类型在字段满足条件时自动获得；带自定义 `destroy` 的资源类型不会自动获得，因为析构可能要求特定线程或特定平台上下文。作者可以用 `trust impl Send for T {}` 或 `trust impl Sync for T {}` 声明这些额外不变量，编译器必须把它们记录到信任报告。

接口拥有者也必须保留线程安全能力。`Box<Writer>` 不能跨线程移动；需要定义 `interface ThreadWriter: Writer, Send {}` 并使用 `Box<ThreadWriter>`。`Shared<Writer>` 不能跨线程共享；需要定义 `interface SharedWriter: Writer, Send, Sync {}` 并使用 `Shared<SharedWriter>`。

拒绝：

```zn
var count = 0;
Thread.spawn(move () {
    count = count + 1;
}); // expected-error: writable access cannot escape to unscoped thread
```

允许：

```zn
let count = Atomic<U64>.new(0);
let shared = Shared.new(move count);
let t = Thread.spawn(move () {
    shared.fetchAdd(1, Ordering.Relaxed);
});
try t.join();
```

## 10. FFI 与底层安全

裸 FFI 声明必须写成 `trust extern`：

```zn
trust extern "C" fn read(fd: I32, buffer: USize, length: USize) -> ISize;
```

导出给外部 ABI 必须显式写 `@export("symbol", abi: C)`。导出函数只能使用 C-compatible 签名，不能让 panic unwind 穿过 ABI 边界，也不能把 Zeno 资源拥有者布局暴露给 C 侧猜测。

裸 FFI 调用、地址构造和指针操作必须出现在 `trust` 块中：

```zn
fn readFileRaw(fd: I32, mut out: ArraySlice<U8>) -> Result<USize, IoError> {
    let n = trust {
        read(fd, out.rawAddress(), out.len)
    };

    if n < 0 {
        return Err(IoError.LastOsError);
    }
    return Ok(n as USize);
}
```

安全包装必须声明并维护：

- 集合和句柄的所有权。
- 是否允许 null。
- 长度之间的关系。
- 错误转换。
- 线程安全假设。
- 回调是否可能逃逸。

包装层表面 API 必须使用 Zeno 类型，例如 `ArraySlice<T>`、`Array<T>`、`Vector<T>`、`Handle<T>`、`Result<T, E>` 和能力对象。

普通代码：

```zn
let bytes = try fs.readFile("config.zn");
```

`trust` 边界应尽量小。公开 API 泄露裸指针、裸地址或平台 ABI 细节时，编译器应警告，构建策略可以拒绝。

编译器必须生成信任报告，至少记录：

- `Zeno.toml` 中启用的 `trust` 能力和 manifest hash。
- `trust` 的源码位置。
- 使用的能力类别，例如 `ffiC`、`rawMemory`、`mmio`、`inlineAsm`、`interrupt`。
- 是否从公开 API 泄露裸能力。
- 调用链中哪些公开函数依赖该 `trust`。

## 11. 诊断要求

安全诊断应该说明失败规则和相关所有权路径。

必须支持的诊断：

- use after move。
- double move。
- 读取未初始化值。
- 访问值逃逸出底层存储。
- 可写访问与另一个活动访问重叠。
- 在禁止分配的上下文中发生隐藏分配。
- 没有 `trust` 的裸 FFI 声明或底层操作。
- `trust` 边界中违反普通所有权、初始化或类型规则。
- 在要求静态派发的上下文中使用接口派发。

好的诊断是语言设计的一部分。没有清晰诊断，安全系统语言会变得惩罚性太强。
