# Zeno 语言规范

状态：草案 v0.1
源码扩展名：`.zn`

## 1. 设计契约

Zeno 是面向内核、运行时、编译器、引擎、数据库、嵌入式组件和高性能库的系统编程语言。

语言必须遵守这些硬约束：

- 不强制垃圾回收。
- 可恢复错误不使用异常。
- 不使用继承式 class 对象模型。
- 不隐藏堆分配、引用计数、锁或调度器。
- 不强制全局运行时；线程池、async executor、反射元数据、格式化大模块和调用栈符号化都是按需能力。
- 不执行隐式模块初始化、全局构造函数或全局析构函数。
- 普通用户代码没有 `unsafe` 语言模式。
- 裸指针、裸地址、裸 C ABI、inline asm 和硬件访问必须出现在显式 `trust` 边界中。
- 必须能编译 freestanding 程序，不依赖 hosted 标准库。

Zeno 代码应该让资源所有权、分配、同步、接口派发和调度成本在类型或调用位置上可见。

高性能契约见 [PERFORMANCE.md](PERFORMANCE.md)。主规则是：普通抽象不隐藏分配、异常、动态派发、锁、调度器或引用计数；编译器只能把已经证明成立的别名、范围、布局和逃逸事实交给后端。

## 2. 程序结构与 manifest

一个 Zeno 源文件包含可选模块声明、导入和声明。包模式下模块路径默认由 `src/` 下的文件路径推断。

```zn
import core.result.{Result, Ok};
import std.io;

pub fn main() -> Result<Unit, io.Error> {
    try io.println("hello, zeno");
    return Ok(());
}
```

顶层声明可以定义：

- `fn`
- `struct`
- `enum`
- `interface`
- `impl`
- `const`
- `type`
- `module`
- `import`

Zeno 使用 C/Swift 风格的大括号语法。控制流在合适时可以作为表达式。

包级构建策略由 `Zeno.toml` manifest 声明，不写进源码语法。manifest 至少决定：

- `target.triple` 和 `target.profile`。
- 是否有 profile 默认 allocator。
- `panic`、调用栈和 OOM 策略。
- `trust` 底层能力是否允许。

真实包构建以 `Zeno.toml` 或等价命令行参数为准。规格测试里的 `// profile: freestanding` 只是测试 harness shorthand。manifest 规范见 [MANIFEST.md](MANIFEST.md)。

manifest 会影响类型检查和 codegen，例如：

- 没有默认 allocator 时，无 `In` 后缀的分配 API 被拒绝。
- `oom.strategy = "panic"` 时，`@noPanic` 中可能分配的 API 被拒绝。
- `panic.strategy = "unwind"` 时，panic 路径生成 unwind 清理；abort / trap 不生成 unwind 清理。
- `trust` 字段决定 `trust` 边界内可使用哪些底层能力。

profile 让能力可用，但不代表自动链接或自动初始化能力。Hosted profile 可以使用 OS、默认 allocator、线程和 hosted `std`，但程序只有在源码可达调用实际使用这些能力时才链接对应 runtime shim 或库模块。

manifest 不改变普通所有权、move、初始化、访问值和 `Send` / `Sync` 的核心规则。

模块、包、文件路径、依赖和导入解析规则见 [MODULES.md](MODULES.md)。包模式构建中，`.zn` 文件可以省略 `module` 声明；若写出，必须和 `src/` 下的文件路径匹配。`import` 只用于外部依赖和内建包，不执行代码。

## 3. 词法规则

长期目标支持 Unicode 源码文本，但 stage0 编译器只需要支持 ASCII 标识符：

```text
identifier = [A-Za-z][A-Za-z0-9]*
```

单独的 `_` 只能作为丢弃模式使用，不是普通标识符。

v0.1 关键字：

```text
as async await break const continue destroy else enum extern false
fn for if impl import in interface match module move mut private pub
return self Self static struct true trust try type val var while
```

`unsafe` 不是用户语言关键字。符合规范的编译器必须拒绝普通包中的 `unsafe` 块、模式或声明。

关键字职责：

| 关键字 | 作用 |
| --- | --- |
| `as` | 显式类型转换，例如 `byte as U32`。 |
| `async` | 声明返回 `Future` 状态机的异步函数，不启动线程或任务。 |
| `await` | 在 async 上下文中等待 `Future<T>` 或 `Task<T>` 继续执行；等待命名任务句柄会消费该句柄。 |
| `break` | 跳出循环，可选携带循环表达式的值。 |
| `const` | 声明编译期值，或在泛型参数列表中声明常量参数。 |
| `continue` | 跳到下一次循环迭代。 |
| `destroy` | 在 `impl Type` 中定义类型销毁前运行的生命周期清理块。 |
| `else` | `if` 的备用分支。 |
| `enum` | 定义代数枚举 / sum type。 |
| `extern` | 声明外部 ABI 绑定；裸声明必须写成 `trust extern`。 |
| `false` | 布尔假值。 |
| `fn` | 定义函数或函数签名。 |
| `for` | 遍历循环。 |
| `if` | 条件表达式或条件语句。 |
| `impl` | 给类型添加方法，或声明类型实现某个接口。 |
| `import` | 导入外部依赖或内建包的模块 / 公开项。 |
| `in` | `for item in items` 中连接循环变量和被遍历对象。 |
| `interface` | 定义能力契约。 |
| `match` | 模式匹配表达式，必须穷尽。 |
| `module` | 可选声明当前文件所属模块；省略时由文件路径推断。 |
| `move` | 显式转移所有权，或声明闭包按值捕获。 |
| `mut` | 声明可写参数、可写接收者、调用点可写访问或 pattern 可写访问。 |
| `private` | 声明文件私有的顶层项或字段。 |
| `pub` | 导出声明，使其对外部 package 可见。 |
| `return` | 从当前函数返回。 |
| `self` | 方法中的当前接收者值。 |
| `Self` | 当前实现类型的类型名别名，只能在 `impl` 或 `interface` 相关上下文中使用。 |
| `static` | 声明静态存储项；默认不可变，跨线程可变状态必须放在同步类型中。 |
| `struct` | 定义结构体和值布局。 |
| `true` | 布尔真值。 |
| `trust` | 声明信任边界，允许作者承担编译器无法独立证明的底层不变量。 |
| `try` | 展开 `Result` 或 `Option`，失败分支从当前函数提前返回。 |
| `type` | 定义类型别名。 |
| `val` | 创建不可变绑定。 |
| `var` | 创建可变绑定。 |
| `while` | 条件循环。 |

## 4. 属性

属性用于给声明附加编译期契约。

```zn
@noAlloc
fn kernelTick() {
    updateTimer();
}
```

v0.1 用户级属性：

- `@noAlloc`：被标注函数及其可达调用图不得执行堆分配，除非编译器能证明该路径不可达。
- `@noPanic`：被标注函数不得触发 `panic` 路径；若当前 profile 把 OOM 配置成 `panic`，会分配的 API 也算 panic 路径。它不等价于“不可能终止”，需要禁止堆分配时应使用 `@noAlloc`。
- `@layout(Source)`、`@layout(C)`、`@layout(Packed(N))`：声明结构体内存布局策略。普通结构体默认使用 Auto layout。
- `@export("symbol", abi: C)`：把非泛型顶层 `pub fn` 作为严格外部 C ABI 符号导出。
- `@export("symbol", bridge: C)`：把非泛型顶层 `pub fn` 通过编译器生成的 C ABI thunk 和头文件导出。

编译器发行包专用属性：

- `@intrinsic`：把声明绑定到编译器内建行为。

发行包专用属性不是普通包能给自己授予的能力。用户写底层代码应使用 `trust` 边界，而不是属性。

## 5. 绑定与可变性

`val` 创建不可变绑定。`var` 创建可变绑定。Zeno 使用 Kotlin 风格的局部绑定关键字，不使用 Rust 风格的声明可变性写法。

```zn
val answer: I32 = 42;
var count: USize = 0;
count = count + 1;
```

可变性属于绑定或访问方式，而不是永远属于某个值。不可变拥有者可以暴露只读访问；可变拥有者可以在没有其他访问时暴露唯一可写访问。

延迟初始化使用同一组关键字：

```zn
val file: File;
var buffer: Buffer;
```

`val name: T;` 必须在所有使用路径前被赋值一次，初始化后不可重新赋值。`var name: T;` 也必须在使用前初始化，但初始化后可以重新赋值；重新赋值非平凡资源时按赋值规则先销毁旧值再写入新值。延迟初始化声明只允许单个名字 pattern，并且必须写类型标注。

局部绑定的初始化状态由编译器在控制流图上跟踪：

- `uninit`：尚未初始化，没有销毁义务。
- `partial`：聚合值的部分字段已初始化，只能继续初始化字段或离开作用域清理已初始化字段。
- `init`：完整初始化，可以读取、移动、返回或调用方法。
- `moved`：拥有值已经被移动走，旧位置不能读取或销毁。
- `maybe-init`：控制流合流后某些路径初始化、某些路径未初始化；读取必须被拒绝，`var` 赋值需要条件销毁，`val` 赋值必须被拒绝。

`val` 初始化规则：

- `val x = expr;` 立即初始化，之后不可重新赋值。
- `val x: T;` 可以延迟初始化，但每条执行路径最多成功初始化一次。
- 给 `val` 写入时，编译器必须证明该绑定在当前控制流点是 `uninit`，且从未在当前路径成功初始化过。
- `val` 被移动后进入 `moved`，不能再次初始化。
- `val object: Struct; object.field = expr;` 可以作为部分初始化写入；一旦所有字段都初始化，整个 `object` 变成不可变完整值，之后不能再写字段。

`var` 初始化规则：

- `var x = expr;` 立即初始化，之后可以重新赋值。
- `var x: T;` 可以延迟初始化；使用前必须被证明为 `init`。
- `var` 被移动后进入 `moved`，之后可以通过赋值重新初始化。
- 对 `init` 或 `maybe-init` 的 `var` 赋值时，右侧先求值；右侧成功后，对旧值执行确定销毁或条件销毁，再写入新值。右侧失败时，旧初始化状态保持不变。
- `var object: Struct; object.field = expr;` 可以部分初始化字段；完整初始化后，字段仍可在没有活动访问时按普通可写字段规则修改。

控制流合流规则：

- 只有所有进入边都为 `init`，该位置才是确定已初始化。
- 只有所有进入边都为 `uninit` 或 `moved` 且没有成功初始化过，`val` 才能继续初始化。
- 对 `var`，`maybe-init` 可以被赋值覆盖，但不能读取；覆盖时生成条件销毁。
- `return`、`try`、`break`、`continue`、`panic` 或作用域自然结束时，只清理当前路径上仍拥有且已初始化的值。

局部绑定的最终销毁顺序按源码声明顺序的逆序决定，延迟初始化和重新赋值不改变该 cleanup 位置；未初始化或已移动的位置跳过销毁。

## 6. 常量、CTFE 与静态存储

`const` 声明编译期值。它没有运行期存储身份，使用处可以被内联。

```zn
const pageSize: USize = 4096;
const version: StringSlice = "0.1.0";
```

`const` 可以是顶层项、`impl` 项，也可以是块内局部编译期绑定。`const` 初始化必须通过编译期执行完成，初始化环是编译错误。

Zeno 支持完整 CTFE：编译期上下文可以执行普通 Zeno 代码，包括函数、方法、泛型、控制流、`match`、闭包、结构体、枚举、集合临时值、移动语义、RAII 和 `destroy`。Zeno 不引入 `comptime` 关键字，也不要求用户写 `const fn`；任何普通函数只要被用在编译期上下文中，编译器就尝试解释执行。

```zn
fn fib(n: U32) -> U32 {
    if n < 2 {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

const fib10: U32 = fib(10);
```

编译期上下文包括：

- `const` 初始化。
- `static` 初始化。
- 常量泛型实参。
- 属性参数。
- `sizeOf<T>()`、`alignOf<T>()`、`offsetOf<T>("field")`。
- pattern range 端点和其他要求编译期常量的位置。

CTFE 是 hermetic 的。编译期执行不能调用 `trust` / FFI、裸内存、硬件访问、inline asm、线程、任务、`await`、当前时间、随机数、环境变量、网络或未声明文件输入。需要文件内容进入编译期时，必须通过构建系统显式声明，并让内容 hash 进入缓存 key。

CTFE 可以使用编译器管理的编译期 arena 来构造临时 `String`、`Array`、`Vector`、`Map` 和 `Set`。这不代表运行期堆分配。编译期拥有值进入运行期时必须遵守成本模型：`Copy` 值可以内联；`StringSlice` / `ArraySlice<T>` 可以指向只读静态数据；需要运行期拥有 `String`、`Array<T>` 或 `Vector<T>` 时必须显式调用分配 / 复制 API。

`static` 声明拥有固定存储地址和整个程序生命周期的静态项。`static` 不表示运行期初始化钩子。

```zn
static REQUESTS: Atomic<U64> = Atomic<U64>.new(0);
```

规则：

- `static` 初始化式必须能在编译期完成，并物化为静态数据或目标支持的静态初始化记录。
- 普通 `static` 默认不可变。
- Zeno 不提供裸 `static mut` 全局变量；跨线程可变全局状态必须放在 `Atomic<T>`、`Mutex<T>` 或其他同步类型中。
- v1 不提供隐式动态 `static` 初始化；不会生成模块初始化函数、全局构造函数、CRT 构造函数或等价启动钩子。
- `static` 不能要求程序退出时自动运行 `destroy`；v1 不提供隐藏全局析构链或 `atexit` 注册。
- `static` 初始化不能执行运行期 I/O、FFI 调用、系统调用、线程启动、任务运行时创建、默认 allocator 分配或其他只能在运行期完成的操作。
- CTFE 可以使用编译期 arena 和临时集合计算值，但最终 `static` 只能物化为静态数据。需要运行期拥有者、堆分配、OS 句柄、线程池或缓存时，必须在 `main`、显式 `init` / `open` / `load` / `new` API，或显式懒初始化类型中完成。
- 显式懒初始化必须把成本放在类型里，例如 `Once<T>`、`Mutex<T>`、`Atomic<T>` 或平台库定义的同步容器；这些类型的静态初始化本身仍必须是 CTFE / 静态可物化的。

示例：

