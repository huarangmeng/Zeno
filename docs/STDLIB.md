# Zeno 核心标准库

Zeno 把始终可用的 `core` 库和依赖平台的 hosted 库分开。

`core` 必须能在 freestanding 构建中工作。`std` 可以依赖 OS、分配器、文件系统、线程和 I/O。

本文不是完整标准库 API 清单。v0.1 设计阶段只定义足以验证语言语义和性能模型的最小库边界：基础 enum、拥有容器、分配显式性、同步/并发边界、FFI 安全包装和少量审计表。完整 I/O、网络、路径、时间、格式化、序列化和生态 API 应在语言设计冻结后单独设计。

## 1. 库分层

`core`：

- 基础接口。
- `Option`、`Result`。
- `ArraySlice`、`StringSlice`。
- `CachePadded<T>` 这类不依赖分配器的低层布局工具。
- 编译器需要的 move / destroy 辅助能力。
- 分配器接口定义，但不要求全局分配器。
- `panic(message) -> Never`、`PanicInfo` 诊断访问值和 `oom(layout) -> Never` 的 profile 绑定声明。
- 目标支持时提供原子类型。
- 编译器发行包中的 intrinsic 声明。

`alloc`：

- `Box`。
- `Array`。
- `Vector`。
- `Map`。
- `Set`。
- `Shared`。
- `String` 的拥有式构造、增长和构建器。
- 默认 allocator 入口。
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
    fn okOrElse<E>(move self, move makeError: OnceFn<E>) -> Result<T, E>;
    fn unwrapOrElse(move self, move makeFallback: OnceFn<T>) -> T;
}

impl<T: Copy> Option<T> {
    fn unwrapOr(self, fallback: T) -> T;
}

impl<T, E> Result<T, E> {
    fn isOk(self) -> Bool;
    fn isErr(self) -> Bool;
    fn mapErr<F>(move self, move mapper: OnceFn<E, F>) -> Result<T, F>;
}
```

`try expr` 可以用于 `Result<T, E>` 或 `Option<T>`，但不做隐式 `Option` 到 `Result` 转换，也不做隐式错误类型转换。需要跨类型传播时使用 `okOr`、`okOrElse` 或 `mapErr`。

`Option<T>` 和 `Result<T, E>` 的只读查询方法使用普通只读 `match`，不移动载荷。`okOr`、`okOrElse`、`unwrapOrElse` 和 `mapErr` 使用 `match move self` 取得成功或错误载荷所有权。`unwrapOr` 只为 `T: Copy` 提供，因为它需要在只读路径返回 payload 或 fallback 的复制值。

标准库不得为 `Option` / `Result` 提供隐藏分配或隐藏动态派发的自定义 pattern。所有 `Some`、`None`、`Ok`、`Err` 匹配都使用语言内建 enum pattern lowering。

`okOr(error)` 和 `unwrapOr(fallback)` 适合便宜值。`okOrElse(makeError)` 和 `unwrapOrElse(makeFallback)` 只在 `None` 路径调用闭包；`mapErr(mapper)` 只在 `Err` 路径调用闭包。这些 lazy API 接收 `OnceFn`，因为闭包最多调用一次，可以移动捕获资源。传入这些 API 的闭包默认是非逃逸 callable，不分配，编译器应能内联为普通分支。

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

布局优化和协议端序所需的底层事实由编译器与 core/std 的可信实现表达，不暴露成普通用户必须书写的特殊非零类型或端序包装类型。用户面对的 API 应该使用自然类型，例如 `U16`、`U32`、`FileDescriptor` 或 `Ipv4Header.totalLength()`。

`fromChecked` 表示检查转换；`truncate` 表示明确截断到目标类型的低位位模式；`saturate` 表示把数学值钳制到目标整数类型的最小值和最大值之间。普通 `as` 只用于无损转换。

```zn
val wide: U32 = byte as U32;
val maybeByte = U8.fromChecked(wide);
val low = U8.truncate(wide);
val clamped = U8.saturate(wide);
```

## 4. 集合

Zeno 的 v1 核心集合分为连续集合和关联集合。连续集合负责最高吞吐的数组式访问：

```zn
Array<T>             // 拥有、连续、长度创建后固定
Vector<T>            // 拥有、连续、可增长、有 capacity
ArraySlice<T>        // 不拥有、连续、长度稳定的数组片段
```

关联集合负责按键查找和去重：

```zn
Map<K, V>            // 拥有、哈希表、键值映射
Set<T>               // 拥有、哈希表、唯一值集合
```

公开 API 通过参数模式表达访问能力：

```zn
fn checksum(data: ArraySlice<U8>) -> U32;         // 只读访问
fn fill(mut data: ArraySlice<U8>, value: U8);     // 可写元素访问
fn push(mut data: Vector<U8>, value: U8);         // 可能增长
fn consume(move data: Vector<U8>);                // 接收所有权
fn parse(data: ArraySlice<U8>) -> Token;          // 临时只读视图
```

调用 `move` 参数时，已有命名 owner 必须在调用点写 `move owner`；临时值和字面量没有后续可用绑定，可以直接传给 `move` 参数。`move` 参数在函数体内是唯一拥有者，可以作为 `mut` 接收者或 `mut` 实参使用。

必要方法：

```zn
impl<T> Array<T> {
    fn len(self) -> USize;
    fn isEmpty(self) -> Bool;
    fn asSlice(self) -> ArraySlice<T>;
    fn splitDisjoint(mut self, parts: USize) -> DisjointSlices<T>;
    fn replaceAt(mut self, index: USize, move value: T) -> T;
}

impl<T: Copy> Array<T> {
    fn get(self, index: USize) -> Option<T>;
    fn filled(count: USize, value: T) -> Array<T>;
    fn filledIn<A: Allocator>(count: USize, value: T, mut allocator: A) -> Array<T>;
    fn clone(self) -> Array<T>;
    fn cloneIn<A: Allocator>(self, mut allocator: A) -> Array<T>;
}

