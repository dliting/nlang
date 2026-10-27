> 译文仅供参考，以英文规范为准（informative translation; the English specification is normative）。

# 2. 语法

## 2.1 记法

语法以扩展巴科斯-诺尔范式（EBNF）书写。定义具有形式`name = body ;`。在体中：

- `|`分隔备选项；
- `[ x ]`使`x`可选（零次或一次）；
- `{ x }`将`x`重复零次或多次；
- `( x | y )`分组，并在体内分隔备选项。

终结符以双引号写出其确切词素，例如`"if"`或`"+"`。记号类别以尖括号写出：`<identifier>`（§1.3）以及字面量记号`<int-literal>`、`<long-literal>`、`<ulong-literal>`、`<float-literal>`、`<double-literal>`、`<char-literal>`与`<string-literal>`（§1.5）。数组类型后缀`"[]"`与`"[,,]"`是单个记号；后者代表一个逗号数从一个起、数量任一的记号（§1.6）。类型实参位置上的`"<"`是类型实参括号，不是比较运算符（§1.6）。

开始符号是`CompileUnit`。下文每个定义都是一条语言产生式；有两处解析器恢复细节属资料性、不是语法的一部分：解析器额外接受一条`error ';'`语句以便在语法错误后重新同步；它还保留了一条已移除的`import "string";`拼写，作为报告该移除的仅诊断用产生式。

## 2.2 编译单元

```ebnf
CompileUnit     = ImportList UsingList TopMemberList ;
ImportList      = { "import" ImportTail ";" } ;
ImportTail      = ImportPath [ "." "*" ] ;
ImportPath      = <identifier> { "." <identifier> } ;
UsingList       = { Using } ;
Using           = "using" NameExpr ";"
                | "using" NameExpr "=" Type ";" ;
TopMemberList   = { TopItem } ;
TopItem         = NamespaceMember | Statement | TopBareFunc | TopModFunc ;
TopBareFunc     = TopBareHeader Paragraph
                | "func" TopBareHeader ";" ;
TopBareHeader   = ( HeadType | "void" ) <identifier> "(" [ FormalParamList ] ")" ;
TopModFunc      = TopModHeader FunctionBodyOrSemi ;
TopModHeader    = ( AccessType [ NodeFlags ] | NodeFlags )
                  ( HeadType | "void" ) <identifier> "(" [ FormalParamList ] ")" ;
```

顶层函数不带访问修饰符且不带标志时即为*裸函数*（第7章）；`TopModHeader`要求二者至少居其一，这正是区分两种形式的判据。

## 2.3 类型与类型实参

```ebnf
Type            = ( NameExpr | QualifiedType
                  | IdentifierExpr "<" TypeArgs ">"
                  | "func" "<" TypeArgs ">" )
                  { "[]" | "[,,]" } ;
TypeArgs        = TypeList | "void" | "void" "," TypeList ;
TypeList        = TypeArg { "," TypeArg } ;
TypeArg         = [ "out" ] Type ;
TypeName        = NameExpr | QualifiedType ;
NameExpr        = IdentifierExpr ;
QualifiedType   = NameExpr "." <identifier> { "." <identifier> } ;
NameList        = TypeName { "," TypeName } ;
HeadType        = ( IdentifierExpr [ "<" TypeArgs ">" ]
                  | "func" "<" TypeArgs ">" | MemberExpr )
                  { "[]" | "[,,]" } ;
```

`TypeArgs`允许`void`作为第一个类型实参（泛型类型的无结果形式），也允许`void`后接其余实参；哪些泛型类型接受哪些形式由第4章定义。`HeadType`是局部声明中可用的类型拼写形式；它与`Type`的唯一区别是允许以`MemberExpr`为基础（经由成员表达式到达的限定名）。

## 2.4 声明