```zn
static Hits: Atomic<U64> = Atomic<U64>.new(0); // ok: 静态可物化

static Config = File.readToString("app.toml"); // error: runtime IO in static initializer
static Cache = Map<String, User>.withCapacity(1024); // error: runtime allocation/global cleanup
```

运行期初始化必须显式出现在可执行路径里：

```zn
fn main() -> Result<Unit, Error> {
    val config = try Config.load("app.toml");
    var app = try App.init(move config);
    return mut app.run();
}
```

完整规则见 [COMPTIME.md](COMPTIME.md)。

## 7. 类型

基础类型：

```text
Bool
I8 I16 I32 I64 ISize
U8 U16 U32 U64 USize
F32 F64
Char
Unit
Never
```

`Never` 没有任何可构造的值，表示函数不会正常返回。`Never` 可以在类型检查中流入任意期望类型，因为执行不会继续到需要该值的位置。`panic(...)`、`oom(...)`、永久循环和目标 trap 都可以返回 `Never`。

内建复合形式：

```text
Array<T>          拥有、连续、长度创建后固定的数组
Vector<T>         拥有、连续、可增长的数组
ArraySlice<T>     不拥有、连续、长度稳定的数组片段
Map<K, V>         拥有、哈希表、键值映射
Set<T>            拥有、哈希表、唯一值集合
String            拥有、UTF-8、可增长的文本值
StringSlice       不拥有、UTF-8、长度稳定的文本片段
CachePadded<T>    显式 cache line padding 包装，不分配
Tuple<T...>       元组值
Fn<A..., R>       只读、可重复调用的非拥有 callable
MutFn<A..., R>    可变、可重复调用的非拥有 callable
OnceFn<A..., R>   消费式、最多调用一次的 callable
```

`Fn<R>`、`MutFn<R>` 和 `OnceFn<R>` 表示无参数 callable，返回 `R`；`Fn<A, R>` 表示一个参数 `A`、返回 `R`；更多参数依次写在返回类型前。`Fn`、`MutFn` 和 `OnceFn` 是编译器认识的 callable 能力，不要求每个闭包都分配对象或生成接口表。

命名规则：

- 所有类型名使用 PascalCase，包括基础类型、集合、接口、结构体、枚举和错误类型。
- 关键字使用小写。
- 函数名、方法名、字段名、局部变量、模块路径段和属性名使用 lowerCamelCase。
- 常量项也是值名，默认使用 lowerCamelCase，例如 `pageSize`。
- 普通标识符不使用 snake_case；单独的 `_` 只用于丢弃模式。
- 语言不提供小写类型别名；例如使用 `String`，不使用 `str`；使用 `U8`，不使用 `u8`。

堆拥有、同步等能力是库类型，不是魔法语法：

```text
Box<T>
Shared<T>
Mutex<T>
Atomic<T>
```

普通代码通常写 `Box<T>` 和 `Shared<T>`；需要显式 allocator 时用 `Box.newIn` / `Shared.newIn` 等 `In` 后缀构造 API，而不是把 allocator 传进所有调用点。

`Box`、`Array`、`Vector`、`Map`、`Set`、`Shared`、`Mutex` 和原子类型必须在源码中显式出现，因为它们携带分配或同步成本。

### 7.1 类型布局与 ABI

结构体默认使用 Auto layout：编译器可以重排字段以减少 padding、降低数组元素跨度、改善默认缓存密度。字段初始化、字段访问、move 状态和销毁语义仍按源码字段名和声明顺序检查。

需要手动字段顺序时写 `@layout(Source)`；需要 C 互操作时写 `@layout(C)`；需要字节级协议或磁盘格式时写 `@layout(Packed(N))`。

```zn
struct Entry {
    flag: Bool,
    id: U64,
    kind: U8,
}

@layout(Source)
struct HotState {
    hotA: U64,
    hotB: U64,
    coldFlag: Bool,
}

@layout(C)
pub struct CPoint {
    pub x: F32,
    pub y: F32,
}

@layout(Packed(1))
struct Header {
    tag: U8,
    len: U16,
}
```

单真实存储字段结构体默认是零成本包装：它和唯一非零大小字段有相同大小、对齐和值表示，但类型系统仍把它们视为不同类型。

```zn
struct FileDescriptor {
    value: I32,
}
```

默认 enum layout 可以做 niche optimization。编译器必须保证 `Option<Box<T>>` 与 `Box<T>` 同大小，`Option<Shared<T>>` 与 `Shared<T>` 同大小；core/std 中声明了无效句柄值的句柄类型也应使用无额外 tag 的表示。

`@layout(C)` 结构体只接受 C-compatible 字段，不能有自定义 `destroy`，并且布局、对齐和聚合传参规则由目标 C ABI 决定。普通 enum、`Option<T>`、`Result<T, E>`、`Char`、普通 struct 和未标注 `@layout(C)` 的零成本包装不自动 C-compatible。

`pub` 只表示源码 API 对外部 package 可见，不表示布局或 ABI 稳定。完整布局规则见 [LAYOUT.md](LAYOUT.md)。

## 8. 整数运算与转换

Zeno 的整数运算必须高性能且行为确定。普通整数算术不插入隐藏溢出检查，也不会产生未定义行为。

规则：

- 整数 `+`、`-`、`*` 默认按类型位宽 wrapping。
- wrapping 结果等价于数学结果对 `2^位宽` 取模。
- 有符号整数也使用同一条位模式 wrapping 规则，例如 `I8` 的 `127 + 1` 结果是 `-128`。
- 除法、取模和移位不使用 wrapping 兜底；除以零、取模零、移位量大于等于位宽必须被拒绝或在运行时 trap，具体 trap 形式由 profile 定义。
- 常量表达式中的除零、取模零、非法移位是编译错误。

```zn
val a: U8 = 250;
val b: U8 = 10;
val c = a + b; // c == 4
```

需要显式溢出处理时使用整数方法：

```zn
val checked: Option<U8> = a.checkedAdd(b);       // None
val wrapped: U8 = a.wrappingAdd(b);              // 4
val saturated: U8 = a.saturatingAdd(b);          // 255
val both: (U8, Bool) = a.overflowingAdd(b);      // (4, true)
```

`checked*` 方法返回 `Option<T>`：

- `Some(value)` 表示运算成功。
- `None` 表示溢出、除零、取模零，或有符号最小值除以 `-1` 这类不可表示结果。

Zeno 不为整数溢出定义 `OverflowError`，因为溢出没有额外错误信息。需要进入 `Result` 错误流时，由调用方把 `None` 转换成自己的错误类型。

```zn
val size = match count.checkedMul(elementSize) {
    Some(value) => value,
    None => return Err(AllocError.TooLarge),
};
```

类型转换规则：

- `as` 只允许无损数值转换，例如小整数到大整数、无符号到能完整表示其范围的有符号类型。
- 可能截断或改变符号含义的转换不能使用 `as`。
- 检查窄化使用 `T.fromChecked(value) -> Option<T>`。
- 明确截断使用 `T.truncate(value)`。
- 钳制转换使用 `T.saturate(value) -> T`，把数学值限制在目标整数类型可表示范围内。
- 明确按位重解释只能出现在专门 API 或 `trust` 边界中，不能由普通 `as` 隐藏。

```zn
val byte: U8 = 42;
val wide: U32 = byte as U32;              // ok: 无损扩大
val maybeByte = U8.fromChecked(wide);     // Option<U8>
val low = U8.truncate(wide);              // 明确截断
val clamped = U8.saturate(wide);          // 超过 U8.max 时得到 255
```

## 9. 值、Copy 与 Move

每个值要么是 `Copy`，要么是资源拥有值。

`Copy` 值可以通过赋值、传参和返回复制：

```zn
val a: I32 = 1;
val b = a;
val c = a + b;
```

资源拥有值不会隐式复制。非 `Copy` 值进入拥有位置时会转移所有权，移动资源会让原绑定不可再用。普通函数参数 `x: T` 不触发所有权转移；它是只读访问。接收所有权的参数通过声明处的 `move x: T` 表达，消费方法通过 `move self` 表达。`move` 参数在函数体内是唯一拥有者，因此可以作为 `mut` 调用的接收者或实参使用；调用点仍然只写 `move value`，不引入 `mut move`。

Zeno 采用混合显式移动规则：

- 赋值、`val` / `var` 绑定、`return`、结构体字段初始化、enum payload 构造等拥有位置可以自动移动非 `Copy` 值，因为语法本身已经在构造新的拥有者。
- 函数和方法的 `move` 参数从已有命名位置接收所有权时，调用点必须显式写 `move value`。
- `mut self` 方法从已有命名接收者取得唯一可写访问时，调用点必须显式写 `mut receiver.method(...)`。
- `move self` 方法从已有命名接收者接收所有权时，调用点必须显式写 `move receiver.method(...)`。
- 临时值、字面量、结构体字面量和函数返回值没有后续可用绑定，传给 `move` 参数时不需要额外写 `move`。
- 编译器必须在移动后继续使用时报告 use-after-move，并指出是哪一个显式或自动拥有位置触发了移动。

```zn
val file = try File.open("data.txt");
val owner = file;
// file 现在不可再用
```

`move` 不是普通表达式前缀。它只出现在参数声明、`move self`、调用实参、消费接收者、闭包捕获、`match move`、`if move pattern = expr`、`while move pattern = expr` 和 `for move` 这类所有权模式位置。`return move value` 和普通 `val owner = move value` 是无效语法；这些拥有位置直接写 `return value`、`val owner = value`。

`Copy` 是显式选择：

```zn
struct Point: Copy {
    x: F64,
    y: F64,
}
```

只有当所有字段都是 `Copy` 且类型没有自定义 `destroy` 块时，类型才能实现 `Copy`。

复制和克隆规则：

- 非 `Copy` 拥有者不能通过赋值、传参或返回隐式复制。
- 非 `Copy` 拥有者进入拥有位置时自动移动，例如 `val b = packet;`。之后继续使用 `packet` 是错误。
- `Array<T>`、`Vector<T>`、`Map<K, V>`、`Set<T>`、`String`、`Box<T>`、`Shared<T>`、`Mutex<T>` 和带 `destroy` 的类型默认不是 `Copy`。
- 深拷贝必须显式调用 `clone`，不能由普通赋值隐藏。
- `Array<T>.clone`、`Vector<T>.clone`、`Map<K, V>.clone` 和 `Set<T>.clone` 表示深拷贝元素和存储，不是共享底层缓冲区。默认 clone 使用 profile 默认 allocator；指定 allocator 时使用 `cloneIn`。v1 先为 `Copy` 元素提供集合 clone；非 `Copy` 元素的深拷贝接口后续单独设计。
- `String.clone()` 是显式拥有文本复制，可能分配；指定 allocator 时使用 `String.cloneIn(mut allocator)`；`@noAlloc` 中必须被拒绝。
- 引用计数增加只能通过 `Shared<T>.clone` 这类类型名中显式包含 `Shared` 的 API 表达。

```zn
val packet = makePacket();
val b = packet;                         // ok: 所有权转移
// packet 现在不可再用
val c = b.clone();                      // ok: 显式深拷贝，可能分配
val d = c.cloneIn(mut arena);           // ok: 指定 allocator 的深拷贝
```

## 10. 赋值与替换

赋值表达式写入一个已经初始化的位置时，语义是“先得到新值，再销毁旧值，再写入新值”。它不返回旧值。

```zn
var file = try File.open(path1);
file = try File.open(path2);   // 右侧先成功产生新 File，再销毁旧 file，再写入新 file

items[i] = user;               // 销毁旧元素，写入新元素；user 被移动
packet.header = newHeader;     // 销毁旧字段，写入新字段；newHeader 被移动
```

规则：

- 左侧必须是可写位置，例如 `var` 绑定、可写字段、可写元素或 `mut self` 内的可写字段。
- `val` 只能在延迟初始化期间写入；一旦完整初始化或移动后，不能重新赋值。
- `var` 移动后可以重新赋值；重新赋值前读取该位置仍然是 use-after-move / use-before-init。
- 右侧先求值。若右侧 panic、OOM 或通过 `try` 提前返回，左侧保持原值不变。
- 左侧已经初始化且类型不是平凡 `Copy` store 时，赋值必须先销毁旧值，再把新值写入该位置；左侧为 `maybe-init` 的 `var` 时，销毁旧值是条件执行。
- `Copy` 类型赋值是普通复制和 store，不需要销毁旧值。
- 没有 `destroy` 且字段销毁全为空的类型，优化后可以把赋值降低为直接 store、memcpy 或标量写入。
- 若左侧未初始化，赋值只是初始化该位置，不销毁旧值；若是 `val`，该写入会消耗唯一初始化机会。
- 不能覆盖当前存在活动只读访问或可写访问的位置；这包括局部变量、字段、数组元素和 `Vector<T>` 元素。
- 对非 `Copy` 的命名右侧值，赋值会自动移动右侧值，例如 `file = nextFile;`。之后继续使用 `nextFile` 是错误。

需要拿回旧值时不能使用普通赋值，必须使用显式替换 API：

```zn
val old = mut items.replaceAt(i, move user);
use(old);                      // old 被移动给 use
```

`replaceAt(index, move value)` 语义是：边界检查通过后，移动出旧元素作为返回值，再把新元素写入原位置。它不销毁旧元素；旧元素的销毁责任转移给返回值拥有者。若新值求值失败，容器保持不变。

## 11. RAII 与销毁

资源生命周期是确定性的。已经移动走的值不会在旧位置销毁。完整初始化的资源会被销毁一次，除非它被移动给新的拥有者。

```zn
struct Buffer {
    bytes: Vector<U8>,
}

impl Buffer {
    destroy {
        // 自定义清理在字段销毁前运行
    }
}
```

大多数类型不需要自定义 `destroy` 块。字段会自动销毁。自定义销毁主要用于 OS 句柄、socket、GPU 资源、内存映射区域等外部资源。

`destroy` 是不可失败的兜底清理：

