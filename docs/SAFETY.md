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
- 所有权转移由拥有位置表达：赋值、绑定、返回和字段初始化可以自动移动；函数 / 方法 `move` 参数从已有命名位置接收所有权时必须在调用点写 `move value`；`move self` 方法从已有命名接收者接收所有权时必须写 `move receiver.method()`；`match move`、`for move` 和 `move` 闭包捕获也会触发所有权转移。
- 被移动后的绑定不可再用。
- 活跃拥有者会被销毁一次。
- 部分初始化状态会记录哪些字段需要销毁。

深拷贝必须通过显式 `clone` API 表达。`Array<T>` / `Vector<T>` / `Map<K, V>` / `Set<T>` / `String` 这类可能分配的 clone 失败时调用当前 profile 的 `oom(layout) -> Never`；指定 allocator 时使用 `cloneIn`。引用计数增加只能通过 `Shared<T>.clone` 等类型名显式暴露共享成本的 API 表达。

compile-fail 示例：

```zn
let a = File.open("log.txt")?;
let b = a;
mut a.write("bad"); // expected-error: use of moved value
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
- 有自定义 `destroy` 块的类型在安全代码中禁止移出非 `Copy` 字段；类型自己的 `move self` 方法也只能完成资源状态，不能拆字段。
- 对已初始化位置赋值会先求右侧，再销毁旧值，再写入新值；右侧失败时旧值保持不变。
- 赋值不能覆盖当前存在活动访问的位置；需要取回旧值时使用 `replaceAt` 或类型提供的显式替换 API。
- 只读 `match` 不移动 enum payload；`match move` 消耗整个 enum 并允许移动出被选中 payload；`match mut` 只给 payload 唯一可写访问，不给所有权。
- `if let` 和 `while let` 遵守同样的只读 / `move` / `mut` pattern 规则。
- `let` 解构只允许不可失败 pattern，避免在初始化语义中引入隐式失败路径。
- or pattern 两侧必须绑定相同名字、类型和访问模式，防止某条分支中绑定不存在或所有权不同。
- 带 guard 的 pattern 不参与穷尽证明，guard 中不能移动出只读绑定。
- pattern 不能调用用户代码，不能隐藏分配、动态派发、反射、regex 或临时 collection slice。
- 带自定义 `destroy` 的类型不能在安全 pattern 中移动出非 `Copy` 字段或 payload。

## 4. 访问模式

大多数参数是非拥有访问路径。源码中使用普通类型名表达，编译器负责检查。

参数规则是：默认读，`mut` 写，`move` 拿走。`move` 参数进入函数体后是唯一拥有者，可以作为 `mut` 接收者或 `mut` 实参使用；调用方不写 `mut move`，仍然只写 `move value`。

只读访问：

- 是参数默认行为。
- 可以与其他只读访问共存。
- 不允许修改。
- 可以在被证明有效的存储生命周期内自由传递。
- 对非 `Copy` 资源不移动、不复制。
- 不能被当成拥有值返回、保存到拥有容器，或移动进 `Box<T>`、`Shared<T>`、线程、任务、future。

函数参数中的接口名表示匿名静态接口参数。例如 `writer: Writer` 在语义上等价于隐藏的 `W: Writer`，调用点按具体类型单态化，不生成接口表。函数返回中的接口名表示静态接口返回，所有返回路径必须产生同一个具体实现类型。接口名不能作为字段、集合元素、`static` 或其他长期存储类型；需要保存具体实现时使用显式泛型字段 `W: Writer`，需要拥有异构实现时使用 `Box<Writer>`，需要共享拥有时使用 `Shared<Writer>`。

唯一可写访问：

- 用 `mut` 表达。
- 排除对重叠存储的其他活动访问。
- 允许修改。
- 只能通过经过检查的库 API 缩短或拆分。
- 不取得所有权。

所有权接收：

- 用 `move` 参数表达。
- 从已有命名位置传入普通函数或方法的 `move` 参数时，调用点必须写 `move`；调用后源绑定不可再用。
- 被移动后的源绑定不可再用。
- 函数负责销毁该值，或继续把它移动给新的拥有者。

方法接收者遵守同一套规则：

- `self` 方法只读访问接收者。
- `mut self` 方法唯一可写访问接收者，不取得所有权；已有命名接收者调用时写成 `mut receiver.method(...)`，且接收者必须是可写位置。可写位置包括 `var` 绑定、可写字段、可写元素、`move` 参数和方法体内的 `move self`。
- `move self` 方法取得接收者所有权；已有命名接收者调用时写成 `move receiver.method(...)`，方法调用后原接收者不可再用。

拒绝示例：

```zn
let s = String.from("abc");
let v = s;
drop(v);
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
- 对 `Vector<T>`，改变长度或容量的结构性修改必须被拒绝；元素替换也必须遵守普通只读 / 可写访问冲突规则。
- 只读查询可以继续执行。

