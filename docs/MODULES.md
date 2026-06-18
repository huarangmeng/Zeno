# Zeno 模块、包与依赖系统

Zeno 的模块系统目标是：同包代码写起来轻，外部依赖边界清楚，构建仍然可以确定、可增量、可审计。

## 1. 包与默认布局

包是一个包含 `Zeno.toml` 的目录。v1 固定源码根目录为 `src/`，普通包不需要在 manifest 里配置源码路径。

典型布局：

```text
Zeno.toml
src/
  main.zn
  net/http.zn
  net/url.zn
  util/hash.zn
```

最小包配置：

```toml
[package]
name = "app"
```

`package.name` 是包身份，用于依赖图、锁文件、诊断和信任报告。包名可以包含 `-`，因为它不直接进入源码模块路径。

## 2. 文件路径与可选 module

包内 `.zn` 文件的模块路径由 `src/` 下的文件路径推断：

```text
src/main.zn       -> main
src/net/http.zn   -> net.http
src/util/hash.zn  -> util.hash
```

文件可以省略 `module` 声明：

```zn
pub fn main() -> Unit {}
```

如果写了 `module`，它只是校验声明，必须和文件路径一致：

```zn
module net.http;
```

规则：

- 每个 `.zn` 文件最多声明一个 `module`。
- 文件路径和显式 `module` 声明必须匹配。
- 不做 `mod.zn` 魔法查找，不做大小写宽容。
- 包根下的源码必须放在 `src/`；自定义源码根延后设计。

## 3. 同包直接可用

同一个 package 内的顶层声明默认直接可见，不需要 import。

```text
src/net/http.zn
src/main.zn
```

`src/net/http.zn`：

```zn
struct Client {
    id: U64,
}

fn connect() -> Client {
    return Client { id: 1 };
}
```

`src/main.zn`：

```zn
pub fn main() -> U64 {
    let client = connect();
    return client.id;
}
```

名字解析时，编译器把同包所有文件作为一个包作用域处理。若同包内只有一个可见声明叫 `Client`，任意文件都可以直接写 `Client`。

## 4. 重载、冲突与限定名

Zeno 支持函数重载。多个函数或方法可以使用同一个名字，只要参数形状不同。

重载键：

- 函数名。
- 参数数量。
- 参数类型列表。
- 参数访问模式：只读、`mut`、`move`。
- 方法还包括接收者类型和接收者模式：`self`、`mut self`、`move self`。

返回类型不参与重载键。只靠返回类型不同的两个函数是重复定义：

```zn
fn parse(text: StringSlice) -> I32 { ... }
fn parse(text: StringSlice) -> U32 { ... } // error: overload cannot differ only by return type
```

虽然调用点有 `move value` 标记，同一个重载集合仍不能只靠只读参数和 `move` 参数区分。若两个候选的名字、参数数量和参数类型列表相同，只是某个普通参数访问模式在只读和 `move` 之间变化，声明会被判为可读性歧义，必须改名或改变参数类型。`mut` 参数仍可通过调用点的 `mut` 访问形式区分。

允许：

```zn
fn size(value: StringSlice) -> USize { ... }
fn size(value: ArraySlice<U8>) -> USize { ... }
fn size(value: StringSlice, radix: U8) -> USize { ... }
```

调用解析是纯编译期规则：

1. 先按可见性收集同名候选。
2. 过滤参数数量和 `mut` 调用形式不匹配的候选；调用点没有 `move` 形式。
3. 用参数类型和泛型约束检查候选是否可用。
4. 选择唯一最佳候选；如果没有候选或有多个同等最佳候选，报错。

排序规则保持简单：

- 精确非泛型匹配优先。
- 精确泛型匹配次之。
- 需要语言允许的访问转换时排在精确匹配之后，例如 `Array<T>` / `Vector<T>` 到 `ArraySlice<T>`。
- 如果最高优先级里仍有多个候选，调用歧义，必须加类型标注、模块限定名，或改用更明确的函数名。

