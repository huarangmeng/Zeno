# Zeno 项目 Manifest

`Zeno.toml` 是包级构建 manifest。它不属于语言语法；它是编译器、构建系统和审计工具读取的项目配置。

manifest 的目标是把会影响安全检查和代码生成的策略集中声明：

- 当前包名和入口。
- 包依赖和内建库依赖。
- 目标 triple 与 profile。
- 是否存在 profile 默认 allocator。
- `panic`、调用栈和 OOM 策略。
- `trust` 底层能力边界。
- 诊断、审计和发布策略。

源码文件不应该自己声明这些策略。测试文件顶部的 `// profile: hosted` 只是规格测试 shorthand，真实包构建以 `Zeno.toml` 或等价命令行参数为准。

## 1. 文件位置

默认路径：

```text
Zeno.toml
```

编译器从包根目录向上查找最近的 `Zeno.toml`。多包 workspace 后续可以引入 workspace manifest，但 v1 先只定义单包 manifest。

manifest 是构建输入的一部分。编译器生成对象、缓存、信任报告和诊断报告时必须记录 manifest 内容 hash。

## 2. 最小 hosted 应用

```toml
[package]
name = "hello"
version = "0.1.0"

[dependencies]
std = "builtin"

[target]
triple = "x86_64-unknown-linux-gnu"
profile = "hosted"

[allocator]
default = true

[panic]
strategy = "abort"
stack = "symbols"

[oom]
strategy = "abort"

[trust]
ffi = false
hardware = false
inlineAsm = false
interrupts = false
dependencyTrust = false
```

这个配置表达：

- 有 hosted 标准库和默认 allocator。
- `String.from`、`Vector.withCapacity`、`Box.new` 等无 `In` 后缀 API 可用。
- `panic` 默认 abort，但 debug 诊断可以输出符号化调用栈。
- OOM 直接 abort，不返回 `Result`。
- 普通应用包不能写裸 FFI、硬件地址、inline asm 或中断入口。

## 3. 最小 freestanding 库

```toml
[package]
name = "cryptoCore"
version = "0.1.0"

[target]
triple = "wasm32-unknown-unknown"
profile = "freestanding"

[allocator]
default = false

[panic]
strategy = "trap"
stack = "none"

[oom]
strategy = "trap"

[trust]
ffi = false
hardware = false
inlineAsm = false
interrupts = false
dependencyTrust = false
```

这个配置下：

- 无默认 allocator；源码必须使用 `String.fromIn`、`Vector.withCapacityIn`、`Box.newIn` 等显式 allocator API。
- `panic` 和 OOM 都降低为目标 trap。
- 不需要 unwinder、符号表或 hosted 运行时。

## 4. Kernel / embedded 配置

```toml
[package]
name = "kernel"
version = "0.1.0"

[dependencies]
platform = { path = "../platform" }

[target]
triple = "x86_64-unknown-none"
profile = "kernel"

[allocator]
default = false

[panic]
strategy = "handler"
handler = "kernel.panicHandler"
stack = "addresses"

[oom]
strategy = "handler"
handler = "kernel.oomHandler"

[trust]
ffi = true
hardware = true
inlineAsm = true
interrupts = true
dependencyTrust = false
requireReport = true
```

kernel / embedded profile 必须显式给出 `panic.handler` 和 `oom.handler`，二者在源码中都必须返回 `Never`。handler 可以记录诊断后 halt、reset 或执行目标 trap。

`panic.stack = "addresses"` 表示 handler 至少可以无分配遍历 instruction pointer；不要求符号化。

## 5. 字段规则

### package

```toml
[package]
name = "myPackage"
version = "0.1.0"
entry = "main.main"
```

- `name` 必填，使用 ASCII 标识符或带 `-` 的包名；用于依赖图、锁文件和信任报告。
- `version` 可选，推荐语义化版本。
- `entry` 可选；应用包可指定入口函数，库包可以省略。若省略，hosted 应用默认寻找 `src/main.zn` 中的 `main` 函数，即 `main.main`。

源码根固定为 `src/`。文件路径推断模块路径，`module` 声明可省略；详细规则见 [MODULES.md](MODULES.md)。