- `destroy` 不返回值。
- `destroy` 不是 `move self` 方法；它由编译器在对象最终销毁前自动调用，用户不能直接调用。
- `destroy` 中不能使用 `try`。
- `destroy` 中不能使用 `await`，也不能依赖异步清理完成。
- `destroy` 隐式满足 `@noPanic` 和 `@noAlloc`；它不能直接或间接调用可能 `panic`、可能 OOM、可能分配或可能通过运行时检查进入 panic 路径的操作。
- `destroy` 不能作为常规错误流的一部分。
- `destroy` 调用的清理 API 必须是不可失败或 best-effort API，返回 `Unit`。
- 需要处理清理错误时，类型应提供显式的 `close`、`flush`、`finish` 或类似 `move self` 方法，返回 `Result<T, E>`。
- `destroy` 可以读取字段和更新本对象内部状态，但不能移动字段，不能返回字段，不能创建逃逸访问。
- 如果调用方没有显式完成资源，`destroy` 只做兜底清理；它不能把错误传播给离开作用域的代码。

```zn
impl File {
    fn close(move self) -> Result<Unit, IoError> {
        try os.closeChecked(self.handle);
        self.closed = true;
        return Ok(());
    }

    destroy {
        if !self.closed {
            os.closeBestEffort(self.handle);
        }
    }
}
```

显式 `close` 成功后应让资源进入已完成状态，避免随后运行的 `destroy` 重复清理。`close(move self)` 消费的是调用方的拥有者，但方法体中的 `self` 在退出时仍然按普通 RAII 规则销毁，因此 `destroy` 仍会运行；通常通过 `closed`、`finished` 或等价状态让它变成 no-op。若 `close` 失败并通过 `try` 返回错误，`destroy` 仍会执行 best-effort 兜底清理。

销毁顺序：

1. 局部绑定按源码声明顺序的逆序销毁；未初始化或已移动的位置跳过销毁。
2. 如果类型有 `destroy` 块，先运行该块。
3. 结构体字段按源码声明顺序的逆序销毁；这不受 Auto layout 的内存字段重排影响。
4. 部分初始化的值只销毁已经初始化的字段。
5. 完整对象的 `destroy` 只在整个对象已经完整初始化时运行；部分初始化失败路径只销毁已经初始化的字段，不运行外层完整对象的 `destroy`。
6. `panic` 不属于常规错误流；具体 abort、trap 或 unwind 行为由 profile 失败策略决定。

字段移动规则：

- 只有拥有整个对象时，才能移动出非 `Copy` 字段，例如把 `self.bytes` 或 `packet.bytes` 返回、传给 `move` 参数，或写入另一个拥有位置。
- 从 `self` 或 `mut self` 接收者中不能移动字段，因为它们不拥有对象。
- 字段进入拥有位置后会把该字段标记为已移出；函数退出时只销毁仍然初始化的字段。
- 移出字段后，不能再读取该字段，也不能再把原对象当作完整值使用。
- 有自定义 `destroy` 块的类型在安全代码中不能移动出非 `Copy` 字段，包括类型自己的 `move self` 方法。原因是 `destroy` 可能依赖完整对象不变量。
- 类型自己的 `move self` 方法仍然可以作为显式完成 API，例如 `close`、`flush`、`finish` 或 `intoRaw`，但它只能修改完成状态、返回 `Copy`/新构造的值，或移动整个 `self` 给另一个拥有者，不能安全地拆出非 `Copy` 字段。
- 如果确实需要跳过 `destroy` 并拆出底层字段，必须使用受审计的 `trust` 边界或核心库提供的受信拆解 primitive。普通 `trust` 仍不关闭类型检查和初始化检查；编译器必须记录这种销毁解除行为。

```zn
struct Packet {
    bytes: Array<U8>,
    header: Header,
}

impl Packet {
    fn intoBytes(move self) -> Array<U8> {
        return self.bytes;
        // self.header 仍会被销毁；self.bytes 不会被销毁两次
    }
}
```

## 12. 集合访问

Zeno 不要求用户写生命周期标注。日常表面规则是：

- 没有修饰的参数是只读访问。
- `mut` 参数是唯一可写访问。
- `move` 参数接收所有权。

只读连续访问使用 `ArraySlice<T>`。它不拥有数据，不分配，只保存连续存储的位置和长度。

```zn
fn checksum(data: ArraySlice<U8>) -> U32 {
    var sum: U32 = 0;
    for byte in data {
        sum = sum + byte as U32;
    }
    return sum;
}
```

唯一可写连续访问：

```zn
fn fill(mut out: ArraySlice<U8>, value: U8) {
    for i in 0..out.len {
        out[i] = value;
    }
}
```

拥有集合：

```zn
val page = Array<U8>.filled(4096, 0);          // 固定长度，默认 allocator
val bytes = Vector<U8>.withCapacity(1024);      // 可增长，默认 allocator
val counts = Map<U64, USize>.withCapacity(128); // 哈希映射，默认 allocator
val seen = Set<U64>.withCapacity(128);          // 哈希集合，默认 allocator
val arenaBytes = Vector<U8>.withCapacityIn(1024, mut arena); // 指定 allocator
```

集合类型含义：

- `Array<T>`：拥有连续存储，长度创建后固定，适合页缓冲区、帧缓冲区和固定批量数据。
- `Vector<T>`：拥有连续存储，长度可增长，带 `capacity`，适合构建结果和追加写入。
- `ArraySlice<T>`：不拥有连续存储，只表达一段数组访问，适合 API 参数、局部切片和窗口。
- `Map<K, V>`：拥有哈希表，保存唯一键到值的映射，适合按键查找、计数表、符号表和索引。
- `Set<T>`：拥有哈希表，保存唯一值，适合去重、成员关系和已访问集合。

访问规则：

- 多个只读访问可以共存。
- 可写访问在其活动区域内必须唯一。
- 访问值不能比它引用的存储活得更久。
- 可写调用必须在调用点显式写出：`fill(mut bytes, 0)`。
- 普通函数的可写访问和所有权转移由签名与调用点共同表达：`fill(mut bytes, 0)` 会把 `bytes` 短期借给 `fill(mut data: ArraySlice<U8>)` 修改；`consume(move bytes)` 会把 `bytes` 移给 `consume(move data: Vector<U8>)`。方法也一样：`mut receiver.method()` 调用 `mut self` 方法，`move receiver.method()` 调用 `move self` 方法。
- `Array<T>` 和 `Vector<T>` 可以在参数位置自动转成 `ArraySlice<T>`。
- `Array<T>` 和 `Vector<T>` 可以在调用点 `mut` 转成 `mut ArraySlice<T>`，前提是没有重叠活动访问。
- 当 `Array<T>` 或 `Vector<T>` 存在活跃 `ArraySlice<T>` 时，拥有者不能被移动或销毁。
- `Vector<T>` 存在活跃视图时，禁止 `push`、`pop`、`reserve`、`insert`、`removeAt`、`swapRemove`、`clear`、`shrinkToFit` 等改变长度、容量或元素位置的结构性修改，也禁止 `replaceAt` 这类元素替换。
- 活跃视图结束后，`Vector<T>` 可以继续增长或收缩。
- `reserve(additional)` 和 `reserveExact(additional)` 提前扩容，失败时调用当前 profile 的 `oom(layout) -> Never`。
- `tryReserve(additional)` 和 `tryReserveExact(additional)` 返回 `Result<Unit, AllocError>`，只预留容量，不插入元素；失败时原集合内容不变。它们用于复杂流程前先保证后续 `push` 不会中途 OOM。
- `for item in data` 对 `Array<T>`、`Vector<T>` 和 `ArraySlice<T>` 做只读元素遍历。
- `for mut item in mut data` 对连续集合做唯一可写元素遍历。
- `for move item in data` 消耗拥有集合，逐个移动元素；只适用于拥有的 `Array<T>` 和 `Vector<T>`，不适用于 `ArraySlice<T>`。
- `splitDisjoint(parts)` 把连续集合切成互不重叠的 scoped 可写片段。它返回的 `DisjointSlices<T>` 是 view-like 值，只能局部使用，特别适合 `Thread.scope`。

关联集合是标准库类型，不是语言关键字。`Map<K, V>` 和 `Set<T>` 默认使用高性能哈希表实现，元素顺序不稳定，不能依赖遍历顺序做持久化、网络协议或可复现输出。需要有序遍历时，后续可由 `OrderedMap` / `OrderedSet` 或排序后的 `Vector<T>` 表达，不能让默认 `Map` 为顺序付出隐藏成本。

```zn
interface Hash {
    fn hash(self, mut state: Hasher);
}

interface HashKey: Hash, Eq {}
interface LookupKey<K: HashKey>: Hash {
    fn equalsKey(self, key: K) -> Bool;
}
```

`Map<K, V>` 要求 `K: HashKey`，`Set<T>` 要求 `T: HashKey`。如果两个键通过 `Eq.eq` 判断相等，它们的 `Hash.hash` 必须向同一 hasher 写入相同语义内容。插入到 `Map` / `Set` 后，键不能通过公开 API 被可写访问，因此不能在表内改变会影响 hash 或 equality 的字段。

查找不应该消费 key。标准库用普通接口约束表达 lookup-compatible key，例如 `LookupKey<K>`：`K` 自己可以作为 lookup key，`StringSlice` 也可以作为 `String` 的 lookup key。`LookupKey<K>` 是库接口，不是关键字；它的 hash 必须和等价的 `K` 一致，`equalsKey(self, key: K)` 只读比较调用方 lookup key 和表内 key。它允许 `Map<String, V>.get(name)` 和 `Map<String, V>.get("literal")` 不分配、不移动 `String`。

必要操作：

- `Map.empty()` / `Set.empty()` 创建空集合，可以不分配。
- `withCapacity(capacity)` 预留容量，失败时调用 `oom(layout) -> Never`。
- `tryReserve(additional)` 和 `tryReserveExact(additional)` 是可恢复 OOM 入口，失败时集合内容保持不变。
- `Map.get(key) -> Option<V>` 和 `Map.containsKey(key) -> Bool` 只读 lookup key，不移动调用方的 key。
- `Map.insert(move key, move value) -> Option<V>` 调用签名接收 key/value 所有权；命中已有等价 key 时保留表内旧 key，销毁传入的新 key，并返回旧 value。
- `Map.remove(key) -> Option<V>` 只读 lookup key，移除对应的 value，并销毁表内 key；不移动调用方的 key。
- `Map.removeEntry(key) -> Option<(K, V)>` 只读 lookup key，移除并返回表内 key 与 value。
- `Set.insert(move value) -> Bool` 调用签名接收 value 所有权，返回是否真正插入；已有等价值时销毁传入 value。
- `Set.contains(value) -> Bool` 只读 lookup value，不移动调用方的 value。
- `Set.remove(value) -> Bool` 只读 lookup value，移除并销毁表内值。
- `Set.take(value) -> Option<T>` 只读 lookup value，移除并返回表内值。

`Map.get(key) -> Option<V>` 只为 `V: Copy` 提供，因为它返回 value 的复制；这里的 `key` 是只读 lookup key。非 `Copy` value 的短期访问使用 checked index 语义：

```zn
if counts.containsKey(id) {
    val count = counts[id];        // USize 是 Copy，复制值
}

if objects.containsKey(id) {
    objects[id].touch();           // 非 Copy value 的短期只读访问
}

mut objects[id].markLive();        // 短期唯一可写访问；key 不存在时 panic
```

`map[key]` 和 `mut map[key]` 与数组索引一样是检查访问。key 不存在时调用当前 profile 的 panic 处理。编译器和标准库实现应让 `containsKey` 后紧跟同一 key 的索引访问共享查找事实，避免无意义的重复哈希；高性能更新代码可以使用 `entry` API 做单次探测。

`Map.entry(mut self, move key)` 的签名接收 key 所有权，返回 view-like 的 `MapEntry<K, V>`，用于“可能插入”的单次探测场景。因为缺失时 entry 需要把 key 存入表内，所以它必须拥有 key。只查找或只删除时应使用 `get`、索引访问、`containsKey`、`remove` 或 `removeEntry`，它们不移动 lookup key。`MapEntry<K, V>` 不能保存到结构体、集合、`static`、`Box`、`Shared`、逃逸闭包、线程、任务或 async future 中。

标准库 API 必须遵守所有权审计表：lookup 不移动，存储才移动，`mut self` 调用点写 `mut receiver.method(...)`，`move self` 调用点写 `move receiver.method(...)`。完整表见 `docs/STDLIB.md`。

```zn
val entry = counts.entry(id);
if entry.exists() {
    entry.replace(entry.value() + 1);
} else {
    mut entry.insert(1);
}
```

`Map` / `Set` 的实现要求：

- 默认实现使用开放寻址或等价 cache-friendly 哈希表，不为每个元素做单独堆分配。
- 扩容和 rehash 成本只出现在 `reserve`、`tryReserve`、`insert`、`entry.insert` 等会改变容量的 API 中。
- `for move item in set` 消耗集合并逐个移动出元素。
- `for move pair in map` 消耗集合并逐个移动出 `(K, V)`。
- 只读遍历和可写遍历不得分配迭代器对象；迭代顺序不稳定。
- `Map<K, V>.clone` 只在 `K: Copy` 且 `V: Copy` 时进入 v1；`Set<T>.clone` 只在 `T: Copy` 时进入 v1。

索引表达式只表达元素访问，不表达从容器中取走所有权：

```zn
val byte = bytes[i];          // T: Copy 时复制元素值
val id = users[i].id;         // 非 Copy 元素上读取 Copy 字段，不移动元素
users[i] = user;              // 替换元素；user 被移动
normalize(mut users[i]);      // 把元素作为唯一可写访问传入
```

- `collection[index]` 默认做检查访问，越界调用当前 profile 的 panic 处理；优化器必须消除可证明冗余的边界检查。
- 对 `Copy` 元素，读索引可以复制元素值。
- 对非 `Copy` 元素，读索引产生短期只读元素访问；它不能被保存到字段、集合、逃逸闭包、线程、任务或 async future。
- `mut collection[index]` 产生短期唯一可写元素访问，要求底层集合本身处于可写位置，且没有重叠活动访问。
- `collection[index]` 不能进入拥有位置并移动出元素；容器元素不能通过索引留下未初始化洞。
- 需要从 `Vector<T>` 中取走拥有值时使用方法：`pop()`、`removeAt(index)` 或 `swapRemove(index)`。
- 需要替换元素并取回旧值时使用方法：`replaceAt(index, move value)`。方法签名接收 `value` 所有权；`Array<T>` 和 `Vector<T>` 都支持该操作。
- `get(index) -> Option<T>` 只为 `T: Copy` 的集合提供。非 `Copy` 元素的无 panic optional 访问在 v1 不引入额外包装类型；用户可以先检查 `index < len` 再使用 `collection[index]`，编译器应消除重复边界检查。

