# Zeno 编译模型、并行增量与缓存

Zeno 编译器目标是：前端可并行、增量失效精确、缓存安全、诊断稳定。性能不仅来自生成代码，也来自大型工程的构建速度。

## 1. 构建输入

一次构建的输入集合：

- `Zeno.toml` 内容 hash。
- `Zeno.lock` 内容 hash。
- package 源码内容 hash。
- compiler identity 和 compiler package hash。
- backend identity，包括 LLVM major 版本、target data layout provider、linker flavor 和运行时内建包版本。
- target triple、cpu、features。
- profile、allocator、panic/OOM、trust 配置。
- builtin/core/alloc/std 包 hash。
- 声明式编译期 build input 内容 hash。
- 构建模式：debug/release、优化等级、诊断严格度。

这些输入进入 build session fingerprint。任何一项变化，都不能复用不安全缓存。

## 2. 编译单元

Zeno 的语义编译单元是 package。

原因：

- 同 package 内声明默认直接可见。
- 同 package 文件可以互相引用签名。
- 可见性、重载集合、接口 impl、layout 和 `Send` / `Sync` 推导都需要 package 级全局视图。

但物理执行必须更细：

- parse 单元：单个 `.zn` 文件。
- declaration 单元：顶层声明签名。
- body-check 单元：函数体、方法体、闭包体。
- layout 单元：每个具体类型。
- const-eval 单元：每个 `const`、`static` 初始化、常量泛型实参、属性参数和布局查询。
- mono 单元：每个单态化函数 / 方法 / 类型实例。
- codegen 单元：MIR partition 或 mono item group。

这样语义仍正确，执行可以并行和增量。

## 3. 编译流水线

推荐流水线：

```text
package graph
  -> read manifests and lockfile
  -> SourceManager / FileId / Span
  -> Lexer
  -> Parser
  -> AST
  -> Declaration Collection
  -> build package symbol table
  -> Name / Module Resolution
  -> HIR
  -> Type / Interface / Ownership Sema
  -> const evaluation
  -> layout computation
  -> monomorphization planning
  -> MIR
  -> MIR verifier and optimization
  -> monomorphization emission
  -> LLVM IR
  -> object/archive/link
```

每一阶段输出都要有 fingerprint，用于增量复用和错误定位。诊断必须携带稳定错误码、主 span、notes 和 help；缓存命中时也必须重放同一诊断。

阶段职责：

- SourceManager 是所有 span 的唯一来源；parser、HIR、MIR 和 LLVM diagnostic mapping 都不能自己重新解释路径和行列。
- AST cache 只依赖源码 hash、lexer/parser 版本和语法配置。
- HIR cache 依赖声明、名字解析、可见性、泛型和接口上下文。
- Sema cache 依赖类型上下文、接口 impl 集、layout 摘要、profile、trust 和 staged feature 配置。
- MIR cache 依赖 checked HIR、drop glue、layout、panic/OOM 策略和目标无关优化配置。
- LLVM codegen cache 依赖 mono MIR、target triple、cpu/features、LLVM major 版本、data layout 和 codegen 配置。

## 4. 并行调度

并行策略：

- package 图按拓扑层并行；依赖完成后，下游 package 可以开始。
- 同 package 内 parse 全文件并行。
- declaration collection 可以并行收集，再按模块路径和源码 span 稳定合并。
- name resolution 使用 package 级 symbol table，按声明 SCC 并行。
- 函数体检查按依赖和可见签名并行。
- layout 计算按类型依赖图 SCC 并行。
- CTFE 按依赖图 SCC 并行；同一 SCC 中的 const 初始化环必须稳定诊断。
- monomorphization 按实例并行。
- MIR 优化和 LLVM codegen 按 codegen unit 并行。

并行不能影响语义和诊断顺序。所有集合合并必须按稳定 key 排序：

```text
package name
module path
source file path
byte offset
symbol name
stable node id
```

