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
val handle = try Thread.spawn(move () -> Result<Unit, WorkError> {
    return process(move work);
});

try move handle.join();
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

- 大量小型、不会长期阻塞 worker 的任务。
- 结构化应用并发。
- 需要可配置调度的服务端运行时。

示例：

```zn
async fn run(move runtime: Runtime, move work: Work) -> Result<Unit, WorkError> {
    val task = try runtime.spawn({
        return process(move work);
    });

    try await task;
    return Ok(());
}
```

规则：

- runtime 是显式值。
- 没有程序选择的 runtime 或 executor，任务不能运行。
- `Runtime.spawn` 接收并消费 `Future<T>`，返回 `Task<T>`。
- `spawn` 可能分配 task control block，也可能因为 runtime 已关闭、队列饱和或 profile 定义的资源限制失败。
- 临时 async 调用和 Future block 实参可以直接传给 `spawn`；已有命名 future 必须在调用点写 `move future`。
- `await task` 是语言操作，会消费 `Task<T>` 句柄；等待后原 task 绑定不可再用。
- 同步入口等待任务必须写成 `mut runtime.blockOn(move task)`；它阻塞当前线程，成本由 `Runtime` 和方法名共同暴露。
- 可跨 worker 运行的 future 状态和返回值必须是 `Send`。
- 普通 `spawn` 不接收同步闭包；`spawn({ ... })` 中的 block 是 Future block，会 lowered 为 future 状态机。同步阻塞工作使用 `spawnBlocking`，底层 OS 线程使用 `Thread.spawn`。
- `spawn({ ... })` 的非 `Copy` 捕获默认移动进任务。捕获后外层继续使用该 owner 是编译错误；需要共享时必须显式使用 `Shared`、`Mutex`、原子类型或手动 `clone()`。
- 库 API 不得偷偷捕获全局 runtime。

命名 future 示例：

```zn
async fn runWork(move work: Work) -> Result<Unit, WorkError> {
    return process(move work);
}

async fn runNamed(move runtime: Runtime, move work: Work) -> Result<Unit, WorkError> {
    val future = runWork(move work);
    val task = try runtime.spawn(move future);

    return try await task;
}
```

阻塞工作示例：

```zn
async fn readConfig(move runtime: Runtime, move path: Path) -> Result<Bytes, IoError> {
    val task = try runtime.spawnBlocking(move () -> Result<Bytes, IoError> {
        return readFileSync(move path);
    });

    return try await task;
}
```

`spawnBlocking` 规则：

- `spawnBlocking` 接收并消费同步 `OnceFn<T>` 闭包，返回可 `await` / `blockOn` 的 `Task<T>`。
- blocking 任务运行在独立 blocking pool，不能占用普通 async worker。
- blocking pool 必须有上限，并且上限、队列策略和饱和行为必须由 runtime 构造参数或 profile 文档暴露；实现可以排队、背压或返回错误，但不能为每个任务无界创建 OS 线程。
- blocking 任务的捕获状态和返回值必须是 `Send`。
- blocking 任务一旦开始执行，运行时不能安全地强杀它；取消是协作式的，`cancel` 只发出请求并丢弃最终结果。
- 长时间 CPU 计算、同步文件 I/O、DNS、压缩、加密和调用阻塞 C API 都应走 `spawnBlocking`，除非程序显式选择底层 `Thread.spawn`。

同步入口示例：

```zn
fn main() -> Result<Unit, WorkError> {
    var runtime = try Runtime.withWorkerCount(8);
    val task = try runtime.spawn({
        return work();
    });

    try mut runtime.blockOn(move task);
    return Ok(());
}
```

`blockOn` 规则：

