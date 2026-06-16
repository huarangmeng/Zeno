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
- 普通用户代码没有 `unsafe` 语言模式。
- 裸指针、裸地址、裸 C ABI、inline asm 和硬件访问必须出现在显式 `trust` 边界中。
- 必须能编译 freestanding 程序，不依赖 hosted 标准库。

Zeno 代码应该让资源所有权、分配、同步、接口派发和调度成本在类型或调用位置上可见。

## 2. 程序结构

一个 Zeno 源文件包含可选模块声明、导入和声明。

```zn
module app.main;

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

## 3. 词法规则

长期目标支持 Unicode 源码文本，但 stage0 编译器只需要支持 ASCII 标识符：

```text
identifier = [A-Za-z][A-Za-z0-9]*
```

单独的 `_` 只能作为丢弃模式使用，不是普通标识符。

v0.1 关键字：

```text
as async await break const continue defer destroy else enum extern false
fn for if impl import in interface let match module move mut
pub return self Self static struct true trust try type var while
```

`unsafe` 不是用户语言关键字。符合规范的编译器必须拒绝普通包中的 `unsafe` 块、模式或声明。

关键字职责：

| 关键字 | 作用 |
| --- | --- |
| `as` | 显式类型转换，例如 `byte as U32`。 |
| `async` | 声明返回 `Future` 状态机的异步函数，不启动线程或任务。 |
| `await` | 在 async 上下文中等待 future 继续执行，需要运行时或 executor 驱动。 |
| `break` | 跳出循环，可选携带循环表达式的值。 |
| `const` | 声明编译期常量或常量项。 |
| `continue` | 跳到下一次循环迭代。 |
| `defer` | 注册当前作用域退出时运行的清理动作，按逆序执行。 |
| `destroy` | 在 `impl Type` 中定义类型销毁前运行的生命周期清理块。 |
| `else` | `if` 的备用分支。 |
| `enum` | 定义代数枚举 / sum type。 |
| `extern` | 声明外部 ABI 绑定；裸声明必须写成 `trust extern`。 |
| `false` | 布尔假值。 |
| `fn` | 定义函数或函数签名。 |
| `for` | 遍历循环。 |
| `if` | 条件表达式或条件语句。 |
| `impl` | 给类型添加方法，或声明类型实现某个接口。 |
| `import` | 导入模块或模块成员。 |
| `in` | `for item in items` 中连接循环变量和被遍历对象。 |
| `interface` | 定义能力契约。 |
| `let` | 创建不可变绑定。 |
| `match` | 模式匹配表达式，必须穷尽。 |
| `module` | 声明当前文件所属模块。 |
| `move` | 显式转移所有权，或声明闭包按值捕获。 |
| `mut` | 声明可变绑定、可写参数、可写接收者或调用点可写访问。 |
| `pub` | 导出声明，使其对模块外可见。 |
| `return` | 从当前函数返回。 |
| `self` | 方法中的当前接收者值。 |
| `Self` | 当前实现类型的类型名别名，只能在 `impl` 或 `interface` 相关上下文中使用。 |
| `static` | 声明静态存储项；默认不可变，跨线程可变状态必须放在同步类型中。 |
| `struct` | 定义结构体和值布局。 |
| `true` | 布尔真值。 |
| `trust` | 声明信任边界，允许作者承担编译器无法独立证明的底层不变量。 |
| `try` | 展开 `Result` 或 `Option`，失败分支从当前函数提前返回。 |
| `type` | 定义类型别名。 |
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
- `@noPanic`：被标注函数不得触发 panic 路径。

编译器发行包专用属性：

- `@intrinsic`：把声明绑定到编译器内建行为。

发行包专用属性不是普通包能给自己授予的能力。用户写底层代码应使用 `trust` 边界，而不是属性。

## 5. 绑定与可变性

`let` 创建不可变绑定。`var` 创建可变绑定。

```zn
let answer: I32 = 42;
var count: USize = 0;
count = count + 1;
```

可变性属于绑定或访问方式，而不是永远属于某个值。不可变拥有者可以暴露只读访问；可变拥有者可以在没有其他访问时暴露唯一可写访问。

## 6. 常量与静态存储

`const` 声明编译期常量。它没有运行期存储身份，使用处可以被内联。

```zn
const pageSize: USize = 4096;
```

`static` 声明拥有固定存储地址和整个程序生命周期的静态项。

```zn
static VERSION: String = "0.1.0";
static REQUESTS: Atomic<U64> = Atomic<U64>.new(0);
```

规则：

- `static` 初始化式必须能在编译期或加载期完成。
- 普通 `static` 默认不可变。
- Zeno 不提供裸 `static mut` 全局变量；跨线程可变全局状态必须放在 `Atomic<T>`、`Mutex<T>` 或其他同步类型中。
- Freestanding profile 可以选择不支持需要动态初始化的 `static`。

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

内建复合形式：

```text
Array<T>          拥有、连续、长度创建后固定的数组
Vector<T>         拥有、连续、可增长的数组
ArraySlice<T>     不拥有、连续、长度稳定的数组片段
String            UTF-8 文本值
Tuple<T...>       元组值
Fn<A..., R>       非拥有 callable 接口
```

命名规则：

- 所有类型名使用 PascalCase，包括基础类型、集合、接口、结构体、枚举和错误类型。
- 关键字使用小写。
- 函数名、方法名、字段名、局部变量、模块路径段和属性名使用 lowerCamelCase。
- 常量项也是值名，默认使用 lowerCamelCase，例如 `pageSize`。
- 普通标识符不使用 snake_case；单独的 `_` 只用于丢弃模式。
- 语言不提供小写类型别名；例如使用 `String`，不使用 `str`；使用 `U8`，不使用 `u8`。

堆拥有、同步等能力是库类型，不是魔法语法：

```text
Box<T, A>
Shared<T, A>
Mutex<T>
Atomic<T>
```

`Box`、`Array`、`Vector`、`Shared`、`Mutex` 和原子类型必须在源码中显式出现，因为它们携带分配或同步成本。

## 8. 整数运算与转换

Zeno 的整数运算必须高性能且行为确定。普通整数算术不插入隐藏溢出检查，也不会产生未定义行为。

规则：

- 整数 `+`、`-`、`*` 默认按类型位宽 wrapping。
- wrapping 结果等价于数学结果对 `2^位宽` 取模。
- 有符号整数也使用同一条位模式 wrapping 规则，例如 `I8` 的 `127 + 1` 结果是 `-128`。
- 除法、取模和移位不使用 wrapping 兜底；除以零、取模零、移位量大于等于位宽必须被拒绝或在运行时 trap，具体 trap 形式由 profile 定义。
- 常量表达式中的除零、取模零、非法移位是编译错误。

```zn
let a: U8 = 250;
let b: U8 = 10;
let c = a + b; // c == 4
```

需要显式溢出处理时使用整数方法：

```zn
let checked: Option<U8> = a.checkedAdd(b);       // None
let wrapped: U8 = a.wrappingAdd(b);              // 4
let saturated: U8 = a.saturatingAdd(b);          // 255
let both: (U8, Bool) = a.overflowingAdd(b);      // (4, true)
```

`checked*` 方法返回 `Option<T>`：

- `Some(value)` 表示运算成功。
- `None` 表示溢出、除零、取模零，或有符号最小值除以 `-1` 这类不可表示结果。

Zeno 不为整数溢出定义 `OverflowError`，因为溢出没有额外错误信息。需要进入 `Result` 错误流时，由调用方把 `None` 转换成自己的错误类型。

```zn
let size = match count.checkedMul(elementSize) {
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
let byte: U8 = 42;
let wide: U32 = byte as U32;              // ok: 无损扩大
let maybeByte = U8.fromChecked(wide);     // Option<U8>
let low = U8.truncate(wide);              // 明确截断
let clamped = U8.saturate(wide);          // 超过 U8.max 时得到 255
```

## 9. 值、Copy 与 Move

每个值要么是 `Copy`，要么是资源拥有值。

`Copy` 值可以通过赋值、传参和返回复制：

```zn
let a: I32 = 1;
let b = a;
let c = a + b;
```

资源拥有值不会隐式复制。需要转移所有权时使用 `move`，移动资源会让原绑定不可再用。普通函数参数 `x: T` 不触发所有权转移；它是只读访问。消费方法通过 `move self` 在方法签名中表达所有权转移。

```zn
let file = try File.open("data.txt");
let owner = move file;
// file 现在不可再用
```

对非 `Copy` 类型，源码层面的所有权转移必须写 `move`，除非值来自临时表达式、函数返回，或调用的是签名中声明 `move self` 的消费方法。这让资源转移清楚可见，同时不引入 Rust 式生命周期标注。

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
- 需要转移非 `Copy` 拥有者时必须写 `move`，例如 `let b = move packet;`。
- `Array<T>`、`Vector<T>`、`String`、`Box<T>`、`Shared<T>`、`Mutex<T>` 和带 `destroy` 的类型默认不是 `Copy`。
- 深拷贝必须显式调用 `clone`，并且如果会分配，必须接收 allocator 并返回 `Result<T, AllocError>`。
- `Array<T>.clone` 和 `Vector<T>.clone` 表示深拷贝元素和存储，不是共享底层缓冲区。v1 先为 `T: Copy` 元素提供集合 clone；非 `Copy` 元素的深拷贝接口后续单独设计。
- 引用计数增加只能通过 `Shared<T>.clone` 这类类型名中显式包含 `Shared` 的 API 表达。

```zn
let packet = makePacket();
let a = packet;                         // error: non-Copy value requires move or clone
let b = move packet;                    // ok: 所有权转移
let c = try b.clone(mut allocator);     // ok: 显式深拷贝，可能分配
```

## 10. RAII 与销毁

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
- `destroy` 中不能使用 `try`。
- `destroy` 不能作为常规错误流的一部分。
- `destroy` 调用的清理 API 必须是不可失败或 best-effort API，返回 `Unit`。
- 需要处理清理错误时，类型应提供显式的 `close`、`flush`、`finish` 或类似 `move self` 方法，返回 `Result<T, E>`。
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

显式 `close` 成功后应让资源进入已完成状态，避免随后运行的 `destroy` 重复清理。若 `close` 失败并通过 `try` 返回错误，`destroy` 仍会执行 best-effort 兜底清理。

销毁顺序：

1. 局部变量按初始化顺序的逆序销毁。
2. 如果类型有 `destroy` 块，先运行该块。
3. 结构体字段按声明顺序销毁。
4. 部分初始化的值只销毁已经初始化的字段。
5. `panic` 不属于常规错误流；freestanding 目标可以把 panic 配置为 abort。

字段移动规则：

- 只有拥有整个对象时，才能移动出非 `Copy` 字段，例如 `move self.bytes` 或 `move packet.bytes`。
- 从 `self` 或 `mut self` 接收者中不能移动字段，因为它们不拥有对象。
- `move self.field` 会把该字段标记为已移出；函数退出时只销毁仍然初始化的字段。
- 移出字段后，不能再读取该字段，也不能再把原对象当作完整值使用。
- 有自定义 `destroy` 块的类型默认禁止从外部移出字段，因为 `destroy` 可能依赖完整对象不变量。
- 例外：类型自己的 `move self` 方法可以作为官方拆解 API 移出 `self` 字段。该方法必须在移出字段前完成 `destroy` 原本负责的必要清理，并保持类型不变量。
- 如果带 `destroy` 的类型在 `move self` 方法中移出了字段，编译器不会再运行该对象的 `destroy` 块；它只销毁仍然初始化的字段。

```zn
struct Packet {
    bytes: Array<U8>,
    header: Header,
}

impl Packet {
    fn intoBytes(move self) -> Array<U8> {
        return move self.bytes;
        // self.header 仍会被销毁；self.bytes 不会被销毁两次
    }
}
```

## 11. 集合访问

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
let page = try Array<U8>.filled(4096, 0, mut allocator);          // 固定长度
let bytes = try Vector<U8>.withCapacity(1024, mut allocator);      // 可增长
```

集合类型含义：

- `Array<T>`：拥有连续存储，长度创建后固定，适合页缓冲区、帧缓冲区和固定批量数据。
- `Vector<T>`：拥有连续存储，长度可增长，带 `capacity`，适合构建结果和追加写入。
- `ArraySlice<T>`：不拥有连续存储，只表达一段数组访问，适合 API 参数、局部切片和窗口。

访问规则：

- 多个只读访问可以共存。
- 可写访问在其活动区域内必须唯一。
- 访问值不能比它引用的存储活得更久。
- 可写调用必须在调用点显式写出：`fill(mut bytes, 0)`。
- 普通函数的所有权转移仍然必须显式写出：`consume(move bytes)`；消费方法通过方法签名中的 `move self` 表达。
- `Array<T>` 和 `Vector<T>` 可以在参数位置自动转成 `ArraySlice<T>`。
- `Array<T>` 和 `Vector<T>` 可以在调用点 `mut` 转成 `mut ArraySlice<T>`，前提是没有重叠活动访问。
- 当 `Array<T>` 或 `Vector<T>` 存在活跃 `ArraySlice<T>` 时，拥有者不能被移动或销毁。
- `Vector<T>` 存在活跃视图时，禁止 `push`、`pop`、`reserve`、`insert`、`remove`、`clear`、`shrink` 等改变长度或容量的结构性修改。
- 活跃视图结束后，`Vector<T>` 可以继续增长或收缩。

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
    let bytes = try Array<U8>.filled(64, 0, mut allocator);
    return Ok(bytes.asSlice()); // error: view outlives local storage
}
```

`ArraySlice<T>` 也不能进入长期或逃逸位置：`Box<T>`、`Shared<T>`、`Array<T>`、`Vector<T>`、`static`、逃逸闭包、线程、任务和 async future 都不能保存它。需要这些能力时，保存拥有者。

活跃视图期间，`Vector<T>` 的只读查询仍然允许：

```zn
let view = data.asSlice();
let n = data.len;       // ok
try data.push(1);       // error: live view depends on data
```

如果需要结构性修改，让视图先结束：

```zn
{
    let view = data.asSlice();
    parse(view);
}