编译器通过词法作用域、控制流分析和逃逸分析推断访问生命周期。用户在 v0.1 不写生命周期参数。

`ArraySlice<T>` 不能作为 `struct`、`enum`、`tuple` 或其他用户定义类型的字段，也不能通过 `Array<ArraySlice<T>>`、`Vector<ArraySlice<T>>` 等形式间接保存。结构体如果需要保存数据，字段必须保存拥有者，例如 `Array<T>`、`Vector<T>`，或确实需要共享时保存 `Shared<Array<T>>`。

```zn
struct Packet {
    bytes: Array<U8>,
}
```

函数仍然可以使用临时切片：

```zn
fn parsePacket(packet: Packet) -> Result<Token, ParseError> {
    return parseBytes(packet.bytes.asSlice());
}
```

拒绝把切片放进字段：

```zn
struct BadPacket {
    bytes: ArraySlice<U8>, // error: ArraySlice fields are not allowed
}
```

允许从参数派生并返回切片：

```zn
fn withoutHeader(bytes: ArraySlice<U8>) -> ArraySlice<U8> {
    return bytes.dropPrefix(4);
}
```

返回值隐藏依赖于参数 `bytes` 的有效范围。调用方不写生命周期标注，编译器负责把这个依赖带到返回值上。

拒绝悬垂视图：

```zn
fn bad(mut allocator: GlobalAllocator) -> Result<ArraySlice<U8>, AllocError> {
    val bytes = Array<U8>.filledIn(64, 0, mut allocator);
    return Ok(bytes.asSlice()); // error: view outlives local storage
}
```

`ArraySlice<T>` 也不能进入长期或逃逸位置：`Box<T>`、`Shared<T>`、`Array<T>`、`Vector<T>`、`static`、逃逸闭包、线程、任务和 async future 都不能保存它。`StringSlice` 遵守同一条访问值规则。`DisjointSlices<T>` 和它产出的 scoped 可写片段也遵守同一条 view-like 规则。需要这些能力时，保存拥有者。

字符串遵守同一套拥有者 / 访问值区分：

```zn
val view = "hello";               // StringSlice，不分配
val owned = String.from("hello");      // String，拥有文本，可能分配
```

- 字符串 literal 的类型是 `StringSlice`，指向静态只读 UTF-8 数据。
- `StringSlice` 是文本访问值，适合参数、局部窗口和常量 literal；它不能作为结构体字段、集合元素、`static`、逃逸闭包捕获、线程任务捕获或 async future 状态保存。
- 需要长期保存运行期文本时使用 `String`。
- 拥有字符串统一用 `String.from(text)` 构造；不允许通过 `val text: String = "hello"` 隐式构造，也不引入 `new String(...)`。
- `String.from`、`String.push` 和 `String.clone` 可能分配；`String.fromIn`、`cloneIn` 等 `In` 后缀 API 用于指定 allocator；`@noAlloc` 中必须拒绝。
- `String.reserve` / `String.reserveExact` 失败时调用当前 profile 的 `oom(layout) -> Never`；`String.tryReserve` / `String.tryReserveExact` 返回 `Result<Unit, AllocError>`，失败时保持原字符串内容不变。
- `String.asBytes()` 和 `StringSlice.asBytes()` 返回只读 `ArraySlice<U8>`；UTF-8 字符遍历使用 `chars()`，不提供 O(1) 随机 `Char` 索引。

分配模型：

- 无 `In` 后缀的分配 API 使用 profile 默认 allocator，例如 `Array.filled`、`Vector.withCapacity`、`Map.withCapacity`、`Set.withCapacity`、`String.from`、`Box.new` 和 `Shared.new`。
- 带 `In` 后缀的 API 显式接收 allocator，例如 `Array.filledIn`、`Vector.withCapacityIn`、`Map.withCapacityIn`、`Set.withCapacityIn`、`String.fromIn`、`Box.newIn` 和 `Shared.newIn`。
- 默认分配 API 和 `In` 后缀分配 API 都不返回 `Result`。分配失败时调用当前 profile 的 `oom(layout) -> Never` 策略，不返回到调用点。
- v1 标准集合只在容量预留上提供可恢复 OOM：`Vector.tryReserve`、`Vector.tryReserveExact`、`Map.tryReserve`、`Map.tryReserveExact`、`Set.tryReserve`、`Set.tryReserveExact`、`String.tryReserve` 和 `String.tryReserveExact`。构造、`push`、`insert`、`clone`、`Box.new` 和 `Shared.new` 不提供 `Result` 版本。
- Freestanding profile 默认没有全局 allocator；除非目标 profile 明确配置默认 allocator，否则只能使用 `In` 后缀 API。
- Hosted profile 可以提供默认 allocator，让应用代码避免反复传 allocator。
- 拥有者必须记录释放和后续增长所需的 allocator 信息；实现可以对零大小 allocator、默认 allocator 或可静态证明的 allocator 做专用化和字段消除。
- allocator 分为 escaping allocator 和 scoped allocator。只有实现 `EscapingAllocator` 的 allocator 可以创建普通可逃逸拥有者；`ArenaAllocator`、`BumpAllocator`、`FixedBufferAllocator` 这类 scoped allocator 创建的拥有者不能比 allocator 活得更久。
- `In` 后缀 API 接收普通 `A: Allocator` 时，返回值会携带隐藏 allocation region；如果该 region 来自 scoped allocator，编译器必须拒绝返回、长期保存或移入非 scoped 并发 / async 状态。
- 泛型 API 如果要返回由 allocator 创建的拥有者，应约束为 `A: EscapingAllocator`。

```zn
fn buildLocal(mut arena: ArenaAllocator) {
    val bytes = Vector<U8>.withCapacityIn(1024, mut arena);
    consume(bytes.asSlice());
} // ok

fn badReturn(mut arena: ArenaAllocator) -> Vector<U8> {
    return Vector<U8>.withCapacityIn(1024, mut arena); // error: scoped allocation escapes allocator
}

fn buildReturn<A: EscapingAllocator>(mut allocator: A) -> Vector<U8> {
    return Vector<U8>.withCapacityIn(1024, mut allocator);
}
```

活跃视图期间，`Vector<T>` 的只读查询仍然允许：

```zn
val view = data.asSlice();
val n = data.len;       // ok
mut data.push(1);       // error: live view depends on data
```

如果需要结构性修改，让视图先结束：

```zn
{
    val view = data.asSlice();
    parse(view);
}

mut data.push(1);       // ok
```

`Array<T>` 长度固定，没有增长和收缩操作。修改元素仍然遵守普通访问规则：存在重叠只读视图时不能取得可写访问；编译器能证明不重叠的拆分访问可以通过。这个规则不需要运行时引用计数、锁或 GC。

## 13. 函数

```zn
fn add(a: I32, b: I32) -> I32 {
    return a + b;
}
```

参数行为：

- `x: T`：默认只读访问。对非 `Copy` 资源不移动、不复制；函数不能销毁或保存该资源。对 `Copy` 值，编译器可以按值传递。
- `mut x: T`：唯一可写访问。函数可以修改被访问的值，但不取得所有权；调用点必须写 `mut`。
- `move x: T`：接收所有权。函数结束时销毁该值，或继续把它移动给其他拥有者；从已有命名位置传入时调用点必须写 `move`，非 `Copy` 实参在调用后不可再用。
- `Box<T>`、`Vector<T>`、`Shared<T>`、`Mutex<T>` 等库类型让拥有关系或运行时成本显式可见。

例子：

```zn
fn inspect(file: File) {
    // 只读访问；调用后调用方仍然拥有 file
}

fn rewrite(mut file: File) -> Result<Unit, IoError> {
    // 可写访问；调用后调用方仍然拥有 file
    return file.flush();
}

fn close(move file: File) {
    // 取得所有权；函数结束时 file 被销毁
}

val file = try File.open("data.txt");
inspect(file);
rewrite(mut file);
close(move file);
// file 现在不可再用
```

拥有数组字段的类型也遵守同一套规则：

```zn
struct Packet {
    bytes: Array<U8>,
}

fn parse(packet: Packet) -> Result<Token, ParseError> {
    // 默认只读访问；不取得 Packet 所有权
    return parseBytes(packet.bytes.asSlice());
}

fn normalize(mut packet: Packet) {
    // 唯一可写访问；调用后调用方仍然拥有 Packet
    fill(mut packet.bytes, 0);
}

fn archive(move packet: Packet) {
    // 接收所有权；函数结束时 Packet 被销毁，或继续移动给别的拥有者
}
```

编译器必须在移动后继续使用时给出明确诊断：

```zn
close(move file);
inspect(file); // error: use of moved value `file`
```

编译器也必须拒绝把只读参数当成拥有值返回或保存：

```zn
fn badReturn(packet: Packet) -> Packet {
    return packet; // error: read parameter is not owned
}

fn badSave(packet: Packet, mut allocator: GlobalAllocator) -> Box<Packet> {
    return Box.newIn(move packet, mut allocator); // error: read parameter cannot be moved into Box
}
```

虽然调用点会写 `move`，重载集仍不能只靠同一位置的只读参数和 `move` 参数区分。若两个候选的名字、参数数量、参数类型都相同，只是某个普通参数从 `x: T` 改成 `move x: T`，这是容易误读的声明，必须改名或改参数类型。`mut` 参数仍然通过调用点的 `mut` 访问形式区分。

函数按值返回。语义允许时必须支持返回值优化。返回 `Array<T>` 或 `Vector<T>` 表示返回拥有者；返回 `ArraySlice<T>` 表示返回非拥有访问，必须通过逃逸分析证明底层存储仍然有效。

### 12.1 函数重载

Zeno 支持函数和方法重载。多个函数或方法可以使用同一个名字，只要参数形状不同。

重载键由名字、参数数量、参数类型、参数访问模式组成。方法还把接收者类型和接收者模式纳入重载键。返回类型不参与重载；只靠返回类型不同的两个声明是重复定义。

```zn
fn parse(text: StringSlice) -> I32 { ... }
fn parse(text: StringSlice) -> U32 { ... } // error: return type is not an overload key
```

允许的重载例子：

```zn
fn size(text: StringSlice) -> USize { ... }
fn size(bytes: ArraySlice<U8>) -> USize { ... }
fn size(bytes: ArraySlice<U8>, radix: U8) -> USize { ... }

fn inspect(packet: Packet) -> I32 { ... }
fn archive(move packet: Packet) -> I32 { ... }
```

调用解析在类型检查期完成：

1. 收集当前可见的同名候选。
2. 按参数数量和调用点的 `mut` 形式过滤；调用点没有 `move` 形式。
3. 检查参数类型、接口约束和泛型约束。
4. 选择唯一最佳候选；没有候选或多个同等最佳候选时诊断为错误。

最佳候选排序保持保守：精确非泛型匹配优先，然后是精确泛型匹配；需要语言允许的访问转换时排在精确匹配之后。Zeno 不做隐式数值转换、隐式分配或基于返回类型的选择。整数字面量可以由唯一候选推断类型；多个候选都能接收同一个 literal 时，调用方必须加类型标注。

重载没有运行时派发成本。选中目标在编译期固定；泛型重载按普通泛型规则单态化。

## 14. 控制流

当所有分支产生兼容类型时，`if`、`match`、`while`、`for` 和块可以作为表达式。

```zn
val sign = if value < 0 { -1 } else { 1 };
```

`if pattern = expr` 和 `while pattern = expr` 使用第 15 节的 pattern 系统：

```zn
if Some(value) = maybe {
    use(value);
}

while move Some(item) = iterator.next() {
    consume(move item);
}
```

- `if pattern = expr` 在 pattern 匹配时执行 then 分支，不匹配时跳过或进入 `else`。
- `while pattern = expr` 每次循环先求值并匹配，匹配时进入循环体，不匹配时结束循环。
- `if move pattern = expr` 和 `while move pattern = expr` 消耗匹配值。
- `if mut pattern = place` 和 `while mut pattern = place` 要求右侧是可写位置，并给 payload 提供唯一可写访问。
- `val pattern = expr` 只允许不可失败 pattern；可能失败的 pattern 必须写 `match`、`if pattern = expr` 或 `while pattern = expr`。

循环：

```zn
while condition {
    step();
}

for item in items {
    visit(item);
}
```

`for` 循环遵守默认只读、`mut` 可写、`move` 消耗的访问规则。

只读遍历：

```zn
for item in items {
    inspect(item);
}
```

- 右侧集合以只读方式访问。
- 对非 `Copy` 元素，`item` 是只读元素访问，不隐式复制、不移动元素。
- 对 `Copy` 元素，编译器可以按值传递或寄存器化，但语义仍是只读遍历。

可写遍历：

```zn
for mut item in mut items {
    item = normalize(item);
}
```

- 右侧必须提供唯一可写访问，例如 `mut items`。
- `item` 是当前元素的唯一可写访问，可以赋值或传给 `mut` 参数。
- 循环期间不能对同一 `Vector<T>` 做结构性修改，例如 `push`、`reserve` 或 `clear`。

消耗遍历：

```zn
for move item in items {
    consume(move item);
}
```

- 右侧必须是拥有集合。循环模式中的 `for move` 已经表示消耗右侧拥有者，右侧表达式不再写 `move items`。
- `ArraySlice<T>` 不能消耗遍历，因为它不拥有元素。
- 每次迭代把一个元素移动到 `item`；如果 `item` 没有继续移动出去，它会在该次迭代结束时销毁。
- 循环正常结束后，集合的元素都已被移动或销毁，原集合绑定不可再用。
- 如果 `break`、`return`、`try` 或 `panic` 提前离开循环，尚未迭代的元素按 RAII 规则销毁，底层存储释放。

整数范围遍历：

```zn
for i in 0..data.len {
    sum = sum + data[i];
}
```

- `a..b` 是半开区间，包含 `a`，不包含 `b`。
- v1 的范围遍历只要求支持整数端点。
- 范围遍历不分配 iterator 对象，不做接口派发。
- 对 `Array<T>`、`Vector<T>`、`ArraySlice<T>` 和整数范围的 `for` 应降低成普通循环；优化后应消除可证明冗余的边界检查。

通用拥有式迭代使用 `Iterator<T>`：

```zn
interface Iterator<T> {
    fn next(mut self) -> Option<T>;
}

for move item in iterator {
    consume(move item);
}
```