```ebnf
NamespaceMember = EnumDecl | StructDecl | ClassDecl | InterfaceDecl ;
AccessType      = "private" | "protected" | "public" ;
NodeFlag        = "const" | "static" | "virtual" | "native" ;
NodeFlags       = NodeFlag { NodeFlag } ;

ClassDecl       = "class" <identifier> ClassInheritOpt ImplementsOpt
                  "{" ClassMemberList "}" ;
ClassInheritOpt = [ ":" TypeName ] ;
ImplementsOpt   = [ "implements" NameList ] ;
ClassMemberList = ClassMember { ClassMember } ;
ClassMember     = [ AccessType ]
                  ( [ NodeFlags ] MethodDecl | FieldDecl ) ;
MethodDecl      = [ Type | "void" ] <identifier>
                  "(" [ FormalParamList ] ")" FunctionBodyOrSemi ;
FieldDecl       = Type <identifier> [ "=" Expression ] ";" ;

StructDecl      = "struct" <identifier> "{" StructMemberList "}" ;
StructMemberList = StructMember { StructMember } ;
StructMember    = StructField | StructCtor ;
StructField     = [ AccessType ] Type <identifier> [ "=" Expression ] ";" ;
StructCtor      = [ AccessType ] [ NodeFlags ] <identifier>
                  "(" [ FormalParamList ] ")" FunctionBodyOrSemi ;

InterfaceDecl   = "interface" <identifier>
                  "{" [ InterfaceMemberList ] "}" ;
InterfaceMemberList = InterfaceMember { InterfaceMember } ;
InterfaceMember = [ AccessType ] [ NodeFlags ]
                  ( ( Type | "void" ) <identifier>
                    "(" [ FormalParamList ] ")" FunctionBodyOrSemi
                  | ConstDecl ) ;
ConstDecl       = Type <identifier> [ "=" Expression ] ";" ;

EnumDecl        = "enum" <identifier> "{" EnumMemberList EnumMethodSection "}" ;
EnumMemberList  = EnumMember { "," EnumMember } ;
EnumMember      = <identifier> [ "=" Expression ] ;
EnumMethodSection = [ ";" [ EnumMethodList ] ] ;
EnumMethodList  = EnumMethod { EnumMethod } ;
EnumMethod      = [ AccessType ] ( Type | "void" ) <identifier>
                  "(" [ FormalParamList ] ")" FunctionBodyOrSemi ;

FormalParamList = FormalParam { "," FormalParam } ;
FormalParam     = [ NodeFlags ]
                  ( "out" Type <identifier>
                  | Type <identifier> [ "=" Expression ] ) ;
FunctionBodyOrSemi = Paragraph | ";" ;
```

`class`体要求至少一个成员；`interface`体可以为空。不带返回类型的方法声明命名一个构造器（第8章）。`out`参数不接受默认值。

## 2.5 语句

```ebnf
Statement       = ";" | ReturnStmt | InvokeStmt | LocalDeclStmt
                | AssignStmt | CompoundAssignStmt | AssertStmt
                | SubscriptAssignStmt | IfStmt | WhileStmt | DoStmt
                | ForStmt | ForeachStmt | SwitchStmt | BreakStmt
                | ContinueStmt | TryStmt | ThrowStmt | SuperCallStmt
                | Paragraph ;
StatementList   = { Statement } ;
Paragraph       = "{" StatementList "}" ;

LocalDeclStmt   = [ "const" ] HeadType LocalDeclList ";" ;
LocalDeclList   = LocalDecl { "," LocalDecl } ;
LocalDecl       = <identifier>
                  [ "=" Expression | "=" BracedInitExpr
                  | "(" [ ConcreteParamList ] ")"
                  | "[" Expression [ "," ConcreteParamList ] "]"
                    [ "=" Expression | "=" BracedInitExpr ] ] ;

AssignStmt      = ( IdentifierExpr | MemberExpr )
                  "=" ( Expression | BracedInitExpr ) ";" ;
CompoundAssignStmt = ( IdentifierExpr | MemberExpr | SubscriptExpr )
                     CompoundOp Expression ";" ;
CompoundOp      = "+=" | "-=" | "*=" | "/=" | "%=" | "<<=" | ">>=" ;
SubscriptAssignStmt = Expression "[" Expression "]"
                      "=" ( Expression | BracedInitExpr ) ";"
                | Expression "[" ConcreteParamList "]" "=" Expression ";" ;
InvokeStmt      = InvokeExpr ";" | MemberExpr ";" ;
SuperCallStmt   = "super" "(" [ ConcreteParamList ] ")" ";" ;
ReturnStmt      = "return" ( Expression | BracedInitExpr ) ";"
                | "return" ";" ;

IfStmt          = "if" "(" Expression ")" Statement
                  [ "else" Statement ] ;
WhileStmt       = "while" "(" Expression ")" Statement ;
DoStmt          = "do" Statement "while" "(" Expression ")" ";" ;
ForStmt         = "for" "(" InitFor ";" Expression ";" FiniFor ")" Statement ;
InitFor         = ε | Type LocalDeclList | InvokeExpr
                | IdentifierExpr "=" Expression ;
FiniFor         = ε | InvokeExpr | IdentifierExpr "=" Expression
                | IdentifierExpr CompoundOp Expression ;
ForeachStmt     = "foreach" "(" Type <identifier> "in"
                  ( Expression | BracedInitExpr ) ")" Statement ;

SwitchStmt      = "switch" "(" Expression ")"
                  "{" CaseClauseList DefaultCase "}" ;
CaseClauseList  = { CaseClause } ;
CaseClause      = "case" CaseLabelList ":" StatementList ;
CaseLabelList   = Expression { "," Expression } ;
DefaultCase     = [ "default" ":" StatementList ] ;

TryStmt         = "try" Statement CatchClauseList FinallyClauseOpt ;
CatchClauseList = { CatchClause } ;
CatchClause     = "catch" "(" Type <identifier> ")" Statement ;
FinallyClauseOpt = [ "finally" Statement ] ;
ThrowStmt       = "throw" [ Expression ] ";" ;
AssertStmt      = "assert" "(" Expression ")" ";" ;
BreakStmt       = "break" ";" ;
ContinueStmt    = "continue" ";" ;
```