try data.push(1);       // ok
```

`Array<T>` 长度固定，没有增长和收缩操作。修改元素仍然遵守普通访问规则：存在重叠只读视图时不能取得可写访问；编译器能证明不重叠的拆分访问可以通过。这个规则不需要运行时引用计数、锁或 GC。

## 12. 函数

```zn
fn add(a: I32, b: I32) -> I32 {
    return a + b;
}
```

参数行为：

- `x: T`：默认只读访问。对非 `Copy` 资源不移动、不复制；函数不能销毁或保存该资源。对 `Copy` 值，编译器可以按值传递。
- `mut x: T`：唯一可写访问。函数可以修改被访问的值，但不取得所有权；调用点必须写 `mut`。
- `move x: T`：接收所有权。函数结束时销毁该值，或继续把它移动给其他拥有者；非 `Copy` 实参必须在调用点写 `move`。
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

let file = try File.open("data.txt");
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

编译器必须拒绝把非 `Copy` 资源隐式传给 `move` 参数：

```zn
close(file); // error: move parameter requires `move file`
```

编译器也必须拒绝把只读参数当成拥有值返回或保存：

```zn
fn badReturn(packet: Packet) -> Packet {
    return packet; // error: read parameter is not owned
}

fn badSave(packet: Packet, mut allocator: GlobalAllocator) -> Result<Box<Packet>, AllocError> {
    return Box.new(move packet, mut allocator); // error: read parameter cannot be moved into Box
}
```

函数按值返回。语义允许时必须支持返回值优化。返回 `Array<T>` 或 `Vector<T>` 表示返回拥有者；返回 `ArraySlice<T>` 表示返回非拥有访问，必须通过逃逸分析证明底层存储仍然有效。

## 13. 控制流

当所有分支产生兼容类型时，`if`、`match`、`while`、`for` 和块可以作为表达式。

```zn
let sign = if value < 0 { -1 } else { 1 };
```

循环：

```zn
while condition {
    step();
}

