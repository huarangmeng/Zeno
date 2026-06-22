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

- 固定宽度整数 `I8` / `I16` / `I32` / `I64` / `U8` / `U16` / `U32` / `U64`。
- 指针宽度整数 `ISize` / `USize`，按目标 C ABI 的 `intptr_t` / `uintptr_t` 等价表示处理。
- 浮点 `F32` / `F64`。
- `Bool`，按目标 C ABI 的 C `_Bool` 表示处理。
- `Unit` 作为无返回值时对应 C `void`。
- `@layout(C)` 结构体，且所有字段都是 C-compatible，并且类型本身没有自定义 `destroy`。
- core/std 明确标注为 C-compatible 的句柄类型。

默认不允许跨 C ABI：

- `Char`。需要跨 C 表示字符时使用明确宽度的整数或 `@layout(C)` 包装。
- `String`、`StringSlice`。
- `Array<T>`、`Vector<T>`、`ArraySlice<T>`。
- `Box<T>`、`Shared<T>`、`Mutex<T>`、同步 guard。
- 静态接口参数/返回、`Box<Interface>`、`Shared<Interface>`。
- 闭包 / callable。
- 带 `destroy` 的资源拥有类型。
- 普通 enum，包括 `Option<T>`、`Result<T, E>` 和用户 enum；C enum / 固定整数 enum layout 不进入 v1。
- 没有 `@layout(C)` 的普通 struct，即使它是单字段零成本包装。
- 泛型类型参数未单态化为明确 C-compatible 类型的签名。

C ABI 签名还必须满足：

- `@export` 函数不能是泛型函数，不能有方法接收者，不能是闭包。
- `trust extern` 声明不能是泛型函数。
- 出现在 `@export` 签名中的命名结构体类型必须是 `pub`，否则外部 ABI 暴露了无法被外部用户命名的类型。
- `@layout(C)` 泛型实例只有在所有类型实参已经是具体 C-compatible 类型时才可参与 C ABI；`@export` 本身不能暴露未实例化的泛型。

需要传缓冲区给 C 时，普通 API 应接受 `ArraySlice<U8>` 或 `mut ArraySlice<U8>`，在很小的 `trust` 块里取出 raw address：

```zn
pub fn readInto(fd: FileDescriptor, mut out: ArraySlice<U8>) -> Result<USize, IoError> {
    val n = trust {
        c_read(fd.value, out.rawAddress(), out.len)
    };
    return os.decodeReadResult(n);
}
```

## 5. C bridge 导出

`abi: C` 是裸 ABI 层，故意严格。日常把 Zeno API 暴露给 C 时可以使用 bridge 导出：

```zn
@export("zeno_read", bridge: C)
pub fn read(fd: FileDescriptor, mut out: ArraySlice<U8>) -> Result<USize, IoError> {
    return fd.read(mut out);
}
```

`bridge: C` 不把 Zeno 类型直接跨 C ABI 传递，而是让编译器生成一个 C ABI thunk 和配套头文件。Zeno 函数体仍按普通 Zeno 语义检查；生成的 thunk 必须只使用 C-compatible 低层签名。

上例的 C 侧概念签名类似：

```c
int32_t zeno_read(FileDescriptor fd, uint8_t* out_data, size_t out_len, size_t* out_value);
```

bridge 导出的规则：

- 只能标在非泛型顶层 `pub fn` 上，不能是方法或闭包。
- 生成的 thunk 名使用 `@export` 的外部符号字符串；Zeno 原函数仍使用内部符号。
- bridge 转换必须是零分配、零复制、零动态派发；否则编译器必须拒绝。
- bridge thunk 不能让 panic unwind 穿过 C ABI，panic 边界策略与 `abi: C` 相同。
- thunk 的低层签名、头文件内容、target triple 和目标 C ABI 必须进入 ABI fingerprint 与增量缓存 key。
- 导出 `bridge: C` 本身不需要 manifest 开启 `trust.ffi`；只有 thunk 内部或同包封装实际声明/调用裸 `trust extern` 时才需要。

bridge-compatible 类型是比 C-compatible 更高一层的源码签名集合：

- 所有 C-compatible 类型直接通过。
- `ArraySlice<T>` 和 `mut ArraySlice<T>` 可以桥接为 `T* + USize`，其中 `T` 必须是 C-compatible 且无 Zeno 析构语义。只读 slice 在 C 头文件中生成为 `const T*`；`mut` slice 生成为 `T*`。
- `StringSlice` 可以桥接为 `const U8* + USize`，表示 UTF-8 字节和长度，不要求 nul 结尾。
- `CStr` 表示 C 的 nul-terminated 字符串；它和 `StringSlice` 是不同类型。
- `Option<T>` 只能作为 bridge 返回类型使用，且 `T` 必须 C-compatible；降低为 `bool found` 加可选 out 参数。
- `Result<Unit, E>` 可以作为 bridge 返回类型，降低为 `I32 status`。
- `Result<T, E>` 可以作为 bridge 返回类型，降低为 `I32 status + T* out`，其中 `T` 必须 C-compatible。
- `E` 必须实现核心接口 `core.ffi.CErrorCode`，由 `toCCode(self) -> I32` 明确选择 C 错误码。
- bridge 约定 `0` 表示成功；`CErrorCode.toCCode` 必须为错误返回非零状态码。编译器应对明显返回 `0` 的错误映射给出诊断，工具链可以提供 lint 检查复杂映射。

