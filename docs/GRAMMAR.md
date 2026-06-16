# Zeno 语法草案

这份语法草案刻意保持小而清晰，便于 stage0 使用手写递归下降解析器或 Pratt 解析器。它还不是完整形式化语法，但仓库中的所有语法示例都应该能落在这个形状里。

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

同一作用域中可以出现多个同名 `fn_decl`，但它们必须形成合法重载集。重载键包含函数名、参数数量、参数类型和参数访问模式；返回类型不参与重载键。同一个重载键重复声明必须在语义阶段报错。

`@layout(Source)`、`@layout(C)`、`@layout(Packed(1))` 和 `@export("symbol", abi: C)` 都按普通属性解析。布局参数和 `abi` 值不是关键字；语义阶段检查它们的可用位置和含义。

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
generic_args    = "<" type ("," type)* ","? ">" ;
access_type     = "mut" type ;
tuple_type      = "(" type "," type ("," type)* ","? ")" ;
fn_type         = "Fn" "<" fn_type_args ">" ;
fn_type_args    = type_list "," type ;

type_list       = type ("," type)* ;
const_expr      = literal | path ;
```

示例：

```zn
Array<U8>
Vector<U8>
ArraySlice<U8>
Box<Node>
Writer
Box<ThreadWorker>
Shared<SharedWorker>
Fn<I32, I32>
MutFn<I32, Unit>
OnceFn<Result<Unit, Error>>
Iterator<U8>
```

接口名在类型位置上表示接口访问类型。泛型约束中的 `W: Writer` 表示静态派发约束。

接口访问类型不使用额外关键字。`writer: Writer` 是动态接口访问，`W: Writer` 是静态泛型约束。
接口能力组合不使用 `+`。需要组合能力时定义命名接口：

```zn
interface ThreadWorker: Worker, Send {}
interface SharedWorker: Worker, Send, Sync {}
```

`Box<Writer>` 不自动等价于 `Box<ThreadWorker>`；接口对象保留哪些能力必须由接口类型本身表达。
裸接口访问类型是短期访问值，语义阶段必须拒绝它出现在长期存储位置，例如结构体字段、枚举载荷、元组字段、`static`、集合元素、逃逸闭包、线程、任务或 async future 捕获。需要长期保存时使用泛型字段 `W: Writer`，或显式拥有者 `Box<Writer>` / `Shared<Writer>`。
裸接口访问不拥有具体对象，语义阶段必须拒绝 `move writer: Writer` 这类参数；需要接收拥有式异构接口值时使用 `move writer: Box<Writer>`，共享所有权使用 `Shared<Writer>`。

语法允许接口方法使用 `Self`、方法级泛型和 `move self`，但语义阶段必须把这些方法标记为静态专用方法：

- `Self` 出现在非接收者位置的方法不能通过接口访问类型动态调用。
- 带方法级泛型参数的方法不能通过接口访问类型动态调用。
- `move self` 接收者方法不能通过接口访问类型动态调用。

## 5. 泛型参数

```ebnf
generic_params  = "<" generic_param ("," generic_param)* ","? ">" ;
generic_param   = ident constraint? ;
constraint      = ":" interface_bound ("," interface_bound)* ;
interface_bound = path_type ;
```

```zn
fn sort<T: Ord, Copy>(mut items: ArraySlice<T>) { ... }
struct Map<K: Hash, Eq, V> { ... }
```

泛型参数列表中的逗号既分隔参数，也分隔约束。解析器可以先保留原始名字序列，语义阶段再根据符号表区分“接口约束”和“新的类型参数”。例如 `Map<K: Hash, Eq, V>` 中，`Hash` 和 `Eq` 是已知接口，因此是 `K` 的约束；`V` 是新的泛型类型参数。

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

参数模式语义：

- 没有模式的参数是只读访问。
- `mut` 参数是唯一可写访问，调用点必须写 `mut`。
- `move` 参数接收所有权，调用点必须写 `move`。
- `self` 接收者是只读访问当前对象。
- `mut self` 接收者是唯一可写访问当前对象；方法调用点不写 `mut`，但接收者必须是可写位置。
- `move self` 接收者接收当前对象所有权；方法调用后原接收者不可再用。

方法可以重载。方法重载键除了普通参数形状，还包含接收者类型和接收者模式。`fn open(self)`、`fn open(mut self)` 和 `fn open(move self)` 是不同重载键；同一个调用点只能选择其中唯一一个最佳候选。

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
stmt            = let_stmt
                | var_stmt
                | return_stmt
                | break_stmt
                | continue_stmt
                | defer_stmt
                | expr_stmt ;

let_stmt        = "let" pattern type_annotation? "=" expr ";" ;
var_stmt        = "var" pattern type_annotation? "=" expr ";" ;
type_annotation = ":" type ;
return_stmt     = "return" expr? ";" ;
break_stmt      = "break" expr? ";" ;
continue_stmt   = "continue" ";" ;
defer_stmt      = "defer" expr ";" ;
expr_stmt       = expr ";" ;
```