for item in items {
    visit(item);
}
```

`defer` 注册词法清理动作。它不得隐藏分配或同步成本。

```zn
fn withLock(m: Mutex<State>) -> Result<Unit, Error> {
    let guard = try m.lock();
    defer guard.unlock();
    return Ok(());
}
```

`defer` 规则：

- `defer` 动作在当前词法作用域退出时运行。
- 多个 `defer` 按注册顺序的逆序运行。
- `return`、`try` 传播错误、`break` 和 `continue` 都会先运行离开的作用域中的 `defer`。
- `defer` 运行时，当前作用域中尚未移动走的局部值仍然有效；`defer` 结束后再按 RAII 规则销毁这些局部值。
- `defer` 不能代替 RAII；拥有资源的类型仍应通过字段销毁或 `destroy` 块清理。

## 14. 枚举与模式匹配

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

`match` 必须穷尽，除非存在通配分支。从模式中移动值遵循普通移动规则。

## 15. 接口与实现

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
- 调用 `mut self` 方法不移动接收者，但接收者必须是可写位置，例如 `var` 绑定、可写字段或可写元素。
- 调用 `move self` 方法接收整个对象所有权；调用后原接收者不可再用。
- `mut self` 方法调用点不写 `mut`，因为可写意图已经由方法签名和接收者的 `var` 绑定共同表达。

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
        return move self.bytes;
    }
}

var packet = makePacket();
let n = packet.len();           // 只读；packet 仍可用
packet.clear();                 // 可写；packet 仍可用
let bytes = packet.intoBytes(); // 消耗；packet 不可再用
```

