# Zeno 包解析、Lockfile 与可复现构建

Zeno 的包系统目标是：依赖解析确定、构建输入可复现、trust 变化可审计。系统软件不能依赖“今天 registry 解析结果碰巧一样”。

## 1. 包身份

包身份来自 `Zeno.toml`：

```toml
[package]
name = "net"
version = "1.2.0"
kind = "library"
```

规则：

- `package.name` 用于诊断、lockfile、发布和信任报告。
- `package.kind` 区分应用和库；应用默认入口来自 `src/main.zn` 的 `main`，库没有入口。
- dependency key 是当前包源码中的 import 根，可以不同于依赖包名。
- `package.version` 使用 SemVer 格式，发布到 registry 的包必须提供。
- path-only 私有包可以省略 version，但如果被 workspace 发布命令选中，必须补齐。

## 2. 依赖声明

`[dependencies]` 支持四类依赖：

```toml
[dependencies]
std = "builtin"
platform = { path = "../platform" }
hash = { git = "https://example.com/hash.git", rev = "4f9c2a..." }
log = { version = "1.2.3" }
```

规则：

- `"builtin"` 表示编译器发行包提供的内建依赖。
- `{ path = "..." }` 表示本地路径依赖。
- `{ git = "...", rev = "..." }` 表示 git 依赖，必须锁到 commit hash。
- `{ version = "..." }` 表示 registry 依赖。
- v1 不允许 git 依赖只写 branch、tag 或 floating ref。
- v1 不允许 registry 依赖使用开放版本范围；必须写精确版本。
- dependency key 必须是合法 Zeno 标识符，作为源码 import 根。
- dependency key 不能和当前包 `src/` 顶层目录、内建包根或其他依赖 key 冲突。

后续可以加入版本范围，但 lockfile 仍必须记录解析出的精确版本和内容 hash。

stage0 MVP 子集：

- 支持 `"builtin"`、path dependency 和 workspace member。
- `core` 始终隐式可用，不进入 `[dependencies]`；`alloc` / `std` 可以作为 builtin 依赖写入用于审计。
- 不做 registry 下载、git fetch、版本范围求解或发布协议。
- 如果 manifest 中出现 `{ git = ... }` 或 `{ version = ... }`，stage0 应给 staged diagnostic，而不是尝试联网解析。

## 3. Workspace

多包仓库使用 workspace：

```toml
[workspace]
members = ["core", "compiler", "tools/*"]
```

规则：

- workspace root 包含根 `Zeno.toml`。
- `members` 是相对 workspace root 的目录模式。
- 每个 member 仍有自己的 `Zeno.toml` 和 `src/`。
- 同 workspace 内成员依赖优先解析到本地 member。
- workspace 共享一个 `Zeno.lock`。
- workspace 不能包含两个同名 package；发现同名 member 必须拒绝。

## 4. Zeno.lock

`Zeno.lock` 记录完整解析结果。应用、工具、内核、编译器和 workspace 必须提交 lockfile；纯库包可以不提交，但发布校验必须能生成 lockfile。

stage0 必须实现 lockfile 的本地 frozen 校验：

- 支持 `path:...` 和 `builtin:name` 来源。
- 校验 manifest hash、源码内容 hash、依赖边、compiler identity、builtin package hash、target/profile 和 trust 能力摘要。
- 默认构建使用 frozen lockfile；lockfile 缺失或过期时报错。
- 开发命令可以后续提供 `--update-lock` 生成本地 lockfile；第一批不需要联网更新。
- 纯库包本地检查可以没有 lockfile，但进入 workspace、CI、发布或编译器自举构建时必须生成并校验 lockfile。

示例：

```toml
version = 1
root = "app"
compiler = "zeno-stage0 0.1.0"

[[package]]
name = "app"
version = "0.1.0"
source = "path:."
manifestHash = "sha256:..."
contentHash = "sha256:..."
dependencies = [
  { key = "platform", package = "platform" },
  { key = "std", package = "std" },
]

[[package]]
name = "platform"
version = "0.1.0"
source = "path:../platform"
manifestHash = "sha256:..."
contentHash = "sha256:..."
trust = ["ffi", "rawMemory"]

[[package]]
name = "std"
version = "builtin"
source = "builtin:std"
compilerPackageHash = "sha256:..."
trust = []
```