Zeno 不做隐式数值转换、隐式分配或基于返回类型的重载选择。整数字面量可以由唯一候选的参数类型推断；若多个候选都能接收该 literal，则调用歧义。

重载没有运行时成本。选中的具体函数在类型检查期确定；泛型重载仍按普通泛型规则单态化。

如果同包内多个模块声明了同名顶层项，直接使用该名字会报歧义：

```text
src/net/http.zn  -> Client
src/db/client.zn -> Client
```

此时使用模块限定名：

```zn
let http: net.http.Client = net.http.connect();
let db: db.client.Client = db.client.connect();
```

限定名只在歧义或表达 API 边界时需要，不是日常写法。

同包内两个非函数顶层声明可以重名，只要使用点能消歧；同一模块内不能有两个同名非函数顶层声明。函数和方法可以按重载规则重名，但同一个重载键不能重复。

## 5. 可见性

顶层声明默认是 package-visible：同包所有文件可见，外部包不可见。

```zn
fn helper() -> I32 {
    return 1;
}
```

`pub` 表示对外部 package 导出：

```zn
pub struct Client {
    pub id: U64,
}
```

`pub` 只是源码 API 可见性，不表示外部 ABI 稳定，也不导出链接符号。需要导出 C ABI 符号时必须使用 `@export("symbol", abi: C)`。

`private` 表示只在当前文件可见：

```zn
private fn parseHeader(text: StringSlice) -> Header {
    ...
}
```

规则：

- 默认顶层声明：同 package 可见。
- `private` 顶层声明：只在当前文件可见。
- `pub` 顶层声明：同 package 可见，并对依赖当前包的外部 package 可见。
- 结构体字段默认同 package 可见；`private` 字段只在定义文件可见；`pub` 字段对外部 package 可见。
- enum 变体跟随 enum 本身可见；变体载荷类型仍然必须满足可见性约束。
- `private` 类型不能出现在非 `private` API 签名中。
- package-visible 类型不能出现在 `pub` API 签名中。
- `pub` API 的签名不能暴露非 `pub` 类型。

v1 不引入 `pub(package)`、friend module 或 re-export。

## 6. 外部导入

同 package 不需要 import。`import` 只用于外部依赖和内建包：

```zn
import std.io;
import platform.os;
import core.result.{Result, Ok, Err};
```

语义：

- `import std.io;` 导入外部模块绑定 `io`，之后写 `io.println` 或 `io.Error`。
- `import platform.os;` 导入依赖包中的模块绑定 `os`。
- `import core.result.{Result, Ok};` 从外部模块导入多个公开项到当前文件作用域。
- 导入只建立名字绑定，不执行代码，不触发隐藏初始化。

v1 不提供 wildcard import，不提供 import alias，不提供 `pub import` re-export。

导入解析：

1. 第一个路径段必须是内建包根或 `[dependencies]` 中声明的依赖 key。
2. `import root.path;` 必须解析到外部模块，并把最后一段作为本文件的模块绑定名。
3. `import root.path.{A, B};` 必须把 `root.path` 解析为外部模块，列表里的名字必须是该模块的 `pub` 项。
4. 导入不能指向当前 package 内部模块；同包声明直接可用。

如果当前包 `src/` 顶层目录和依赖 key 重名，例如当前包有 `src/platform/...` 且 `[dependencies] platform = ...`，manifest 必须报错。这样解析永远不猜。

## 7. 内建包

内建包根：

- `core`：始终可解析，包含基础类型能力、`Option`、`Result`、panic/OOM 绑定和低层抽象。
- `alloc`：编译器发行包提供，包含拥有式堆分配类型和构建器；能否使用无 `In` 后缀 API 由 manifest allocator 策略决定。
- `std`：hosted 标准库；只有 hosted profile 或显式支持 hosted 功能的自定义 profile 可以解析。

`core` 不需要写进 `[dependencies]`。`alloc` 和 `std` 可以写进 `[dependencies]` 用于审计和锁文件，但编译器发行包可以把它们识别为内建依赖。

## 8. 依赖声明

第三方或本地包依赖写在 `Zeno.toml`：