`mut self` 方法不能在不可写接收者上调用：

```zn
let packet = makePacket();
packet.clear(); // error: mut self method requires writable receiver
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
        let n = try w.write(rest);
        rest = rest.dropPrefix(n);
    }
    return Ok(());
}
```

接口也可以直接作为参数类型：

```zn
fn writeAny(mut w: Writer, bytes: ArraySlice<U8>) -> Result<USize, IoError> {
    return w.write(bytes);
}
```

区别是：

- `W: Writer`：每个实例化点编译器知道具体类型，可以静态派发和内联。
- `w: Writer`：参数是短期接口访问，调用方可以传入任意实现，方法调用通过接口表派发。

接口访问参数不意味着堆分配。拥有异构接口值时，必须使用显式拥有者，例如 `Box<Writer>` 或 `Shared<Writer>`。

裸接口访问类型不允许作为长期存储。它不能作为结构体字段、枚举载荷、元组字段、`static`、集合元素，也不能被逃逸闭包、线程、任务或 async future 捕获。原因是裸接口访问不拥有具体对象；允许它长期保存会重新引入隐藏生命周期问题。需要保存实现者时有两种写法：

```zn
struct StaticSink<W: Writer> {
    writer: W,              // ok: 保存具体类型，静态派发，零装箱
}

struct DynamicSink {
    writer: Box<Writer>,    // ok: 拥有异构接口值，分配成本显式
}

struct BadSink {
    writer: Writer,         // error: 裸接口访问不能作为字段长期保存
}
```