- `Iterator<T>` 表示拥有或生成 `T` 值的流。
- `next(mut self)` 最多产生一个拥有的 `T`；`None` 表示结束。
- `for move item in iterator` 消费迭代器对象，循环内每个 `item` 都是拥有值。
- 命名的非 `Copy` 迭代器作为循环源时，由 `for move` 模式触发所有权转移；循环后原迭代器不可再用。
- `Iterator<T>` 默认走静态派发和内联；只有显式使用 `Box<Iterator<T>>` 或 `Shared<Iterator<T>>` 这类接口拥有者时才产生动态派发。
- `Iterator<T>` 不替代核心集合的只读/可写元素遍历。`for item in array` 和 `for mut item in mut array` 仍然直接 lower，避免把短期元素访问装进 `Option` 或堆对象。
- 自定义容器如果要提供拥有式遍历，可以暴露自己的迭代器类型，并让该迭代器实现 `Iterator<T>`。

Zeno v1 不提供 `defer` 关键字。作用域清理统一由 RAII、`destroy` 和 guard 类型表达：

```zn
fn withLock(m: Mutex<State>) -> Result<Unit, Error> {
    val guard = try m.lock();
    updateState(mut guard);
    return Ok(());
} // guard 离开作用域时自动 unlock
```

这样清理逻辑属于类型语义，cleanup 顺序由普通局部绑定销毁规则决定，不需要额外的语句级清理栈。

## 15. 枚举与模式匹配

```zn
enum Option<T> {
    Some(T),
    None,
}

fn unwrapOr<T: Copy>(value: Option<T>, fallback: T) -> T {
    return match value {
        Some(x) => x,
        None => fallback,
    };
}
```

Zeno 的 pattern 系统用于：

- `match expr { ... }`
- `if pattern = expr { ... }`
- `while pattern = expr { ... }`
- `val pattern = expr`，仅限不可失败 pattern

`match` 必须穷尽，除非存在通配分支。`match`、`if pattern = expr` 和 `while pattern = expr` 的接收模式决定被匹配值内部绑定的所有权：

```zn
match result {
    Ok(value) => log(value),          // 默认只读访问 payload，不移动 result
    Err(error) => log(error),
}

match move result {
    Ok(value) => consume(move value), // 消耗 result，拿走 payload 所有权
    Err(error) => return Err(error),
}

match mut state {
    Ready(job) => job.priority = 10,  // 原地可写访问 payload
    Done => {},
}
```

接收模式规则：

- `match value` 默认只读匹配。非 `Copy` payload 绑定是短期只读访问，不能被 `move`、返回、保存到字段、集合、逃逸闭包、线程、任务或 async future。
- `match move value` 消耗整个 enum。被选中分支的 payload 绑定是拥有值，可以进入函数、返回值或新 owner 中。匹配后原 enum 不可再用。
- `match mut value` 对 enum 做唯一可写匹配。payload 绑定是短期唯一可写访问，可以修改 payload 字段或传给 `mut` 参数，但不能移动出 payload。
- `if` pattern 和 `while` pattern 支持相同模式：`if move Some(x) = value`、`if mut Ready(job) = state`。
- `val pattern = expr` 没有 `move` / `mut` 前缀；它根据右侧表达式的普通所有权规则初始化绑定。右侧是命名非 `Copy` 值并需要所有权时会自动移动。
- `Copy` payload 在只读 `match` 中可以复制。
- `_` 只丢弃当前模式位置，不绑定值；它不会绕过 exhaustiveness 检查之外的所有权规则。
- 对带 payload 的 enum，普通只读 `match` 和 `match mut` 都不会销毁 payload；匹配结束后 enum 仍然保持原 variant 和初始化状态。
- `match move` 某个分支移动出 payload 后，只销毁该分支中仍然拥有且未移动的部分；不能 double drop 已移动 payload。
- `match` 作为表达式时，所有产生值的分支必须有兼容类型。`match` 作为语句时，分支可以返回 `Unit`。
- 优化后，普通 enum `match` 应降低为 tag switch 或等价分支，不分配、不动态派发。

v1 支持的 pattern 种类：

```zn
_                              // 通配，不绑定
name                           // 绑定

Some(value)                    // enum variant
Result.Ok(value)               // 可带 enum 名限定
State.Done                     // unit variant

Point { x, y }                 // struct 字段简写
Point { x: px, y: py }         // struct 字段重命名
Point { x, .. }                // 忽略剩余字段

(left, right)                  // tuple

0                              // 整数字面量
true                           // Bool 字面量
false
Status.Ready                   // const 或 unit variant

0..10                          // 半开整数范围
0..=10                         // 闭区间整数范围

A | B                          // or pattern
Some(Point { x, y })           // 嵌套 pattern
```

pattern 规则：

- 绑定名默认继承外层接收模式：只读 pattern 中是只读访问或 `Copy` 值，`match mut` 中是唯一可写访问，`match move` 中是拥有值。
- `_` 不创建绑定，也不触发 move。
- enum variant pattern 必须引用当前 enum 的合法 variant。若 variant 名在当前作用域不唯一，应写 `EnumName.Variant(...)`。
- struct pattern 必须命名字段；v1 不支持按字段顺序解构 struct。`Point { x, .. }` 忽略未列出的字段。
- tuple pattern 按位置匹配，元素数量必须完全相同。
- 字面量和范围 pattern 只支持 `Bool`、整数、`Char` 和 unit enum variant。`String` / `StringSlice` 字面量 pattern 不进入 v1，因为文本比较不是普通 tag / 整数分支。
- 范围 pattern 只支持同一整数或 `Char` 类型的编译期常量端点。空范围是编译错误。范围 pattern 不创建临时对象。
- or pattern 两侧必须绑定完全相同的名字、类型和访问模式。`Some(x) | Err(x)` 只有在两个 `x` 类型相同且访问模式相同时才合法。
- 嵌套 pattern 的所有权由外层接收模式递归决定。
- 带自定义 `destroy` 块的 struct 或 enum 在安全代码中不能通过 `match move` 或 `val` 解构移动出非 `Copy` 字段 / payload；只能读取、可写访问，或通过受审计的 `trust` / 核心库 primitive 拆解。

guard 使用 `if`：

```zn
match token {
    Number(n) if n > 0 => handlePositive(n),
    Number(_) => handleNumber(),
    Ident(name) => handleIdent(name),
}
```

- guard 在 pattern 结构匹配成功后执行。
- guard 只允许读取当前分支绑定和外部可读值；不能移动出只读绑定。
- 带 guard 的分支不参与穷尽证明，因为 guard 可能为 false。
- 不带 guard 的后续分支仍可覆盖 guard 失败的情况。

`val` 解构只允许不可失败 pattern：

```zn
val Point { x, y } = point;       // ok
val (left, right) = pair;         // ok
val Some(value) = maybe;          // error: refutable pattern in val
```

不可失败 pattern 包括单个绑定、通配、tuple 的不可失败子 pattern、struct 的字段 pattern，以及对当前类型唯一可能 variant 的 pattern。普通多 variant enum 的 variant pattern 是可失败 pattern，必须写 `match`、`if pattern = expr` 或 `while pattern = expr`。

穷尽和不可达：

- `match` 必须穷尽。`_` 可以作为兜底。
- `Bool`、enum variant、tuple / struct 中的嵌套 enum、字面量和整数范围都参与覆盖分析。
- v1 可以对复杂整数覆盖保守处理：如果编译器不能证明范围全集已覆盖，应要求 `_` 或补充分支。
- 明显不可达的无 guard 分支应报错，例如 `_` 后面的分支、完全重复的字面量分支、被前面无 guard or/range 覆盖的分支。
- 带 guard 的分支不让后续同形状分支不可达。

v1 明确不支持这些 pattern：

- `String` / `StringSlice` 内容 pattern。
- regex pattern。
- 用户自定义 extractor / active pattern。
- 类型 downcast pattern。
- collection pattern，例如 `[head, ..tail]`。
- 会调用用户代码的 pattern。

这些能力可能隐藏比较、分配、动态派发、反射或临时 slice。以后若引入，必须通过显式成本 API，而不是普通 pattern。

## 16. 接口与实现

接口定义能力契约。实现是显式声明。

```zn
interface Writer {
    fn write(mut self, bytes: ArraySlice<U8>) -> Result<USize, IoError>;
}

impl Writer for File {
    fn write(mut self, bytes: ArraySlice<U8>) -> Result<USize, IoError> {
        return os.writeFile(self, bytes);
    }
}
```

接收者模式：

- `self`：只读访问当前对象。
- `mut self`：唯一可写访问当前对象。
- `move self`：消耗当前对象。

方法接收者和普通参数使用同一套访问模型，但方法调用语法更像面向对象：

- 调用 `self` 方法不移动接收者。
- 调用 `mut self` 方法不移动接收者，但已有命名接收者必须写成 `mut receiver.method(...)`，且接收者必须是可写位置，例如 `var` 绑定、可写字段、可写元素、`move` 参数或方法体内的 `move self`。
- 调用 `move self` 方法接收整个对象所有权；已有命名接收者必须写成 `move receiver.method(...)`，调用后原接收者不可再用。

```zn
struct Packet {
    bytes: Array<U8>,
}

impl Packet {
    fn len(self) -> USize {
        return self.bytes.len;
    }

    fn clear(mut self) {
        fill(mut self.bytes, 0);
    }

    fn intoBytes(move self) -> Array<U8> {
        return self.bytes;
    }
}

var packet = makePacket();
val n = packet.len();           // 只读；packet 仍可用
mut packet.clear();             // 可写；packet 仍可用
val bytes = move packet.intoBytes(); // 消耗；packet 不可再用
```

`mut self` 方法必须显式写 `mut`，且不能在不可写接收者上调用：

```zn
val packet = makePacket();
mut packet.clear(); // error: mut self method requires writable receiver
```

`Self` 表示当前实现类型：

- 在 `impl File` 中，`Self` 等价于 `File`。
- 在 `impl Writer for File` 中，`Self` 也等价于 `File`。
- 在 `interface Writer` 中，`Self` 表示“实现该接口的具体类型”，适合返回同类值或表达同类参数。

泛型调用默认静态派发：

```zn
fn writeAll<W: Writer>(mut w: W, bytes: ArraySlice<U8>) -> Result<Unit, IoError> {
    var rest = bytes;
    while rest.len > 0 {
        val n = try mut w.write(rest);
        rest = rest.dropPrefix(n);
    }
    return Ok(());
}
```

接口本身不是普通存储类型。`Writer` 单独表示“能力名”，可以在函数参数和返回类型位置表示静态接口泛型；不能作为字段、集合元素、`static` 或其他长期存储类型。

接口名可以出现在五类位置：

- 函数参数：`mut writer: Writer`，表示匿名静态泛型参数，默认单态化和静态派发。
- 函数返回：`-> Writer`，表示静态接口返回；具体返回类型由函数实现决定，不能隐藏分配或动态派发。
- 泛型约束：`W: Writer`，表示编译期已知具体类型，默认单态化和静态派发。
- 接口组合：`interface ThreadWriter: Writer, Send {}`，表示命名能力集合。
- 显式拥有者：`Box<Writer>`，表示拥有一个运行期异构实现；高级共享场景可以使用 `Shared<Writer>`。

参数位置的接口名是语法糖：

```zn
fn writeAny(mut w: Writer, bytes: ArraySlice<U8>) -> Result<USize, IoError> {
    return mut w.write(bytes);
}
```

它等价于编译器内部生成一个隐藏类型参数：

```zn
fn writeAny<W: Writer>(mut w: W, bytes: ArraySlice<U8>) -> Result<USize, IoError> {
    return mut w.write(bytes);
}
```

规则：

- `writer: Writer`、`mut writer: Writer`、`move writer: Writer` 都是静态接口参数。
- 每个裸接口参数默认是独立隐藏类型参数；两个参数都写 `Writer` 时，它们可以是不同具体类型。
- 需要表达多个位置必须是同一个具体类型时，才写显式泛型名，例如 `W: Writer`。
- 静态接口参数不生成接口表、不装箱、不分配，调用点按具体类型单态化。

示例：

```zn
fn copy(mut dst: Writer, src: Reader) -> Result<Unit, IoError> {
    // dst 和 src 都是匿名静态接口参数，可以是不同具体类型
    ...
}

fn tee<W: Writer>(mut left: W, mut right: W, bytes: ArraySlice<U8>) -> Result<Unit, IoError> {
    // 显式 W 表示 left 和 right 必须是同一个具体实现类型
    try mut left.write(bytes);
    try mut right.write(bytes);
    return Ok(());
}
```

泛型接口同样适用这套规则：

```zn
interface Consumer<T> {
    fn consume(mut self, move item: T);
}

fn feed<T>(mut consumer: Consumer<T>, move item: T) {
    consumer.consume(move item);
}
```

`consumer: Consumer<T>` 是匿名静态接口参数，内部等价于隐藏的 `C: Consumer<T>`；调用方只写普通调用，编译器从实参推断具体实现类型。

```zn
var logger = EventLogger.new();
feed(mut logger, move event);
```

同一个具体类型可以实现同一个泛型接口的不同类型实参，只要实现实例不重叠：

```zn
impl Consumer<Event> for MultiConsumer { ... }
impl Consumer<Message> for MultiConsumer { ... } // ok: 类型实参不同

impl Consumer<Event> for MultiConsumer { ... } // error: duplicate impl instance
```

接口实现的 coherence key 是 `(实现类型, 接口名, 接口类型实参)`。同一个 package 或依赖图中不能出现两个相同 key 的实现；带泛型参数的实现还必须经过重叠检查，不能让某个具体实例同时匹配两个实现。

### 16.1 静态接口参数推断

函数签名先被展开成显式泛型约束。每个裸接口参数都会引入一个新的隐藏具体类型参数：

```zn
fn feed<T>(mut consumer: Consumer<T>, move item: T)
```

等价于：

```zn
fn feed<T, C: Consumer<T>>(mut consumer: C, move item: T)
```

两个裸接口参数默认互相独立：

```zn
fn pipe(input: Reader, mut output: Writer) { ... }
```

等价于：

```zn
fn pipe<R: Reader, W: Writer>(input: R, mut output: W) { ... }
```

如果多个位置必须是同一个具体实现类型，必须写显式泛型名：