## 5. 增量失效

编译器必须区分接口变化和实现变化。

每个 package 输出：

- `publicFingerprint`：外部可见 API、`pub` 类型、`pub` 函数签名、静态接口返回的 opaque return identity 与 layout/drop 摘要、`@layout(C)`、`@export`、C bridge thunk 摘要、trust 能力摘要。
- `packageFingerprint`：package-visible 签名、impl 集合、重载集合、const 值摘要、layout 结果。
- `bodyFingerprint`：函数体和局部实现。
- `codegenFingerprint`：MIR、优化配置、target 配置。

失效规则：

- 修改函数体但不改签名：重查该 body 和受调用内联影响的 mono/codegen；下游 package 不失效。
- 修改 package-visible 签名：当前 package 相关 body 失效；外部 package 不失效，除非该签名通过 `pub` API 泄露。
- 修改 `pub` 签名：下游依赖该 API 的 package 失效。
- 修改 `pub const` 值、常量泛型默认值或可见 CTFE 结果：依赖该值的下游 const-eval、layout、mono 和 codegen 失效。
- 修改 `pub fn -> Interface` 的隐藏具体返回类型、layout fingerprint 或 drop glue：依赖该返回值布局或接口调用的下游 mono/codegen 失效。
- 修改 `@layout(C)` 字段、大小、对齐或 C-compatible 状态：所有使用该类型的 FFI/codegen 单元失效。
- 修改 `@export(..., bridge: C)` 源码签名、生成 thunk 低层签名、头文件片段或错误码映射：依赖该外部 ABI 的 FFI/codegen 单元失效。
- 修改 bindgen 输入 header、include path、宏定义、target triple、目标 C/C++ ABI、Clang 资源目录、生成器版本或 Zeno 版本：对应生成包和依赖它的 FFI/codegen 单元失效。
- 修改声明式编译期 build input：依赖该输入的 CTFE、layout、mono 和 codegen 单元失效。
- 修改 Auto layout 类型：当前 package 依赖该 layout 的 codegen 失效；若该类型出现在 `pub` API，依赖方按 ABI 不稳定规则重新检查。
- 修改 `@export` symbol 或 ABI 签名：链接和下游 FFI consumer 失效。
- 修改 `trust` 能力：信任报告、lockfile 校验和依赖策略重新执行。
- 修改 `Zeno.lock`：package graph 和所有受影响 package 的 fingerprint 失效。

## 6. 缓存 key

缓存 key 必须包含：

```text
compiler identity
compiler package hash
backend identity
target triple / cpu / features
optimization level
profile
panic strategy
oom strategy
allocator strategy
trust config
declared build input hashes
Zeno.toml hash
Zeno.lock hash
package source hash
public/package/body/codegen fingerprint
```

缓存分层：

- parse cache：源码 hash -> AST + token spans。
- symbol cache：declaration fingerprints -> symbol table fragment。
- type cache：signature fingerprints -> typed declarations。
- layout cache：type fingerprints + target -> layout result。
- body-check cache：body fingerprint + type context -> checked HIR.
- const-eval cache：CTFE dependency fingerprint + const args + target/profile -> const value / materialized data / diagnostics.
- mono cache：generic item + concrete type args + constraints + MIR verifier facts -> mono MIR / object.
- codegen cache：MIR + target + opt config -> object/bitcode.
- pgo cache：profile data hash + source mapping -> hot/cold and layout/codegen hints.

缓存命中只能跳过计算，不能跳过必要诊断汇总。缓存中应保存可重放诊断，且诊断必须带稳定错误码和 span。

## 7. 泛型与单态化

泛型默认单态化。为了并行和缓存：

