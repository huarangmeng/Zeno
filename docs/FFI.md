# Zeno 外部 ABI 与 FFI

Zeno 的原则是：普通 Zeno ABI 服务优化，外部 ABI 必须显式，`pub` 不等于符号导出。

## 1. ABI 分层

普通 Zeno 函数：

```zn
pub fn add(a: I32, b: I32) -> I32 {
    return a + b;
}
```

规则：

- `pub` 只表示源码 API 对外部 package 可见。
- 普通 Zeno 函数没有稳定外部 ABI。
- 编译器可以按优化需要选择寄存器传参、sret、inlining、monomorphization 和 name mangling。
- 重载、泛型和访问模式只参与 Zeno 内部解析与符号生成，不创建外部符号承诺。

外部 ABI 只能通过显式属性或 `trust extern` 进入。

## 2. 导出 C ABI

导出给 C、动态库、系统 loader 或其他语言时使用 `@export`：

```zn
@export("zeno_add", abi: C)
pub fn add(a: I32, b: I32) -> I32 {
    return a + b;
}
```

规则：

- `@export` 只能标在非泛型顶层函数上。
- `@export` 的第一个参数是外部符号名字符串。
- `abi: C` 表示使用目标平台 C ABI。
- 导出函数必须是 `pub`。
- 导出函数不能重载同一个外部符号。
- 导出函数的参数和返回类型必须是 C-compatible。
- 导出函数不能让 panic unwind 穿过 ABI 边界。

重载函数导出时必须显式不同符号：

```zn
@export("read_u8", abi: C)
pub fn read(value: U8) -> U8 {
    return value;
}

@export("read_u16", abi: C)
pub fn read(value: U16) -> U16 {
    return value;
}
```

## 3. 导入 C ABI

裸外部声明必须写成 `trust extern`：

```zn
trust extern "C" fn read(fd: I32, buffer: USize, length: USize) -> ISize;
```

规则：

- `trust extern` 只能出现在允许 FFI trust 能力的包中。
- 裸 `extern` 没有 `trust` 必须被拒绝。
- extern 函数的参数和返回类型必须是 C-compatible。
- 调用 extern 函数本身是底层行为，必须放在小的 `trust` 块或受信封装中。
- 为立即 FFI 调用从 `ArraySlice<T>`、`StringSlice` 或句柄取出 ABI 地址属于 `ffi` 能力；构造任意地址、裸指针偏移或解引用属于 `rawMemory` 能力。
- 受信封装对外应该暴露 `Result`、handle、slice 或普通值，而不是裸地址和平台错误码。

## 4. C-compatible 类型

可以跨 C ABI 的类型：

- 基础整数、浮点和目标定义清楚的 `Bool` / `Char` 表示。
- `Unit` 作为无返回值时对应 C `void`。
- `@layout(C)` 结构体，且所有字段都是 C-compatible。
- 单真实字段零成本包装，且内部字段是 C-compatible。
- core/std 明确标注为 C-compatible 的句柄类型。

默认不允许跨 C ABI：

- `String`、`StringSlice`。
- `Array<T>`、`Vector<T>`、`ArraySlice<T>`。
- `Box<T>`、`Shared<T>`、`Mutex<T>`、同步 guard。
- 静态接口参数/返回、`Box<Interface>`、`Shared<Interface>`。
- 闭包 / callable。
- 带 `destroy` 的资源拥有类型。
- 泛型类型参数未单态化为明确 C-compatible 类型的签名。

需要传缓冲区给 C 时，普通 API 应接受 `ArraySlice<U8>` 或 `mut ArraySlice<U8>`，在很小的 `trust` 块里取出 raw address：

```zn
pub fn readInto(fd: FileDescriptor, mut out: ArraySlice<U8>) -> Result<USize, IoError> {
    val n = trust {
        c_read(fd.value, out.rawAddress(), out.len)
    };
    return os.decodeReadResult(n);
}
```

## 5. Panic 与清理

`@export(..., abi: C)` 函数不能让 panic unwind 穿过外部 ABI。

允许的策略：

- 当前 profile 是 abort 或 trap，panic 直接终止，不跨边界 unwind。
- 导出函数标注 `@noPanic`，编译器验证可达调用图不会 panic。
- 导出函数内部捕获语言级失败并转换为错误码或 handle 状态；完整 unwind-catching 机制延后设计。

如果 profile 允许 unwind 且导出函数没有 `@noPanic` 或等价边界策略，编译器必须拒绝。

## 6. 所有权与 handle

不要直接把 Zeno 资源拥有者跨 C ABI 传递。用不透明 handle 或 C-compatible 包装：

```zn
@layout(C)
pub struct FileHandle {
    pub value: I32,
}
```

规则：

- C 侧获得的是 handle，不拥有 Zeno 内部对象布局。
- 创建、使用和销毁 handle 应通过成对导出函数表达。
- 如果 handle 实际指向 Zeno 分配对象，必须提供显式 destroy/free 导出函数。
- 所有资源所有权转换都应在受信封装中建模，不能靠 C 侧猜测 Zeno 对象布局。

## 7. 符号与 name mangling

Zeno 内部符号可以包含包名、模块路径、重载键、访问模式和泛型实参。内部 mangling 不稳定。

外部符号只来自 `@export` 的字符串：

```zn
@export("zeno_init", abi: C)
pub fn init() -> I32 {
    return 0;
}
```

两个导出不能使用同一个外部符号名。导出符号冲突是编译错误。

## 8. 诊断

编译器应为 FFI 提供明确诊断：

- `pub` 函数未导出时，不应暗示它有外部符号。
- `@export` 用在泛型、方法、闭包或非 `pub` 函数上时报错。
- `@export` 签名包含非 C-compatible 类型时报错，并指出具体类型。
- 导出函数可能 panic unwind 穿过 C ABI 时报错。
- `trust extern` 被 manifest 禁止时报错，并指出缺少的 trust 能力。
- extern 函数调用不在 `trust` 块中时报错。
