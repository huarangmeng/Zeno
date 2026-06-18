# Zeno 并发模型

Zeno 把并发和核心语言运行时分开。核心语言只提供所有权转移、访问检查和数据竞争防护；具体执行模型由库提供。

并发分三层：

```text
std.thread.Thread   OS 线程，显式且最低层
std.task.Runtime    可选任务调度器 / 线程池
core.async.Future   编译器生成的状态机，没有全局 executor
std.thread.Thread.scope  结构化 scoped 并发，证明 join-before-exit
```

Zeno 默认没有虚拟线程，也不要求全局调度器。Hosted 应用可以选择任务运行时；freestanding 程序可以完全不用这些能力。

## 1. OS 线程

`std.thread.Thread` 映射到操作系统线程。

适用场景：

- 长期运行的任务。
- CPU 密集型工作。
- 专门的阻塞工作。
- 运行时内部实现。

```zn
let handle = try Thread.spawn(move () -> Result<Unit, WorkError> {
    return process(move work);
});

try handle.join();
```

规则：

- `Thread.spawn` 可能分配 OS 资源，也可能失败。
- `Thread.spawn` 接收并消费 `OnceFn<T>`，任务闭包最多执行一次。
- 捕获值必须 `move`，除非它们是 `Copy`。
- 被移动进线程的捕获值必须是 `Send`。
- 线程返回值必须是 `Send`，因为它会通过 `JoinHandle<T>` 回到 join 方。
- 返回的 `JoinHandle<T>` 拥有线程 join 权限。
- 未 join 就丢弃 join handle 的行为由 profile 定义；hosted `std` 应要求显式 `detach` 或 `join`。
- 可写访问不能跨越非 scoped 线程边界。

scoped 并发允许短期访问进入子任务，但必须证明所有子任务在 scope 退出前完成。可写切分必须来自编译器认可的 disjoint API：

```zn
Thread.scope((mut scope) {
    for mut shard in mut shards.splitDisjoint(workerCount) {
        scope.spawn(move () {
            processShard(mut shard);
        });
    }
});
```

普通运行期索引不能证明多个 scoped 任务之间不重叠：

```zn
Thread.scope((mut scope) {
    scope.spawn((workerId: USize) {
        processShard(mut shards[workerId]); // error: disjoint access is not proven
    });
});
```

`Thread.scope` 是库 API，但编译器认识它的 join-before-exit 契约。只有 scoped 任务可以携带短期访问；普通 `Thread.spawn` 仍然只能接收 `move` 的 `Send` 值。

## 2. 任务运行时

`std.task.Runtime` 是可选库能力。它可以实现 work-stealing 池、固定线程池或平台专用调度器。

适用场景：

- 大量小型 CPU 任务。
- 结构化应用并发。
- 需要可配置调度的服务端运行时。

示例：

```zn
let runtime = try Runtime.withWorkerCount(8);

let task = try runtime.spawn(move () -> Result<Unit, WorkError> {
    return process(move work);
});

try task.await();
```

规则：

- runtime 是显式值。
- 没有程序选择的 runtime 或 executor，任务不能运行。
- `spawn` 可能分配，也可能失败，并且消费 `OnceFn<T>` 任务闭包。
- 可跨 worker 运行的任务要求捕获状态和返回值是 `Send`。
- 阻塞 API 应标注，或被路由到 blocking pool。
- 库 API 不得偷偷捕获全局 runtime。

## 3. Future 与 async

`Future<T>` 是表示暂停计算的值。它不是线程，也不是虚拟线程。

`async fn` 降低为状态机：

```zn
async fn readAll(move file: File) -> Result<Vector<U8>, IoError> {
    let header = try await file.readHeader();
    return file.readBody(header);
}
```

规则：

- 创建 future 不会自动开始执行。
- poll 或 await 需要程序选择 executor/runtime。
- future 对象拥有它捕获的状态。
- 跨 `await` 存活的非 `Copy` 值必须由 future 拥有，例如 `move file: File`；短期 `self` / `mut self` 访问不能跨 `await`。
- future 只有在其状态全是 `Send` 时，才可以被移动到其他线程或多线程 runtime。
- 非逃逸 async 状态不应分配，除非函数体显式分配。
- 非逃逸 future 可以被栈上保存、内联 poll 或标量替换。
- 逃逸 future 必须通过显式拥有者表达，例如 task handle、`Box<Future<...>>` 或 runtime spawn。
- 丢弃未完成 future 表示取消；drop 必须销毁当前状态中已经初始化且仍拥有的字段。
- 跨过 `await` 的 `defer` 会成为 future 状态的一部分，并在正常完成、错误提前返回、panic-unwind 或取消 drop 时执行。
- future drop 不能 `await`；跨 `await` 的 `defer` 也不能包含 `await`。
- 非拥有访问值不能跨 `await` 进入 future 状态，因此 v1 不需要用户可见 `Pin` 来保证自引用 future 安全。
- Freestanding 目标可以编译 async 状态机，但不提供 executor。

## 4. 性能策略

Zeno 优先让成本显式：

- OS 线程通过 `Thread` 显式出现。
- 任务调度通过 `Runtime` 显式出现。
- async 暂停点通过 `async` / `await` 显式出现。
- 堆分配仍然通过 `Vector`、`Box`、任务句柄、默认分配 API 或 `In` 后缀 allocator API 可见。
- 数据共享需要 `Shared`、原子类型、锁或其他同步类型。
- 跨线程热点字段需要显式 `CachePadded<T>`，语言不会偷偷插入 cache line padding。

语言不能把普通阻塞调用偷偷变成虚拟线程挂起，除非 API 类型明确表达了这种行为。

## 5. 安全策略

所有并发层都遵守同一套所有权规则：

- 把拥有者移入线程、任务或 future 会转移所有权。
- 跨线程移动要求被移动值是 `Send`。
- 共享不可变数据要求 `Sync`。
- 共享可变数据要求同步。
- scoped 并发 API 只有在证明 join-before-exit 时，才可以允许可写访问。

`Send` / `Sync` 的推导规则由核心标准库定义。普通纯数据类型通常自动获得这两个标记；带自定义 `destroy` 的资源类型、平台句柄和线程亲和对象必须通过 `trust impl` 明确声明。接口拥有者必须把能力写进接口类型，例如 `interface ThreadWriter: Writer, Send {}` 或 `interface SharedWriter: Writer, Send, Sync {}`。