bridge 导出默认不接受：

- `String`、`Array<T>`、`Vector<T>`、`Box<T>`、`Shared<T>`、`Mutex<T>` 等拥有资源类型。
- 接口拥有者、闭包、callable、普通 enum 参数、带 `destroy` 的资源类型。
- 会要求 thunk 分配、复制、释放或保存 Zeno 对象布局的转换。

需要把拥有资源交给 C 时，应使用 handle API：

```zn
@layout(C)
pub struct FileHandle {
    pub value: I32,
}

@export("zeno_file_close", bridge: C)
pub fn close(handle: FileHandle) -> Result<Unit, IoError> {
    return File.close(handle);
}
```

如果 C 侧需要长期保存缓冲区或对象，不能使用 `ArraySlice` / `StringSlice` bridge 参数。必须使用明确所有权的 `CBuffer<T>`、`CHandle<T>` 或项目自定义 handle，并提供成对 create / destroy 函数。

## 6. bindgen 与 C++ 路线

Zeno v1 提供官方 C 绑定生成工具，但不把 C header 解析塞进语言核心：

```sh
zeno bindgen c --header unistd.h --module posix
```

生成结果必须是普通 Zeno 源码：

- `posix.raw`：`trust extern "C"` 和 C-compatible 类型，受 manifest 的 `ffi` / `rawMemory` 能力约束。
- `posix.safe`：可选安全封装，使用 `Result`、`ArraySlice`、`StringSlice`、handle 和能力对象。

`bindgen c` 的输入进入缓存 key：

- header 内容 hash。
- include path、宏定义、目标 triple、目标 C ABI、Clang 资源目录和相关编译参数。
- 生成器版本与 Zeno 版本。

C++ 需要考虑，但不进入 v1 完整实现。后续应提供独立的：

```sh
zeno bindgen cxx --header library.hpp --module library
```

C++ 路线的原则：

- 不让 Zeno 直接依赖不稳定的 C++ name mangling 或异常 ABI。
- 默认生成 C shim：C++ 自由函数、类方法、构造和析构被包装成稳定 `extern "C"` 函数。
- C++ class 默认作为不透明 handle 暴露；不会把对象布局直接暴露给 Zeno，除非类型被证明是目标 ABI 下的标准布局 POD 并显式选择。
- C++ exception 不能跨 Zeno 边界；生成 shim 必须捕获并转换为错误码或错误对象 handle。
- template 只支持显式实例化；不在 Zeno 侧表达完整 C++ 模板系统。
- overload 在生成时获得稳定 Zeno 名称和 C shim 符号，不依赖 C++ mangled name 作为公开 API。
- virtual dispatch、继承、RTTI、引用折叠和复杂生命周期规则默认不自动桥接；需要用户选择 handle 包装。

因此 v1 的语言规范只承诺 C bridge；工具链和缓存设计必须预留 `c` / `cxx` 两种 bindgen 前端，避免未来加 C++ 时破坏包缓存和 ABI fingerprint。

## 7. Panic 与清理

`@export(..., abi: C)` 函数不能让 panic unwind 穿过外部 ABI。

允许的策略：

- 当前 profile 是 abort 或 trap，panic 直接终止，不跨边界 unwind。
- 导出函数标注 `@noPanic`，编译器验证可达调用图不会 panic。
- 导出函数内部捕获语言级失败并转换为错误码或 handle 状态；完整 unwind-catching 机制延后设计。

如果 profile 允许 unwind 且导出函数没有 `@noPanic` 或等价边界策略，编译器必须拒绝。

## 8. 所有权与 handle

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

## 9. 符号与 name mangling

Zeno 内部符号可以包含包名、模块路径、重载键、访问模式和泛型实参。内部 mangling 不稳定。

外部符号只来自 `@export` 的字符串：

```zn
@export("zeno_init", abi: C)
pub fn init() -> I32 {
    return 0;
}
```

两个导出不能使用同一个外部符号名。导出符号冲突是编译错误。

## 10. 诊断

编译器应为 FFI 提供明确诊断：

- `pub` 函数未导出时，不应暗示它有外部符号。
- `@export` 用在泛型、方法、闭包或非 `pub` 函数上时报错。
- `@export` 签名包含非 C-compatible 类型时报错，并指出具体类型。
- `@export(..., bridge: C)` 签名包含非 bridge-compatible 类型时报错，并说明是否因为隐藏分配、隐藏复制、资源所有权或错误码缺失。
- 导出函数可能 panic unwind 穿过 C ABI 时报错。
- `trust extern` 被 manifest 禁止时报错，并指出缺少的 trust 能力。
- extern 函数调用不在 `trust` 块中时报错。
