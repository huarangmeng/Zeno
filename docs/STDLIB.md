# Zeno 核心标准库

Zeno 把始终可用的 `core` 库和依赖平台的 hosted 库分开。

`core` 必须能在 freestanding 构建中工作。`std` 可以依赖 OS、分配器、文件系统、线程和 I/O。

## 1. 库分层

`core`：

- 基础接口。
- `Option`、`Result`。
- `ArraySlice`、`String`。
- 编译器需要的 move / destroy 辅助能力。
- 分配器接口定义，但不要求全局分配器。
- 目标支持时提供原子类型。
- 编译器发行包中的 intrinsic 声明。

`alloc`：

- `Box`。
- `Array`。
- `Vector`。
- `Shared`。
- `String` 的分配式构造和构建器。
- 基于分配器的集合。

`std`：

- 文件系统。
- 网络。
- 进程。
- 环境变量。
- OS 线程。
- 可选任务运行时。
- mutex、condition variable 等同步类型。
- 平台错误转换。

Freestanding 程序可以只使用 `core`。

## 2. Option 与 Result

```zn
enum Option<T> {
    Some(T),
    None,
}

enum Result<T, E> {
    Ok(T),
    Err(E),
}
```

必要方法：

```zn
impl<T> Option<T> {
    fn isSome(self) -> Bool;
    fn isNone(self) -> Bool;
    fn okOr<E>(move self, move error: E) -> Result<T, E>;
}

impl<T: Copy> Option<T> {
    fn unwrapOr(self, fallback: T) -> T;
}

impl<T, E> Result<T, E> {
    fn isOk(self) -> Bool;
    fn isErr(self) -> Bool;
    fn mapErr<F>(move self, mapper: Fn<E, F>) -> Result<T, F>;
}
```

`try expr` 可以用于 `Result<T, E>` 或 `Option<T>`，但不做隐式 `Option` 到 `Result` 转换，也不做隐式错误类型转换。需要跨类型传播时使用 `okOr` 或 `mapErr`。

## 3. 数值

每个整数类型 `T` 都提供同名方法族。这里的 `T` 代表 `I8`、`I16`、`I32`、`I64`、`ISize`、`U8`、`U16`、`U32`、`U64`、`USize` 中的任意一个具体类型。

```zn
impl T {
    fn wrappingAdd(self, rhs: T) -> T;
    fn wrappingSub(self, rhs: T) -> T;
    fn wrappingMul(self, rhs: T) -> T;

    fn checkedAdd(self, rhs: T) -> Option<T>;
    fn checkedSub(self, rhs: T) -> Option<T>;
    fn checkedMul(self, rhs: T) -> Option<T>;
    fn checkedDiv(self, rhs: T) -> Option<T>;
    fn checkedRem(self, rhs: T) -> Option<T>;

    fn saturatingAdd(self, rhs: T) -> T;
    fn saturatingSub(self, rhs: T) -> T;
    fn saturatingMul(self, rhs: T) -> T;

    fn overflowingAdd(self, rhs: T) -> (T, Bool);
    fn overflowingSub(self, rhs: T) -> (T, Bool);
    fn overflowingMul(self, rhs: T) -> (T, Bool);
}
```

普通 `+`、`-`、`*` 等价于对应 wrapping 运算，不隐藏溢出检查。`checked*` 方法返回 `Option<T>`：`Some(value)` 表示成功，`None` 表示溢出、除零、取模零，或有符号最小值除以 `-1` 这类不可表示结果。

转换 API：

```zn
impl T {
    fn fromChecked<U>(value: U) -> Option<T>;
    fn truncate<U>(value: U) -> T;
    fn saturate<U>(value: U) -> T;
}
```

`fromChecked` 表示检查转换；`truncate` 表示明确截断到目标类型的低位位模式；`saturate` 表示把数学值钳制到目标整数类型的最小值和最大值之间。普通 `as` 只用于无损转换。

```zn
let wide: U32 = byte as U32;
let maybeByte = U8.fromChecked(wide);
let low = U8.truncate(wide);
let clamped = U8.saturate(wide);
```

## 4. 集合

Zeno 的 v1 核心集合模型只围绕连续内存：

```zn
Array<T>             // 拥有、连续、长度创建后固定
Vector<T>            // 拥有、连续、可增长、有 capacity
ArraySlice<T>        // 不拥有、连续、长度稳定的数组片段
```

公开 API 通过参数模式表达访问能力：

