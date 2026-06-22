# Zeno 高性能设计契约

Zeno 的目标不是靠单个语法点赢性能，而是把高性能路径设计成默认路径：普通抽象不隐藏成本，编译器能从源码语义中提取足够多的优化事实，后端只接收已经证明合法的信息。

本文件定义 v1 必须追求的最高性能方案。它约束语言设计、标准库 API、MIR 优化和 LLVM 降级。

## 1. 总原则

硬规则：

- 默认不使用 GC。
- 默认不使用异常作为错误流。
- 默认不需要全局运行时。
- 泛型默认单态化。
- 核心连续集合遍历默认直接循环。
- 默认 `Map` / `Set` 使用 cache-friendly 哈希表，不做每元素堆分配。
- 非逃逸闭包默认不分配。
- `async` 默认是状态机，不代表线程、虚拟线程或隐式调度器。
- 分配、引用计数、锁、动态派发、任务调度必须在源码类型或调用形式中显式出现。
- 编译器不得为了性能发出未经证明的 LLVM 属性。

性能分三层：

- 语言语义让优化可证明。
- MIR 把优化事实显式化。
- LLVM 只接收安全、精确、目标相关的低层事实。

## 2. 别名与 provenance

这是 Zeno 最重要的性能核心。

Zeno 不暴露 Rust 式生命周期语法，但编译器内部必须建立 provenance 模型。

内部概念：

```text
MemoryObject     一个拥有存储身份的对象，例如 local、Array buffer、Vector buffer、Box allocation
Provenance       一个访问值从哪个 MemoryObject 派生
AccessPath       对象内路径，例如 .field、[index]、slice range
AccessRegion     一段有效访问范围
DisjointProof    两个访问是否可以证明不重叠
EscapeState      noEscape、scopeEscape、threadEscape、globalEscape
```

`mut` 不只是“可以写”，它还表示这段访问在有效区域内唯一。编译器必须尝试证明：

- 访问来自哪个 `MemoryObject`。
- 访问是否逃逸。
- 两个 field path 是否必然不重叠。
- 两个 slice range 是否必然不重叠。
- `Vector` 结构性修改是否会使旧视图失效。
- `move` 后原访问路径是否彻底不可用。

LLVM 属性规则：

- 只有 `unique && noEscape` 的间接 `mut` 参数可以发 `noalias`。
- 只有 `readonly && noEscape` 的访问可以发 `readonly` / `nocapture`。
- 只有类型和 layout 都保证非空时可以发 `nonnull`。
- 只有对齐事实来自 layout 或 allocation contract 时可以发 `align(N)`。
- 不能因为源码写了 `mut` 就无条件发 `noalias`。
- 不能因为源码写了 `trust` 就产生额外别名、非空、对齐、初始化、生命周期或线程安全事实；裸指针默认按未知 provenance 和未知别名关系处理。

最佳效果：

- `mut` 帮助 load/store forwarding。
- `move` 帮助 drop 消除和死 store 消除。
- 不逃逸视图帮助栈分配、标量替换和向量化。
- 独立 field / 独立 range 可以并行优化。

## 3. 布局优化

默认 `struct` 使用 Auto layout。它不承诺源码字段顺序，不承诺 C ABI。

Auto layout 的目标顺序：

1. 保证语义正确。
2. 减少 size 和 padding。
3. 降低数组元素 stride。
4. 让常用字段更容易落在更少 cache line 中。
5. 让 niche 优化和单字段 wrapper 零成本成立。

Auto layout 可以使用：

- 字段 size / align。
- 字段是否零大小。
- 字段是否带 `destroy`。
- 字段是否包含指针或拥有者。
- PGO 中的字段访问热度。
- 目标 cache line 大小。

Auto layout 不能改变：

- 字段名字。
- 初始化语义。
- 可见性。
- move 状态。
- 销毁顺序。
- 诊断 span。

false sharing 不通过隐藏 padding 自动解决。需要跨线程共享并避免 false sharing 时，使用显式类型：

```zn
struct WorkerCounters {
    local: CachePadded<Atomic<U64>>,
    stolen: CachePadded<Atomic<U64>>,
}
```

`CachePadded<T>` 的额外空间成本在类型中可见。

## 4. 泛型与代码体积

泛型默认单态化，这是性能默认路径。但编译器必须防止代码膨胀破坏 instruction cache。

v1 策略：

- 小泛型函数优先内联。
- 大泛型函数默认单态化，但 cold block 可以 outline。
- `panic`、OOM、错误格式化、复杂诊断路径必须 cold split。
- 没有被代码使用的类型参数不进入 codegen key。
- 只依赖 layout shape 的内部实现可以做 shape sharing。
- 完全相同的 mono body 可以做 identical code folding。
- 跨 package release 构建可以启用 LTO。
- PGO 可以决定是否保留更多专门化实例。