```zn
fn tee<W: Writer>(mut left: W, mut right: W, bytes: ArraySlice<U8>) { ... }
```

调用点推断按以下顺序执行：

1. 先根据非接口参数和显式类型实参推断普通类型参数，例如 `item: T`。
2. 再根据接口参数的实参具体类型查找接口实现，例如 `EventLogger: Consumer<Event>` 可以推出 `T = Event`。
3. 普通参数和接口实现同时给出的约束必须一致。
4. 如果一个类型实现了多个可能匹配的泛型接口实例，且其他参数不能唯一决定类型参数，则调用不明确，必须由调用方补类型标注或显式类型实参。
5. 推断不能使用返回类型做重载选择；返回位置只能作为赋值或初始化后的结果检查。

示例：

```zn
fn feed<T>(mut consumer: Consumer<T>, move item: T) { ... }

val event = Event { id: 1 };
var logger = EventLogger.new();
feed(mut logger, event); // T = Event, hidden C = EventLogger
```

如果只有接口参数，也可以从唯一接口实现推断：

```zn
fn touch<T>(consumer: Consumer<T>) {}

touch(logger); // 如果 EventLogger 只实现 Consumer<Event>，则 T = Event
```

不明确时必须报错：

```zn
struct MultiConsumer {}

impl Consumer<Event> for MultiConsumer { ... }
impl Consumer<Message> for MultiConsumer { ... }

fn touch<T>(consumer: Consumer<T>) { ... }

touch(MultiConsumer {}); // error: T is ambiguous
```

返回位置的接口名表示静态接口返回：

```zn
fn makeWriter() -> Writer {
    return FileWriter.openDefault();
}
```

这等价于函数拥有一个编译器推断的隐藏返回类型 `R: Writer`。所有正常 `return` 路径必须返回同一个具体类型表达式；如果不同分支要返回不同实现，必须显式使用 `Box<Writer>`：

```zn
fn bad(flag: Bool) -> Writer {
    if flag {
        return FileWriter.openDefault();
    }
    return MemoryWriter.new(); // error: static interface return must have one concrete type
}

fn ok(flag: Bool) -> Box<Writer> {
    if flag {
        return Box.new(FileWriter.openDefault());
    }
    return Box.new(MemoryWriter.new());
}
```

静态接口返回也可以来自静态接口参数：

```zn
fn identity(value: Writer) -> Writer {
    return value; // 返回类型和参数的隐藏具体类型相同
}
```

多个独立接口参数不能直接混合作为同一个静态接口返回：

```zn
fn choose(flag: Bool, a: Writer, b: Writer) -> Writer {
    if flag {
        return a;
    }
    return b; // error: a 和 b 是两个独立隐藏具体类型
}

fn chooseSame<W: Writer>(flag: Bool, a: W, b: W) -> Writer {
    if flag {
        return a;
    }
    return b; // ok: a 和 b 明确是同一个具体类型 W
}
```

`pub fn makeWriter() -> Writer` 在包元数据中暴露一个稳定的 opaque return identity，而不是暴露具体类型名。下游可以把返回值当作满足 `Writer` 的静态具体类型使用；编译器仍需要知道布局和 drop 信息以生成代码。修改该函数的隐藏具体返回类型会使依赖该 API 的增量缓存失效。`@export(..., abi: C)` 不允许静态接口返回。

长期保存仍然要写出具体存储形态：

```zn
struct StaticSink<W: Writer> {
    writer: W,              // ok: 保存具体类型，静态派发，零装箱
}

struct DynamicSink {
    writer: Box<Writer>,    // ok: 拥有异构接口值，分配成本显式
}

struct BadSink {
    writer: Writer,         // error: 接口名不能单独作为字段类型
}
```

动态接口对象只在类型中显式写出拥有者时出现，例如 `Box<Writer>`；需要共享拥有时才使用 `Shared<Writer>`。动态接口调用只支持接口中的“动态可派发方法”。规则如下：

- 方法接收者必须是 `self` 或 `mut self`。
- 方法不能有自己的泛型参数。
- 方法签名中不能在非接收者位置使用 `Self`，也就是不能返回 `Self`，不能把 `Self` 作为普通参数类型。
- 方法的参数和返回值 ABI 必须固定，编译器能为接口表生成统一调用入口。
- `move self` 方法需要拥有具体对象，v1 只允许通过静态接口约束调用，不允许通过 `Box<Interface>` / `Shared<Interface>` 动态调用。

这些静态专用方法仍然可以写在接口里。它们表示“所有实现者都必须提供这个能力”，但只能在编译器知道具体类型时调用：

```zn
interface Duplicable {
    fn duplicate(self) -> Self;
}

fn duplicateStatic<T: Duplicable>(value: T) -> T {
    return value.duplicate(); // ok: T 已知，静态派发
}

fn duplicateAny(value: Duplicable) -> Duplicable {
    return value.duplicate(); // ok: 参数和返回都静态化为同一个隐藏具体类型
}

fn duplicateBox(value: Box<Duplicable>) {
    val copy = value.duplicate(); // error: 返回 Self 的方法不能动态派发
}
```

因此 Zeno 不需要 `dyn` 关键字：

- `writer: Writer` 表示匿名静态接口参数，是普通高性能写法。
- `-> Writer` 表示静态接口返回，不分配、不装箱。
- `W: Writer` 表示命名静态接口约束，用于多个位置共享同一个具体类型或结构体字段保存具体实现。
- `Box<Writer>` 表示拥有一个异构接口值，分配和接口表成本由类型名显式暴露。
- `Shared<Writer>` 是 `Shared<T>` 的高级用法，表示共享拥有一个异构接口值；引用计数成本由 `Shared` 显式暴露。它不是第三种接口语义。
- `interface ThreadWriter: Writer, Send {}` 表示命名接口能力组合。构造或转换到该类型时，具体类型必须同时实现 `Writer` 和 `Send`。

接口对象的推荐降低模型如下。它不是稳定外部二进制 ABI；稳定 ABI 在 v1 之后单独设计。但 stage0 和自举编译器必须满足这些语义和成本约束。

动态接口拥有者内部携带类似这样的胖指针：

```text
InterfaceObject<Writer> {
    data: pointer to concrete object,
    table: pointer to interface table for (ConcreteType, Writer),
}
```

接口表由编译器为每个 `(具体类型, 接口)` 组合生成，至少包含：

- 具体值的 `size` 和 `align`。
- 具体值的销毁入口，用于运行字段销毁和自定义 `destroy`。
- 该接口所有动态可派发方法的函数入口，按接口声明的规范顺序排列。

静态专用方法不进入接口表，例如返回 `Self`、普通参数中使用 `Self`、带方法级泛型参数、或 `move self` 的方法。

通过 `Box<Writer>` / `Shared<Writer>` 的动态方法调用降低为一次接口表间接调用。`self` 方法接收只读对象地址；`mut self` 方法接收唯一可写对象地址。用户代码不能取得这些裸地址；它们只是编译器内部 ABI。

接口能力组合不需要额外堆分配。`Box<ThreadWriter>` 和 `Box<Writer>` 的拥有成本相同；区别是前者在类型系统中保留了“底层具体类型可跨线程移动”的证明。`Send`、`Sync` 这类标记接口没有动态方法入口，不增加接口调用成本。

`Box<Writer>` 是拥有式接口对象：

- 它拥有一个由创建 API 绑定的 allocator 分配的具体对象。
- 句柄至少携带 `data` 指针和 `table` 指针；释放所需的 allocator 状态也必须可获得。零大小 allocator、默认 allocator 或可静态证明的 allocator 可以被优化掉。
- 从具体值构造 `Box<Writer>` 时，只分配一次，分配内容是具体类型本身，不额外分配接口包装对象。
- `Box<T>` 在 `T: Writer` 时可以移动转换成 `Box<Writer>`；该转换只增加或替换接口表元数据，不重新分配、不复制、不移动堆内具体值。
- 销毁 `Box<Writer>` 时，先通过接口表销毁具体对象，再用接口表中的布局信息和创建时绑定的 allocator 释放内存。
- 对 `Box<Writer>` 调用接口方法会通过接口表动态派发；调用 `mut self` 方法要求 `Box` 本身处于唯一可写访问中。

`Shared<Writer>` 是共享拥有式接口对象：

- 它拥有一个带引用计数控制块的具体对象。
- 句柄携带对象地址、接口表地址和释放所需的 allocator 状态；实现可以把控制块放在对象前方，并用接口表布局信息计算释放布局。
- `Shared<T>` 在 `T: Writer` 时可以移动转换成 `Shared<Writer>`；该转换不增加引用计数、不重新分配、不复制具体值。
- `Shared<Writer>.clone()` 只增加引用计数，不复制具体值。
- 最后一个 `Shared<Writer>` 销毁时，通过接口表销毁具体对象，再释放整个控制块。
- `Shared<Writer>` 默认只能调用 `self` 接口方法。需要共享可变状态时，必须显式使用 `Shared<Mutex<T>>`、原子类型或其他同步容器。

Zeno v1 不提供从 `Box<Writer>` 或 `Shared<Writer>` 向具体类型的通用 downcast。需要这种能力时，应在接口中显式设计查询方法，或推迟到反射/类型标识设计完成后再引入。

## 17. 泛型

泛型函数和类型默认单态化。

```zn
interface SortKey: Ord, Copy {}

fn max<T: SortKey>(a: T, b: T) -> T {
    if a < b { return b; }
    return a;
}

interface HashKey: Hash, Eq {}

struct Map<K: HashKey, V> {
    // K 是 key 类型，要求 HashKey。
    // V 是 value 类型，没有额外约束。
}

struct RingBuffer<T, const Capacity: USize> {
    data: Array<T>,
    head: USize,
    len: USize,
}
```

规则：

- 类型参数可以受一个命名接口约束，例如 `T: Writer`。
- 泛型参数也可以是常量参数，例如 `const Capacity: USize`。
- 常量泛型实参必须在编译期求值，并进入单态化 key、layout key、ABI fingerprint 和增量缓存 key。
- 常量泛型参数的身份必须可稳定 fingerprint：整数、`Bool`、`Char`、无载荷 enum variant，或由这些值组成的 tuple / 纯 `Copy` struct。任意复杂 CTFE 可以先运行，但最终作为类型身份的结果必须稳定。
- 一个泛型参数位置只允许一个直接约束。需要多个能力时，先定义命名组合接口，例如 `interface SortKey: Ord, Copy {}`，再写 `T: SortKey`。
- 泛型参数列表中的逗号只分隔泛型参数，不分隔约束。Zeno 不提供 `T: Ord, Copy`、`T: Ord + Copy` 或 `where` 约束语法。
- 多个泛型参数用于表达多个未知类型，例如 `Result<T, E>`、`Map<K, V>` 或 `Fn<A, B>`。
- 泛型代码只能使用约束保证的操作。
- 单态化是默认代码生成策略。
- 只有在编译器证明行为和可观察性能没有差异时，才允许共享泛型代码。接口约束仍然是静态约束，不能退化成接口表派发。

## 18. 闭包

闭包是值，捕获方式由使用方式推导。

```zn
val factor = 3;
val scale = (x: I32) => x * factor;
val scaleBlock = (x: I32) -> I32 {
    return x * factor;
};
```

闭包语法：

- 块闭包：`(params) -> ReturnType { ... }`。
- 返回类型可由上下文推断时，可以写成 `(params) { ... }`。
- 短表达式闭包：`(params) => expr`，等价于返回 `expr` 的非逃逸小闭包。
- 零参数闭包写成 `()`，例如 `() => Error.Missing`。
- `move` 写在闭包前，表示按值捕获需要转移所有权的捕获值。

捕获模式：

- `Copy` 捕获可以复制。
- 非 `Copy` 捕获需要 `move`。
- 可变捕获需要闭包生命周期内的唯一访问。
- 非逃逸闭包不得分配。
- 逃逸闭包必须使用显式拥有类型，例如 `Box<Fn<...>>`、`Box<MutFn<...>>`、`Box<OnceFn<...>>` 或任务类型。

调用能力：

- `Fn<A..., R>`：闭包只读访问捕获环境，可通过只读访问调用多次。
- `MutFn<A..., R>`：闭包可以修改捕获环境，调用时需要对闭包值有 `mut` 访问，可调用多次。
- `OnceFn<A..., R>`：闭包可以消耗捕获环境，调用时取得闭包所有权，只能调用一次。
- 只读闭包能力最强：`Fn` 可以传给需要 `MutFn` 或 `OnceFn` 的 API。
- `MutFn` 可以传给需要 `OnceFn` 的 API。
- `OnceFn` 不能传给需要多次调用的 API。
- 如果闭包体从非 `Copy` 捕获中移动出值，该闭包只能实现 `OnceFn`。
- 如果闭包体修改捕获值但不消耗它，该闭包实现 `MutFn`，不能当作 `Fn` 使用。

```zn
val task = move () -> Result<Unit, Error> {
    return move worker.run();
};
val handle = try Thread.spawn(move task);
```

## 19. 错误

可恢复错误使用 `Result<T, E>`。

```zn
fn load(path: StringSlice) -> Result<String, IoError> {
    val file = try File.open(path);
    return file.readToString();
}
```

`try expr` 会解开成功分支，或者从当前函数提前返回失败分支。它只能用于当前函数返回类型匹配的 `Result` 或 `Option` 上。

`try` 的完整规则：

- 如果 `expr` 的类型是 `Result<T, E>`，当前函数必须返回 `Result<U, E>`。
- `Ok(value)` 解开成 `value`。
- `Err(error)` 直接从当前函数返回 `Err(error)`。
- 如果 `expr` 的类型是 `Option<T>`，当前函数必须返回 `Option<U>`。
- `Some(value)` 解开成 `value`。
- `None` 直接从当前函数返回 `None`。
- Zeno 不做隐式 `Option<T>` 到 `Result<T, E>` 转换。
- Zeno 不做隐式错误类型转换。
- `try` 触发提前返回时，会按普通作用域退出路径执行 RAII 销毁。

需要把 `Option<T>` 接入 `Result<T, E>` 错误流时，必须显式选择错误：

```zn
fn parseByte(value: U32) -> Result<U8, ParseError> {
    val byte = try U8.fromChecked(value).okOr(ParseError.OutOfRange);
    return Ok(byte);
}
```

如果错误构造有成本，应使用 lazy 形式。`okOrElse` 的闭包只在 `None` 路径运行，成功路径不会构造错误：