## 10. 表达式

表达式解析应使用优先级。

```ebnf
expr            = if_expr
                | match_expr
                | while_expr
                | for_expr
                | closure_expr
                | await_expr
                | try_expr
                | trust_expr
                | mut_expr
                | move_expr
                | assignment ;

await_expr      = "await" expr ;
try_expr        = "try" expr ;
trust_expr      = "trust" block ;
mut_expr        = "mut" expr ;
move_expr       = "move" expr ;

if_expr         = "if" expr block ("else" (if_expr | block))? ;
while_expr      = "while" expr block ;
for_expr        = "for" for_binding "in" expr block ;
for_binding     = for_binding_mode? pattern ;
for_binding_mode = "mut" | "move" ;
match_expr      = "match" expr "{" match_arm* "}" ;
match_arm       = pattern "=>" expr_or_block ","? ;

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
                | ".." ;

cast_expr       = prefix_expr ("as" type)* ;
prefix_expr     = ("!" | "-") prefix_expr
                | postfix_expr ;

postfix_expr    = primary_expr postfix_part* ;
postfix_part    = call_args
                | index_args
                | "." ident ;

call_args       = "(" argument_list? ")" ;
index_args      = "[" expr "]" ;
argument_list   = expr ("," expr)* ","? ;

primary_expr    = literal
                | path
                | type_static_expr
                | struct_literal
                | tuple_expr
                | unit_expr
                | block ;

type_static_expr = type "." ident call_args? ;
struct_literal  = path "{" field_init* "}" ;
field_init      = ident ":" expr "," ;
tuple_expr      = "(" expr "," expr ("," expr)* ","? ")" ;
unit_expr       = "(" ")" ;
```

`as` 的语法只表示显式转换请求。语义阶段只允许无损数值转换；可能截断或改变符号含义的转换必须使用 `T.fromChecked(value)`、`T.truncate(value)` 或 `T.saturate(value)`。

闭包使用函数形参数列表，而不是 `|...|`。块闭包写作 `(params) -> ReturnType { ... }`，返回类型可推断时可省略为 `(params) { ... }`；短表达式闭包写作 `(params) => expr`。解析器应通过 `=>`、`->` 或参数列表后的 `{` 识别闭包，和普通括号表达式区分。

`try` 的语法只表示提前返回请求。语义阶段只允许：

- 在返回 `Result<U, E>` 的函数中对 `Result<T, E>` 使用 `try`。
- 在返回 `Option<U>` 的函数中对 `Option<T>` 使用 `try`。

`try` 不触发隐式 `Option` 到 `Result` 转换，也不触发隐式错误类型转换。

`for` 绑定上的 `mut` / `move` 表示元素访问模式；`in` 右侧的 `mut expr` / `move expr` 表示集合访问模式。语义阶段必须检查二者匹配，例如 `for mut item in mut items` 和 `for move item in move items`。

## 11. 运算符

推荐优先级，从高到低：

```text
postfix:       call, index, field
prefix:        ! - move mut try trust
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

`&` 是按位与运算符，不是访问或所有权语法。Zeno 的访问通过参数模式和调用点 `mut` / `move` 表达，不使用 `&T` 或 `&mut T`。

## 12. 模式

```ebnf
pattern         = "_"
                | ident
                | literal
                | tuple_pattern
                | enum_pattern ;

tuple_pattern   = "(" pattern ("," pattern)+ ","? ")" ;
enum_pattern    = path "(" pattern_list? ")" ;
pattern_list    = pattern ("," pattern)* ;
```

示例：

```zn
match result {
    Ok(value) => value,
    Err(_) => fallback,
}
```

## 13. stage0 解析器要求

第一版解析器必须：

- 保留源码 span，方便诊断。
- 把 `move` 和调用点 `mut` 解析成表达式形式。
- 把 `trust { ... }` 解析成信任边界表达式。
- 把接口名在类型位置上解析成接口访问类型。
- 把 `W: Writer` 这样的泛型约束保留为静态派发约束。
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
- 裸指针、裸地址、inline asm、volatile/MMIO 和中断入口等底层操作只能出现在 `trust` 边界内。
- `trust` 不关闭普通类型检查、move 检查、初始化检查或可证明的访问检查。
- 编译器必须为 `trust` 边界保留源码 span，用于信任报告和构建策略。
- `@noAlloc` 是普通用户属性，表示被标注函数不能直接或间接执行堆分配。
- `@export("symbol", abi: C)` 是普通用户属性，表示把非泛型顶层 `pub fn` 导出为外部 C ABI 符号；语义阶段必须检查 C-compatible 签名和 panic 边界。