impl<T> Vector<T> {
    fn withCapacity(capacity: USize) -> Vector<T>;
    fn withCapacityIn<A: Allocator>(capacity: USize, mut allocator: A) -> Vector<T>;
    fn len(self) -> USize;
    fn capacity(self) -> USize;
    fn asSlice(self) -> ArraySlice<T>;
    fn splitDisjoint(mut self, parts: USize) -> DisjointSlices<T>;
    fn reserve(mut self, additional: USize);
    fn reserveExact(mut self, additional: USize);
    fn tryReserve(mut self, additional: USize) -> Result<Unit, AllocError>;
    fn tryReserveExact(mut self, additional: USize) -> Result<Unit, AllocError>;
    fn push(mut self, move value: T);
    fn pop(mut self) -> Option<T>;
    fn insert(mut self, index: USize, move value: T);
    fn removeAt(mut self, index: USize) -> T;
    fn swapRemove(mut self, index: USize) -> T;
    fn replaceAt(mut self, index: USize, move value: T) -> T;
    fn clear(mut self);
    fn shrinkToFit(mut self);
}

impl<T: Copy> Vector<T> {
    fn clone(self) -> Vector<T>;
    fn cloneIn<A: Allocator>(self, mut allocator: A) -> Vector<T>;
}

impl<T> ArraySlice<T> {
    fn len(self) -> USize;
    fn isEmpty(self) -> Bool;
    fn dropPrefix(self, count: USize) -> ArraySlice<T>;
    fn splitDisjoint(mut self, parts: USize) -> DisjointSlices<T>;
}

impl<T: Copy> ArraySlice<T> {
    fn get(self, index: USize) -> Option<T>;
}

struct DisjointSlices<T> {
    // scoped view iterator; not a long-lived owner
}
```

关联集合必要 API：

```zn
struct Hasher {
    // 平台和 profile 选择的哈希状态；不分配
}

impl Hasher {
    fn write(mut self, bytes: ArraySlice<U8>);
    fn writeU64(mut self, value: U64);
    fn finish(self) -> U64;
}

interface Hash {
    fn hash(self, mut state: Hasher);
}

interface HashKey: Hash, Eq {}
interface CopyHashKey: HashKey, Copy {}

interface LookupKey<K: HashKey>: Hash {
    fn equalsKey(self, key: K) -> Bool;
}

struct Map<K: HashKey, V> {
    // 开放寻址或等价 cache-friendly 哈希表
}

impl<K: HashKey, V> Map<K, V> {
    fn empty() -> Map<K, V>;
    fn emptyIn<A: Allocator>(mut allocator: A) -> Map<K, V>;
    fn withCapacity(capacity: USize) -> Map<K, V>;
    fn withCapacityIn<A: Allocator>(capacity: USize, mut allocator: A) -> Map<K, V>;
    fn len(self) -> USize;
    fn capacity(self) -> USize;
    fn isEmpty(self) -> Bool;
    fn containsKey<Q: LookupKey<K>>(self, key: Q) -> Bool;
    fn reserve(mut self, additional: USize);
    fn reserveExact(mut self, additional: USize);
    fn tryReserve(mut self, additional: USize) -> Result<Unit, AllocError>;
    fn tryReserveExact(mut self, additional: USize) -> Result<Unit, AllocError>;
    fn insert(mut self, move key: K, move value: V) -> Option<V>;
    fn remove<Q: LookupKey<K>>(mut self, key: Q) -> Option<V>;
    fn removeEntry<Q: LookupKey<K>>(mut self, key: Q) -> Option<(K, V)>;
    fn entry(mut self, move key: K) -> MapEntry<K, V>;
    fn clear(mut self);
    fn shrinkToFit(mut self);
}

impl<K: HashKey, V: Copy> Map<K, V> {
    fn get<Q: LookupKey<K>>(self, key: Q) -> Option<V>;
}

impl<K: CopyHashKey, V: Copy> Map<K, V> {
    fn clone(self) -> Map<K, V>;
    fn cloneIn<A: Allocator>(self, mut allocator: A) -> Map<K, V>;
}

struct MapEntry<K: HashKey, V> {
    // view-like entry; cannot escape
}

impl<K: HashKey, V> MapEntry<K, V> {
    fn exists(self) -> Bool;
    fn insert(mut self, move value: V) -> Option<V>;
    fn remove(mut self) -> Option<V>;
    fn removeEntry(mut self) -> Option<(K, V)>;
}

impl<K: HashKey, V: Copy> MapEntry<K, V> {
    fn value(self) -> V;
    fn replace(mut self, value: V);
}

struct Set<T: HashKey> {
    // 与 Map 共享实现策略，但公开集合语义
}

impl<T: HashKey> Set<T> {
    fn empty() -> Set<T>;
    fn emptyIn<A: Allocator>(mut allocator: A) -> Set<T>;
    fn withCapacity(capacity: USize) -> Set<T>;
    fn withCapacityIn<A: Allocator>(capacity: USize, mut allocator: A) -> Set<T>;
    fn len(self) -> USize;
    fn capacity(self) -> USize;
    fn isEmpty(self) -> Bool;
    fn contains<Q: LookupKey<T>>(self, value: Q) -> Bool;
    fn reserve(mut self, additional: USize);
    fn reserveExact(mut self, additional: USize);
    fn tryReserve(mut self, additional: USize) -> Result<Unit, AllocError>;
    fn tryReserveExact(mut self, additional: USize) -> Result<Unit, AllocError>;
    fn insert(mut self, move value: T) -> Bool;
    fn remove<Q: LookupKey<T>>(mut self, value: Q) -> Bool;
    fn take<Q: LookupKey<T>>(mut self, value: Q) -> Option<T>;
    fn clear(mut self);
    fn shrinkToFit(mut self);
}

