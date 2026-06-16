# Zeno

Zeno 是一门规划中的高性能系统级语言。当前仓库是第一阶段交付物：语言规范、设计约束、示例程序和规格测试草案。

Zeno 的核心目标：

- 可自举：最终用 Zeno 实现 Zeno 编译器前端。
- 高性能：默认无隐藏成本，泛型默认单态化，抽象应接近 C/C++ 生成代码。
- 系统级：支持 freestanding 目标，核心语言不要求 GC、异常或强制运行时。
- 安全默认：底层 FFI、裸指针和硬件访问必须写在显式 `trust` 边界中。
- 非 Rust 式体验：使用所有权、RAII、访问模式和逃逸分析获得内存安全，但不把显式生命周期标注作为日常写法。

## 仓库结构

- [docs/SPEC.md](docs/SPEC.md)：主语言规范。
- [docs/GRAMMAR.md](docs/GRAMMAR.md)：词法与语法草案。
- [docs/MANIFEST.md](docs/MANIFEST.md)：`Zeno.toml` 项目 manifest、profile 和 target 策略。
- [docs/SAFETY.md](docs/SAFETY.md)：内存安全、数据竞争安全和 `trust` 信任边界模型。
- [docs/STDLIB.md](docs/STDLIB.md)：核心标准库边界。
- [docs/CONCURRENCY.md](docs/CONCURRENCY.md)：OS 线程、任务运行时和 async/Future 模型。
- [docs/BOOTSTRAP.md](docs/BOOTSTRAP.md)：C++20 + LLVM stage0 与自举路线。
- [docs/TESTING.md](docs/TESTING.md)：规格测试、性能验收和诊断约定。
- [examples](examples)：规范示例程序。
- [tests/spec](tests/spec)：compile-pass / compile-fail 规格用例草案。

## 当前状态

这是一个规范优先的仓库，目前还没有编译器实现。

第一个实现里程碑是 C++20 stage0 编译器：解析 `.zn` 文件，检查 Zeno 的所有权和访问规则，然后降低到 LLVM IR。