### dependencies

```toml
[dependencies]
platform = { path = "../platform" }
std = "builtin"
```

- dependency key 是当前包中的 import 根，必须是合法 Zeno 标识符。
- `"builtin"` 表示编译器发行包提供的内建依赖，例如 `std`。
- `{ path = "..." }` 表示本地路径依赖。
- `core` 始终隐式可用，不需要写入 `[dependencies]`。
- `alloc` 和 `std` 可以写入 `[dependencies]` 用于审计；`std` 只能在 hosted 或支持 hosted 功能的自定义 profile 中解析。
- dependency key 是本地别名，不要求等于依赖包的 `package.name`。
- dependency key 不能和当前包 `src/` 顶层目录、内建包根或其他依赖 key 冲突。
- 包依赖图不能有环。

### target

```toml
[target]
triple = "aarch64-unknown-none"
profile = "freestanding"
cpu = "generic"
features = ["+fp", "+simd"]
```

- `triple` 必填，决定 ABI、对象格式和目标后端。
- `profile` 必填，v1 内建值为 `"hosted"`、`"freestanding"`、`"kernel"` 和 `"embedded"`。
- `cpu` 和 `features` 可选，直接进入 codegen 配置。

profile 基础含义：

- `hosted`：可以依赖 OS、默认 allocator、线程、文件系统和 hosted `std`。
- `freestanding`：不要求 OS、默认 allocator、线程或 hosted `std`。
- `kernel`：freestanding 的更严格形态，允许硬件能力，但必须显式提供 panic/OOM handler。
- `embedded`：freestanding 的资源受限形态，默认禁止 unwinding 和符号化调用栈。

### allocator

```toml
[allocator]
default = true
symbol = "std.alloc.system"
```

- `default = true` 表示无 `In` 后缀分配 API 可用。
- `default = false` 表示无 `In` 后缀分配 API 必须被拒绝。
- `symbol` 可选，指定默认 allocator 提供者。
- `freestanding` / `kernel` / `embedded` 默认 `default = false`。
- `hosted` 默认 `default = true`。

如果 `default = true`，构建系统必须能解析 `symbol`，或 profile 必须有内建默认 allocator。否则 manifest 无效。

### panic

```toml
[panic]
strategy = "abort"
stack = "symbols"
handler = "kernel.panicHandler"
```

`strategy` 可选值：

- `"abort"`：调用 panic 后终止进程或程序，不展开栈。
- `"trap"`：降低为目标 trap，不展开栈。
- `"unwind"`：启用栈展开，经过的作用域执行 `defer` 和 RAII 销毁。
- `"handler"`：调用 manifest 指定的 `panic.handler`，handler 必须返回 `Never`。

`stack` 可选值：

- `"none"`：不提供调用栈。
- `"addresses"`：提供 instruction pointer / return address，不要求符号化。
- `"symbols"`：尽力符号化函数名、文件和行列信息。

规则：

- 用户源码仍然写 `panic("message")`。
- 编译器自动注入调用点 `SourceLocation`，并在 panic 冷路径构造 `PanicInfo`。
- `stack = "symbols"` 只能在目标支持调试信息或符号化能力时启用。
- `strategy = "handler"` 时 `handler` 必填。
- `kernel` / `embedded` 必须使用 `strategy = "handler"` 并提供 `panic.handler`；handler 内部可以选择 halt、reset 或目标 trap。

### oom

```toml
[oom]
strategy = "abort"
handler = "kernel.oomHandler"
```

`strategy` 可选值：

- `"abort"`：分配失败后终止程序。
- `"trap"`：分配失败后执行目标 trap。
- `"panic"`：分配失败时构造 panic 诊断并进入 panic 策略。
- `"handler"`：调用 manifest 指定的 `oom.handler`，handler 必须返回 `Never`。

规则：