```zn
fn checksum(data: ArraySlice<U8>) -> U32;         // 只读访问
fn fill(mut data: ArraySlice<U8>, value: U8);     // 可写元素访问
fn push(mut data: Vector<U8>, value: U8);         // 可能增长
fn consume(move data: Vector<U8>);                // 接收所有权
fn parse(data: ArraySlice<U8>) -> Token;          // 临时只读视图
```

必要方法：

```zn
impl<T> Array<T> {
    fn len(self) -> USize;
    fn isEmpty(self) -> Bool;
    fn asSlice(self) -> ArraySlice<T>;
}

impl<T: Copy> Array<T> {
    fn get(self, index: USize) -> Option<T>;
    fn filled<A: Allocator>(count: USize, value: T, mut allocator: A) -> Result<Array<T>, AllocError>;
    fn clone<A: Allocator>(self, mut allocator: A) -> Result<Array<T>, AllocError>;
}

impl<T> Vector<T> {
    fn withCapacity<A: Allocator>(capacity: USize, mut allocator: A) -> Result<Vector<T>, AllocError>;
    fn len(self) -> USize;
    fn capacity(self) -> USize;
    fn asSlice(self) -> ArraySlice<T>;
    fn push(mut self, move value: T) -> Result<Unit, AllocError>;
    fn pop(mut self) -> Option<T>;
}

impl<T: Copy> Vector<T> {
    fn clone<A: Allocator>(self, mut allocator: A) -> Result<Vector<T>, AllocError>;
}

impl<T> ArraySlice<T> {
    fn len(self) -> USize;
    fn isEmpty(self) -> Bool;
    fn dropPrefix(self, count: USize) -> ArraySlice<T>;
}

impl<T: Copy> ArraySlice<T> {
    fn get(self, index: USize) -> Option<T>;
}
```

`Array<T>`、`Vector<T>` 和 `ArraySlice<T>` 的索引语法会降低为检查访问，或在编译器能证明安全时降低为无冗余检查访问。

`Array<T>.clone` 和 `Vector<T>.clone` 是显式深拷贝。v1 先为 `T: Copy` 元素提供集合 clone；非 `Copy` 元素的深拷贝接口后续单独设计。集合 clone 可能分配，所以必须接收 allocator 并返回 `Result<_, AllocError>`。`Array<T>` 和 `Vector<T>` 默认不是 `Copy`，也不能通过普通赋值隐藏复制成本。

在 API 参数位置，连续数据优先写成 `ArraySlice<T>`。调用方可以传入 `Array<T>`、`Vector<T>` 或已有的 `ArraySlice<T>`。需要增长时使用 `Vector<T>`，需要固定长度拥有者时使用 `Array<T>`。

`Vector<T>` 的 `push`、`pop`、`reserve`、`insert`、`remove`、`clear`、`shrink` 等结构性修改要求当前没有活跃的 `ArraySlice<T>` 依赖该 `Vector<T>`。只读查询如 `len`、`capacity` 和新的只读 `asSlice` 可以继续使用。

`ArraySlice<T>` 是非拥有访问值，不能作为结构体字段，也不能通过 `Array<ArraySlice<T>>`、`Vector<ArraySlice<T>>` 等形式间接保存。结构体应保存拥有者：

```zn
struct Packet {
    bytes: Array<U8>,
}
```

需要读取时临时产生切片：

```zn
fn parse(packet: Packet) -> Result<Token, ParseError> {
    return parseHeader(packet.bytes.asSlice());
}
```

从参数派生的切片可以返回；返回值的隐藏有效范围由参数决定。

```zn
fn body(bytes: ArraySlice<U8>) -> ArraySlice<U8> {
    return bytes.dropPrefix(4);
}
```

但不能返回指向局部拥有者的切片：

```zn
fn bad(mut allocator: GlobalAllocator) -> Result<ArraySlice<U8>, AllocError> {
    let bytes = try Array<U8>.filled(64, 0, mut allocator);
    return Ok(bytes.asSlice()); // expected-error: view outlives local storage
}
```

`ArraySlice<T>` 也不能进入 `Box<T>`、`Shared<T>`、`Array<T>`、`Vector<T>`、`static`、逃逸闭包、线程、任务或 async future。需要这些能力时，保存 `Array<T>`、`Vector<T>` 或 `Shared<Array<T>>`。需要共享可变状态时，必须显式使用 `Mutex<T>`、原子类型或其他同步容器。

少量原始能力方法只能在 `trust` 边界内调用：

```zn
impl<T> ArraySlice<T> {
    fn rawAddress(self) -> USize;
}
```

普通代码不能调用 `rawAddress`。需要把缓冲区传给 C ABI、DMA 或硬件寄存器时，应在很小的 `trust` 块中取出地址，并立即封装回安全 API。

