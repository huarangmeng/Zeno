# Zeno 语法冻结草案

这份语法草案是 stage0 parser 的冻结入口。它刻意保持小而清晰，便于使用手写递归下降解析器和 Pratt 表达式解析器实现。本文不是机器可直接生成 parser 的完整形式化语法；但仓库中的所有 `.zn` 示例都必须能落在这个形状里，新增语法必须先更新本文。

包级 `Zeno.toml` manifest 是构建配置，不属于 `.zn` 源码语法。

## 1. Token

空白用于分隔 token，本身没有语义。

行注释：

```zn
// 注释
```

块注释：

```zn
/* 注释 */
```

标识符：

```text
ident = [A-Za-z][A-Za-z0-9]*
```

单独的 `_` 是丢弃模式，不是普通标识符。Zeno 源码标识符不使用 snake_case。

整数 literal：

```text
123
0xff
0b1010
1_000_000
```

字符串 literal 默认是 UTF-8 `StringSlice`：

```zn
"hello"
```

```ebnf
literal         = integer_literal
                | float_literal
                | string_literal
                | char_literal
                | "true"
                | "false" ;
```

## 2. 文件

```ebnf
source_file     = module_decl? import_decl* decl* ;
module_decl     = "module" path ";" ;
import_decl     = "import" path import_items? ";" ;
path            = ident ("." ident)* ;
import_items    = ".{" ident ("," ident)* ","? "}" ;
```

包模式构建中 `module_decl` 可以省略。省略时模块路径由 `src/` 下的文件路径推断；写出时必须和文件路径一致。

`import path;` 导入外部依赖或内建包中的模块绑定。`import path.{A, B};` 从外部模块导入多个公开项。同 package 内声明直接可用，不需要 import。v1 不支持 wildcard import、import alias 或 `pub import` re-export。

## 3. 声明

```ebnf
decl            = attribute* visibility? (
                    fn_decl
                  | struct_decl
                  | enum_decl
                  | interface_decl
                  | impl_decl
                  | const_decl
                  | static_decl
                  | type_alias
                  | trust_extern_decl
                  | trust_impl_decl
                  ) ;

attribute       = "@" ident attribute_args? ;
attribute_args  = "(" attribute_arg_list? ")" ;
attribute_arg_list = attribute_arg ("," attribute_arg)* ","? ;
attribute_arg   = ident ":" attribute_value
                | attribute_value ;
attribute_value = ident attribute_args? | literal ;

visibility      = "pub" | "private" ;

fn_decl         = async_marker? "fn" ident generic_params? fn_params return_type? block ;
async_marker    = "async" ;
const_decl      = "const" ident ":" type "=" expr ";" ;
static_decl     = "static" ident ":" type "=" const_expr ";" ;
type_alias      = "type" ident generic_params? "=" type ";" ;
trust_extern_decl = "trust" "extern" string_literal "fn" ident fn_params return_type? ";" ;
trust_impl_decl = "trust" impl_decl ;
```

同一作用域中可以出现多个同名 `fn_decl`，但它们必须形成合法重载集。重载键包含函数名、参数数量、参数类型和参数访问模式；返回类型不参与重载键。同一个重载键重复声明必须在语义阶段报错。虽然调用点有 `move` 实参标记，只靠只读参数和 `move` 参数区分的重载集仍然会降低可读性，必须报错。

`@layout(Source)`、`@layout(C)`、`@layout(Packed(1))`、`@export("symbol", abi: C)` 和 `@export("symbol", bridge: C)` 都按普通属性解析。布局参数、`abi` 值和 `bridge` 值不是关键字；语义阶段检查它们的可用位置和含义。

## 4. 类型