`shape sharing` 不能改变源码成本模型：

- 不允许把静态接口参数、静态接口返回或显式静态接口约束变成隐藏动态派发。
- 不允许引入隐藏分配。
- 不允许改变 `sizeOf`、`alignOf`、drop 或 ABI 语义。
- 只允许在生成代码等价时共享机器代码。

这样 Zeno 保持 C++ template / Rust generics 的性能，同时减少极端模板膨胀。

## 5. 边界检查与 range facts

索引默认安全，但常见循环不能因此变慢。

MIR 必须显式记录：

- `Range<T>` 的 start、end 和半开语义。
- loop induction variable。
- `i < len`、`i <= len`、`start <= end <= len`。
- slice lineage：一个 `ArraySlice<T>` 来自哪个 owner 或 slice。
- slice offset 和 len。
- `Vector.len` 是否在循环内不变。
- `Vector.capacity` 是否由 `tryReserve` 成功证明足够。

必须优化的形式：

```zn
for i in 0..data.len {
    use(data[i]);
}
```

```zn
val tail = data.dropPrefix(4);
for i in 0..tail.len {
    use(tail[i]);
}
```

```zn
try mut out.tryReserve(input.len);
for item in input {
    mut out.push(item);
}
```

前两个形式不应在循环体中保留冗余 bounds check。第三个形式在 `tryReserve` 成功后，不应让每个 `push` 都重复生成扩容慢路径。

## 6. 分配器

分配器是系统语言性能的核心，必须好用但不能隐藏。

Zeno 标准 allocator 分层：

```text
GlobalAllocator       hosted 默认全局分配器入口
ThreadLocalAllocator  线程本地快路径分配器
ArenaAllocator        批量释放，适合编译器和请求生命周期
BumpAllocator         线性增长，几乎无释放成本
FixedBufferAllocator  使用调用方提供的固定内存
PageAllocator         OS 或内核页级分配
```

普通构造使用默认 allocator：

```zn
val text = String.from("hello");
val values = Vector<U32>.withCapacity(128);
val counts = Map<U64, USize>.withCapacity(128);
```

高性能或 freestanding 代码使用 `In` 后缀：

```zn
val text = String.fromIn("hello", mut arena);
val values = Vector<U32>.withCapacityIn(128, mut arena);
val counts = Map<U64, USize>.withCapacityIn(128, mut arena);
```

优化规则：

- allocator 参数是泛型 `A: Allocator` 时默认静态派发，可内联。
- 零大小 allocator 不应增加拥有者大小。
- profile 默认 allocator 可以被专门化为零字段句柄。
- `Vector` / `Map` / `Set` / `String` 只在确实需要释放或增长时保存 allocator binding。
- `tryReserve` 成功后，后续已证明容量内的 `push` 或 `insert` 不生成扩容 / rehash 慢路径。
- scoped allocator 创建的 owner 带隐藏 allocation region，不能逃逸 allocator 作用域。
- 返回 allocator 创建的 owner 的泛型 API 必须使用 `A: EscapingAllocator`，否则只能返回 scoped owner 并受逃逸检查限制。
- `@noAlloc` 中任何可能分配的 API 都必须被拒绝。

分配失败策略：

- 普通构造、`push` 和 `insert` 失败调用 `oom(layout) -> Never`。
- 需要可恢复 OOM 时，先调用 `tryReserve`。
- 底层 `Allocator.allocate` 保留 `Result`，用于实现集合和平台绑定。

## 7. 并发与 async

Zeno 不把并发成本藏进语言运行时。

层级：

```text
Thread             OS 线程
Runtime            显式任务运行时 / 线程池
Future             async 状态机值
Scoped concurrency 证明 join-before-exit 的结构化并发
```

性能规则：

- `Thread.spawn` 是 OS 线程能力，成本显式。
- `Runtime.spawn` 是库调度能力，runtime 必须是显式值；它接收 `Future<T>`。用户可以写 `spawn({ ... })` 内联任务体，lowering 后仍是 future 状态机，不是隐藏同步闭包分配。
- `Runtime.spawnBlocking` 是独立 blocking pool 能力，用于长时间同步工作；不能隐藏成普通 `spawn`，也不能无界创建 OS 线程。
- `TaskGroup<T>` 是结构化并发拥有者，成本由类型显式出现；默认 `joinAll` / `tryJoinAll` 按完成顺序收集，避免不必要的有序结果槽位。
- 需要保持 spawn 顺序时写 `joinAllOrdered` / `tryJoinAllOrdered`，额外索引和结果存储成本由方法名暴露。
- `Runtime.blockOn` / `Executor.blockOn` 是同步阻塞边界，必须显式写出 runtime/executor 和被消费的 future/task。
- `Task<T>` 必须显式收尾；`await`、`blockOn`、`cancel` 和 `detach` 都会消费句柄，析构不能隐藏阻塞、取消或后台运行。
- `TaskContext.isCancellationRequested()` 是显式取消检查，目标实现应降低为任务控制块中的轻量标志读取；不能隐藏 heap allocation、锁、系统调用或线程局部全局查询。
- `async fn` 创建 future 状态机，不自动启动，不自动分配。
- 非逃逸 future 可以栈上或内联 poll。
- 逃逸 future 必须通过显式拥有者，例如 `Box<Future<...>>`、task handle 或 runtime spawn。
- 跨线程 future 只有在状态全是 `Send` 时才可移动。
- 共享状态必须通过 `Shared`、`Mutex`、atomic 或同步容器。