拒绝：

```zn
fn badPush(mut allocator: GlobalAllocator) -> Result<USize, AllocError> {
    var bytes = Vector<U8>.withCapacityIn(4, mut allocator);
    let view = bytes.asSlice();
    mut bytes.push(1); // expected-error: vector structural mutation while view is live
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

    mut bytes.push(1);
    return Ok(());
}
```

这条规则完全静态检查，不需要运行时借用计数、隐藏引用计数或锁。

## 7. 边界与集合

`Array<T>` 是拥有、连续、长度固定的数组。`Vector<T>` 是拥有、连续、可增长的数组。`ArraySlice<T>` 是不拥有的连续数组片段，低层表示是基础能力加长度。`Map<K, V>` 和 `Set<T>` 是拥有哈希集合，不是视图，也不保证遍历顺序。

`Array<T>` 和 `Vector<T>` 可以被只读地看成 `ArraySlice<T>`，也可以作为 `mut ArraySlice<T>` 传入以修改元素。改变长度需要 `mut Vector<T>`。

索引访问不表示所有权转移。`collection[index]` 是检查元素访问；`T: Copy` 时可以复制元素值，非 `Copy` 时只能形成短期只读元素访问。`mut collection[index]` 可以形成短期唯一可写元素访问。普通代码不能把 `collection[index]` 放进拥有位置，因为这会在容器里留下未初始化洞；从 `Vector<T>` 中取走元素必须通过 `pop`、`removeAt` 或 `swapRemove`，替换并取回旧值使用 `replaceAt`。

`Map.get(key)` 只为 `V: Copy` 提供，且 key 是只读 lookup key，不移动调用方的 `String`、`Array` 或其他非 `Copy` key。非 `Copy` value 的短期读取或可写更新使用 `map[key]` / `mut map[key]` 的检查访问。只有 `insert(move key, move value)` 和“可能插入”的 `entry(mut self, move key)` 需要拥有 key。`MapEntry<K, V>` 是短期 view-like 值，不能保存到结构体、集合、`static`、`Box`、`Shared`、逃逸闭包、线程、任务或 async future。

索引必须保证边界安全：

- 常量越界索引是编译错误。
- 动态索引需要检查，除非编译器能证明范围安全。
- `Array<T>`、`Vector<T>` 或 `ArraySlice<T>` 上的典型范围循环应消除冗余边界检查。
- 拆分 API 必须证明可写访问不重叠。
- `for item in data` 不移动非 `Copy` 元素，只创建只读元素访问。
- `for mut item in mut data` 在循环体内持有当前元素的唯一可写访问；循环期间不能结构性修改同一个 `Vector<T>`。
- `for move item in data` 只能用于拥有集合，提前退出时必须销毁尚未迭代的元素。

示例：

```zn
for i in 0..data.len {
    sum = sum + data[i]; // 边界检查应被消除
}
```

## 8. 分配区域安全

`In` 后缀分配 API 可能使用 scoped allocator。编译器必须跟踪 allocation region，防止拥有者比 allocator 活得更久。

规则：

- `EscapingAllocator` 创建的拥有者可以按普通所有权规则逃逸。
- `ArenaAllocator`、`BumpAllocator` 和 `FixedBufferAllocator` 这类 scoped allocator 创建的拥有者只能在 allocator 活跃期间使用。
- scoped allocator owner 不能返回、保存到长期结构体字段、`static`、`Box`、`Shared`、逃逸闭包、非 scoped 线程、任务或 async future 状态。
- 泛型函数如果返回 allocator 创建的 owner，必须要求 `A: EscapingAllocator`。

拒绝：

```zn
fn bad(mut arena: ArenaAllocator) -> Vector<U8> {
    return Vector<U8>.withCapacityIn(1024, mut arena);
} // expected-error: scoped allocation escapes allocator
```

## 9. 销毁安全

销毁逻辑不能复活已移动值，也不能创建比对象活得更久的访问值。

规则：