```ebnf
type            = fn_type
                | access_type
                | primitive_type
                | self_type
                | path_type
                | tuple_type
                ;

primitive_type  = "Bool"
                | "I8" | "I16" | "I32" | "I64" | "ISize"
                | "U8" | "U16" | "U32" | "U64" | "USize"
                | "F32" | "F64"
                | "Char"
                | "String"
                | "StringSlice"
                | "Unit"
                | "Never" ;

self_type       = "Self" ;
path_type       = path generic_args? ;
generic_args    = "<" generic_arg ("," generic_arg)* ","? ">" ;
generic_arg     = type | expr ;
access_type     = "mut" type ;
tuple_type      = "(" type "," type ("," type)* ","? ")" ;
fn_type         = "Fn" "<" fn_type_args ">" ;
fn_type_args    = type_list "," type ;

type_list       = type ("," type)* ;
const_expr      = expr ;
```

示例：

```zn
Array<U8>
Vector<U8>
ArraySlice<U8>
Box<Node>
Box<ThreadWorker>
Shared<SharedWorker>
Fn<I32, I32>
MutFn<I32, Unit>
OnceFn<Result<Unit, Error>>
Iterator<U8>
Consumer<Event>
```

接口名可以在函数参数和函数返回类型位置表示静态接口泛型。泛型约束中的 `W: Writer` 表示命名静态派发约束；显式拥有者 `Box<Writer>` 表示动态接口对象。

接口能力组合不使用 `+`。需要组合能力时定义命名接口：

```zn
interface ThreadWorker: Worker, Send {}
interface SharedWorker: Worker, Send, Sync {}
```

`Box<Writer>` 不自动等价于 `Box<ThreadWorker>`；接口对象保留哪些能力必须由接口类型本身表达。
裸接口类型只能出现在函数参数和函数返回类型位置。参数位置 `writer: Writer` 等价于隐藏的 `W: Writer` 参数，默认单态化；返回位置 `-> Writer` 表示编译器推断的静态接口返回类型。
裸接口类型不能出现在结构体字段、枚举载荷、元组字段、`static`、集合元素、逃逸闭包、线程、任务或 async future 捕获等长期存储位置。需要长期保存具体实现时使用泛型字段 `W: Writer`，需要拥有异构实现时使用显式拥有者 `Box<Writer>`，共享所有权使用 `Shared<Writer>`。
需要消耗具体实现时可以写 `move writer: Writer` 或 `move writer: W`。需要接收拥有式异构接口值时写 `move writer: Box<Writer>`。

泛型接口参数的类型实参在语义阶段求解：

```zn
fn feed<T>(mut consumer: Consumer<T>, move item: T) { ... }
```

解析器只保留 `Consumer<T>` 的语法形状；语义阶段把它展开成隐藏 `C: Consumer<T>`，再从普通参数和接口实现共同推断 `T` 与 `C`。

语法允许接口方法使用 `Self`、方法级泛型和 `move self`，但语义阶段必须把这些方法标记为静态专用方法：

- `Self` 出现在非接收者位置的方法不能通过 `Box<Interface>` / `Shared<Interface>` 动态调用。
- 带方法级泛型参数的方法不能通过 `Box<Interface>` / `Shared<Interface>` 动态调用。
- `move self` 接收者方法不能通过 `Box<Interface>` / `Shared<Interface>` 动态调用。

## 5. 泛型参数

```ebnf
generic_params  = "<" generic_param ("," generic_param)* ","? ">" ;
generic_param   = type_generic_param | const_generic_param ;
type_generic_param = ident constraint? ;
const_generic_param = "const" ident ":" type ;
constraint      = ":" interface_bound ;
interface_bound = path_type ;
```

```zn
interface SortKey: Ord, Copy {}
interface HashKey: Hash, Eq {}

fn sort<T: SortKey>(mut items: ArraySlice<T>) { ... }
struct Map<K: HashKey, V> { ... }
struct RingBuffer<T, const Capacity: USize> { ... }
```

泛型参数列表中的逗号只分隔泛型参数，不分隔约束。一个类型泛型参数只允许一个直接约束；多能力约束必须先定义成命名接口组合。`const` 泛型参数必须写显式类型，实参必须在编译期求值。

## 6. 结构体