- 泛型定义 package 负责类型检查泛型体的约束正确性。
- 使用 package 负责请求具体实例。
- mono item key 包含泛型定义 stable id、类型实参 layout fingerprint、常量实参 value fingerprint、接口约束满足证明、target 和优化配置。
- 未被代码生成实际使用的类型参数不进入 codegen key。
- 只依赖 layout shape 的内部实现可以做 shape sharing；这只影响机器代码复用，不能引入隐藏动态派发。
- cold block、panic/OOM 和错误格式化路径可以 outline，避免每个 mono 实例复制大型冷路径。
- 跨 package 的 mono 实例可以由使用方生成，也可以由缓存共享；语义上不能依赖某个包提前生成。

这类似 C++ template 的性能模型，但必须有稳定 key，避免重复编译爆炸。

## 8. Codegen unit

codegen unit 目标：

- 足够小，支持并行 LLVM。
- 足够大，保留内联和优化空间。

v1 默认策略：

- 每个 package 至少一个 metadata unit。
- mono item 按模块路径和热度估计分组。
- `@export` 函数和 FFI 边界进入稳定 symbol unit。
- release LTO 可以跨 unit 优化，但不能改变语义诊断。
- identical code folding 可以合并完全等价的 mono 机器代码。
- PGO 数据只能作为布局、inline、outline 和 codegen unit 分组提示，不能改变语言语义。

## 9. 诊断稳定性

诊断必须稳定：

- 错误码稳定，例如 `E0201`。
- 主 span 选择稳定。
- 多个错误按 package、file、byte offset、错误码排序。
- 并行执行不能改变输出顺序。
- 同一错误不要因为缓存命中与否改变文案。
- 帮助信息必须基于语义原因，而不是当前实现阶段。

缓存诊断规则：

- parse/type/body/codegen 阶段都可以缓存诊断。
- 复用缓存时必须重放诊断。
- 如果下游错误由上游缺失符号引起，应抑制级联噪音，优先报告根因。

错误码分段冻结为：

```text
E0001-E0099  lexer/parser
E0100-E0199  module/import/package graph
E0200-E0299  name resolution / visibility
E0300-E0399  type checking / overload
E0400-E0499  ownership / move / initialization
E0500-E0599  access / escape / view rules
E0600-E0699  generics / interface / dispatch
E0700-E0799  layout / ABI / FFI / trust
E0800-E0899  const eval / static / compile-time
E0900-E0999  MIR verifier / codegen invariant
E1000-E1099  manifest / lockfile / package manager
E9000-E9099  staged diagnostics
```

human 诊断格式：

```text
error[E0401]: use of moved value
  --> src/main.zn:12:9
   |
10 |     consume(move data);
   |                  ---- value moved here
12 |     use(data);
   |         ^^^^ value used after move
help: clone explicitly if a second owner is required
```

`--diagnostic-format json` 使用 JSON Lines。每条诊断输出一行 JSON object，不能输出顶层数组，方便 IDE / CI 流式读取：

```json
{"schemaVersion":1,"severity":"error","code":"E0401","message":"use of moved value","stage":"sema","category":"ownership","primarySpan":{"file":"src/main.zn","startByte":118,"endByte":122,"startLine":12,"startColumn":9,"endLine":12,"endColumn":13},"labels":[{"span":{"file":"src/main.zn","startByte":72,"endByte":76,"startLine":10,"startColumn":18,"endLine":10,"endColumn":22},"message":"value moved here"}],"notes":[],"help":["clone explicitly if a second owner is required"],"isStaged":false}
```

字段规则：

- `schemaVersion` 第一版固定为 `1`。
- `severity` 取 `"error"`、`"warning"`、`"note"` 或 `"help"`。
- `code` 必须稳定，例如 `"E0401"`。
- `message` 是主诊断文案，不能依赖终端颜色或格式。
- `stage` 使用实现阶段名，例如 `"lexer"`、`"parser"`、`"module"`、`"sema"`、`"mir"`、`"codegen"`、`"manifest"`、`"package"`、`"lowering"`。
- `category` 使用错误码分段对应类别，例如 `"ownership"`、`"staged"`。
- `primarySpan` 必须存在；无源码位置的 CLI 用法错误可以使用 manifest path 或 workspace root 作为 file，并把 byte/line/column 置为 `0`。
- `labels` 是带 span 的辅助标注，按源码顺序稳定排序。
- `notes` 和 `help` 是字符串数组，顺序稳定。
- `isStaged` 标记 staged diagnostic。
- staged diagnostic 必须额外包含 `feature` 字段，例如 `"async"`、`"panic-unwind"`、`"registry"`。

