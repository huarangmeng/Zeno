# Zeno 编译模型、并行增量与缓存

Zeno 编译器目标是：前端可并行、增量失效精确、缓存安全、诊断稳定。性能不仅来自生成代码，也来自大型工程的构建速度。

## 1. 构建输入

一次构建的输入集合：

- `Zeno.toml` 内容 hash。
- `Zeno.lock` 内容 hash。
- package 源码内容 hash。
- compiler identity 和 compiler package hash。
- target triple、cpu、features。
- profile、allocator、panic/OOM、trust 配置。
- builtin/core/alloc/std 包 hash。
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
- mono 单元：每个单态化函数 / 方法 / 类型实例。
- codegen 单元：MIR partition 或 mono item group。

这样语义仍正确，执行可以并行和增量。

## 3. 编译流水线

推荐流水线：

```text
package graph
  -> read manifests and lockfile
  -> parse files
  -> collect top-level declarations
  -> build package symbol table
  -> resolve imports and names
  -> type/interface checking
  -> ownership/init/access/escape checking
  -> layout computation
  -> typed HIR
  -> monomorphization planning
  -> MIR
  -> LLVM IR
  -> object/archive/link
```

每一阶段输出都要有 fingerprint，用于增量复用和错误定位。

## 4. 并行调度

并行策略：

- package 图按拓扑层并行；依赖完成后，下游 package 可以开始。
- 同 package 内 parse 全文件并行。
- declaration collection 可以并行收集，再按模块路径和源码 span 稳定合并。
- name resolution 使用 package 级 symbol table，按声明 SCC 并行。
- 函数体检查按依赖和可见签名并行。
- layout 计算按类型依赖图 SCC 并行。
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

- `publicFingerprint`：外部可见 API、`pub` 类型、`pub` 函数签名、静态接口返回的 opaque return identity 与 layout/drop 摘要、`@layout(C)`、`@export`、trust 能力摘要。
- `packageFingerprint`：package-visible 签名、impl 集合、重载集合、layout 结果。
- `bodyFingerprint`：函数体和局部实现。
- `codegenFingerprint`：MIR、优化配置、target 配置。

失效规则：

- 修改函数体但不改签名：重查该 body 和受调用内联影响的 mono/codegen；下游 package 不失效。
- 修改 package-visible 签名：当前 package 相关 body 失效；外部 package 不失效，除非该签名通过 `pub` API 泄露。
- 修改 `pub` 签名：下游依赖该 API 的 package 失效。
- 修改 `pub fn -> Interface` 的隐藏具体返回类型、layout fingerprint 或 drop glue：依赖该返回值布局或接口调用的下游 mono/codegen 失效。
- 修改 `@layout(C)` 字段、大小、对齐或 C-compatible 状态：所有使用该类型的 FFI/codegen 单元失效。
- 修改 Auto layout 类型：当前 package 依赖该 layout 的 codegen 失效；若该类型出现在 `pub` API，依赖方按 ABI 不稳定规则重新检查。
- 修改 `@export` symbol 或 ABI 签名：链接和下游 FFI consumer 失效。
- 修改 `trust` 能力：信任报告、lockfile 校验和依赖策略重新执行。
- 修改 `Zeno.lock`：package graph 和所有受影响 package 的 fingerprint 失效。

## 6. 缓存 key

缓存 key 必须包含：

```text
compiler identity
compiler package hash
target triple / cpu / features
optimization level
profile
panic strategy
oom strategy
allocator strategy
trust config
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
- mono cache：generic item + concrete type args + constraints -> mono HIR/MIR.
- codegen cache：MIR + target + opt config -> object/bitcode.
- pgo cache：profile data hash + source mapping -> hot/cold and layout/codegen hints.

缓存命中只能跳过计算，不能跳过必要诊断汇总。缓存中应保存可重放诊断，且诊断必须带稳定错误码和 span。

## 7. 泛型与单态化

泛型默认单态化。为了并行和缓存：

- 泛型定义 package 负责类型检查泛型体的约束正确性。
- 使用 package 负责请求具体实例。
- mono item key 包含泛型定义 stable id、类型实参 layout fingerprint、接口约束满足证明、target 和优化配置。
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

## 10. Watch / IDE 模式

watch 模式复用同一编译数据库：

- 文件变更只重新读取受影响文件。
- 修改函数体时应避免重建 package graph。
- 修改 `Zeno.toml` 或 `Zeno.lock` 时重建 package graph。
- IDE 请求可以先返回 parse/name/type 诊断，再异步补 body/codegen 诊断。

这些是工程能力，不改变语言语义。

## 11. Stage0 要求

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