impl<T: CopyHashKey> Set<T> {
    fn clone(self) -> Set<T>;
    fn cloneIn<A: Allocator>(self, mut allocator: A) -> Set<T>;
}
```

## 4.1 API 所有权审计

标准库 API 按“读、改、存储、消费”分类。调用点必须能看出可写访问和所有权结束；查找类 API 不能为了方便实现而移动调用方的 key。

| API | 接收者 | 参数 | 所有权语义 | 调用示例 |
| --- | --- | --- | --- | --- |
| `Array.len` / `Vector.len` / `String.len` | `self` | 无 | 只读，不移动 | `items.len()` |
| `Array.asSlice` / `Vector.asSlice` / `String.asSlice` | `self` | 无 | 只读访问，返回短期 view | `items.asSlice()` |
| `Array.replaceAt` | `mut self` | `index`, `move value` | 修改数组，存储新值，返回旧值 owner | `mut items.replaceAt(i, move value)` |
| `Vector.push` | `mut self` | `move value` | 修改 vector，存储 value | `mut items.push(move value)` |
| `Vector.push` with temporary | `mut self` | temporary value | 修改 vector，临时值直接进入容器 | `mut items.push(Item { id: 1 })` |
| `Vector.pop` / `removeAt` / `swapRemove` | `mut self` | index | 修改 vector，返回被移出的 owner | `val item = mut items.removeAt(i)` |
| `Vector.reserve` / `tryReserve` | `mut self` | size | 修改容量，不移动元素 | `try mut items.tryReserve(n)` |
| `Map.get` / `containsKey` | `self` | `LookupKey<K>` | 只读 lookup，不移动 key，不分配 | `users.get(name)` |
| `Map.remove` | `mut self` | `LookupKey<K>` | 只读 lookup key，移除并销毁表内 key，返回表内 value | `val old = mut users.remove(name)` |
| `Map.removeEntry` | `mut self` | `LookupKey<K>` | 只读 lookup key，返回表内 key/value owner | `val pair = mut users.removeEntry(name)` |
| `Map.insert` | `mut self` | `move key`, `move value` | 修改 map，把 key/value 存入表 | `mut users.insert(move name, move user)` |
| `Map.entry` | `mut self` | `move key` | 可能插入时拥有 key，返回短期 entry view | `val e = mut users.entry(move name)` |
| `Set.contains` | `self` | `LookupKey<T>` | 只读 lookup，不移动 value | `seen.contains(name)` |
| `Set.remove` / `take` | `mut self` | `LookupKey<T>` | 只读 lookup value，移除表内 value | `val old = mut seen.take(name)` |
| `Set.insert` | `mut self` | `move value` | 修改 set，把 value 存入表 | `mut seen.insert(move name)` |
| `String.push` | `mut self` | `StringSlice` | 修改 string，复制 text 字节，参数不移动 | `mut text.push(" suffix")` |
| `String.clone` | `self` | 无 | 显式深拷贝，可能分配 | `val copy = text.clone()` |
| `Box.new` | static | `move value` | 分配并拥有 value | `Box.new(move value)` |
| `Shared.new` | static | `move value` | 分配控制块并共享拥有 value | `Shared.new(move value)` |
| `Shared.clone` | `self` | 无 | 增加引用计数，不深拷贝 | `shared.clone()` |
| `Mutex.new` | static | `move value` | 构造同步容器并拥有 value | `Mutex.new(move value)` |
| `Mutex.lock` | `self` | 无 | 只读访问 mutex，返回 guard；锁成本由类型可见 | `val guard = try mutex.lock()` |
| `MutexGuard.get` | `mut self` | 无 | guard 内取得短期可写访问 | `val value = mut guard.get()` |
| `Thread.spawn` | static | `move task` | 创建 OS 线程并拥有任务闭包 | `Thread.spawn(move task)` |
| `JoinHandle.join` | `move self` | 无 | 消费 join handle，返回线程结果 | `try move handle.join()` |
| `Runtime.spawn` | `self` | `move OnceFn<T>` / `move OnceFn<TaskContext, T>` | 把普通任务放入 runtime worker，返回 `Task<T>` | `runtime.spawn(move task)` |
| `Runtime.spawnBlocking` | `self` | `move OnceFn<T>` / `move OnceFn<TaskContext, T>` | 把阻塞同步工作放入独立 blocking pool，返回 `Task<T>` | `runtime.spawnBlocking(move job)` |
| `await task` | 语言操作 | `Task<T>` 拥有者 | 消费任务句柄，返回任务结果 | `try await task` |
| `Runtime.blockOn` / `Executor.blockOn` | `mut self` | `move Future<T>` / `move Task<T>` | 阻塞当前线程并返回输出 | `try mut runtime.blockOn(move task)` |
| `Task.cancel` | `move self` | 无 | 消费任务句柄，请求协作式取消，丢弃输出 | `val status = move task.cancel()` |
| `Task.detach` | `move self` | 无 | 消费任务句柄，让任务后台继续运行并丢弃输出 | `move task.detach()` |
| `TaskContext.isCancellationRequested` | `self` | 无 | 显式检查当前任务是否收到取消请求 | `ctx.isCancellationRequested()` |
| `Iterator.next` | `mut self` | 无 | 推进迭代器，可能返回一个 owner | `while val Some(x) = mut iter.next()` |
| `File.read` / `write` | `mut self` | buffer | 推进文件状态或写入 OS 缓冲，不移动文件 | `try mut file.read(mut out)` |
| async `File.readU32` | `mut self` | 无 | 立即 await 时可暂停并恢复文件拥有者 | `val x = try await mut file.readU32()` |
| `File.close` | `move self` | 无 | 显式结束资源生命周期，失败时仍触发兜底销毁 | `try move file.close()` |

任务取消状态：

```zn
enum TaskCancelStatus {
    QueuedCancelled,
    Requested,
    AlreadyFinished,
}

struct TaskContext: Copy {
    // opaque task-local capability
}

