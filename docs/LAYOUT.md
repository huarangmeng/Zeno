# Zeno 内存布局与 ABI

Zeno 的布局目标是：默认高性能，系统代码可精确控制，外部 ABI 必须显式，二进制格式不能靠猜。

## 1. 总览

结构体布局有四种策略：

```zn
struct Entry { ... }                   // 默认 Auto layout
@layout(Source) struct HotState { ... } // 源码顺序，自然对齐
@layout(C) struct CPoint { ... }        // C 互操作布局
@layout(Packed(1)) struct Header { ... }// 字节级协议 / 磁盘格式
```

不提供 `Transparent` 布局。只有一个真实存储字段的结构体默认就是零成本包装。

不提供 `Compact` 布局。默认 Auto layout 已经负责字段重排和 padding 压缩。

## 2. 默认 Auto layout

普通 `struct` 使用 Auto layout：

```zn
struct Entry {
    flag: Bool,
    id: U64,
    kind: U8,
}
```

规则：

- 编译器可以重排字段，目标是减少 padding、降低数组元素跨度、改善默认缓存密度。
- 布局在同一 target、同一编译器版本、同一编译选项下确定。
- 字段初始化、字段访问、可见性、move 状态和销毁语义仍按源码字段名和声明顺序检查。
- `sizeOf<T>()`、`alignOf<T>()` 和 `offsetOf<T>("field")` 是编译期常量。
- Auto layout 不承诺字段内存顺序等于源码顺序。
- Auto layout 不承诺 C ABI、磁盘格式、网络格式或跨编译器稳定 ABI。
- `pub` 只表示源码 API 可见，不表示布局稳定。

Auto layout 是默认选择，因为大多数数据结构更需要紧凑数组、少 padding 和更好的 cache 行利用率，而不是手写字段顺序。

Auto layout 的优化优先级：

1. 保证源码语义、销毁顺序和诊断稳定。
2. 减少 size 和 padding。
3. 降低数组元素 stride。
4. 让热字段更容易落在少量 cache line 中。
5. 保留 niche 优化和单字段零成本包装机会。

编译器可以使用字段大小、alignment、是否零大小、是否带 `destroy`、字段访问 PGO 热度和目标 cache line 信息辅助布局。没有 PGO 时，默认以 size、alignment 和 padding 为主。

Auto layout 不自动插入大量 padding 来避免 false sharing。跨线程共享且需要隔离 cache line 时，应使用显式类型，例如 `CachePadded<T>`，让空间成本在源码中可见。

## 3. Source layout

当作者明确需要手动控制字段顺序时使用 `@layout(Source)`：

```zn
@layout(Source)
struct WorkerState {
    taskCount: Atomic<U64>,
    stealCount: Atomic<U64>,
    queueHead: U32,
    queueTail: U32,
    debugName: String,
}
```

规则：

- 字段按源码顺序布局。
- 每个字段使用目标平台的自然 alignment。
- 编译器只插入满足 alignment 所需的 padding。
- `sizeOf<T>()`、`alignOf<T>()` 和 `offsetOf<T>("field")` 是编译期常量。
- Source layout 不等同于 C ABI；需要 C 互操作必须写 `@layout(C)`。

`Source` 适合专家手动组织热字段、避免 false sharing、配合预取或手写 cache locality。它不是日常默认写法。

## 4. C layout

与 C 结构体互操作时使用 `@layout(C)`：

```zn
@layout(C)
pub struct CPoint {
    pub x: F32,
    pub y: F32,
}
```

规则：

- 字段按源码顺序布局。
- padding、alignment 和聚合传参规则遵守目标平台 C ABI。
- 只能包含 C-compatible 字段类型。
- 类型本身不能有自定义 `destroy`，也不能包含需要 Zeno 析构语义的字段。
- `@layout(C)` 不自动导出符号；函数导出和裸 C ABI 绑定仍必须通过显式 ABI / `trust extern` 规则。
- `@layout(C)` 不能和 `@layout(Source)` 或 `@layout(Packed(N))` 同时使用。
- `@layout(C)` 的 size、align、字段 offset 和 by-value 聚合传参规则都依赖 target triple 和目标 C ABI；这些信息必须进入 ABI fingerprint 和增量缓存 key。

C-compatible 字段类型包括固定宽度整数、指针宽度整数、`F32`、`F64`、`Bool` 的目标 C `_Bool` 表示、`@layout(C)` 结构体，以及编译器或核心库明确标注为 C-compatible 的句柄类型。

普通 `Vector<T>`、`String`、`StringSlice`、`Array<T>`、`ArraySlice<T>`、`Box<T>`、`Shared<T>`、接口拥有者、闭包、普通 enum、`Option<T>`、`Result<T, E>`、没有 `@layout(C)` 的 struct、`Char` 和带销毁语义的资源类型默认不是 C-compatible。

## 5. Packed layout

字节级协议、文件格式和硬件描述表使用 `@layout(Packed(N))`：

```zn
@layout(Packed(1))
private struct Ipv4HeaderRaw {
    versionAndIhl: U8,
    dscpAndEcn: U8,
    totalLength: U16,
}
```

规则：

- 字段按源码顺序布局。
- 字段 alignment 被限制到 `N`，`N` 必须是 1、2、4、8 或 16。
- `Packed(1)` 表示不插入自然 padding。
- packed 类型只能包含 `Copy` 且无 `destroy` 的字段。
- packed 类型不能包含拥有资源、接口拥有者、闭包、`ArraySlice<T>`、`StringSlice` 或同步 guard。
- 读取 packed 字段时，编译器生成安全的 unaligned load；普通只读传参可以先加载到临时值。
- 写入 packed 字段时，编译器生成安全的 unaligned store。
- 不能把 packed 字段作为 `mut` 访问、长期访问或要求 aligned field storage 的 API 参数，因为这会伪造对齐字段访问。