- 默认分配 API 和 `In` 后缀分配 API 分配失败时调用 `oom(layout) -> Never`。
- `tryReserve` 系列返回 `Result<Unit, AllocError>`，不走 `oom`。
- `strategy = "handler"` 时 `handler` 必填。
- 若 `strategy = "panic"`，`@noPanic` 中调用可能分配的 API 必须被拒绝，除非编译器能证明不会失败。
- `kernel` / `embedded` 必须使用 `strategy = "handler"` 并提供 `oom.handler`；handler 内部可以选择 halt、reset 或目标 trap。

### trust

```toml
[trust]
ffi = true
hardware = true
inlineAsm = false
interrupts = false
dependencyTrust = false
requireReport = true
allowedPackages = ["kernel", "platform"]
```

字段含义：

- `ffi`：允许 `trust extern` 和裸 ABI 绑定。
- `hardware`：允许裸硬件地址、MMIO、端口 I/O 和链接段控制。
- `inlineAsm`：允许 inline asm。
- `interrupts`：允许中断入口、裸调用约定和目标特殊入口。
- `dependencyTrust`：允许依赖包包含 `trust` 边界。
- `requireReport`：构建必须输出信任报告。
- `allowedPackages`：限制哪些包可以包含 `trust` 边界。

默认值：

- hosted：全部底层能力默认 `false`；可以显式开启 `ffi` 写平台绑定，但不能直接开启 `hardware`、`inlineAsm` 或 `interrupts`。
- freestanding：`ffi` 可由目标打开；硬件、inline asm 和中断默认 `false`。
- kernel / embedded：可以打开硬件能力，但必须启用 `requireReport`。

manifest 只能允许某类底层能力；具体源码仍然必须写 `trust`。没有 `trust` 的裸底层操作永远非法。

## 6. 校验规则

编译器必须在类型检查前校验 manifest：

- 未知字段默认报错，避免拼写错误静默失效。
- `[dependencies]` 的 key 必须是合法 Zeno 标识符。
- dependency key 不能和当前包 `src/` 顶层目录、内建包根或其他依赖 key 冲突。
- 包依赖图不能有环。
- `target.profile` 必须是已知 profile。
- `panic.strategy = "handler"` 必须提供 `panic.handler`。
- `oom.strategy = "handler"` 必须提供 `oom.handler`。
- `panic.stack = "symbols"` 需要目标声明支持符号化，或编译器降级并给出显式警告；严格模式下必须报错。
- `allocator.default = true` 必须有可解析的默认 allocator。
- `hosted` profile 不能启用 `hardware`、`inlineAsm` 或 `interrupts`；需要这些能力时应使用 `freestanding`、`kernel`、`embedded` 或自定义低层 profile。
- `kernel` / `embedded` 必须使用 handler panic/OOM 策略，且不能默认开启 unwinding。
- `trust.dependencyTrust = false` 时，依赖包中的 `trust` 会让构建失败。
- `trust.allowedPackages` 存在时，不在列表中的包不能包含 `trust`。

## 7. 对类型检查的影响

manifest 会影响这些规则：

- 无默认 allocator 时，`String.from`、`Vector.withCapacity`、`Array.filled`、`Box.new` 和 `Shared.new` 等无 `In` 后缀 API 被拒绝。
- 显式 `module` 声明必须匹配 `src/` 下的文件路径；省略时由文件路径推断。
- `import` 根必须是内建包根或 `[dependencies]` 中声明的依赖；同包声明不需要 import。
- `panic.strategy` 决定 panic 是否产生 unwind 清理路径。
- `panic.stack` 决定 `PanicInfo.stack()` 能提供的最低诊断能力。
- `oom.strategy = "panic"` 会让可能分配的 API 在 `@noPanic` 中被拒绝。
- `trust` 字段决定 `trust` 边界内可使用的底层能力类别。
- `target.profile` 决定 hosted `std`、OS 线程、文件系统和动态初始化 `static` 是否可用。

manifest 不会改变普通所有权、move、初始化、访问值和 `Send` / `Sync` 的核心规则。

## 8. 测试 shorthand

规格测试文件可以用顶部注释模拟 manifest 字段：

```zn
// profile: hosted
// allocator.default: true
// panic.strategy: abort
// panic.stack: symbols
// oom.strategy: panic
```

这些注释只服务测试 harness。真实包不从源码注释读取 profile 策略。