impl TaskContext {
    fn isCancellationRequested(self) -> Bool;
}
```

规则：

- `get`、`contains`、`containsKey`、`remove`、`removeEntry` 和索引 lookup 都不能移动调用方的 key。需要支持 `String` / `StringSlice`、owned / slice、大小写折叠等查找形态时，用 `LookupKey<K>` 或专门 lookup API 表达。
- `insert`、`push`、`Box.new`、`Shared.new`、`Mutex.new`、`Thread.spawn`、`Runtime.spawn` 和 `Runtime.spawnBlocking` 这类会长期保存或消费值的 API，命名 owner 实参必须写 `move`。
- `Runtime.spawnBlocking` 是显式阻塞工作边界，必须使用独立且有上限的 blocking pool；捕获状态和返回值必须是 `Send`。
- `Task<T>` 没有 `await` 方法；等待任务写成 `await task`，由语言操作消费句柄。
- `blockOn` 是同步阻塞边界，不允许在 async 上下文调用；它必须显式写出 `mut runtime` / `mut executor` 和 `move task` / `move future`。
- `Task<T>` 是必须显式收尾的资源。任务句柄可以移动或返回给调用方，但不能在作用域末尾隐式 drop；收尾方式只有 `await task`、`blockOn(move task)`、`move task.cancel()` 和 `move task.detach()`。
- `Task.cancel(move self) -> TaskCancelStatus` 不阻塞等待任务真正停止。`QueuedCancelled` 表示任务尚未开始且已被跳过；`Requested` 表示运行时记录了协作取消请求；`AlreadyFinished` 表示任务已经完成且结果被丢弃。
- `Task.detach(move self)` 不取消任务；它只显式表达 fire-and-forget，任务完成时丢弃输出并销毁捕获资源。
- `TaskContext` 只由 `Runtime.spawn` / `Runtime.spawnBlocking` 传给声明了 `TaskContext` 参数的任务闭包。零参数任务闭包不会收到上下文，也不承担取消检查成本。
- `TaskContext.isCancellationRequested()` 不能分配、加锁或执行系统调用；它只检查当前任务的取消位，不负责同步其他用户数据。
- `TaskContext` 可以复制并传给当前任务内的普通 helper 函数，也可以作为当前任务 future 状态跨 `await` 存活；它不能返回、不能保存进长期拥有者，也不能被线程、子任务或逃逸闭包捕获。
- async `mut self` 调用必须立即 `await`，且接收者由当前 future 拥有；这个调用产生的 future 不能保存、返回、放入容器、传给 `spawn` 或跨另一个 `await`。
- `mut self` 方法调用已有命名接收者时必须写 `mut receiver.method(...)`；`move self` 方法必须写 `move receiver.method(...)`。
- 临时值、字面量和函数返回值没有后续可用绑定，传入 `move` 参数时不需要额外 `move`。

`Map<K, V>` 和 `Set<T>` 是拥有集合，默认不是 `Copy`。move 是 O(1) 转移表所有权；clone 是显式深拷贝，v1 只为 `Copy` key/value 开放。无 `In` 后缀 API 使用 profile 默认 allocator，`In` 后缀 API 使用调用方提供的 allocator。

`Hash` 和 `Eq` 必须一致：若 `a.eq(b)` 为 true，则 `a.hash(state)` 和 `b.hash(state)` 必须写入同一语义内容。标准库提供基础整数、`Bool`、`Char`、`String` 和其他常见 key 类型的 `Hash` / `Eq` 实现；`StringSlice` 是非拥有访问值，不能作为长期 key 保存到 `Map` 或 `Set` 中，需要长期保存时使用 `String`。

默认 `Map` / `Set` 不保证遍历顺序。实现可以使用 per-instance seed、SIMD control bytes、Robin Hood / SwissTable 风格探测或其他等价策略；这些都是内部优化，不改变源码语义。哈希结果不用于持久化格式，也不保证跨编译器版本稳定。

`insert`、`reserve`、`shrinkToFit` 和 rehash 可能分配。分配失败时调用当前 profile 的 `oom(layout) -> Never`。需要可恢复 OOM 时，调用方先使用 `tryReserve` 或 `tryReserveExact`，成功后再进行一组 `insert` / `entry.insert`。

`Map.get(key)` 只在 `V: Copy` 时返回 `Option<V>`。`get`、`containsKey`、`remove` 和 `removeEntry` 的 key 是 `LookupKey<K>`，只读、不移动、不分配；例如 `Map<String, V>` 可以用 `String` 或 `StringSlice` 查找。非 `Copy` value 的读取应使用 checked index 产生短期访问，或使用 `entry` API 做单次探测更新。`MapEntry<K, V>` 是 view-like 值，不能长期保存；它持有一次查找结果和可能的待插入 key，用于避免 `containsKey` + `insert` 的双重哈希。

`Set.insert(move value)` 的签名接收 value 所有权；集合已有等价值时返回 false，并销毁传入 value。集合中原有 value 保持不变。`Set.take(value)` 用于把集合中的真实存储值移动出来。

`Map` / `Set` 的只读遍历、可写遍历和消耗遍历不得分配 iterator 对象。`for move pair in map` 逐个移动出 `(K, V)`；`for move item in set` 逐个移动出 `T`。

`Array<T>`、`Vector<T>` 和 `ArraySlice<T>` 的索引语法会降低为检查访问，或在编译器能证明安全时降低为无冗余检查访问。索引访问不从容器中取走所有权；非 `Copy` 元素只能被短期只读访问或通过 `mut collection[index]` 短期可写访问。`collection[index] = value` 销毁旧元素并写入新元素，不返回旧元素；若 `value` 是非 `Copy` 命名值，它会被移动。需要从 `Vector<T>` 中取走拥有值时使用 `pop`、`removeAt` 或 `swapRemove`；需要替换元素并取回旧值时使用 `replaceAt`。

`Array<T>.replaceAt` 和 `Vector<T>.replaceAt` 返回旧元素所有权，不销毁旧元素。普通索引赋值和 `replaceAt` 的区别必须清楚：前者是覆盖并销毁旧值，后者是移动出旧值交给调用方。两者都要求没有活动访问依赖被替换的位置。

`Array<T>.clone` 和 `Vector<T>.clone` 是显式深拷贝。v1 先为 `T: Copy` 元素提供集合 clone；非 `Copy` 元素的深拷贝接口后续单独设计。普通 `clone` 可能分配，失败时调用当前 profile 的 `oom(layout) -> Never`。无 `In` 后缀的 clone 使用 profile 默认 allocator；需要指定 allocator 时使用 `cloneIn`。`Array<T>` 和 `Vector<T>` 默认不是 `Copy`，也不能通过普通赋值隐藏复制成本。

在 API 参数位置，连续数据优先写成 `ArraySlice<T>`。调用方可以传入 `Array<T>`、`Vector<T>` 或已有的 `ArraySlice<T>`。需要增长时使用 `Vector<T>`，需要固定长度拥有者时使用 `Array<T>`。

`Vector<T>.reserve(additional)` 和 `reserveExact(additional)` 用于提前扩容，失败时调用当前 profile 的 `oom(layout) -> Never`。`tryReserve(additional)` 和 `tryReserveExact(additional)` 是 v1 标准集合唯一的可恢复 OOM API；它们只预留容量，不插入元素，失败时保留原 `Vector<T>` 内容不变。调用方如果需要避免复杂流程中途 OOM，应先调用 `tryReserve`，成功后再执行后续 `push`。

`Vector<T>` 的 `push`、`pop`、`reserve`、`insert`、`removeAt`、`swapRemove`、`clear`、`shrinkToFit` 等结构性修改，以及 `replaceAt` 这类元素替换，要求当前没有活跃的 `ArraySlice<T>` 依赖该 `Vector<T>`。只读查询如 `len`、`capacity` 和新的只读 `asSlice` 可以继续使用。

`splitDisjoint(parts)` 返回编译器认可的 scoped view iterator。它把同一个连续存储切成互不重叠的可写片段，用于 scoped 并发和局部并行循环。`DisjointSlices<T>` 和它产生的片段都是 view-like 值，不能保存到结构体、集合、`static`、`Box`、`Shared`、逃逸闭包、非 scoped 线程、任务或 async future 中。

`for` 对核心连续集合是编译器识别的语法 lowering，不要求分配 iterator 对象：

- `for item in data`：只读元素遍历，可用于 `Array<T>`、`Vector<T>` 和 `ArraySlice<T>`。
- `for mut item in mut data`：唯一可写元素遍历，可用于 `Array<T>`、`Vector<T>` 和 `ArraySlice<T>`。
- `for move item in data`：消耗元素遍历，只用于拥有的 `Array<T>` 和 `Vector<T>`。
- `for i in start..end`：整数半开区间遍历。

这些形式不得通过隐藏接口派发或堆分配实现。

通用拥有式迭代协议：

```zn
interface Iterator<T> {
    fn next(mut self) -> Option<T>;
}
```

`Iterator<T>` 表示拥有或生成 `T` 值的流。`for move item in iterator` 可以用于实现了 `Iterator<T>` 的具体迭代器类型。核心集合的只读和可写元素遍历不强制走 `Iterator<T>`，因为它们产生的是短期元素访问，不应被包装进 `Option` 或长期对象。

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
    val bytes = Array<U8>.filledIn(64, 0, mut allocator);
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

普通代码不能调用 `rawAddress`。需要把缓冲区传给 C ABI、DMA 或硬件寄存器时，应在很小的 `trust` 块中取出地址，并立即封装回安全 API。为立即 FFI 调用取出地址属于 `ffi` 能力；构造任意地址、裸指针偏移或解引用属于 `rawMemory` 能力；MMIO / DMA 设备访问属于 `hardware` 能力。

## 5. 字符串

Zeno 区分文本拥有者和文本访问值：

```zn
String        // 拥有、UTF-8、可增长
StringSlice   // 不拥有、UTF-8、长度稳定
```

字符串 literal 的类型是 `StringSlice`。它指向静态只读 UTF-8 数据，不分配。

```zn
val view = "hello";                  // StringSlice
val owned = String.from("hello"); // String
```

`String` 语义上等价于带 UTF-8 不变量的可增长字节缓冲区。它不是 `Copy`，move 是 O(1)，clone 必须显式。实现可以使用小字符串优化或静态字面量优化，但这些优化不能改变可观察语义。

必要 API：

```zn
impl String {
    fn from(text: StringSlice) -> String;
    fn fromIn<A: Allocator>(text: StringSlice, mut allocator: A) -> String;
    fn len(self) -> USize;
    fn isEmpty(self) -> Bool;
    fn asSlice(self) -> StringSlice;
    fn asBytes(self) -> ArraySlice<U8>;
    fn reserve(mut self, additionalBytes: USize);
    fn reserveExact(mut self, additionalBytes: USize);
    fn tryReserve(mut self, additionalBytes: USize) -> Result<Unit, AllocError>;
    fn tryReserveExact(mut self, additionalBytes: USize) -> Result<Unit, AllocError>;
    fn push(mut self, text: StringSlice);
    fn clone(self) -> String;
    fn cloneIn<A: Allocator>(self, mut allocator: A) -> String;
}