- `blockOn` 只用于同步函数和同步入口；async 上下文必须用 `await task` 或 `await future`。
- `blockOn` 是 `mut self` 方法，因为它可能推进调度器、运行 ready queue、park/unpark 当前线程或执行本地 executor 状态机。
- `blockOn(move future)` 消费非逃逸 future 时不应分配；实现可以在当前栈帧内 poll 到完成。
- `blockOn(move task)` 消费任务句柄并阻塞当前线程直到任务完成。
- `blockOn` 返回被等待对象的输出；如果输出是 `Result<T, E>`，调用方可以写 `try mut runtime.blockOn(move task)`。

### TaskGroup 结构化并发

`TaskGroup<T>` 是一组同类型任务的拥有者，用来表达“启动多个任务，然后在当前结构内统一收尾”。它解决两件事：避免把并发写成层层回调；同时保证任务不会在函数返回时被遗忘。

主路径用 `joinAll` / `tryJoinAll`，一次性消费 group：

```zn
async fn loadAll(move runtime: Runtime, move jobs: Vector<Job>) -> Result<Vector<Page>, FetchError> {
    var group = TaskGroup<Result<Page, FetchError>>.withCapacity(jobs.len(), mut runtime);

    for move job in jobs {
        try mut group.spawn({
            return fetch(move job);
        });
    }

    return try await move group.tryJoinAll();
}
```

流式处理完成结果时，用 `next` / `tryNext`：

```zn
async fn loadStreaming(move runtime: Runtime, move jobs: Vector<Job>) -> Result<Unit, FetchError> {
    var group = TaskGroup<Result<Page, FetchError>>.withCapacity(jobs.len(), mut runtime);

    for move job in jobs {
        try mut group.spawn({
            return fetch(move job);
        });
    }

    while Some(page) = try await mut group.tryNext() {
        consume(move page);
    }

    return Ok(());
}
```

必要 API：

```zn
impl<T: Send> TaskGroup<T> {
    fn withCapacity(capacity: USize, mut runtime: Runtime) -> TaskGroup<T>;
    fn spawn(mut self, move future: Future<T>) -> Result<Unit, SpawnError>;
    async fn next(mut self) -> Option<T>;
    async fn joinAll(move self) -> Vector<T>;
    async fn joinAllOrdered(move self) -> Vector<T>;
    fn cancelRemaining(move self) -> TaskCancelSummary;
}

impl<T: Send, E: Send> TaskGroup<Result<T, E>> {
    async fn tryNext(mut self) -> Result<Option<T>, E>;
    async fn tryJoinAll(move self) -> Result<Vector<T>, E>;
    async fn tryJoinAllOrdered(move self) -> Result<Vector<T>, E>;
}
```

规则：

- `TaskGroup` 是显式拥有者；它可能为任务表、完成队列和 task control block 分配内存。
- `withCapacity` 预留 group 元数据容量；它不启动任务。
- `group.spawn(future)` 消费 future，并要求 future 状态和输出满足跨 worker 的 `Send` 规则。
- `joinAll` / `tryJoinAll` 消费 group，等待所有任务，并按完成顺序返回结果；这是最低额外协调成本的默认行为。
- 需要保持 spawn 顺序时使用 `joinAllOrdered` / `tryJoinAllOrdered`。有序收集可能需要额外索引和结果槽位，成本由方法名表达。
- `next` / `tryNext` 每次返回一个完成结果，顺序是完成顺序。循环提前退出时，调用方必须用 `move group.cancelRemaining()` 或其他消费 API 显式收尾。
- `next` / `tryNext` 返回 `None` 后，group 进入 drained 状态，离开作用域不再需要额外收尾。编译器必须认识 `while Some(x) = await mut group.next()` 和 `while Some(x) = try await mut group.tryNext()` 这类 drain 循环。
- `tryNext` / `tryJoinAll` 遇到第一个 `Err` 时，会请求取消剩余任务并把 group 标记为 settled。等待或丢弃结果的策略由 runtime profile 定义，但不能让未收尾任务逃出 group。
- 对 `try await mut group.tryNext()`，`try` 的提前返回路径被视为已经收尾，因为 `tryNext` 的 `Err` 路径已经 settled group。
- `cancelRemaining(move self)` 消费 group，请求取消仍未完成的任务并丢弃后续输出；它不隐藏阻塞等待。
- `TaskGroup<T>` 不能被隐式 drop。离开作用域前必须被 `joinAll`、`tryJoinAll`、`cancelRemaining` 消费，或者被 `next` / `tryNext` 完整 drain 到 `None`。