动态接口调用只支持接口中的“动态可派发方法”。规则如下：

- 方法接收者必须是 `self` 或 `mut self`。
- 方法不能有自己的泛型参数。
- 方法签名中不能在非接收者位置使用 `Self`，也就是不能返回 `Self`，不能把 `Self` 作为普通参数类型。
- 方法的参数和返回值 ABI 必须固定，编译器能为接口表生成统一调用入口。
- `move self` 方法需要拥有具体对象，v1 只允许通过 `T: Interface` 静态约束调用，不允许通过 `value: Interface` 动态调用。

这些静态专用方法仍然可以写在接口里。它们表示“所有实现者都必须提供这个能力”，但只能在编译器知道具体类型时调用：

```zn
interface Duplicable {
    fn duplicate(self) -> Self;
}

fn duplicateStatic<T: Duplicable>(value: T) -> T {
    return value.duplicate(); // ok: T 已知，静态派发
}

fn duplicateAny(value: Duplicable) -> Duplicable {
    return value.duplicate(); // error: 返回 Self 的方法不能动态派发
}
```

因此 Zeno 不需要 `dyn` 关键字：

- `W: Writer` 表示静态接口约束，优先单态化、内联和去虚化。
- `writer: Writer` 表示短期接口访问，方法调用通过接口表派发，不能长期保存。
- `Box<Writer>` 或 `Shared<Writer>` 表示拥有一个异构接口值，分配或引用计数成本由类型名显式暴露。
- `interface ThreadWriter: Writer, Send {}` 表示命名接口能力组合。构造或转换到该类型时，具体类型必须同时实现 `Writer` 和 `Send`。