## 5. 字符串

`String` 是 UTF-8 文本类型。字符串 literal 的类型是 `String`。

规则：

- 普通参数 `text: String` 是只读访问，不复制文本。
- 需要接收字符串所有权时必须写 `move text: String`。
- 从字节转换到 `String` 必须验证 UTF-8；跳过验证只能出现在 `trust` 边界中。
- 会分配的字符串拼接必须返回 `String`，并要求分配器上下文。
- 会分配的格式化 API 必须在返回类型或调用形式中体现。

## 6. 分配

分配器接口：

```zn
interface Allocator {
    fn allocate(mut self, layout: Layout) -> Result<Allocation, AllocError>;
    fn deallocate(mut self, allocation: Allocation, layout: Layout);
}
```

拥有式 box：

```zn
struct Box<T, A: Allocator> {
    // 内部表示
}
```

`Box.new(move value, allocator)` 是显式分配并接收 `value` 所有权。Hosted profile 可以提供 `GlobalAllocator`，但核心语言语义不能假设它存在。

`Box<Interface, A>` 表示拥有式异构接口值。若 `T: Interface`，从 `T` 构造 `Box<Interface, A>` 只分配一次具体 `T`；从 `Box<T, A>` 移动转换到 `Box<Interface, A>` 不重新分配、不复制值，只携带接口表元数据。`Box<Interface, A>` 销毁时通过接口表销毁具体值，再用 allocator `A` 释放内存。

```zn
fn boxWriter<A: Allocator>(move file: File, mut allocator: A) -> Result<Box<Writer, A>, AllocError> {
    return Box.new(move file, mut allocator);
}

fn eraseWriter<A: Allocator>(move file: Box<File, A>) -> Box<Writer, A> {
    return move file;
}
```

共享拥有：

```zn
struct Shared<T, A: Allocator> {
    // 内部表示
}

impl<T, A: Allocator> Shared<T, A> {
    fn new(move value: T, mut allocator: A) -> Result<Shared<T, A>, AllocError>;
    fn clone(self) -> Shared<T, A>;
    fn strongCount(self) -> USize;
}
```

`Shared<T>` 表示引用计数共享所有权。引用计数成本必须在类型名里可见。`Shared<T>.clone` 只增加引用计数，不深拷贝底层值；这个成本通过 `Shared` 类型名显式暴露。`Shared<T>` 默认只提供不可变共享；共享可变状态需要 `Mutex<T>`、原子类型或其他同步容器。`Shared<T, A>` 只有在 `T: Send, Sync` 且 `A: Send, Sync` 时才是 `Send` 和 `Sync`。

`Shared<Interface, A>` 表示共享拥有式异构接口值。若 `T: Interface`，`Shared<T, A>` 可以移动转换成 `Shared<Interface, A>`，不增加引用计数、不重新分配、不复制具体值。`Shared<Interface, A>.clone()` 仍然只增加引用计数。`Shared<Interface, A>` 只能提供只读接口访问；如果接口方法需要 `mut self`，调用方必须使用 `Box<Interface, A>`、具体唯一拥有者，或显式同步容器。需要跨线程共享接口对象时，应定义包含 `Send` 和 `Sync` 的命名接口，例如 `interface SharedWriter: Writer, Send, Sync {}`，然后使用 `Shared<SharedWriter, A>`。

## 7. 接口

基础接口：

```zn
interface Copy {}
interface Eq { fn eq(self, other: Self) -> Bool; }
interface Ord: Eq { fn cmp(self, other: Self) -> Ordering; }
interface Send {}
interface Sync {}
```

`Send` 和 `Sync` 是编译器认识的标记接口。

- `Send`：值的所有权可以跨线程、任务或可跨线程 future 边界移动。
- `Sync`：同一个值的只读共享访问可以被多个线程同时使用。

这两个接口都没有方法，不进入接口表，也不是锁。共享可变状态仍然必须通过 `Mutex<T>`、原子类型或其他同步容器表达。

自动推导规则：

- 标量基础类型、`String`、`Unit`、`Never` 自动是 `Send` 和 `Sync`。
- `Array<T>`、`Vector<T>`、`Option<T>`、`Result<T, E>`、元组和普通 enum 按元素或载荷推导。
- 普通结构体在没有自定义 `destroy` 块且所有字段满足对应条件时自动推导。
- `Box<T, A>`：`Send` 需要 `T: Send` 和 `A: Send`；`Sync` 需要 `T: Sync` 和 `A: Sync`。
- `Shared<T, A>`：`Send` 和 `Sync` 都需要 `T: Send, Sync` 和 `A: Send, Sync`。
- `Mutex<T>`：`Send` 和 `Sync` 都需要 `T: Send`。
- `Atomic<T>` 对受支持载荷自动是 `Send` 和 `Sync`。
- `ArraySlice<T>`、裸接口访问和其他非拥有访问值不能跨非 scoped 并发边界。