```toml
[dependencies]
platform = { path = "../platform" }
math = { path = "../math" }
std = "builtin"
```

规则：

- dependency key 是当前包中的外部 import 根，必须是合法 Zeno 标识符。
- dependency key 是本地别名，不要求等于依赖包的 `package.name`。
- v1 支持 `"builtin"`、`{ path = "..." }`、`{ git = "...", rev = "..." }` 和 `{ version = "..." }`。
- git、registry、workspace 和 lockfile 的完整解析规则见 [PACKAGE.md](PACKAGE.md)。
- 依赖图以包为单位构建，不能出现包依赖环。
- 同一个 import 根不能同时指向两个包。
- dependency key 不能和当前包 `src/` 顶层目录、内建包根或其他依赖 key 冲突。

例子：

```toml
[package]
name = "app"

[dependencies]
platform = { path = "../platform" }
```

源码：

```zn
import platform.os;

pub fn main() -> I32 {
    return os.pid();
}
```

如果没有 `[dependencies].platform`，`import platform.os;` 必须被拒绝。

## 9. 名字解析

名字解析分层：

1. 当前局部作用域。
2. 当前文件的 `private` 顶层声明。
3. 同 package 中所有 package-visible / `pub` 顶层声明。
4. 显式 import 绑定。
5. 编译器内建基础类型和复合类型，例如 `I32`、`Bool`、`Array<T>`、`Vector<T>`、`String`、`Box<T>`。

每一层可以产生重载集合。若可见候选都是函数或方法，进入重载解析。若同一层产生多个非函数声明，未限定名字报歧义。使用模块限定名消歧，例如 `net.http.Client`。

导入不能覆盖同 package 名字。若 import 绑定名和同 package 中可见的非函数声明冲突，报错；若二者都是函数，只有在重载键不重复时才能合并为一个重载集合。

Zeno v1 不提供隐式 prelude wildcard。`Result`、`Option`、`Thread`、`Mutex` 等库项应通过 import、完整路径或编译器认可的内建类型使用。少数语言内建类型可以直接使用，因为它们参与类型系统和成本模型。

## 10. 循环

包依赖图不能有环。

同包文件天然处于一个包作用域，可以互相引用类型和函数签名。编译分成签名阶段和实现阶段：

- 签名阶段读取同包所有顶层类型、函数签名、接口和 const 类型。
- 实现阶段检查函数体、初始化表达式和 codegen。

允许：

- 两个文件的类型签名互相引用 package-visible 类型。
- 接口和 impl 分布在不同文件中，前提是可见性允许。

拒绝：

- `const` 初始化环。
- `static` 初始化环。
- 两个文件通过顶层初始化互相要求运行时执行。
- by-value 递归结构体字段，除非通过 `Box<T>`、`Shared<T>` 或其他拥有指针打断无限大小。

导入本身永远不执行代码，因此外部 import 不会产生隐藏运行时初始化。

## 11. 初始化

顶层只能包含声明，没有顶层语句。

- `const` 必须在编译期求值。
- `static` 初始化式必须满足当前 profile 的静态初始化规则。
- hosted profile 可以支持受控动态 static 初始化，但必须按依赖图拓扑排序；循环初始化报错。
- freestanding / kernel / embedded profile 默认拒绝需要运行时动态初始化的 `static`。

## 12. trust 与依赖审计

`Zeno.toml` 的 `trust` 配置和依赖系统一起决定底层能力：

- 当前包要使用 FFI、硬件、inline asm 或中断入口，源码必须写 `trust`，manifest 也必须允许对应能力。
- `trust.allowedPackages` 按包名或依赖 key 匹配允许包含 `trust` 的包。
- `trust.dependencyTrust = false` 时，依赖包中出现任何 `trust` 边界都会让构建失败，除非该依赖在 `allowedPackages` 中并被策略显式允许。
- 信任报告必须把 `trust` 边界归属到具体包、推断模块路径、文件和公开 API。

这让系统级包可以写驱动和平台绑定，同时让普通应用依赖图里出现底层能力时有清晰审计边界。