每个 package 记录：

- `name`、`version`。
- `source`：`path:...`、`git:...#rev`、`registry:name@version` 或 `builtin:name`。
- `manifestHash`。
- `contentHash`：源码和构建相关文件的内容 hash。
- 依赖边：源码 import key 到解析后 package 的映射。
- trust 能力摘要：例如 `ffi`、`rawMemory`、`hardware`、`inlineAsm`、`interrupts`、`threadSafety`。
- 对 registry 包，还要记录 registry URL、包内容 hash 和签名/校验信息。
- 对 git 包，必须记录 commit hash，不能记录 branch 作为最终来源。

lockfile 不记录机器绝对路径。path 依赖以 workspace root 或当前包根为基准记录可移植相对路径。

## 5. 解析算法

解析顺序：

1. 找到 workspace root 或当前包 root。
2. 读取 root/member `Zeno.toml`。
3. 展开 workspace members。
4. 解析 builtin 依赖。
5. 解析 path 依赖。
6. 解析 git / registry 依赖到精确包。
7. 构建包依赖图。
8. 检查 dependency key 冲突、包名冲突和依赖环。
9. 校验 trust 策略。
10. 生成或校验 `Zeno.lock`。

默认构建要求 lockfile 是最新的。开发命令可以提供显式 `--update-lock` 更新 lockfile；CI / release 命令必须使用 frozen lockfile。

## 6. Trust 审计

lockfile 必须记录依赖图中每个包使用的 trust 能力。

构建策略：

- `trust.dependencyTrust = false` 时，依赖包出现任何 trust 能力都必须失败，除非它在 `allowedPackages` 中。
- `trust.requireReport = true` 时，构建必须输出信任报告。
- lockfile 更新时，如果 trust 能力集合扩大，工具必须突出显示。
- CI 可以配置为拒绝 lockfile 中新增 trust 能力。

这让平台包、驱动包和 FFI 包可以存在，但普通应用能看见它们。

## 7. SemVer 与兼容性

Zeno 使用 SemVer，但系统语言兼容性比普通库更严格。

patch 版本不能破坏：

- `pub` API 名称、类型签名和可见性。
- `pub const` 值、常量泛型参数和可见 CTFE 结果的稳定 fingerprint。
- `@layout(C)` 类型的字段、顺序、大小、对齐和 C-compatible 状态。
- `@export(..., bridge: C)` 生成的 thunk 低层签名、头文件片段、错误码映射和 bridge-compatible 状态。
- bindgen 输入摘要，包括前端类型 `c` / `cxx`、header hash、include path、宏定义、target triple、目标 ABI、Clang 资源目录、生成器版本和 Zeno 版本。
- `@export` 外部符号名、ABI、参数和返回类型。
- manifest 中声明的 profile 要求和 trust 能力集合。

minor 版本可以新增 `pub` API，但不能破坏已有 API。

major 版本可以破坏 API，但 lockfile 必须记录精确版本，不能静默升级。

`0.x` 版本不自动兼容；v1 registry 依赖要求精确版本，所以不会用兼容范围猜测。

## 8. 可复现构建

同一输入应产生确定构建输入集合：

```text
Zeno.lock
Zeno.toml
源码内容 hash
compiler identity
target triple / cpu / features
profile
内建包 hash
```

规则：

- registry 包必须用内容 hash 校验。
- git 包必须锁到 commit hash。
- builtin 包必须记录 compiler package hash。
- lockfile 校验失败时，默认构建拒绝继续。
- 构建缓存 key 必须包含 manifest hash、lockfile hash、compiler identity、target 和 trust 配置。

字节级完全相同输出还依赖 debug info、时间戳、链接器和目标平台；v1 先保证构建输入可复现，并要求 release 模式默认禁用非确定时间戳。

## 9. 延后内容

v1 暂不设计：

- 开放版本范围求解。
- registry 认证、发布协议和签名细节。
- 特性 feature unification。
- binary artifact 依赖。
- 多版本同包同时进入同一构建图。

这些可以后续加入，但不能破坏 lockfile 的可审计性。