带自定义 `destroy` 块、裸句柄、裸地址、平台线程亲和资源或其他编译器无法证明的类型，不自动获得 `Send` / `Sync`。手动声明必须使用 `trust impl`：

```zn
trust impl Send for OsHandle {}
trust impl Sync for SharedKernelTable {}
```

普通包中的 `impl Send for T {}` 或 `impl Sync for T {}` 会被拒绝。`Copy` 仍由显式 `: Copy` 和编译器字段检查控制。销毁由类型可选的 `destroy` 块控制，不通过公开接口实现。

## 8. 同步

Hosted 同步类型位于 `std.sync`。

```zn
struct Mutex<T> { ... }
struct MutexGuard<T> { ... }

impl<T> Mutex<T> {
    fn new(move value: T) -> Mutex<T>;
    fn lock(self) -> Result<MutexGuard<T>, LockError>;
}

impl<T> MutexGuard<T> {
    fn get(mut self) -> mut T;
}
```

锁获取在类型上可见。锁后的修改必须通过 guard。

原子类型：

```zn
struct Atomic<T> { ... }
enum Ordering { Relaxed, Acquire, Release, AcqRel, SeqCst }
```

只有受支持的整数、布尔和指针大小能力类型可以是 atomic。

## 9. trust 与 FFI 包装

安全平台 API 应把资源句柄暴露为拥有或访问能力类型：

```zn
struct File {
    handle: os.Handle,
}

impl File {
    fn open(path: String) -> Result<File, IoError>;
    fn read(mut self, mut out: ArraySlice<U8>) -> Result<USize, IoError>;
    fn write(mut self, bytes: ArraySlice<U8>) -> Result<USize, IoError>;
    fn close(move self) -> Result<Unit, IoError>;
}
```

裸句柄、裸地址和裸指针可以存在于 `trust` 边界内。普通调用方拿到的是带 RAII 的类型化句柄。

示例：

```zn
trust extern "C" fn close(fd: I32) -> I32;

struct File {
    handle: os.Handle,
    closed: Bool,
}

impl File {
    fn close(move self) -> Result<Unit, IoError> {
        let rc = trust { close(self.handle.rawFd) };
        if rc < 0 {
            return Err(IoError.LastOsError);
        }
        self.closed = true;
        return Ok(());
    }

    destroy {
        if !self.closed {
            closeBestEffort(self.handle.rawFd);
        }
    }
}

fn closeBestEffort(rawFd: I32) {
    let ignored = trust { close(rawFd) };
}
```

`File.close` 是显式错误处理 API。`destroy` 中只能调用 `closeBestEffort` 这类不可失败兜底 API，不能使用 `try`。显式 `close` 成功后应解除兜底清理，避免 `destroy` 二次关闭。

标准库应把 `trust` 边界保持得很小，并在公开 API 中使用 `Result`、`Handle<T>`、`ArraySlice<T>`、`Mmio<T>`、`Port<T>` 等有语义的类型。

## 10. Panic

`panic` 由 profile 提供：

- Freestanding profile：abort 或目标 trap。
- Hosted profile：可配置 abort 或 unwind。

可恢复错误必须使用 `Result`。

## 11. 命名规则

标准 API 应该暴露成本：

- 除非返回类型显示分配，否则 `new` 应只用于低成本构造。
- 分配应使用 `Box.new`、`Array.filled`、`Vector.withCapacity` 或 allocator 参数。
- `clone` 用于显式复制；`Array<T>` / `Vector<T>` 的 `clone` 是深拷贝并显式接收 allocator。
- 引用计数共享使用 `Shared.new`；引用计数增加使用 `Shared.clone`，成本由 `Shared` 类型名显式暴露。
- 静态接口约束写成泛型边界，例如 `W: Writer`；需要动态接口派发的短期访问 API 直接使用接口名，例如 `Writer`。
- 需要长期保存具体实现时优先使用泛型字段，例如 `writer: W` 且 `W: Writer`。
- 需要拥有异构接口值时必须写出拥有者，例如 `Box<Writer>` 或 `Shared<Writer>`；单独的 `Writer` 不表示隐藏分配，也不能作为字段或集合元素长期保存。
