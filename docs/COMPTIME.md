# Zeno 编译期执行

Zeno 的编译期执行目标是：表达力完整、构建可复现、运行时零隐藏成本。

Zeno 不引入 `comptime` 关键字。编译期能力通过现有 `const` 和“编译期上下文”触发。

## 1. 基本模型

`const` 声明编译期值：

```zn
const pageSize: USize = 4096;
const greeting: StringSlice = "hello";
```

规则：

- `const` 没有运行期地址和存储身份。
- 每次使用 `const` 都等价于使用它的编译期值。
- `const` 可以是顶层项、`impl` 项，也可以是块内局部编译期绑定。
- `const` 初始化必须在编译期执行完成。
- `const` 初始化环是编译错误。

`static` 声明运行期静态存储：

```zn
static requests: Atomic<U64> = Atomic<U64>.new(0);
```

规则：

- `static` 有固定地址和整个程序生命周期。
- `static` 初始化式默认也必须通过编译期执行生成可物化的静态数据。
- Zeno 不提供裸 `static mut`。
- 需要全局可变状态时必须使用 `Atomic<T>`、`Mutex<T>` 或其他同步类型。
- 隐式动态 static 初始化不进入 v1；以后如果支持，必须用显式属性和 profile 策略开启。

## 2. 编译期上下文

以下位置要求表达式在编译期求值：

- `const` 初始化。
- `static` 初始化。
- 常量泛型实参。
- 属性参数，例如 `@layout(Packed(1))` 和 `@export("symbol", abi: C)`。
- `sizeOf<T>()`、`alignOf<T>()`、`offsetOf<T>("field")` 等布局查询。
- pattern range 端点。
- 编译期可证明的索引、除法、移位等常量诊断。

编译器也可以为了优化在其他位置执行常量折叠，但只有上述上下文会把“必须在编译期完成”作为语义要求。

## 3. 完整 CTFE

Zeno 的 CTFE 是完整的普通语言解释执行，而不是只支持少量字面量表达式。

在编译期上下文中，编译器可以执行普通 Zeno 代码：

- 函数、方法和泛型函数。
- `if`、`while`、`for`、`match`、`break`、`continue`、`return`。
- 结构体、枚举、元组、数组、字符串、集合和 pattern。
- 闭包和高阶函数，只要它们不逃到运行期。
- 静态接口派发和泛型单态化。
- RAII、移动语义、部分初始化和 `destroy`。
- 编译期临时分配，用于构造 `String`、`Array`、`Vector`、`Map`、`Set` 等编译期临时值。

不需要 `const fn`。任何普通函数都可以在编译期上下文中被尝试执行。若执行路径触达不能在编译期完成的操作，编译器在该调用点报错，并保留编译期调用栈。

```zn
fn fib(n: U32) -> U32 {
    if n < 2 {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

const fib10: U32 = fib(10);
```

## 4. 可复现性边界

完整 CTFE 不等于能访问任意外部世界。编译期执行必须是 hermetic 的。

CTFE 中禁止：

- `trust`、`trust extern` 调用、裸 FFI。
- 裸地址、裸指针、MMIO、volatile 硬件访问、inline asm 和中断入口。
- 线程、任务、async `await` 和阻塞同步等待。
- 读取当前时间、随机数、环境变量、进程状态、网络和未声明文件。
- 修改运行期 `static` 存储。
- 依赖地址身份、对象真实运行期地址或 allocator 运行期身份。

允许的文件输入必须通过构建系统显式声明，例如后续的 `@embedFile("path")` 或 manifest-declared build input。文件内容 hash 必须进入缓存 key。v1 可以先保留该能力为工具链扩展，不要求完整实现。

## 5. 编译期分配与物化

CTFE 可以使用编译器管理的编译期 arena。它服务于编译期计算，不是运行期堆。

因此下面的代码可以在编译期构造临时 `Vector`，最终产出常量结果：

```zn
fn sumFirst(n: USize) -> USize {
    var values = Vector<USize>.withCapacity(n);
    var i: USize = 0;

    while i < n {
        mut values.push(i);
        i = i + 1;
    }

    var sum: USize = 0;
    for value in values {
        sum = sum + value;
    }
    return sum;
}

const total: USize = sumFirst(1024);
```

编译期拥有值进入运行期时必须遵守成本模型：

- `Copy` 标量和纯 `Copy` 聚合可以直接内联到运行期。
- `StringSlice` 和 `ArraySlice<T>` 可以指向编译器物化的只读静态数据。
- `String`、`Array<T>`、`Vector<T>`、`Map<K, V>`、`Set<T>`、`Box<T>`、`Shared<T>` 这类拥有者不能因为引用 `const` 而偷偷创建运行期堆对象。
- 需要运行期拥有者时，必须显式调用会分配或复制的 API，例如 `String.from(constText)` 或 `Array.fromConst(constTable)`。

换句话说，CTFE 可以为了编译期计算分配；运行期分配仍必须在源码中可见。

## 6. 常量泛型

泛型参数可以是类型参数，也可以是常量参数：

```zn
struct RingBuffer<T, const Capacity: USize> {
    data: Array<T>,
    head: USize,
    len: USize,
}
```

规则：

- 常量参数使用现有 `const` 关键字，不新增语法关键字。
- 常量参数可以出现在类型、函数、接口和 impl 的泛型参数列表中。
- 常量参数必须有显式类型。
- 常量实参必须在编译期求值。
- 常量实参进入单态化 key、layout key、ABI fingerprint 和增量缓存 key。

可作为常量参数的值必须有稳定结构身份：

- 整数、`Bool`、`Char`。
- 无载荷 enum variant。
- 由上述值组成的 tuple 或 `@layout(Source)` / `@layout(C)` 纯 `Copy` struct。

这不是 CTFE 表达力限制，而是类型身份限制。任意复杂编译期计算都可以先运行，最终作为常量泛型实参的结果必须能稳定 fingerprint。

## 7. 资源限制与诊断

CTFE 必须给出确定诊断：

- 编译期 panic 是编译错误，并显示编译期调用栈。
- 编译期 OOM 是编译错误，不走运行期 OOM profile。
- 无限递归或无限循环通过 fuel / step limit 诊断；这是编译器资源保护，不改变语言语义。
- 访问未初始化值、use-after-move、越界、除零、非法移位等错误和运行期代码使用同一套语义检查。

实现可以提供构建参数调整 CTFE fuel，但默认值必须足够支持真实项目的表生成、协议描述、泛型元编程和编译器自举。

## 8. 缓存

CTFE 结果必须可缓存且可复现。

缓存 key 至少包含：

- 被执行函数和被读取声明的 stable id / fingerprint。
- 常量实参值 fingerprint。
- target triple、cpu、features 和 profile。
- layout、panic/OOM、allocator 和 trust 配置。
- 编译器版本、core/alloc/std hash。
- 声明的外部 build input hash。

CTFE 诊断也必须可重放，不能因为并行执行顺序改变错误顺序。