span 规则：

- `startByte` / `endByte` 是文件内 UTF-8 byte offset，`endByte` 为半开区间终点。
- `startLine` / `startColumn` / `endLine` / `endColumn` 使用 1-based 行列。
- stage0 的 column 按 UTF-8 byte column 计算。未来若支持 Unicode display width，可以新增字段，不能改变旧字段语义。

staged diagnostic JSON 示例：

```json
{"schemaVersion":1,"severity":"error","code":"E9001","message":"async lowering is not implemented in stage0","stage":"lowering","category":"staged","feature":"async","primarySpan":{"file":"src/main.zn","startByte":118,"endByte":123,"startLine":12,"startColumn":5,"endLine":12,"endColumn":10},"labels":[],"notes":["async syntax is reserved for the full language"],"help":["use synchronous code in stage0 MVP"],"isStaged":true}
```

退出码规则：

- 任何 `severity = "error"` 的诊断让命令退出 `1`。
- warning / note / help 不影响退出码。
- CLI 使用错误仍退出 `2`，并可输出诊断 code，但不进入源码测试匹配。

## 10. Watch / IDE 模式

watch 模式复用同一编译数据库：

- 文件变更只重新读取受影响文件。
- 修改函数体时应避免重建 package graph。
- 修改 `Zeno.toml` 或 `Zeno.lock` 时重建 package graph。
- IDE 请求可以先返回 parse/name/type 诊断，再异步补 body/codegen 诊断。

这些是工程能力，不改变语言语义。

## 11. CLI 与产物

stage0 CLI 固定为：

```text
zeno check
zeno build
zeno test
```

通用选项：

```text
--manifest <path>
--workspace <path>
--target <triple>
--profile <hosted|freestanding|kernel|embedded>
--release
--emit mir|llvm-ir
--frozen
--update-lock
--diagnostic-format human|json
--color auto|always|never
-v / --verbose
```

默认行为：

- `--frozen` 默认开启。`--update-lock` 显式出现时才允许更新 `Zeno.lock`。
- human 诊断用于终端；json 诊断用于 IDE / CI。
- 并行执行、缓存命中和目标差异不能改变诊断排序。
- 成功退出码为 `0`，编译/测试失败为 `1`，CLI 使用错误或无效参数为 `2`。

`zeno check`：

- 解析 manifest / lockfile，构建 package graph。
- 执行 SourceManager、Lexer、Parser、AST、Declaration Collection、Name / Module Resolution、HIR、Type / Interface / Ownership Sema、MIR verifier。
- 不执行 LLVM codegen，不生成 object。
- 默认使用 frozen lockfile。
- 遇到 async lowering、panic unwind、git/registry 等 stage0 延后能力时给 staged diagnostic。

`zeno build`：

- 包含 `zeno check` 的全部步骤。
- 执行 monomorphization、MIR optimization、LLVM IR、object emission 和必要链接。
- `--release` 使用 release 优化配置。
- `--emit mir` 输出文本 MIR；`--emit llvm-ir` 输出 LLVM IR。二者用于调试和规格测试，不改变语义。
- stage0 不支持的 codegen 能力必须报 staged diagnostic，不能生成半成品二进制。

产物目录：

```text
target/<triple>/<profile>/
  bin/
  lib/
  meta/
  obj/
  mir/
  ir/
```

`kind = "application"` 产物：