接口对象的推荐降低模型如下。它不是稳定外部二进制 ABI；稳定 ABI 在 v1 之后单独设计。但 stage0 和自举编译器必须满足这些语义和成本约束。

接口访问 `Writer` 是一个短期胖访问值：

```text
InterfaceAccess<Writer> {
    data: pointer to concrete object,
    table: pointer to interface table for (ConcreteType, Writer),
}
```

接口表由编译器为每个 `(具体类型, 接口)` 组合生成，至少包含：

- 具体值的 `size` 和 `align`。
- 具体值的销毁入口，用于运行字段销毁和自定义 `destroy`。
- 该接口所有动态可派发方法的函数入口，按接口声明的规范顺序排列。

静态专用方法不进入接口表，例如返回 `Self`、普通参数中使用 `Self`、带方法级泛型参数、或 `move self` 的方法。

动态方法调用降低为一次接口表间接调用。`self` 方法接收只读对象地址；`mut self` 方法接收唯一可写对象地址。用户代码不能取得这些裸地址；它们只是编译器内部 ABI。

接口能力组合不需要额外堆分配。`Box<ThreadWriter>` 和 `Box<Writer>` 的拥有成本相同；区别是前者在类型系统中保留了“底层具体类型可跨线程移动”的证明。`Send`、`Sync` 这类标记接口没有动态方法入口，不增加接口调用成本。

`Box<Writer, A>` 是拥有式接口对象：

- 它拥有一个由 allocator `A` 分配的具体对象。
- 句柄至少携带 `data` 指针和 `table` 指针；有状态 allocator 也必须被携带，零大小 allocator 可以被优化掉。
- 从具体值构造 `Box<Writer, A>` 时，只分配一次，分配内容是具体类型本身，不额外分配接口包装对象。
- `Box<T, A>` 在 `T: Writer` 时可以移动转换成 `Box<Writer, A>`；该转换只增加或替换接口表元数据，不重新分配、不复制、不移动堆内具体值。
- 销毁 `Box<Writer, A>` 时，先通过接口表销毁具体对象，再用接口表中的布局信息和 allocator `A` 释放内存。
- 对 `Box<Writer, A>` 调用接口方法会临时借出内部对象形成 `Writer` 访问；调用 `mut self` 方法要求 `Box` 本身处于唯一可写访问中。