impl StringSlice {
    fn len(self) -> USize;
    fn isEmpty(self) -> Bool;
    fn asBytes(self) -> ArraySlice<U8>;
    fn dropPrefix(self, byteCount: USize) -> StringSlice;
    fn chars(self) -> Utf8Chars;
}

struct Utf8Chars { ... }

impl Iterator<Char> for Utf8Chars {
    fn next(mut self) -> Option<Char>;
}
```

规则：

- `val text = "hello";` 推断为 `StringSlice`，不分配。
- `val text: String = "hello";` 不允许；拥有字符串必须显式写 `String.from("hello")`。
- `String.from(text)` 是普通用户的拥有字符串构造入口，使用 profile 默认 allocator，并直接返回 `String`。
- `String.fromIn(text, mut allocator)` 指定 allocator，并直接返回 `String`。
- `String.reserve(additionalBytes)` 和 `reserveExact(additionalBytes)` 用于提前预留字节容量，失败时调用当前 profile 的 `oom(layout) -> Never`。
- `String.tryReserve(additionalBytes)` 和 `tryReserveExact(additionalBytes)` 返回 `Result<Unit, AllocError>`，失败时保留原字符串内容不变；这是需要可恢复 OOM 的文本构建代码应使用的入口。
- `String.push(text)` 可能分配，失败时调用当前 profile 的 `oom(layout) -> Never`。
- 会分配的格式化 API 默认直接返回 `String`。
- `@noAlloc` 中调用这些会分配的 API 必须被拒绝。
- `StringSlice` 是访问值，不能作为结构体字段、集合元素、`static`、逃逸闭包捕获、线程任务捕获或 async future 状态保存。字符串 literal 可以作为 `const StringSlice` 使用；需要长期运行期保存时使用 `String`。
- 从字节转换到 `String` 或 `StringSlice` 必须验证 UTF-8；跳过验证只能出现在 `trust` 边界中。
- `String.len` 和 `StringSlice.len` 返回字节长度，不是 Unicode scalar 数量。
- 文本不能用随机整数索引直接返回 `Char`，因为 UTF-8 字符访问不是 O(1)。需要字符遍历时使用 `chars()`。
- 原始二进制数据使用 `Array<U8>` / `ArraySlice<U8>`，不要使用 `String`。

## 6. 分配

分配器接口：

```zn
struct Layout {
    size: USize,
    align: USize,
}