允许：

```zn
@layout(Packed(1))
struct NativeRecord {
    tag: U8,
    count: U16,
}

val count = native.count; // unaligned load into a value
inspect(native.count);    // ok: unaligned load into a temporary value
native.count = 20;        // unaligned store for a native integer field
```

拒绝：

```zn
normalize(mut native.count); // error: packed field cannot escape as mut access
```

如果函数签名或上下文需要长期访问，先显式加载到局部值：

```zn
val count = native.count;
inspect(count);
```

Packed layout 只解决字节位置和未对齐访问。packed 字段可以直接使用 `U16`、`U32`、`U64` 等普通整数类型；读写这些字段时，编译器生成安全的 unaligned load/store。

端序不进入语言核心，不提供语言级 `endian` 属性，也不要求用户写端序包装类型。跨平台网络 / 磁盘格式应通过标准库或协议类型的安全 API 暴露普通整数：

```zn
pub struct Ipv4Header {
    raw: Ipv4HeaderRaw,
}

impl Ipv4Header {
    fn totalLength(self) -> U16 {
        return ipv4.readTotalLength(self.raw);
    }

    fn setTotalLength(mut self, value: U16) {
        ipv4.writeTotalLength(mut self.raw, value);
    }
}
```

端序转换、校验和未对齐 load/store 是这些封装的实现细节。普通用户不需要在字段类型或调用点写端序包装类型或端序属性。

不要用 packed 结构体表达 MMIO 寄存器语义。MMIO 需要 `trust` 边界和 volatile API，例如 `Mmio<T>.loadVolatile()` / `storeVolatile()`。

## 6. 零成本单字段包装

单真实存储字段结构体默认是零成本包装：

```zn
struct FileDescriptor {
    value: I32,
}
```

规则：

- 结构体和唯一非零大小字段具有相同 `sizeOf`、`alignOf` 和值表示。
- Zeno 内部调用 ABI 可以按内部字段传递，不增加包装成本。
- 类型系统仍然把包装类型和内部字段类型视为不同类型。
- 这个规则不自动创建 C ABI 承诺；跨 C 边界时结构体本身必须写 `@layout(C)`，或由 core/std 明确标注为 C-compatible。

如果结构体有多个真实存储字段，或包含影响布局的字段，不能套用零成本包装规则。

## 7. 枚举与 niche 优化

默认 enum 使用 Auto enum layout。编译器可以选择 tag 位置、payload 布局和 niche optimization。

必须保证这些核心优化：

```zn
Option<Box<T>>         // 与 Box<T> 同大小
Option<Shared<T>>      // 与 Shared<T> 同大小
Option<CoreHandle>     // 对 core/std 中声明了无效句柄值的类型，同句柄大小
```

规则：

- 默认 enum layout 不承诺 C ABI 或跨编译器稳定二进制格式。
- `Result<T, E>` 等普通 enum 可以使用 tag + payload，也可以使用 payload niche 消除显式 tag。
- niche 信息来自编译器和核心库的可信声明，不要求普通用户写特殊非零标记类型。
- enum 的语义匹配、穷尽性和析构顺序不依赖具体 tag 表示。
- C enum 和固定整数 enum layout 不进入 v1；需要跨 C ABI 时使用明确宽度整数和命名常量，或用 `@layout(C)` 结构体包装。

## 8. 函数 ABI 与优化信息

Zeno 默认函数 ABI 服务于优化，不是稳定外部 ABI。

降低建议：

- 小 `Copy` 值优先寄存器传递。
- 大 aggregate 可用间接传递、sret 或目标 ABI 支持的聚合传递形式。
- 只读参数在可证明时降低为 `readonly`、`nocapture`。
- `mut` 参数表示唯一可写访问，可在可证明时降低为 `noalias`，帮助向量化和 load/store 消除。
- `move` 参数表示 callee 取得所有权，可利用唯一所有权做销毁点和别名优化。
- `ArraySlice<T>` 降低为指针加长度，不分配。
- 接口拥有者只在源码类型显式出现时产生间接派发；`writer: Writer` 和 `W: Writer` 都是静态接口调用。

外部 ABI 必须显式。`pub` 不是 ABI 稳定承诺，也不是符号导出承诺。

## 9. Cache padding

标准库提供显式 cache line padding 包装：

```zn
struct CachePadded<T> {
    value: T,
}
```

规则：

- `CachePadded<T>` 让 `value` 至少按目标 cache line 对齐，并把整体 size 补齐到 cache line 的整数倍。
- 目标 cache line 大小来自 target 配置；未知目标可以使用保守默认值。
- 额外空间成本通过类型名可见，编译器不能把普通字段自动变成 cache padded。
- `CachePadded<T>` 的 `Send` / `Sync` 按 `T` 推导。
- `CachePadded<T>` 适合跨线程热点计数器、队列头尾和 runtime work-stealing 元数据。

## 10. 诊断

编译器应该提供布局诊断：

- Auto layout 可以报告实际字段顺序、size、align 和 padding。
- Source layout 可以提示 padding 浪费，并建议改字段顺序或改回默认 Auto。
- Packed layout 访问被拒绝时，应提示先加载到局部值。
- C layout 包含非 C-compatible 字段时，应指出具体字段和原因。
- 多线程 profile 下，诊断可以提示热点 atomic 字段考虑显式 `CachePadded<T>`。

这些诊断不改变语义，但它们是 Zeno 高性能体验的一部分。