- `destroy` 不返回值，不能使用 `try`，不能参与常规错误流。
- `destroy` 不是 `move self` 方法，用户不能直接调用。
- `destroy` 调用的清理 API 必须不可失败或 best-effort，返回 `Unit`。
- 需要报告清理错误时，资源类型必须提供显式 `close`、`flush`、`finish` 等 `move self` 方法返回 `Result<T, E>`。
- `destroy` 块以隐式 `self` 接收最终拥有者。
- `destroy` 块运行时可以访问字段和更新本对象内部状态，但不能移动字段、返回字段或创建逃逸访问。
- `destroy` 块之后，字段按源码声明顺序的逆序销毁；这属于语义顺序，不受 Auto layout 的内存字段重排影响。
- 如果对象被部分移动，只有仍初始化的字段会被销毁。
- 带 `destroy` 的类型即使进入 `move self` 方法，方法体中的 `self` 在退出时仍然按普通 RAII 规则销毁；显式完成方法应通过状态位让 `destroy` no-op。
- 析构逻辑不能移动出另一个字段析构所依赖的字段，除非类型显式建模该依赖。

stage0 可以采用保守规则：`destroy` 块可以读取字段和调用方法，但不能移出字段；带 `destroy` 的类型在安全代码中也不能从 `move self` 方法移出非 `Copy` 字段。

## 10. 数据竞争自由

数据竞争指并发访问同一存储，至少一个访问是写入，并且缺少同步。普通 Zeno 代码不应能表达这种状态。

线程规则：

- 把拥有者移动到另一个线程，会让源线程中的绑定不可用。
- 被移动到另一个线程、任务或可跨线程 future 的值必须是 `Send`。
- `Copy` 值可以复制进线程。
- 共享不可变访问要求类型是 `Sync`。
- 共享可变访问必须经过同步类型。
- 可写访问不能跨越非 scoped 线程边界。
- scoped 线程可以携带短期可写访问，但并行切分必须来自 `splitDisjoint`、`splitAt` 或其他被编译器认可的不重叠 API。

`Send` / `Sync` 是结构化自动推导的标记接口。普通纯数据类型在字段满足条件时自动获得；带自定义 `destroy` 的资源类型不会自动获得，因为析构可能要求特定线程或特定平台上下文。作者可以用 `trust impl Send for T {}` 或 `trust impl Sync for T {}` 声明这些额外不变量，编译器必须把它们记录到信任报告。

接口约束的线程安全由具体类型推导，和普通泛型参数一样。接口拥有者也必须保留线程安全能力。`Box<Writer>` 不能跨线程移动；需要定义 `interface ThreadWriter: Writer, Send {}` 并使用 `Box<ThreadWriter>`。`Shared<Writer>` 不能跨线程共享；需要定义 `interface SharedWriter: Writer, Send, Sync {}` 并使用 `Shared<SharedWriter>`。

拒绝：

```zn
var count = 0;
Thread.spawn(move () {
    count = count + 1;
}); // expected-error: writable access cannot escape to unscoped thread
```

拒绝未证明的不重叠访问：

```zn
Thread.scope((mut scope) {
    scope.spawn((workerId: USize) {
        process(mut shards[workerId]);
    });
}); // expected-error: disjoint mutable access is not proven
```

允许：

```zn
let count = Atomic<U64>.new(0);
let shared = Shared.new(count);
let t = Thread.spawn(move () {
    shared.fetchAdd(1, Ordering.Relaxed);
});
try move t.join();
```

## 11. Future 取消安全

`async fn` 生成拥有状态机。future 被 drop 时表示取消。

规则：

- future drop 必须根据当前状态销毁已经初始化且仍拥有的字段。
- 跨 `await` 存活的 `defer` 必须进入 future 状态，并在完成、错误提前返回、panic-unwind 或取消 drop 时执行。
- future drop 不能 `await`；跨 `await` 的 `defer` 不能包含 `await`。
- 短期访问、`ArraySlice<T>`、`StringSlice` 和 scoped allocator owner 不能作为独立值跨 `await` 进入 future 状态。接口约束按具体类型处理；若具体类型需要跨 `await`，它必须由 future 拥有并满足对应所有权规则。
- `await mut receiver.method(...)` 允许立即等待 async `mut self` 调用，前提是 `receiver` 由当前 future 拥有；编译器把可写访问限制在被等待调用内部。把这个调用结果先保存、返回、放入结构体、传给 `spawn` 或跨另一个 `await` 使用都必须拒绝。
- v1 通过禁止访问值逃逸跨 `await` 避免用户可见 `Pin`。

## 12. FFI 与底层安全

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

## 13. 诊断要求

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