```text
target/<triple>/<profile>/bin/<package-name>
target/<triple>/<profile>/meta/<package-name>.zmeta
```

application 有入口函数，默认是 `src/main.zn` 的 `main`。构建时链接为可执行程序；如果请求 `--emit mir` / `--emit llvm-ir`，额外输出 MIR / LLVM IR 调试产物。

`kind = "library"` 产物：

```text
target/<triple>/<profile>/lib/lib<package-name>.a
target/<triple>/<profile>/meta/<package-name>.zmeta
```

library 没有入口函数，不链接为可执行程序。stage0 只要求静态 archive 和 `.zmeta` 编译器元数据；动态库、稳定外部 ABI、发布包格式和二进制 artifact 依赖延后。

`.zmeta` 至少记录：

- `pub` API、package-visible 摘要和可见性。
- 类型 layout fingerprint、drop glue 摘要和 `Send` / `Sync` 证明。
- 接口、impl、泛型签名、静态接口返回 identity。
- `@export` 符号、C ABI / bridge 摘要和 trust 能力摘要。
- 依赖摘要、builtin package hash、target/profile、panic/OOM/allocator 策略。

library 中的 `@export(..., abi: C)` 或 `@export(..., bridge: C)` 会在 `.a` 中产生对应外部符号。application 也可以包含 `@export`，但符号唯一性和 ABI 边界规则仍然生效。

`zeno test`：

- stage0 默认运行 `tests/spec` 规格测试。
- 默认 `zeno test` 等价于 `zeno test --stage mvp`。
- `zeno test --stage full-spec` 运行完整规格测试。
- `--feature`、`--target` 按测试元数据过滤。
- compile-fail / manifest-fail / module-fail / package-fail / incremental-fail / codegen-fail 必须匹配 `expected-error`。
- codegen / incremental 测试读取 `case.toml`。

staged diagnostic 必须明确说明“语法或能力已保留，但当前 stage0 未实现”，并包含稳定错误码、主 span、notes/help。出现 staged diagnostic 后不能继续生成二进制产物。

示例：

```text
error[E9001]: async lowering is not implemented in stage0
  --> src/main.zn:12:5
   |
12 |     await task
   |     ^^^^^
help: async syntax is reserved; use synchronous code in stage0 MVP
```

## 12. Stage0 要求

stage0 工具链基线：

- C++20。
- LLVM 21。
- CMake + Ninja。
- host：macOS arm64、Linux x86_64。
- target：`aarch64-apple-darwin`、`x86_64-unknown-linux-gnu`。
- CLI：`zeno check`、`zeno build`、`zeno test`。

stage0 实现布局以 [BOOTSTRAP.md](BOOTSTRAP.md) 为准：

- `compiler/stage0`：C++20 编译器实现，按 `base/source/diag/lex/parse/ast/package/names/hir/sema/mir/mono/codegen/driver` 分层。
- `lib/zeno/core`、`lib/zeno/alloc`、`lib/zeno/std`：编译器发行包内建声明包。
- `runtime/stage0`：panic/OOM、默认 allocator、线程、FFI/C bridge 等必要 C++ runtime shim。

编译器内部依赖必须单向推进。`ast`、`hir` 和 `mir` 不依赖 LLVM；LLVM 只出现在 `codegen` 和必要的 driver glue 中。这样未来用 Zeno 重写前端时，可以复用 AST/HIR/MIR 概念和测试，不需要推翻后端边界。

stage0 不需要做到最完美的细粒度增量，但必须按这个模型设计数据结构：

- stable node id。
- package graph fingerprint。
- manifest/lockfile fingerprint。
- public/package/body/codegen fingerprint。
- 可并行 parse 和 codegen。
- 可缓存 layout、mono 和 codegen 结果。
- 可记录 provenance、disjoint proof、allocation region 和 escape state。
- 稳定诊断排序。

这样 stage0 后续扩展增量编译时不用推翻前端。