```ebnf
struct_decl     = "struct" ident generic_params? copy_marker? struct_body ;
copy_marker     = ":" "Copy" ;
struct_body     = "{" field_decl* "}" ;
field_decl      = visibility? ident ":" type "," ;
```

```zn
pub struct Point: Copy {
    pub x: F64,
    pub y: F64,
}

@layout(Source)
struct HotState {
    hotA: U64,
    hotB: U64,
    coldFlag: Bool,
}

@layout(Packed(1))
struct Header {
    tag: U8,
    length: U16,
}
```

未标注布局的结构体使用默认 Auto layout，语义阶段允许编译器重排字段。`Source` 保留源码字段顺序，`C` 遵守目标 C ABI，`Packed(N)` 用于字节级格式并限制字段访问方式。

## 7. 枚举

```ebnf
enum_decl       = "enum" ident generic_params? "{" enum_variant* "}" ;
enum_variant    = ident enum_payload? "," ;
enum_payload    = "(" type_list? ")" | struct_body ;
```

```zn
enum Result<T, E> {
    Ok(T),
    Err(E),
}
```

## 8. 接口与 impl 块

```ebnf
interface_decl  = "interface" ident generic_params? interface_parents? "{" interface_item* "}" ;
interface_parents = ":" interface_bound ("," interface_bound)* ;
interface_item  = fn_signature ";" ;

impl_decl       = "impl" generic_params? impl_target "{" impl_item* "}" ;
impl_target     = type | path_type "for" type ;
impl_item       = fn_decl | const_decl | destroy_decl ;
destroy_decl    = "destroy" block ;

fn_signature    = "fn" ident generic_params? fn_params return_type? ;
fn_params       = "(" param_list? ")" ;
param_list      = param ("," param)* ","? ;
param           = param_mode? ident ":" type
                | receiver_param ;
param_mode      = "mut" | "move" ;
receiver_param  = "self" | "mut" "self" | "move" "self" ;
return_type     = "->" type ;
```

`destroy` 只能写成 `destroy { ... }`，不能带参数、返回类型、泛型参数或 async 标记；它的 no-fail、no-alloc 和 no-panic 语义由语义阶段检查。

参数模式语义：

- 没有模式的参数是只读访问。
- `mut` 参数是唯一可写访问，调用点必须写 `mut`。
- `move` 参数接收所有权；从已有命名位置传入时调用点必须写 `move`，非 `Copy` 实参在调用后不可再用。函数体内的 `move` 参数是唯一拥有者，可以作为 `mut` 接收者或 `mut` 实参使用。
- `self` 接收者是只读访问当前对象。
- `mut self` 接收者是唯一可写访问当前对象；已有命名接收者调用时写成 `mut receiver.method(...)`，且接收者必须是可写位置。
- `move self` 接收者接收当前对象所有权；已有命名接收者调用时写成 `move receiver.method(...)`，方法调用后原接收者不可再用。

方法可以重载。方法重载键除了普通参数形状，还包含接收者类型和接收者模式。同一重载集中不能只靠只读参数和 `move` 参数区分；接收者模式也不能造成调用点无法唯一选择的歧义。

```zn
interface Writer {
    fn write(mut self, bytes: ArraySlice<U8>) -> Result<USize, IoError>;
}

impl Writer for File {
    fn write(mut self, bytes: ArraySlice<U8>) -> Result<USize, IoError> {
        return os.writeFile(self, bytes);
    }
}
```

## 9. 语句

Zeno 块中包含语句。块的最后一个表达式可以作为块值。

```ebnf
block           = "{" stmt* expr? "}" ;
stmt            = val_stmt
                | var_stmt
                | const_stmt
                | return_stmt
                | break_stmt
                | continue_stmt
                | expr_stmt ;

val_stmt        = "val" pattern type_annotation? ("=" expr)? ";" ;
var_stmt        = "var" pattern type_annotation? ("=" expr)? ";" ;
const_stmt      = "const" ident type_annotation? "=" expr ";" ;
type_annotation = ":" type ;
return_stmt     = "return" expr? ";" ;
break_stmt      = "break" expr? ";" ;
continue_stmt   = "continue" ";" ;
expr_stmt       = expr ";" ;
expr_or_block   = expr | block ;
```