`Shared<Writer, A>` 是共享拥有式接口对象：

- 它拥有一个带引用计数控制块的具体对象。
- 句柄携带对象地址、接口表地址和 allocator 状态；实现可以把控制块放在对象前方，并用接口表布局信息计算释放布局。
- `Shared<T, A>` 在 `T: Writer` 时可以移动转换成 `Shared<Writer, A>`；该转换不增加引用计数、不重新分配、不复制具体值。
- `Shared<Writer, A>.clone()` 只增加引用计数，不复制具体值。
- 最后一个 `Shared<Writer, A>` 销毁时，通过接口表销毁具体对象，再释放整个控制块。
- `Shared<Writer, A>` 默认只提供只读接口访问，只能调用 `self` 方法。需要共享可变状态时，必须显式使用 `Shared<Mutex<T>>`、原子类型或其他同步容器。

裸接口访问不拥有对象，所以 `move writer: Writer` 在 v1 中无效。接收拥有式异构接口值必须写 `move writer: Box<Writer>`，或按共享语义传递 `Shared<Writer>`。

Zeno v1 不提供从 `Box<Writer>` 或 `Shared<Writer>` 向具体类型的通用 downcast。需要这种能力时，应在接口中显式设计查询方法，或推迟到反射/类型标识设计完成后再引入。

## 16. 泛型

泛型函数和类型默认单态化。

```zn
fn max<T: Ord, Copy>(a: T, b: T) -> T {
    if a < b { return b; }
    return a;
}

struct Map<K: Hash, Eq, V> {
    // K 是 key 类型，要求 Hash 和 Eq。
    // V 是 value 类型，没有额外约束。
}
```

规则：

- 类型参数可以受接口约束。
- 多个约束使用逗号，不使用 `+`，也不需要 `where`。
- 多个泛型参数用于表达多个未知类型，例如 `Result<T, E>`、`Map<K, V>` 或 `Fn<A, B>`。
- 泛型参数列表中的名字由语义阶段解析：已知接口名作为约束，新引入的名字作为类型参数。
- 泛型代码只能使用约束保证的操作。
- 单态化是默认代码生成策略。
- 只有在编译器证明行为和可观察性能没有差异时，才允许共享泛型代码。接口类型参数使用接口派发。

## 17. 闭包

闭包是值，捕获方式由使用方式推导。