### Task 句柄生命周期

`Task<T>` 是必须显式收尾的资源。一个任务句柄不能在作用域末尾被隐式丢弃；它必须被移动给调用方，或通过下面任意一种方式消费：

- `await task`：async 上下文等待完成并取得输出。
- `mut runtime.blockOn(move task)` / `mut executor.blockOn(move task)`：同步上下文阻塞等待并取得输出。
- `move task.cancel()`：请求协作式取消并丢弃输出。
- `move task.detach()`：让任务继续后台运行并丢弃输出。

示例：

```zn
async fn runOrStop(move runtime: Runtime, move work: Work) -> Result<Unit, WorkError> {
    val task = try runtime.spawn({
        return process(move work);
    });

    try await task;
    return Ok(());
}

fn cancelQueued(move runtime: Runtime, move work: Work) -> Result<TaskCancelStatus, WorkError> {
    val task = try runtime.spawn({
        return process(move work);
    });

    return Ok(move task.cancel());
}

fn launchBackground(move runtime: Runtime, move work: Work) -> Result<Unit, WorkError> {
    val task = try runtime.spawn({
        return process(move work);
    });

    move task.detach();
    return Ok(());
}
```

取消状态：

```zn
enum TaskCancelStatus {
    QueuedCancelled,
    Requested,
    AlreadyFinished,
}
```

`Task.cancel(move self)` 不保证任务已经停止，也不能隐藏阻塞等待：

- `QueuedCancelled` 表示任务尚未开始，runtime 已经跳过该任务并销毁捕获状态。
- `Requested` 表示任务已经开始，runtime 已经记录协作取消请求；任务会在 poll / yield 边界或显式取消检查处观察请求。
- `AlreadyFinished` 表示任务已经完成；结果被丢弃。
- 对 `spawnBlocking` 任务，如果尚未开始可以跳过；一旦开始，只能记录取消请求，不能强杀底层阻塞调用。

`Task.detach(move self)` 不取消任务。任务完成时，其结果被丢弃，捕获资源按普通所有权规则销毁。

`Task<T>` 的析构不能 `await`、不能阻塞、不能隐式 cancel，也不能隐式 detach。普通代码中未收尾的 `Task<T>` 是编译错误；析构只作为 profile 诊断或崩溃恢复的兜底。

### 取消检查

任务内部需要主动检查取消请求时，使用显式上下文入口。普通 `Runtime.spawn(future)` 不创建用户可见 `TaskContext`，因此没有额外参数和检查成本。

```zn
async fn runCancellable(move runtime: Runtime, move work: Work) -> Result<Unit, WorkError> {
    val task = try runtime.spawnWithContext(
        (ctx: TaskContext) {
            if ctx.isCancellationRequested() {
                return Err(WorkError.Cancelled);
            }

            return process(move work);
        }
    );

    return try await task;
}
```

同步阻塞任务同样可以接收 `TaskContext`，但只能在阻塞调用之间检查：

```zn
fn launchBlocking(move runtime: Runtime, move path: Path) -> Result<TaskCancelStatus, IoError> {
    val task = try runtime.spawnBlockingWithContext(move (ctx: TaskContext) -> Result<Bytes, IoError> {
        if ctx.isCancellationRequested() {
            return Err(IoError.Cancelled);
        }

        return readFileSync(move path);
    });

    return Ok(move task.cancel());
}
```

规则：