enum AllocError {
    OutOfMemory,
    InvalidLayout,
    TooLarge,
}

struct Allocation {
    // 只允许 trust 封装解释的不透明分配句柄
}

interface Allocator {
    fn allocate(mut self, layout: Layout) -> Result<Allocation, AllocError>;
    fn deallocate(mut self, allocation: Allocation, layout: Layout);
}

interface EscapingAllocator: Allocator {}
```

`Layout` 描述分配大小和对齐；非法对齐或大小溢出必须在构造 `Layout` 时被拒绝，或由低层 allocator 返回 `AllocError.InvalidLayout` / `AllocError.TooLarge`。`Allocation` 不暴露普通用户可做算术的裸地址；把它转换成可读写内存对象只能发生在 `trust` 封装内部。

分配 API 分为两层：

- 无 `In` 后缀的 API 使用 profile 默认 allocator，例如 `String.from`、`Vector.withCapacity`、`Map.withCapacity`、`Set.withCapacity`、`Array.filled`、`Box.new` 和 `Shared.new`。
- 带 `In` 后缀的 API 显式接收 allocator，例如 `String.fromIn`、`Vector.withCapacityIn`、`Map.withCapacityIn`、`Set.withCapacityIn`、`Array.filledIn`、`Box.newIn` 和 `Shared.newIn`。
- 默认分配 API 和 `In` 后缀分配 API 不返回 `Result`。分配失败时调用当前 profile 的 `oom(layout) -> Never` 策略，不返回到调用点。
- v1 标准集合只在容量预留上提供可恢复 OOM：`Vector.tryReserve`、`Vector.tryReserveExact`、`Map.tryReserve`、`Map.tryReserveExact`、`Set.tryReserve`、`Set.tryReserveExact`、`String.tryReserve` 和 `String.tryReserveExact`。构造、`push`、`insert`、`clone`、`Box.new` 和 `Shared.new` 不提供 `Result` 版本。
- Freestanding profile 默认没有全局 allocator；除非目标 profile 明确配置默认 allocator，否则只能使用 `In` 后缀 API。
- Hosted profile 可以提供默认 allocator，让普通应用代码避免到处传 allocator。
- 拥有者必须记录释放和后续增长所需的 allocator 信息。实现可以对零大小 allocator、默认 allocator 或可静态证明的 allocator 做专用化和字段消除。
- `@noAlloc` 中禁止调用无 `In` 和带 `In` 的分配 API。
- 低层 `Allocator.allocate` 保留 `Result`，因为它是实现集合、arena、内核页分配器和平台绑定的底层入口；普通用户构造集合和值拥有者时不直接暴露这个错误流。

allocator 生命周期规则：

- `EscapingAllocator` 是编译器认识的标记接口，表示用它创建的拥有者可以在普通所有权规则下逃逸当前作用域。
- 没有实现 `EscapingAllocator` 的 allocator 是 scoped allocator。用它创建的 `Array`、`Vector`、`Map`、`Set`、`String`、`Box` 或 `Shared` 带有隐藏 allocation region，不能比 allocator 活得更久。
- scoped allocator 创建的拥有者不能从函数返回，不能保存到长期结构体字段、`static`、`Box`、`Shared`、逃逸闭包、非 scoped 线程、任务或 async future 状态中。
- scoped allocator 创建的拥有者可以在 allocator 活跃期间作为局部值使用、传给非逃逸函数、切片、遍历和销毁。
- 如果泛型函数需要返回由传入 allocator 创建的拥有者，allocator 约束必须是 `A: EscapingAllocator`，而不是普通 `A: Allocator`。
- `ThreadLocalAllocator` 可以是 `EscapingAllocator`，但它创建的拥有者是否 `Send` 仍取决于 allocator 是否支持跨线程释放。

示例：

```zn
fn buildLocal(mut arena: ArenaAllocator) {
    val bytes = Vector<U8>.withCapacityIn(1024, mut arena);
    use(bytes.asSlice());
} // ok: bytes 在 arena 前销毁

fn badReturn(mut arena: ArenaAllocator) -> Vector<U8> {
    return Vector<U8>.withCapacityIn(1024, mut arena); // expected-error
}

fn buildReturn<A: EscapingAllocator>(mut allocator: A) -> Vector<U8> {
    return Vector<U8>.withCapacityIn(1024, mut allocator);
}
```

标准 allocator 类型：

```zn
struct GlobalAllocator {}
struct ThreadLocalAllocator {}
struct ArenaAllocator { ... }
struct BumpAllocator { ... }
struct FixedBufferAllocator { ... }
struct PageAllocator { ... }
```

规则：

- `GlobalAllocator` 是 hosted 默认全局分配器入口，可以是零大小类型。
- `ThreadLocalAllocator` 提供线程本地快路径，跨线程释放能力由类型的 `Send` / `Sync` 推导控制。
- `ArenaAllocator` 适合批量释放，常用于编译器、解析器和请求生命周期。
- `BumpAllocator` 适合线性构建，单次释放整个区域。
- `FixedBufferAllocator` 使用调用方提供的固定内存，适合 freestanding 和嵌入式。
- `PageAllocator` 表示 OS 或内核页级分配能力。
- `GlobalAllocator`、`ThreadLocalAllocator` 和 `PageAllocator` 可以实现 `EscapingAllocator`。
- `ArenaAllocator`、`BumpAllocator` 和 `FixedBufferAllocator` 默认不实现 `EscapingAllocator`；它们用于 scoped owner。

优化要求：

- `A: Allocator` 的 `In` 后缀 API 默认静态派发 allocator 方法，允许内联。
- 零大小 allocator 不应增加拥有者大小。
- profile 默认 allocator 可以专门化为零字段句柄。
- `Vector<T>`、`Map<K, V>`、`Set<T>` 和 `String` 只在释放或增长确实需要时保存 allocator binding。
- `tryReserve` 成功后，容量范围内的后续 `push` 不应重复生成扩容慢路径。
- `Map.tryReserve` / `Set.tryReserve` 成功后，容量范围内的后续 `insert` / `entry.insert` 不应重复生成 rehash 慢路径。

拥有式 box：

```zn
struct Box<T> {
    // 内部表示
}