scoped concurrency 是性能重要能力，因为它允许短期访问不逃逸到线程外：

```zn
Thread.scope((mut scope) {
    for mut shard in mut shards.splitDisjoint(workerCount) {
        scope.spawn(move () {
            processShard(mut shard);
        });
    }
});
```

`Thread.scope` 必须证明所有 scoped 任务在 scope 退出前 join。只有这样，短期 `mut` 访问才可以跨 scoped 任务边界。跨 scoped 任务的可写访问必须来自 `splitDisjoint`、`splitAt` 或其他被编译器认可的不重叠切分 API；不能靠普通运行期索引表达不重叠。非 scoped `Thread.spawn` 仍然只能接收 `move` 的 `Send` 值。

## 8. 字符串与集合

字符串 literal 是 `StringSlice`，不分配。

拥有字符串必须显式：

```zn
val name = String.from("zeno");
```

优化允许但不可改变语义：

- 小字符串优化。
- 静态 literal 优化。
- 空字符串共享。
- clone-on-write 不作为 v1 默认语义，因为它隐藏引用计数和写时分支成本。

集合规则：

- `Array<T>` 固定长度拥有连续内存。
- `Vector<T>` 可增长拥有连续内存。
- `ArraySlice<T>` 非拥有访问，不能长期保存。
- `Map<K, V>` 拥有哈希表，默认无稳定顺序。
- `Set<T>` 拥有哈希表，默认无稳定顺序。
- 核心集合 `for` 不分配 iterator。
- `Iterator<T>` 用于拥有式或生成式迭代，不强迫 slice 元素访问走 `Option`。

关联集合规则：

- 默认 `Map` / `Set` 应使用开放寻址、SwissTable 风格 control bytes、Robin Hood probing 或等价 cache-friendly 策略。
- 不允许为每个元素单独分配节点；需要链表、树或有序结构时应使用显式不同类型。
- `Map.entry` 必须能把查找、插入和更新合并为一次探测，避免 `containsKey` + `insert` 的双重哈希。
- 默认遍历顺序不稳定；标准库不能为了稳定顺序给普通 `Map` / `Set` 增加隐藏指针或链表。
- `Hash` 调用默认静态派发并可内联；基础整数 key 的 hash 应降低为少量混合指令。

## 9. 必须避免的性能陷阱

Zeno 编译器和标准库不得引入这些隐藏成本：

- 把 `try` 降低为异常。
- 把普通闭包降成 heap object。
- 把泛型约束、静态接口参数或静态接口返回降成隐藏接口表。
- 把核心集合遍历降成 boxed iterator。
- 把普通 `Map` / `Set` 降成每元素 heap node。
- 为普通 `Map` / `Set` 维护隐藏稳定插入顺序。
- 把普通字符串 literal 复制成 `String`。
- 把 `Shared<T>` clone 伪装成普通复制。
- 把锁或 runtime 放入普通 API 背后。
- 把 panic stack 符号化放在正常路径。
- 为普通整数 `+`、`-`、`*` 生成隐藏溢出检查。
- 为 packed 字段生成自然对齐 load/store。

## 10. 验收标准

release 编译器必须能证明：

- 热循环的冗余 bounds check 可以消除。
- `tryReserve` 后容量内 push 的扩容慢路径可以消除。
- `Map.tryReserve` / `Set.tryReserve` 后容量内 insert 的 rehash 慢路径可以消除。
- `Map.entry` 更新不重复哈希。
- `Map` / `Set` 元素存储不产生 per-element allocator call。
- 非逃逸闭包和非逃逸 future 不分配。
- 静态泛型和静态接口约束不做动态派发。
- `mut` / `move` 的别名事实能转化为合法 LLVM 属性。
- Auto layout 可以降低普通结构体 size 或 stride。
- 默认错误流不生成异常机制。
- 默认 panic/OOM 只污染冷路径。
- allocator 静态派发可以内联，零大小 allocator 可以字段消除。

这些验收必须进入 `tests/spec/codegen-pass` 和 `tests/spec/codegen-fail`。没有测试约束的性能承诺不算语言契约。