- `Runtime.spawn(future)` 是日常异步入口，接收 `Future<T>`。
- `Runtime.spawn({ ... })` 是 Future block 实参，会直接 lowered 为 future 状态机；用户不需要写 `async` 或捕获 `move`。`spawn` 仍然是普通函数调用，括号不能省略。
- `TaskGroup.spawn({ ... })` 使用同样的 Future block 简写。
- Future block 会捕获外层变量：`Copy` 值复制，非 `Copy` owner 移动进 future。编译器按 move 规则禁止捕获后继续使用该 owner。
- Future block 不会自动 `clone`、不会自动 `Shared.clone()`、不会自动分配捕获盒子；需要共享时成本必须在源码中显式出现。
- `Runtime.spawnWithContext(makeFuture)` 是高级入口，接收 `OnceFn<TaskContext, Future<T>>`。闭包只在启动时调用一次，runtime 保存它返回的 future，不保存闭包本身；闭包体可以直接写 Future block。
- `Runtime.spawnBlocking(job)` 接收同步 `OnceFn<T>`。
- `Runtime.spawnBlockingWithContext(job)` 接收同步 `OnceFn<TaskContext, T>`。
- `TaskContext` 是 task-local 的轻量能力值，通常 lowered 为任务控制块中的取消位读取；`isCancellationRequested()` 不能分配、不能加锁、不能执行系统调用。
- `isCancellationRequested()` 只报告取消请求是否已经对当前任务可见，不会自动返回、不会 panic，也不会改变任务返回类型。任务用自己的错误类型或返回值表达“已取消”。
- `TaskContext` 可以复制并传给当前任务内的普通 helper 函数；它可以作为当前任务 future 状态的一部分跨 `await` 存活。
- `TaskContext` 不能返回、不能保存进结构体/集合/`Box`/`Shared`/`static`，也不能被线程、子任务或其他逃逸闭包捕获。
- Zeno v1 不提供隐藏的 `Task.current()` / 线程局部取消查询。需要取消检查时，源码中必须能看到 `TaskContext` 参数。

## 3. Future 与 async

`Future<T>` 是表示暂停计算的值。它不是线程，也不是虚拟线程。

`async fn` 降低为状态机：

```zn
async fn readAll(move file: File) -> Result<Vector<U8>, IoError> {
    val header = try await mut file.readHeader();
    return try await mut file.readBody(header);
}
```

规则：

- 创建 future 不会自动开始执行。
- poll 或 await 需要程序选择 executor/runtime。
- future 对象拥有它捕获的状态。
- 跨 `await` 存活的非 `Copy` 值必须由 future 拥有，例如 `move file: File`；短期 `self` / `mut self` 访问不能作为独立值跨 `await`。
- `await mut file.readHeader()` 这种立即等待的 async `mut self` 调用允许用于 future 自己拥有的值；它不能被拆成 `val pending = mut file.readHeader(); await pending;`，也不能被返回或传给 `spawn`。
- future 只有在其状态全是 `Send` 时，才可以被移动到其他线程或多线程 runtime。
- 非逃逸 async 状态不应分配，除非函数体显式分配。
- 非逃逸 future 可以被栈上保存、内联 poll 或标量替换。
- 逃逸 future 必须通过显式拥有者表达，例如 task handle、`Box<Future<...>>` 或 runtime spawn。
- 丢弃未完成 future 表示取消；drop 必须销毁当前状态中已经初始化且仍拥有的字段。
- 跨过 `await` 的 RAII guard 和其他拥有资源会成为 future 状态的一部分，并在正常完成、错误提前返回、panic-unwind 或取消 drop 时销毁。
- future drop 不能 `await`；需要异步清理的资源必须通过显式 async API 完成，不能依赖析构里的异步操作。
- 非拥有访问值不能作为独立状态字段或可逃逸 future 跨 `await`，因此 v1 不需要用户可见 `Pin` 来保证自引用 future 安全。
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