impl<T> Box<T> {
    fn new(move value: T) -> Box<T>;
    fn newIn<A: Allocator>(move value: T, mut allocator: A) -> Box<T>;
}
```

`Box.new(move value)` 的签名接收 `value` 所有权，是显式分配。`Box.newIn(move value, mut allocator)` 用于 arena、页分配器、固定池等场景。

`Box<Interface>` 表示拥有式异构接口值。若 `T: Interface`，从 `T` 构造 `Box<Interface>` 只分配一次具体 `T`；从 `Box<T>` 移动转换到 `Box<Interface>` 不重新分配、不复制值，只携带接口表元数据。`Box<Interface>` 销毁时通过接口表销毁具体值，再用创建时绑定的 allocator 释放内存。

```zn
fn boxWriter(move file: File) -> Box<Writer> {
    return Box.new(move file);
}

fn boxWriterIn<A: Allocator>(move file: File, mut allocator: A) -> Box<Writer> {
    return Box.newIn(move file, mut allocator);
}

fn eraseWriter(move file: Box<File>) -> Box<Writer> {
    return file;
}
```

共享拥有：

```zn
struct Shared<T> {
    // 内部表示
}

impl<T> Shared<T> {
    fn new(move value: T) -> Shared<T>;
    fn newIn<A: Allocator>(move value: T, mut allocator: A) -> Shared<T>;
    fn clone(self) -> Shared<T>;
    fn strongCount(self) -> USize;
}
```

`Shared<T>` 表示引用计数共享所有权。引用计数成本必须在类型名里可见。`Shared<T>.clone` 只增加引用计数，不深拷贝底层值；这个成本通过 `Shared` 类型名显式暴露。`Shared<T>` 默认只提供不可变共享；共享可变状态需要 `Mutex<T>`、原子类型或其他同步容器。`Shared<T>` 只有在 `T: Send, Sync` 且创建时绑定的 allocator 可跨线程释放时才是 `Send` 和 `Sync`。

`Shared<Interface>` 表示共享拥有式异构接口值，是 `Shared<T>` 的高级用法，不是第三种接口语义。若 `T: Interface`，`Shared<T>` 可以移动转换成 `Shared<Interface>`，不增加引用计数、不重新分配、不复制具体值。`Shared<Interface>.clone()` 仍然只增加引用计数。`Shared<Interface>` 只能调用 `self` 接口方法；如果接口方法需要 `mut self`，调用方必须使用 `Box<Interface>`、具体唯一拥有者，或显式同步容器。需要跨线程共享接口对象时，应定义包含 `Send` 和 `Sync` 的命名接口，例如 `interface SharedWriter: Writer, Send, Sync {}`，然后使用 `Shared<SharedWriter>`。

## 7. 接口

基础接口：

```zn
interface Copy {}
interface Eq { fn eq(self, other: Self) -> Bool; }
interface Ord: Eq { fn cmp(self, other: Self) -> Ordering; }
interface Hash { fn hash(self, mut state: Hasher); }
interface HashKey: Hash, Eq {}
interface CopyHashKey: HashKey, Copy {}
interface LookupKey<K: HashKey>: Hash { fn equalsKey(self, key: K) -> Bool; }
interface Send {}
interface Sync {}
interface EscapingAllocator: Allocator {}
```

Callable 能力由编译器内建识别，不作为普通用户可实现接口开放：

```zn
Fn<A..., R>       // 只读调用，可调用多次
MutFn<A..., R>    // 可变调用，可调用多次
OnceFn<A..., R>   // 消费调用，最多调用一次
```

`Fn` 可用于要求 `MutFn` 或 `OnceFn` 的位置；`MutFn` 可用于要求 `OnceFn` 的位置；反向不允许。

通用拥有式迭代接口：

```zn
interface Iterator<T> {
    fn next(mut self) -> Option<T>;
}
```

`Iterator<T>` 是普通接口，可以由用户类型实现。为了保持成本可见，`for move item in iterator` 对具体迭代器默认静态派发；泛型约束 `I: Iterator<T>` 也是静态派发。只有显式写成 `Box<Iterator<T>>` 或 `Shared<Iterator<T>>` 这类接口拥有者时，才会出现接口派发。

`Send` 和 `Sync` 是编译器认识的标记接口。

- `Send`：值的所有权可以跨线程、任务或可跨线程 future 边界移动。
- `Sync`：同一个值的只读共享访问可以被多个线程同时使用。

这两个接口都没有方法，不进入接口表，也不是锁。共享可变状态仍然必须通过 `Mutex<T>`、原子类型或其他同步容器表达。

自动推导规则：

- 标量基础类型、`String`、`Unit`、`Never` 自动是 `Send` 和 `Sync`。
- `Array<T>`、`Vector<T>`、`Option<T>`、`Result<T, E>`、元组和普通 enum 按元素或载荷推导。
- 普通结构体在没有自定义 `destroy` 块且所有字段满足对应条件时自动推导。
- `Box<T>`：`Send` 需要 `T: Send` 且创建时绑定的 allocator 可跨线程释放；`Sync` 需要 `T: Sync` 且 allocator 可跨线程共享释放元数据。
- `Shared<T>`：`Send` 和 `Sync` 都需要 `T: Send, Sync` 且创建时绑定的 allocator 可跨线程释放。
- `Mutex<T>`：`Send` 和 `Sync` 都需要 `T: Send`。
- `Atomic<T>` 对受支持载荷自动是 `Send` 和 `Sync`。
- `ArraySlice<T>` 和其他非拥有访问值不能跨非 scoped 并发边界。接口参数 `writer: Writer` 和接口约束 `W: Writer` 都按具体类型推导线程安全。

带自定义 `destroy` 块、裸句柄、裸地址、平台线程亲和资源或其他编译器无法证明的类型，不自动获得 `Send` / `Sync`。手动声明必须使用 `trust impl`：

```zn
trust impl Send for OsHandle {}
trust impl Sync for SharedKernelTable {}
```

普通包中的 `impl Send for T {}` 或 `impl Sync for T {}` 会被拒绝。`Copy` 仍由显式 `: Copy` 和编译器字段检查控制。销毁由类型可选的 `destroy` 块控制，不通过公开接口实现。

显式 cache padding：

```zn
struct CachePadded<T> { ... }
```

`CachePadded<T>` 不分配，只影响布局和 alignment。它用于跨线程共享热点字段，避免 false sharing。它的空间成本通过类型可见，普通 Auto layout 不会偷偷插入这种级别的 padding。

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
    fn open(path: StringSlice) -> Result<File, IoError>;
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
        val rc = trust { close(self.handle.rawFd) };
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
    val ignored = trust { close(rawFd) };
}
```

