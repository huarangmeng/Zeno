# Zeno 自举计划

完成规范里程碑后，第一版实现是基于 C++20 和 LLVM 的 stage0 编译器。

## 1. stage0 目标

stage0 必须做到：

- 解析 `.zn` 源文件。
- 生成带源码 span 的可用诊断。
- 解析模块、名字、类型和接口。
- 检查 `Copy`、move、初始化和访问规则。
- 单态化泛型函数和类型。
- 把检查后的 Zeno IR 降低到 LLVM IR。
- 编译足够多的 `core`，以运行 compile-pass、compile-fail 和基础 run-pass 测试。

stage0 暂不需要：

- 一开始就自举。
- 高级增量编译。
- 完整 IDE 协议支持。
- 稳定 ABI。
- 宏展开。

## 2. 推荐架构

```text
source
  -> 词法器
  -> 解析器
  -> AST
  -> name resolution
  -> type/interface checking
  -> ownership + initialization checking
  -> access + escape checking
  -> typed HIR
  -> monomorphization
  -> MIR
  -> LLVM IR
  -> object / executable
```

AST、HIR 和 MIR 应与 LLVM 解耦，方便未来自举前端复用同一套概念管线。

## 3. stage0 语言子集

最小子集：

- 模块与导入。
- 函数。
- `let`、`var`、块、`if`、`while`、`for`。
- 基础类型。
- 结构体和枚举。
- `Option` 和 `Result`。
- `try`，包括 `Result` / `Option` 限定传播、`okOr` 和 `mapErr` 检查。
- `Copy`、move 和确定性销毁。
- `Array`、`Vector`、`ArraySlice`、`mut` 访问和 `move` 所有权转移。
- 整数 wrapping 运算、checked 运算、无损 `as` 转换检查、`fromChecked` / `truncate` / `saturate` 转换 API。
- `trust` 边界、`trust extern` 和信任报告的基础记录。
- 带接口约束的泛型函数。
- 静态接口派发。
- `Send` / `Sync` 自动推导和 `trust impl` 记录。

可以推迟到后续 stage0 迭代：

- 接口类型参数派发。
- 完整闭包捕获分析。
- OS 线程 API。
- async 语法和任务运行时。
- hosted 文件系统 API。
- C ABI 包装生成器。
- 高级模式匹配。

## 4. C++20 实现建议

推荐组件：

- 手写词法器和解析器，以获得控制力和诊断质量。
- 编译器 AST/HIR 节点使用 arena 分配。
- intern 标识符。
- 持久源码映射。
- 接口求解使用约束图。
- `Send` / `Sync` 推导作为接口求解的一部分处理，并在并发边界生成专门诊断。
- 初始化和 move 状态使用数据流分析。
- inferred access scope 使用 region-like 内部表示。
- 早期测试可以使用 LLVM ORC 或普通 object emission。

不要让 C++ 实现便利性反过来限制语言设计。

## 5. IR 策略

HIR 应保留语言语义：

- 所有权转移。
- 销毁点。
- 访问区域。
- 整数转换检查结果和钳制转换。
- 泛型约束。
- `Send` / `Sync` 自动推导结果和 `trust impl` 来源。
- 模式穷尽性。

MIR 应显式化 lowering 细节：

- 控制流图。
- 类 SSA 临时值。
- `try` 的成功边、提前返回边和清理边。
- 部分初始化 drop flag。
- 边界检查。
- 单态化函数。
- 必要的接口派发调用。

LLVM IR 应接收已经检查过的内存安全操作。不能指望 LLVM 在 lowering 后恢复 Zeno 的安全语义。

## 6. 自举路径

阶段 A：规范和示例。

阶段 B：C++20 stage0 编译器能编译小程序。

阶段 C：stage0 编译 `core` 和规格测试套件。

阶段 D：用受限 Zeno 子集编写词法器、解析器和诊断库。

阶段 E：stage0 编译 Zeno 前端。

阶段 F：Zeno 前端 + C++/LLVM 后端编译自身。

阶段 G：逐步替换后端组件，或保留 LLVM 作为生产后端。

当 Zeno 编写的编译器能够从源码编译出下一个等价 Zeno 编译器时，认为自举完成。

## 7. 性能验收

编译器最终应证明：

- 泛型抽象能像 C++ template 一样单态化和内联。
- 非逃逸闭包不分配。
- 访问检查除了无法证明的边界检查外都在编译期完成。
- 典型 `ArraySlice<T>` / `Array<T>` / `Vector<T>` 循环中的冗余边界检查被消除。
- 普通整数 `+`、`-`、`*` 不生成隐藏溢出检查。
- `checkedAdd` 等方法使用目标支持的 overflow intrinsic 或等价高效 lowering。
- `truncate` 降低为截断，`saturate` 降低为比较/选择或目标饱和指令。
- RAII 销毁降低为直接调用，没有隐藏引用计数。
- 接口派发只在短期接口访问参数或显式接口拥有者中出现，例如 `writer: Writer`、`Box<Writer>`、`Shared<Writer>`；`W: Writer` 这样的泛型约束保持静态派发。
- `Box<T>` 到 `Box<Interface>` 的擦除转换不重新分配；`Shared<T>` 到 `Shared<Interface>` 的擦除转换不增加引用计数。

## 8. trust 交付与审计

第一版编译器应支持用户包中的 `trust` 边界。编译器发行包仍可包含 intrinsic 声明，但不再是用户写底层代码的唯一入口。

构建应记录：

- 编译器版本。
- 编译器发行包 hash。
- 目标 triple。
- 启用 profile：`freestanding`、`hosted` 或自定义。
- 分配器和 panic 策略。
- 每个 `trust` 边界的位置、能力类别和公开 API 影响范围。