没有初始化表达式的 `val` / `var` 是延迟初始化声明，只允许单个名字 pattern，并且必须写类型标注；其他 pattern 必须立即初始化。

## 10. 表达式

表达式解析应使用优先级。

```ebnf
expr            = if_expr
                | match_expr
                | while_expr
                | for_expr
                | async_expr
                | closure_expr
                | await_expr
                | try_expr
                | trust_expr
                | move_receiver_call
                | mut_expr
                | assignment ;

async_expr      = "async" capture_marker? block ;
await_expr      = "await" expr ;
try_expr        = "try" expr ;
trust_expr      = "trust" block ;
move_receiver_call = "move" method_receiver "." ident call_args? ;
method_receiver = primary_expr method_receiver_part* ;
method_receiver_part = call_args | index_args | "." ident ;
mut_expr        = "mut" expr ;

if_expr         = "if" if_condition block ("else" (if_expr | block))? ;
if_condition    = val_condition | expr ;
val_condition   = "val" pattern_mode? pattern "=" expr ;
while_expr      = "while" while_condition block ;
while_condition = val_condition | expr ;
for_expr        = "for" for_binding "in" expr block ;
for_binding     = for_binding_mode? pattern ;
for_binding_mode = pattern_mode ;
match_expr      = "match" match_mode? expr "{" match_arm* "}" ;
match_mode      = pattern_mode ;
match_arm       = pattern match_guard? "=>" expr_or_block ","? ;
match_guard     = "if" expr ;
pattern_mode    = "mut" | "move" ;

closure_expr    = capture_marker? closure_params (return_type? block | "=>" expr) ;
capture_marker  = "move" ;
closure_params  = "(" closure_param_list? ")" ;
closure_param_list = closure_param ("," closure_param)* ","? ;
closure_param   = pattern type_annotation? ;

assignment      = binary_expr (assign_op assignment)? ;
assign_op       = "=" | "+=" | "-=" | "*=" | "/=" | "%=" ;

binary_expr     = cast_expr (binary_op cast_expr)* ;
binary_op       = "||" | "&&" | "|" | "^" | "&"
                | "==" | "!=" | "<" | "<=" | ">" | ">="
                | "<<" | ">>" | "+" | "-" | "*" | "/" | "%"
                | ".." | "..=" ;

cast_expr       = prefix_expr ("as" type)* ;
prefix_expr     = ("!" | "-") prefix_expr
                | postfix_expr ;

postfix_expr    = primary_expr postfix_part* ;
postfix_part    = call_args
                | index_args
                | "." ident ;

call_args       = "(" argument_list? ")" ;
index_args      = "[" expr "]" ;
argument_list   = argument ("," argument)* ","? ;
argument        = argument_mode? expr ;
argument_mode   = "mut" | "move" ;

primary_expr    = literal
                | type_static_expr
                | struct_literal
                | path
                | tuple_expr
                | paren_expr
                | unit_expr
                | block ;

type_static_expr = type "." ident call_args? ;
struct_literal  = path_type "{" field_init_list? "}" ;
field_init_list = field_init ("," field_init)* ","? ;
field_init      = ident ":" expr ;
tuple_expr      = "(" expr "," expr ("," expr)* ","? ")" ;
paren_expr      = "(" expr ")" ;
unit_expr       = "(" ")" ;
```

`as` 的语法只表示显式转换请求。语义阶段只允许无损数值转换；可能截断或改变符号含义的转换必须使用 `T.fromChecked(value)`、`T.truncate(value)` 或 `T.saturate(value)`。