`for`语句要求给出条件：`for (i = 0; ; i += 1)`是语法错误。`else`归属于最近的外层`if`（§2.7）。`ε`表示空体。

## 2.6 表达式

```ebnf
Expression      = ParenthesesExpr | MemberExpr | LiteralExpr
                | InvokeExpr | IdentifierExpr | SubscriptExpr
                | "this" | "null"
                | "[" [ InitEntryList ] "]"
                | <identifier> "{" [ InitEntryList ] "}"
                | <identifier> "<" TypeList ">" "{" [ InitEntryList ] "}"
                | Expression BinaryOp Expression
                | UnaryOp Expression
                | Expression "as" TypeName
                | "func" "<" TypeList ">" "(" [ ConcreteParamList ] ")" ;
BinaryOp        = "+" | "-" | "*" | "/" | "%"
                | "<" | "<=" | ">" | ">=" | "<<" | ">>"
                | "==" | "!=" | "&&" | "||" ;
UnaryOp         = "-" | "+" | "!" ;

ParenthesesExpr = "(" Expression ")" ;
LiteralExpr     = <int-literal> | <long-literal> | <ulong-literal>
                | <float-literal> | <double-literal>
                | <char-literal> | <string-literal>
                | "true" | "false" ;
IdentifierExpr  = <identifier> | TypeKeyword ;
TypeKeyword     = "int" | "byte" | "ubyte" | "short" | "ushort"
                | "uint" | "long" | "ulong" | "float" | "double"
                | "string" | "bool" | "char" ;
MemberExpr      = Expression "." ( InvokeExpr | IdentifierExpr ) ;
InvokeExpr      = <identifier> "(" [ ConcreteParamList ] ")" ;
SubscriptExpr   = Expression "[" ( Expression | ConcreteParamList ) "]" ;

BracedInitExpr  = "{" InitEntries "}" ;
InitEntries     = [ InitEntryList ] ;
InitEntryList   = InitEntry { "," InitEntry } ;
InitEntry       = Expression [ ":" ( Expression | BracedInitExpr ) ]
                | BracedInitExpr ;

ConcreteParamList = ConcreteParam { "," ConcreteParam } ;
ConcreteParam   = <identifier> "=" ( Expression | BracedInitExpr )
                | "out" <identifier>
                | Expression | BracedInitExpr ;
```

`IdentifierExpr`容纳基本类型关键字，使类型名可以出现在期望表达式头部的位置（静态风格的成员访问与类型实参头部）。`func<...>(...)`形式是函数值表达式（第7章）。

## 2.7 运算符优先级

运算符按下列分组结合，从最高优先级到最低；组内适用所列的结合性。

| 优先级 | 运算符 | 结合性 |
|---|---|---|
| 调用、下标、成员 | `( )` `[ ]` `.` | 左结合；调用`( )`不可结合 |
| 转换 | `as` | 左结合 |
| 一元 | `!` `-` `+` | 右结合 |
| 乘性 | `*` `/` `%` | 左结合 |
| 加性 | `+` `-` | 左结合 |
| 移位 | `<<` `>>` | 左结合 |
| 关系 | `<` `<=` `>` `>=` | 左结合 |
| 相等 | `==` `!=` | 左结合 |
| 逻辑与 | `&&` | 左结合 |
| 逻辑或 | `\|\|` | 左结合 |

复合赋值是语句而非表达式：其右操作数是一个完整的`Expression`，因此复合赋值不嵌套。`if`的`else`绑定到最近的、尚无`else`的外层`if`。