```zn
let factor = 3;
let scale = (x: I32) => x * factor;
let scaleBlock = (x: I32) -> I32 {
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
- 逃逸闭包必须使用显式拥有类型，例如 `Box<Fn<...>>` 或任务类型。

```zn
let task = move () -> Result<Unit, Error> {
    return worker.run();
};
let handle = try Thread.spawn(move task);
```

## 18. 错误

可恢复错误使用 `Result<T, E>`。

```zn
fn load(path: String) -> Result<String, IoError> {
    let file = try File.open(path);
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
- `try` 触发提前返回时，会按普通作用域退出路径执行 `defer` 和 RAII 销毁。

需要把 `Option<T>` 接入 `Result<T, E>` 错误流时，必须显式选择错误：

```zn
fn parseByte(value: U32) -> Result<U8, ParseError> {
    let byte = try U8.fromChecked(value).okOr(ParseError.OutOfRange);
    return Ok(byte);
}
```

需要转换错误类型时，必须显式写出转换：

```zn
fn readConfig(path: String) -> Result<String, ConfigError> {
    let text = try fs.readFile(path).mapErr(ConfigError.Io);
    return Ok(text);
}
```

`try` 不分配、不使用异常，也不要求 unwinding。编译器应把它降低为普通分支和清理边。

`panic` 表示程序不变量被破坏，不是 `Result` 的替代品。Hosted 目标可以 unwind 或 abort；freestanding 目标可以 abort。

## 19. 并发

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
- 裸接口访问、`ArraySlice<T>` 和其他非拥有访问值不能跨非 scoped 并发边界。

核心库类型的规则：

- `Box<T, A>` 是 `Send` 当且仅当 `T: Send` 且 `A: Send`；是 `Sync` 当且仅当 `T: Sync` 且 `A: Sync`。
- `Shared<T, A>` 是 `Send` 和 `Sync` 当且仅当 `T: Send, Sync` 且 `A: Send, Sync`。最后一次释放和销毁可能发生在任意持有者线程，所以 `T` 必须是 `Send`；多个线程可同时只读访问，所以 `T` 也必须是 `Sync`。
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
let data = move buildData();
let handle = try Thread.spawn(move () {
    process(move data);
});
try handle.join();
```

任务运行时是显式值：

```zn
let runtime = try Runtime.withWorkerCount(8);
let task = try runtime.spawn(move () {
    process(move data);
});
try task.await();
```

`async fn` 编译成 `Future` 状态机。创建 future 不会启动虚拟线程；执行它需要程序选择 executor 或 runtime。

## 20. FFI、硬件与 trust 边界

Zeno 不使用 `unsafe`。当代码需要执行编译器无法独立证明的底层操作时，作者必须写出 `trust` 边界。

`trust` 表示：

- 作者向编译器声明：这里依赖的底层不变量由作者负责保证。
- 编译器仍然执行普通类型检查、所有权检查、初始化检查和可达的访问检查。
- `trust` 只放开裸 FFI、裸地址、裸指针、volatile/MMIO、inline asm、链接段和中断入口这类底层能力。
- 编译器必须记录每个 `trust` 边界的位置和使用的底层能力，生成信任报告。

裸 C ABI 声明必须写成 `trust extern`：

```zn
trust extern "C" fn read(fd: I32, buffer: USize, length: USize) -> ISize;
```

底层操作必须放在 `trust` 块里：

```zn
fn sysRead(fd: I32, mut out: ArraySlice<U8>) -> Result<USize, IoError> {
    let n = trust {
        read(fd, out.rawAddress(), out.len)
    };

    if n < 0 {
        return Err(IoError.LastOsError);
    }
    return Ok(n as USize);
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

`trust` 边界应尽量小。推荐把 `trust` 封装成普通安全 API，对外暴露 `ArraySlice<T>`、`Result<T, E>`、`Handle<T>`、`Mmio<T>`、`Port<T>`、`DmaBuffer` 等有语义的类型。

构建系统可以提供策略：

- 禁止依赖包中的 `trust`。
- 只允许指定包或路径使用 `trust`。
- 要求发布包附带信任报告。
- 在 freestanding / kernel profile 中允许更多底层能力，在 hosted profile 中默认限制 inline asm、裸硬件地址和中断入口。

## 21. 成本可见性

这些操作必须在源码或类型签名中可见：

- 堆分配：`Box`、`String`、`Array`、`Vector`、allocator 参数或 `alloc` 函数。
- 引用计数 / 共享所有权：`Shared`。
- 接口派发：短期接口访问参数 `writer: Writer`，或显式接口拥有者 `Box<Writer>` / `Shared<Writer>`。
- 锁：`Mutex`、`RwLock`、`Once` 或等价 guard 类型。
- 原子操作：`Atomic<T>`。
- 非 `Copy` 值的所有权转移：普通调用点的 `move value`，或方法签名中的 `move self`。
- 逃逸 callable 分配：显式 `Box<Fn<...>>` 或任务类型。
- 底层信任边界：`trust`。

如果库 API 以误导性的名称或返回类型隐藏成本，编译器应该给出警告。

`@noAlloc` 会把可达调用图中的隐藏分配变成硬错误。

## 22. 兼容性与延后内容

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