闭包使用函数形参数列表，而不是 `|...|`。块闭包写作 `(params) -> ReturnType { ... }`，返回类型可推断时可省略为 `(params) { ... }`；短表达式闭包写作 `(params) => expr`。解析器应通过 `=>`、`->` 或参数列表后的 `{` 识别闭包，和普通括号表达式区分。

`async { ... }` 是 async block，产生 `Future<T>`，不会启动线程或任务。`async move { ... }` 是移动捕获的 async block，适合保存为命名 future 或返回 future。

当调用参数的期望类型是 `Future<T>` 时，Zeno 允许直接写 block 实参作为 Future block 简写：

```zn
runtime.spawn({
    return work();
});

runtime.spawn({
    return process(move work);
});
```

上面都是 Future block 实参。`spawn` 仍然是普通函数调用，括号不能省略。这个简写只在期望类型明确为 `Future<T>` 的参数位置生效；普通 block 仍然是立即求值的块表达式。

这个 Future block 简写也适用于“期望返回类型是 `Future<T>`”的闭包体，例如 `runtime.spawnWithContext((ctx) { ... })`。Future block 的 `return` 返回 future 的输出，不返回外层函数。

Future block 实参的捕获按逃逸 future 处理：`Copy` 值复制，非 `Copy` owner 自动移动进 future 状态。捕获后外层继续使用该 owner 是编译错误。编译器不会自动 clone、不会自动引用计数、不会自动装箱；需要共享时必须显式写 `Shared`、`Mutex`、原子类型或 `clone()`。

`await` 只在 async 上下文中有效。语义阶段只允许等待 `Future<T>`、`Task<T>` 或立即等待的 async `mut self` 调用。`await future` 和 `await task` 会消费已有命名拥有者；等待后原绑定不可再用。

`try` 的语法只表示提前返回请求。语义阶段只允许：

- 在返回 `Result<U, E>` 的函数中对 `Result<T, E>` 使用 `try`。
- 在返回 `Option<U>` 的函数中对 `Option<T>` 使用 `try`。

`try` 不触发隐式 `Option` 到 `Result` 转换，也不触发隐式错误类型转换。

调用实参上的 `mut` / `move` 表示参数访问模式。`mut receiver.method(...)` 表示调用 `mut self` 方法并取得已有命名接收者的短期唯一可写访问；`move receiver.method(...)` 表示调用 `move self` 方法并消费已有命名接收者。`await mut receiver.method(...)` 表示立即等待一次 async `mut self` 调用；这个调用产生的 future 不能被命名、保存或逃逸。`await task` 等待并消费 `Task<T>` 句柄，不使用 `Task.await()` 方法。`move` 实参只用于从已有命名位置传给 `move` 参数；临时值、字面量、结构体字面量和函数返回值传给 `move` 参数时不需要额外标记。`Thread.spawn(move () { ... })` 这类闭包字面量中的 `move` 属于闭包捕获标记，不是实参标记；把已经命名的闭包任务传入时才写 `Thread.spawn(move task)`。`return move value`、`val owner = move value` 和独立 `move value;` 无效。

`match`、`if (expr is pattern)`、`while (expr is pattern)` 和 `for` 绑定上的 `mut` / `move` 表示 pattern 访问模式；`for` 的 `in` 右侧只保留 `mut expr` 这种可写访问形式。消耗遍历写成 `for move item in items`，由 `for move` 本身表示消耗右侧拥有者。

## 11. 运算符

推荐优先级，从高到低：

```text
postfix:       call, index, field
prefix:        ! - async mut try trust
cast:          as
multiplicative:* / %
additive:      + -
shift:         << >>
comparison:    < <= > >=
equality:      == !=
bit_and:       &
bit_xor:       ^
bit_or:        |
logic_and:     &&
logic_or:      ||
range:         ..
assignment:    = += -= *= /= %=
```

`&` 是按位与运算符，不是访问或所有权语法。Zeno 的访问通过参数模式、调用点 `mut` 和控制流里的 `move` 模式表达，不使用 `&T` 或 `&mut T`。

## 12. 模式