`File.close` 是显式错误处理 API。`destroy` 中只能调用 `closeBestEffort` 这类不可失败兜底 API，不能使用 `try`。显式 `close` 成功后应解除兜底清理，避免 `destroy` 二次关闭。

标准库应把 `trust` 边界保持得很小，并在公开 API 中使用 `Result`、`Handle<T>`、`ArraySlice<T>`、`Mmio<T>`、`Port<T>` 等有语义的类型。

## 10. Panic 与 OOM

`core.panic` 提供 profile 绑定的失败入口：

```zn
fn panic(message: StringSlice) -> Never;
fn oom(layout: Layout) -> Never;

interface PanicInfo {
    fn message(self) -> StringSlice;
    fn location(self) -> SourceLocation;
    fn stack(self) -> StackFrames;
}

struct SourceLocation { ... }

impl SourceLocation {
    fn file(self) -> StringSlice;
    fn function(self) -> StringSlice;
    fn line(self) -> U32;
    fn column(self) -> U32;
}

struct StackFrame { ... }

impl StackFrame {
    fn instructionPointer(self) -> USize;
    fn returnAddress(self) -> USize;
    fn symbol(self) -> Option<StringSlice>;
}

struct StackFrames { ... }

impl Iterator<StackFrame> for StackFrames {
    fn next(mut self) -> Option<StackFrame>;
}

fn panicHandler(info: PanicInfo) -> Never;
```

语义：

- `panic(message)` 表示程序不变量被破坏，不用于业务错误。
- `oom(layout)` 表示默认分配 API 或 `In` 后缀分配 API 遇到内存耗尽。
- 二者都返回 `Never`，调用后不会正常回到调用点。
- `panic` 和 `oom` 的具体行为由 profile 选择；库代码只依赖它们不返回。
- `panic(message)` 会被编译器补充调用点 `SourceLocation`，并交给 profile 的 `panicHandler(info)`。
- `PanicInfo`、`StackFrames` 和相关诊断访问值只能在 panic handler 的调用链中使用，不能保存或逃逸。
- `PanicInfo.stack()` 返回惰性 `StackFrames`，用于在 handler 中无分配遍历调用栈。
- `StackFrame.symbol()` 是 best-effort；没有符号表或禁用符号化时返回 `None`，handler 仍可输出 `instructionPointer()`。

默认 profile 行为：

- Hosted profile：`panic` 默认 abort，可配置 unwind；debug 配置默认启用调用栈；`oom` 默认 abort，可配置为调用 `panic` 或目标 handler。
- Freestanding profile：`panic` 和 `oom` 默认目标 trap 或 abort；不要求 unwind、调用栈符号化，也不要求默认 allocator。
- Kernel / embedded profile：必须显式提供 `panicHandler` 和 `oomHandler`，二者都返回 `Never`。

清理规则：

- panic-unwind profile 必须在展开经过的作用域执行 RAII 销毁。
- abort / trap / halt / reset profile 不展开栈，也不保证执行普通析构。
- OOM 默认不 unwind；只有目标明确把 OOM 映射到 panic-unwind 时，才执行 panic-unwind 清理。

属性关系：

- `@noPanic` 拒绝直接或间接 `panic`，以及当前 profile 中会降低成 `panic` 的运行时检查。
- 当前 profile 若把 OOM 配置成 `panic`，`@noPanic` 中可能分配的 API 也必须被拒绝，除非编译器能证明不会失败。
- `@noAlloc` 拒绝堆分配，是排除分配触发 OOM 的主要工具。

可恢复错误必须使用 `Result`。

## 11. 命名规则

标准 API 应该暴露成本：

- `new` 不是语言级通用构造关键字。它只作为少数类型方法出现：显式拥有资源构造如 `Box.new` / `Shared.new`，或低成本常量初始化如 `Atomic.new`。
- 字符串不使用 `new String(...)`；拥有字符串统一使用 `String.from(text)` 或 `String.fromIn(text, mut allocator)`。
- 默认分配应使用 `Box.new`、`Shared.new`、`Array.filled`、`Vector.withCapacity`、`Map.withCapacity`、`Set.withCapacity`、`String.from`、`String.push` 等直接返回值的 API。
- 指定 allocator 时使用 `In` 后缀：`Box.newIn`、`Shared.newIn`、`Array.filledIn`、`Vector.withCapacityIn`、`Map.withCapacityIn`、`Set.withCapacityIn`、`String.fromIn`、`cloneIn`。
- `clone` 用于显式复制；指定 allocator 的深拷贝使用 `cloneIn`。
- 引用计数共享使用 `Shared.new`；引用计数增加使用 `Shared.clone`，成本由 `Shared` 类型名显式暴露。
- 函数参数可以直接写接口名，例如 `writer: Writer`；它表示匿名静态接口参数，默认单态化、可内联。
- 函数返回可以直接写接口名，例如 `-> Writer`；它表示静态接口返回，所有返回路径必须是同一个具体实现类型。
- 需要多个位置共享同一个具体实现类型，或需要把实现保存进结构体字段时，写显式泛型边界，例如 `W: Writer`。
- 需要拥有异构接口值时必须写出拥有者，例如 `Box<Writer>`；需要共享拥有时才使用 `Shared<Writer>`。单独的 `Writer` 不表示隐藏分配，也不表示隐藏动态派发。