```zn
fn lookupUser(id: U64) -> Result<User, LookupError> {
    val user = try cache.find(id).okOrElse(() => LookupError.Missing(id));
    return Ok(user);
}
```

需要转换错误类型时，必须显式写出转换：

```zn
fn readConfig(path: StringSlice) -> Result<String, ConfigError> {
    val text = try fs.readFile(path).mapErr(ConfigError.Io);
    return Ok(text);
}
```

`mapErr` 的转换函数只在 `Err` 路径运行。`unwrapOrElse` 的 fallback 闭包只在 `None` 路径运行。传给这些 API 的非逃逸闭包不得分配，除非闭包体本身显式使用分配 API。

`okOrElse`、`unwrapOrElse` 和 `mapErr` 接收 `OnceFn`，因为它们最多调用闭包一次。闭包可以移动捕获资源；如果成功路径不需要该闭包，捕获资源会按普通所有权规则销毁。

`try` 不分配、不使用异常，也不要求 unwinding。编译器应把它降低为普通分支和清理边。

`panic` 表示程序不变量被破坏，不是 `Result` 的替代品。具体策略见下一节。

## 20. Panic、OOM 与 profile 失败策略

Zeno 把普通错误、程序 bug 和资源耗尽分开：

- 可恢复业务错误使用 `Result<T, E>`。
- 可选值缺失使用 `Option<T>`。
- 程序不变量被破坏使用 `panic(message)`。
- 默认分配 API 或 `In` 分配 API 遇到内存耗尽时使用 `oom(layout)`。

可失败 API 规则：

- 普通拥有值构造不因为分配失败返回 `Result`。例如 `String.from(text)`、`Vector.withCapacity(n)`、`Map.withCapacity(n)`、`Box.new(value)` 和 `Shared.new(value)` 失败时进入当前 profile 的 OOM 策略。
- 指定 allocator 的普通拥有值构造同样不返回 `Result`。例如 `String.fromIn(text, mut allocator)` 和 `Vector.withCapacityIn(n, mut allocator)` 失败时进入 OOM 策略；如果 allocator 是 scoped allocator，逃逸检查由类型系统处理。
- 可恢复 OOM 只通过显式容量预留入口表达，例如 `tryReserve` / `tryReserveExact`。这些 API 返回 `Result<Unit, AllocError>`，成功后为后续容量内操作提供优化事实。
- 系统资源获取、I/O、线程、运行时创建、锁获取、FFI 包装和业务校验可以返回 `Result`，调用方用 `try` 传播。例如 `File.open(path)`、`Thread.spawn(move job)` 和 `Runtime.withWorkerCount(n)` 不是普通内存构造，它们可能因为 OS 或 profile 条件失败。
- 不引入 `tryNew`、`tryFrom`、`tryClone`、`tryPush` 这类成套 fallible 构造 API。需要可恢复分配时，先显式 `tryReserve`；需要可恢复业务或系统错误时，直接让该操作返回 `Result`。
- `Result` 表示调用方应当能处理的失败；OOM 策略表示当前 profile 选择的资源耗尽终止路径。二者不能混用来隐藏成本或规避 `@noAlloc` / `@noPanic` 检查。

核心声明形状：

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

用户代码仍然只调用 `panic(message)`。编译器把它降低为带调用点信息的 profile `panicHandler(info)` 调用，其中 `PanicInfo`、`StackFrames` 和相关诊断访问值只能在 panic handler 的调用链中使用，不能被保存到结构体、集合、闭包、线程或 `static` 中。

`panic` 和 `oom` 都由 profile 提供具体实现。它们返回 `Never`，因此可以出现在任何返回类型的位置：

```zn
fn requirePositive(value: I32) -> I32 {
    if value > 0 {
        return value;
    }

    panic("expected positive value");
}
```

`PanicInfo.stack()` 返回惰性的 `StackFrames` 迭代器。profile 可以在 handler 中输出调用栈：

```zn
fn panicHandler(info: PanicInfo) -> Never {
    log(info.message());
    log(info.location().file());

    var frames = info.stack();
    for move frame in frames {
        logAddress(frame.instructionPointer());
    }

    abort();
}
```

调用栈规则：

- `panic(message)` 自动携带消息和调用点位置，普通用户不需要手动传文件名、行号或函数名。
- 调用栈采集只发生在 panic 冷路径；正常路径不得为构造完整栈信息做堆分配或运行时登记。
- `StackFrames` 必须支持无分配遍历。Hosted debug profile 可以用 frame pointer、unwind table 或平台 backtrace API 生成帧；release profile 可以只给地址、截断帧，或按配置关闭栈。
- Freestanding / kernel profile 可以只输出指令地址，或在目标不支持时返回空栈；语言不强制引入 unwinder。
- 符号化是 profile 能力，不是语言语义要求。没有符号表时，handler 仍可输出 instruction pointer。

OOM 规则：

- `Array.filled`、`Vector.withCapacity`、`Map.withCapacity`、`Set.withCapacity`、`String.from`、`Box.new`、`Shared.new`、普通 `push`、`insert`、普通 `clone` 以及对应 `In` 后缀 API 分配失败时调用 `oom(layout)`。
- `oom(layout)` 不返回；调用点不需要写 `try`。
- `Vector.tryReserve`、`Vector.tryReserveExact`、`Map.tryReserve`、`Map.tryReserveExact`、`Set.tryReserve`、`Set.tryReserveExact`、`String.tryReserve` 和 `String.tryReserveExact` 是 v1 标准库提供的可恢复 OOM 入口，失败时返回 `Err(AllocError)`，不会调用 `oom`。
- 低层 allocator 的 `allocate(layout) -> Result<Allocation, AllocError>` 仍然返回 `Result`，用于实现集合、arena、驱动和平台绑定。

profile 默认策略由 manifest 决定：

- Hosted profile：`panic` 默认 abort，可配置为 unwind；`oom` 默认 abort，可配置为调用 `panic` 或目标自定义 handler。
- Freestanding profile：`panic` 和 `oom` 默认目标 trap 或 abort；不要求 unwind，也不要求默认 allocator。
- Kernel / embedded profile：必须显式声明 `panicHandler` 和 `oomHandler`，二者都必须返回 `Never`，常见实现是记录诊断后 halt、reset 或目标 trap。

销毁语义：

- `try` 不是异常，它总是按普通提前返回路径执行 RAII 销毁。
- 如果 profile 把 `panic` 配置为 unwind，栈展开经过的作用域必须执行 RAII 销毁。
- `destroy` 隐式 `@noPanic` / `@noAlloc`，因此 unwind cleanup 中的自定义销毁体不能引入新的 panic、OOM 或分配路径。
- 如果 profile 把 `panic` 或 `oom` 配置为 abort / trap / halt / reset，不执行栈展开，也不保证运行普通析构。
- `oom` 默认不做 unwind。只有目标明确把 OOM 映射到 `panic` 且启用 unwind 时，才按 panic-unwind 规则清理。

属性关系：

- `@noPanic` 拒绝直接或间接 `panic`，也拒绝当前 profile 中会降低成 `panic` 的运行时检查。
- 若当前 profile 把 OOM 配置成 `panic`，`@noPanic` 中调用可能分配的 API 必须被拒绝，除非编译器能证明不会分配失败。
- `@noAlloc` 拒绝堆分配，因此同时排除由这些分配触发的 OOM 路径。
- 需要硬实时或内核热路径时，推荐同时使用 `@noAlloc` 和 `@noPanic`，并避免目标 trap 的未证明运行时检查。

stage0 冻结：

- `panic(message)` 和 `oom(layout)` 都是 `Never` 终点，MIR 中必须是显式终止控制流。
- `try` 只降低为普通分支和 cleanup edge，不使用异常。
- 默认分配 API 和 `In` 后缀分配 API 失败时调用 `oom(layout)`，不返回 `Result`。
- 可恢复 OOM 只通过 `tryReserve*` 和低层 allocator `allocate(...) -> Result<Allocation, AllocError>` 表达。
- Hosted stage0 默认 `panic.strategy = "abort"`、`oom.strategy = "abort"`、`panic.stack = "addresses"` 或 `"symbols"` 由 debug 配置选择。
- Freestanding stage0 默认 `panic.strategy = "trap"`、`oom.strategy = "trap"`、`panic.stack = "none"`。
- Kernel / embedded stage0 必须要求 `panic.handler` 和 `oom.handler`，二者返回 `Never`。
- stage0 可以先不实现 panic unwind；若 manifest 请求 `panic.strategy = "unwind"`，编译器应给 staged diagnostic，直到 unwind cleanup 通过 MIR verifier。
- `PanicInfo.stack()` 第一版至少支持无分配地址遍历；符号化是 hosted debug profile 能力，不是语言语义依赖。
- `@noPanic` 和 `@noAlloc` 第一版可以采用保守可达调用图检查；无法证明无 panic / 无分配时拒绝，而不是放行。

## 21. 并发

Zeno 的普通代码目标是防止数据竞争。

执行模型分层：

- `std.thread.Thread`：OS 线程，最底层且显式。
- `std.task.Runtime`：可选任务调度器或线程池。
- `core.async.Future`：编译器生成的状态机，没有全局 executor。

Zeno 默认没有虚拟线程，也不要求全局调度器。Freestanding 程序可以完全不使用并发运行时。

所有权转移规则：

- 把拥有值移入线程、任务或 future 会转移所有权。
- 跨线程移动要求被移动值是 `Send`。
- 跨线程共享只读访问要求被共享值是 `Sync`。
- 共享可变数据必须经过 `Mutex<T>`、原子类型或其他同步类型。
- 可写访问不能跨越非结构化并发边界，除非同步或 scoped API 能证明 join-before-exit。
- `Thread.spawn` 消费一个 `OnceFn<T>` 任务闭包；任务最多执行一次，因此允许闭包移动捕获值。
- scoped 并发允许短期访问进入子任务，但可写切分必须来自编译器认可的 disjoint API，例如 `splitDisjoint`、`splitAt` 或等价库契约；不能靠任意运行期索引猜测不重叠。

`Send` 和 `Sync` 是编译器认识的标记接口：

- `Send`：值的所有权可以跨线程、任务或可跨线程 future 边界移动。
- `Sync`：同一个值的只读共享访问可以被多个线程同时使用。
- 二者都不是锁，不提供同步；共享可变状态仍然必须写 `Mutex<T>`、原子类型或同步容器。

自动推导规则是保守的：

- 标量基础类型、`Bool`、`Char`、`Unit`、`Never` 自动是 `Send` 和 `Sync`。
- `String`、`Array<T>`、`Vector<T>`、`Option<T>`、`Result<T, E>`、元组和普通 enum 按字段或载荷结构推导。
- 普通 `struct` 在没有自定义 `destroy` 块、没有底层信任字段、且所有字段满足对应接口时，自动获得 `Send` / `Sync`。
- 泛型类型的推导变成条件约束，例如 `struct Pair<T, U>` 只有在 `T: Send` 且 `U: Send` 时才是 `Send`。
- 带自定义 `destroy` 块的类型不自动获得 `Send` 或 `Sync`，因为析构可能有线程亲和性；需要作者用 `trust impl` 明确承担不变量。
- `ArraySlice<T>` 和其他非拥有访问值不能跨非 scoped 并发边界。接口约束按具体类型处理，是否可跨边界由具体类型的 `Send` / `Sync` 约束决定。

核心库类型的规则：

- `Box<T>` 是 `Send` 当且仅当 `T: Send` 且创建时绑定的 allocator 可跨线程释放；是 `Sync` 当且仅当 `T: Sync` 且 allocator 可跨线程共享释放元数据。
- `Shared<T>` 是 `Send` 和 `Sync` 当且仅当 `T: Send, Sync` 且创建时绑定的 allocator 可跨线程释放。最后一次释放和销毁可能发生在任意持有者线程，所以 `T` 必须是 `Send`；多个线程可同时只读访问，所以 `T` 也必须是 `Sync`。
- `Mutex<T>` 是 `Send` 和 `Sync` 当且仅当 `T: Send`；锁把共享可变访问转化为受 guard 保护的唯一访问。
- `MutexGuard<T>` 在 v1 中不是 `Send`，不能跨线程、任务或 future 边界移动。
- `Atomic<T>` 对受支持的原子载荷类型自动是 `Send` 和 `Sync`。

接口拥有者不会凭空保留线程安全能力：

- `Box<Writer>` 不一定是 `Send`，因为它可以保存任意 `Writer` 实现。
- 需要跨线程移动的接口拥有者必须使用包含 `Send` 的命名接口，例如 `Box<ThreadWriter>`。
- 需要跨线程共享的接口拥有者必须使用包含 `Send` 和 `Sync` 的命名接口，例如 `Shared<SharedWriter>`。

手动声明 `Send` 或 `Sync` 必须写在 `trust impl` 中：

```zn
struct OsHandle {
    raw: I32,
}

impl OsHandle {
    destroy {
        closeBestEffort(self.raw);
    }
}

trust impl Send for OsHandle {}
```

普通 `impl Send for OsHandle {}` 会被拒绝。`trust impl` 不关闭类型检查、初始化检查或 move 检查，但它声明作者已经审计析构、别名、底层句柄和平台线程安全不变量。

```zn
val data = buildData();
val handle = try Thread.spawn(move () {
    process(move data);
});
try move handle.join();
```

任务运行时是显式值：

```zn
async fn runTask(move runtime: Runtime, move data: Data) -> Result<Unit, WorkError> {
    val task = try runtime.spawn({
        return process(move data);
    });

    try await task;
    return Ok(());
}
```

长期阻塞的同步工作必须显式进入 blocking pool：

```zn
async fn loadConfig(move runtime: Runtime, move path: Path) -> Result<Bytes, IoError> {
    val task = try runtime.spawnBlocking(move () -> Result<Bytes, IoError> {
        return readFileSync(move path);
    });

    return try await task;
}
```

`async fn`、`async { ... }` / `async move { ... }` block，以及 `Future<T>` 参数位置的 Future block 实参都会编译成 `Future` 状态机。创建 future 不会启动虚拟线程；执行它需要程序选择 executor 或 runtime。

future 取消和销毁规则：