```ebnf
pattern         = pattern_alt ("|" pattern_alt)* ;
pattern_alt     = "_"
                | ident
                | literal_pattern
                | range_pattern
                | tuple_pattern
                | struct_pattern
                | enum_pattern
                | path_pattern ;

literal_pattern = literal ;
range_pattern   = pattern_const (".." | "..=") pattern_const ;
pattern_const   = literal | path ;
tuple_pattern   = "(" pattern ("," pattern)+ ","? ")" ;
struct_pattern  = path "{" field_pattern_list? "}" ;
field_pattern_list = field_pattern ("," field_pattern)* ","? ;
field_pattern   = ident | ident ":" pattern ;
enum_pattern    = path "(" pattern_list? ")" ;
path_pattern    = path ;
pattern_list    = pattern ("," pattern)* ","? ;
```

示例：

```zn
match result {
    Ok(value) => value,
    Err(_) => fallback,
}
```

`match` guard 写在 pattern 后：

```zn
match event {
    Event.Key(code) | Event.Code(code) if code < 10 => code,
    Event.Key(10..=20) => 20,
    Event.At(Point { x, y }) => x + y,
}
```

## 13. stage0 解析器要求

第一版解析器必须：

- 保留源码 span，方便诊断。
- 把调用点 `mut` / `move` 实参标记、可写接收者 `mut receiver.method(...)`、消费接收者 `move receiver.method(...)`、闭包捕获、`for` 绑定和 `match move` 这类控制流模式解析为不同 AST 节点；`move` 不作为普通表达式前缀。
- 把 `trust { ... }` 解析成信任边界表达式。
- 把 `W: Writer` 这样的泛型约束保留为静态派发约束。
- 支持 `const` 泛型参数、常量泛型实参、泛型结构体字面量和类型静态方法调用，例如 `RingBuffer<U8, pageSize> { ... }` 与 `Vector<U8>.withCapacity(4)`。
- 支持括号表达式、tuple、unit、结构体 pattern、or-pattern、range pattern 和 match guard。
- 把函数参数位置的裸接口名展开为匿名静态接口参数。
- 把函数返回位置的裸接口名记录为静态接口返回，并在语义阶段检查所有返回路径的具体类型一致。
- 拒绝裸接口名出现在字段、集合元素、`static` 等长期存储类型位置。
- 在普通包中拒绝 `unsafe`，并给出明确诊断。
- 支持声明、泛型参数和字面量中的尾逗号。
- 让语法层与 LLVM 解耦，方便未来自举前端复用。

## 14. trust 语法

`trust` 是 Zeno 唯一的底层信任边界关键字。

```ebnf
trust_expr      = "trust" block ;
trust_extern_decl = "trust" "extern" string_literal "fn" ident fn_params return_type? ";" ;
```

规则：

- 裸 `extern` 声明必须写成 `trust extern`。
- 裸指针、裸地址、inline asm、volatile/MMIO 和中断入口等底层操作只能出现在 `trust` 边界内，并且需要 manifest 对应能力。
- `trust` 不关闭普通类型检查、move 检查、初始化检查或可证明的访问检查。
- `trust impl` 只允许用于编译器认可的标记接口，例如 `Send` / `Sync` 这类需要人工证明的平台或线程安全不变量；普通接口实现不能写成 `trust impl`。
- 编译器必须为 `trust` 边界保留源码 span，用于信任报告和构建策略。
- `@noAlloc` 是普通用户属性，表示被标注函数不能直接或间接执行堆分配。
- `@export("symbol", abi: C)` 是普通用户属性，表示把非泛型顶层 `pub fn` 导出为严格外部 C ABI 符号；语义阶段必须检查 C-compatible 签名和 panic 边界。
- `@export("symbol", bridge: C)` 是普通用户属性，表示把非泛型顶层 `pub fn` 通过生成的 C ABI thunk 导出；语义阶段必须检查 bridge-compatible 签名、错误码映射、panic 边界和无隐藏成本 lowering。