- 丢弃未完成 future 表示取消该计算。
- future drop 必须根据当前状态销毁所有已经初始化且仍拥有的字段。
- 跨 `await` 存活的非 `Copy` 值必须由 future 拥有；短期只读访问、`mut` 访问和视图值不能作为独立值跨 `await` 保存。
- `await future` 会消费 `Future<T>` 拥有者并返回其输出；等待后原 future 绑定不可再用。
- `await mut receiver.method(...)` 只在“立即 await”的 async `mut self` 调用中允许，且 `receiver` 必须由当前 future 拥有。编译器把拥有者保存在 future 状态中，把可写访问限制在被等待调用内部；这个调用产生的 future 不能绑定到变量、返回、放入结构体、传给 `spawn` 或跨另一个 `await` 逃逸。
- `await task` 是语言操作，不是 `Task.await()` 方法。它消费 `Task<T>` 句柄并返回任务结果；等待后原 task 绑定不可再用。同步阻塞等待必须由显式 blocking API 表达，不能伪装成普通方法调用。
- 同步代码等待 future 或 task 必须使用显式 executor/runtime，例如 `mut runtime.blockOn(move task)` 或 `mut executor.blockOn(move future)`。`blockOn` 是阻塞当前线程的 `mut self` API，只能出现在同步上下文；async 上下文中必须使用 `await`。
- `Runtime.spawn(future)` 是日常异步入口。它接收并消费 `Future<T>`，返回 `Task<T>`；临时 async 调用和 `spawn({ ... })` Future block 实参可以直接传入，已有命名 future 必须写 `move future`。
- `runtime.spawn({ ... })` 和 `mut group.spawn({ ... })` 是 Future block 实参。它们保留普通函数调用括号，但用户不需要写 `async` 或捕获 `move`。`Copy` 捕获复制，非 `Copy` owner 自动移动进 future；捕获后外层继续使用该 owner 是编译错误。
- `Runtime.spawnWithContext((ctx) { ... })` 使用同一条 Future block 规则；闭包只负责接收 `TaskContext`，闭包体 lowered 为 future。
- Future block 实参不会自动 clone、不会自动引用计数、不会自动装箱。需要共享时必须显式写 `Shared`、`Mutex`、原子类型或 `clone()`。
- 普通 `Runtime.spawn` 不接收同步任务闭包。同步阻塞工作使用 `spawnBlocking`，需要底层 OS 线程时使用 `Thread.spawn`。
- `Runtime.spawnWithContext(makeFuture)` 是高级取消入口，接收 `OnceFn<TaskContext, Future<T>>`。闭包只在启动时调用一次，runtime 保存它返回的 future，不保存闭包本身。
- `Runtime.spawnBlocking(move job)` 用于同步文件 I/O、DNS、压缩、加密、阻塞 C API 和长时间不让出 worker 的 CPU 工作。它返回普通 `Task<T>`，但创建点必须写出 `spawnBlocking`。blocking pool 必须与普通 worker pool 分离且有上限；上限、队列策略和饱和行为必须由 runtime 构造参数或 profile 文档暴露，实现不能偷偷为每个 blocking job 创建无界 OS 线程。
- `Runtime.spawnBlockingWithContext(move job)` 是阻塞任务的显式取消检查入口，接收 `OnceFn<TaskContext, T>`。
- `TaskGroup<T>` 是结构化并发拥有者。它通过 `spawn(future)` 启动一组同类型 future，通过 `joinAll` / `tryJoinAll` 消费 group 并统一收尾；流式完成处理使用 `next` / `tryNext`。
- `TaskGroup.joinAll` / `tryJoinAll` 默认按完成顺序返回，这是最低额外协调成本的路径；需要 spawn 顺序时显式使用 `joinAllOrdered` / `tryJoinAllOrdered`。
- `TaskGroup.next` / `tryNext` 返回 `None` 后，group 进入 drained 状态。`tryNext` / `tryJoinAll` 的 `Err` 路径必须请求取消剩余任务并把 group 标记为 settled。
- `TaskGroup<T>` 不能隐式 drop。离开作用域前必须被 `joinAll`、`tryJoinAll`、`cancelRemaining` 消费，或者被 `next` / `tryNext` 完整 drain 到 `None`。
- `Task<T>` 是必须显式收尾的资源。任务句柄可以被移动或返回给调用方，但不能在作用域末尾隐式 drop；否则是编译错误。
- `Task<T>` 的合法收尾方式包括 `await task`、`mut runtime.blockOn(move task)` / `mut executor.blockOn(move task)`、`move task.cancel()` 和 `move task.detach()`。
- `Task.cancel(move self) -> TaskCancelStatus` 消费任务句柄，请求协作式取消并丢弃输出。`TaskCancelStatus` 包含 `QueuedCancelled`、`Requested` 和 `AlreadyFinished`：尚未开始的任务可以被跳过并销毁捕获状态；已经开始的任务只能在 poll / yield 边界或显式取消检查处观察请求；已经完成的任务只丢弃结果。
- `Task.detach(move self) -> Unit` 消费任务句柄，让任务继续运行并在完成时丢弃输出。`detach` 不取消任务，也不等待任务完成。
- `Task<T>` 的析构不能 `await`、不能阻塞、不能隐式取消，也不能隐式 detach。未收尾任务的错误必须由编译器在普通控制流中发现；析构只作为 profile 诊断或崩溃恢复兜底。
- 需要任务内部感知取消时，源码中必须出现 `TaskContext` 参数；普通 `Runtime.spawn(future)` 没有用户可见上下文，也不承担取消检查成本。
- `TaskContext.isCancellationRequested(self) -> Bool` 是显式取消检查。它只读取当前任务的取消请求状态，不自动返回、不 panic、不改变任务返回类型。
- `TaskContext` 是 task-local 轻量能力值，可以复制并传给当前任务内的 helper 函数，也可以作为当前任务 future 状态的一部分跨 `await` 存活。它不能返回、不能存入长期拥有者、不能被线程、子任务或其他逃逸闭包捕获。
- 标准库和编译器不得提供隐藏的全局当前任务查询来替代 `TaskContext` 参数；取消检查的能力必须在源码签名中可见。
- 跨 `await` 存活的 RAII guard 和其他拥有资源会进入 future 状态；正常返回、`try` 提前返回、panic-unwind 和 future drop 都必须按状态销毁已经初始化且仍拥有的字段。
- future drop 不能 `await`，因此异步清理必须通过显式 API 完成，不能依赖析构中的异步操作。
- `ArraySlice<T>`、`StringSlice` 和其他非拥有访问值不能作为独立状态字段跨 `await`；如果只在 await 前使用并结束访问，则允许。接口约束按具体类型处理，跨 `await` 时必须由 future 拥有该具体值。
- v1 不需要用户可见 `Pin`。禁止访问值逃逸跨 `await` 后，future 在没有被 runtime 拥有时可以按普通 move 规则移动。
- future 被 `Runtime.spawn` 或任务句柄拥有后，移动的是句柄，不是正在被调度器 poll 的内部状态。

## 22. FFI、硬件与 trust 边界

Zeno 不使用 `unsafe`。当代码需要执行编译器无法独立证明的底层操作时，作者必须写出 `trust` 边界。

`trust` 表示：

- 作者向编译器声明：这里依赖的底层不变量由作者负责保证。
- 编译器仍然执行普通类型检查、所有权检查、初始化检查和可达的访问检查。
- `trust` 只放开裸 FFI、裸内存、裸地址、裸指针、volatile/MMIO、inline asm、链接段和中断入口这类底层能力。
- `trust` 不会让优化器获得额外别名、初始化、非空、对齐、线程安全或生命周期事实；这些事实必须来自类型、参数模式、属性或受信 API 的显式契约。
- 编译器必须记录每个 `trust` 边界的位置、能力类别和公开 API 影响范围，生成信任报告。

底层能力分为固定类别：

- `ffi`：`trust extern` 声明、调用外部 ABI，以及为了立即调用外部 ABI 从已有 slice、handle 或拥有者中取出 ABI 地址。
- `rawMemory`：从整数构造裸地址 / 裸指针、裸指针偏移、解引用、按位重解释、手动对齐声明和 provenance 断言。
- `hardware`：MMIO、端口 I/O、volatile 硬件访问、物理地址映射和链接段控制。
- `inlineAsm`：内联汇编。
- `interrupts`：中断入口、裸调用约定、启动入口和目标特殊入口。

`trust impl Send for T {}` / `trust impl Sync for T {}` 是线程安全信任声明。它不属于上面的底层操作类别，但仍然是信任边界，必须进入信任报告，并受 `allowedPackages`、`dependencyTrust` 和 `requireReport` 这类策略约束。

裸 C ABI 声明必须写成 `trust extern`：

```zn
trust extern "C" fn read(fd: I32, buffer: USize, length: USize) -> ISize;
```

普通 `pub fn` 不是外部 ABI 承诺。导出给 C 或动态库必须显式写 `@export`：

```zn
@export("zeno_add", abi: C)
pub fn add(a: I32, b: I32) -> I32 {
    return a + b;
}
```

`@export(..., abi: C)` 函数必须使用 C-compatible 参数和返回类型，不能是泛型函数，不能是方法，不能暴露非 `pub` 命名结构体类型，也不能让 panic unwind 穿过外部 ABI。完整规则见 [FFI.md](FFI.md)。

普通库想把 Zeno 风格 API 暴露给 C 时，优先使用 bridge 导出：

```zn
@export("zeno_read", bridge: C)
pub fn read(fd: FileDescriptor, mut out: ArraySlice<U8>) -> Result<USize, IoError> {
    return fd.read(mut out);
}
```

`bridge: C` 允许源码签名使用有限的 bridge-compatible 类型，例如 C-compatible 类型、`ArraySlice<T>`、`mut ArraySlice<T>`、`StringSlice`、`Option<T>` 返回和 `Result<T, E>` 返回。编译器生成的 thunk 必须降低成严格 C-compatible 签名，并且转换必须零分配、零复制、零动态派发。`Result<T, E>` bridge 返回要求 `E` 实现 `core.ffi.CErrorCode`，从而把错误明确映射成 C `I32` 状态码；`0` 表示成功，错误码必须非零。

`bridge: C` 仍然不允许把 `String`、`Array<T>`、`Vector<T>`、`Box<T>`、`Shared<T>`、`Mutex<T>`、接口拥有者、闭包或带 `destroy` 的资源类型自动跨 C。拥有资源必须通过 `@layout(C)` handle、`CHandle<T>`、`CBuffer<T>` 或项目自定义 handle 暴露，并提供显式 create / destroy API。

官方工具链提供 `zeno bindgen c` 生成 C 绑定。v1 不实现完整 C++ 绑定，但 bindgen 架构、缓存 key 和 ABI fingerprint 必须预留后续 `zeno bindgen cxx`：C++ 默认通过生成 C shim、opaque handle、显式模板实例化和异常到错误码转换来接入，不把 C++ name mangling、异常 ABI 或复杂类布局直接作为 Zeno 语言承诺。

底层操作必须放在 `trust` 块里：

```zn
fn sysRead(fd: I32, mut out: ArraySlice<U8>) -> Result<USize, IoError> {
    val n = trust {
        read(fd, out.rawAddress(), out.len)
    };

    if n < 0 {
        return Err(IoError.LastOsError);
    }
    return Ok(n as USize);
}
```

`trust extern` 只声明外部符号；调用外部函数仍然必须在 `trust` 块或受信封装内部：

```zn
fn bad() -> I32 {
    return read(0, 0, 0); // error: extern call requires trust
}
```

硬件访问也走同一条规则：

```zn
fn mapUart(base: USize) -> Mmio<UartRegs> {
    return trust {
        Mmio.mapRaw<UartRegs>(base, 4096)
    };
}
```

裸内存操作必须同时满足源码 `trust` 和 manifest 的 `rawMemory` 能力：

```zn
fn readByte(address: USize) -> U8 {
    return trust {
        RawPointer<U8>.fromAddress(address).read()
    };
}
```

`trust` 边界应尽量小。推荐把 `trust` 封装成普通安全 API，对外暴露 `ArraySlice<T>`、`Result<T, E>`、`Handle<T>`、`Mmio<T>`、`Port<T>`、`DmaBuffer` 等有语义的类型。

`Zeno.toml` 的 `trust` 配置提供构建策略：

- 禁止依赖包中的 `trust`。
- 只允许指定包或路径使用 `trust`。
- 要求发布包附带信任报告。
- 在 freestanding / kernel profile 中允许更多底层能力，在 hosted profile 中默认限制 inline asm、裸硬件地址和中断入口。

## 23. 成本可见性

这些操作必须在源码或类型签名中可见：

- 堆分配：`Box`、`Shared`、`String.from`、`String.push`、`String.clone`、`Array`、`Vector`、`Map`、`Set`、`In` 后缀 allocator API 或 `alloc` 函数。
- 引用计数 / 共享所有权：`Shared`。
- 接口派发：显式接口拥有者 `Box<Writer>` / `Shared<Writer>`。`writer: Writer` 和 `W: Writer` 都是静态约束，不属于动态派发成本。
- 锁：`Mutex`、`RwLock`、`Once` 或等价 guard 类型。
- 原子操作：`Atomic<T>`。
- 非 `Copy` 值的所有权转移：被调用函数签名中的 `move` 参数、方法签名中的 `move self`、`match move`、`for move` 或 `move` 闭包捕获。
- 逃逸 callable 分配：显式 `Box<Fn<...>>`、`Box<MutFn<...>>`、`Box<OnceFn<...>>` 或任务类型。
- 底层信任边界：`trust`。
- 任务运行时 / executor：显式 `Runtime`、`Executor`、`TaskGroup` 或等价类型。
- 全局懒初始化和同步全局状态：显式 `Once<T>`、`Mutex<T>`、`Atomic<T>` 或同步容器。
- 反射、类型名、调用栈符号化和高级格式化：显式 API、manifest/profile 能力或可达库模块。

如果库 API 以误导性的名称或返回类型隐藏成本，编译器应该给出警告。

`@noAlloc` 会把可达调用图中的隐藏分配变成硬错误。

## 24. 兼容性与延后内容

v0.1 暂不包含：

- 垃圾回收。
- 异常。
- class 继承。
- 默认虚拟线程。
- 强制 Actor 运行时。
- 宏系统。
- 完整反射。
- 用户自定义生命周期参数。
- 稳定二进制包 ABI。

这些特性未来可以重新讨论，但必须保留 Zeno 的成本和安全契约。
