# 2. Syntax

## 2.1 Notation

The grammar is written in extended Backus–Naur form (EBNF). A
definition has the shape `name = body ;`. In a body:

- `|` separates alternatives;
- `[ x ]` makes `x` optional (zero or one);
- `{ x }` repeats `x` zero or more times;
- `( x | y )` groups and separates alternatives inside a body.

A terminal is written in double quotes with its exact lexeme, such
as `"if"` or `"+"`. A token class is written in angle brackets:
`<identifier>` (§1.3) and the literal tokens `<int-literal>`,
`<long-literal>`, `<ulong-literal>`, `<float-literal>`,
`<double-literal>`, `<char-literal>` and `<string-literal>` (§1.5).
The array type suffixes `"[]"` and `"[,,]"` are single tokens; the
second stands for one token with any number of commas from one
(§1.6). A `"<"` in a type-argument position is the type-argument
bracket, not the comparison operator (§1.6).

The start symbol is `CompileUnit`. Every definition below is a
language production; two parser-recovery details are informative and
not part of the grammar: the parser additionally accepts an `error
';'` statement to resynchronize after a syntax error, and it keeps a
removed `import "string";` spelling as a diagnostic-only production
that reports the removal.

## 2.2 Compilation Units

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

A top-level function is *bare* when it carries no access modifier and
no flags (chapter 7); `TopModHeader` requires at least one of the
two, which is what separates the two forms.

## 2.3 Types and Type Arguments

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

`TypeArgs` allows `void` as the first type argument (the result-less
form of a generic type) and `void` followed by the remaining
arguments; chapter 4 defines which generic types accept which forms.
`HeadType` is the type spelling usable in a local declaration; it
differs from `Type` only in admitting a `MemberExpr` base (a
qualified name reached through a member expression).

## 2.4 Declarations

```ebnf
NamespaceMember = EnumDecl | StructDecl | ClassDecl | InterfaceDecl ;
AccessType      = "private" | "protected" | "public" ;
NodeFlag        = "const" | "static" | "virtual" | "native" ;
NodeFlags       = NodeFlag { NodeFlag } ;

ClassDecl       = "class" <identifier> TypeParamsOpt ClassInheritOpt
                  ImplementsOpt "{" ClassMemberList "}" ;
TypeParamsOpt   = [ TypeParams ] ;
TypeParams      = "<" <identifier> { "," <identifier> } ">" ;
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

A `class` body requires at least one member; an `interface` body may
be empty. A method declaration with no return type names a
constructor (chapter 8). An `out` parameter takes no default value.

## 2.5 Statements

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

The `for` statement requires its condition: `for (i = 0; ; i += 1)`
is a syntax error. An `else` belongs to the nearest enclosing `if`
(§2.7). `ε` denotes the empty body.

## 2.6 Expressions

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

`IdentifierExpr` admits the primitive type keywords so that a type
name can appear where an expression head is expected (static-style
member access and type-argument heads). The `func<...>(...)` form is
a function value expression (chapter 7).

## 2.7 Operator Precedence

Operators bind in the following groups, from highest precedence to
lowest; within a group the listed associativity applies.

| Precedence | Operators | Associativity |
|---|---|---|
| call, subscript, member | `( )` `[ ]` `.` | left; the call `( )` non-associative |
| cast | `as` | left |
| unary | `!` `-` `+` | right |
| multiplicative | `*` `/` `%` | left |
| additive | `+` `-` | left |
| shift | `<<` `>>` | left |
| relational | `<` `<=` `>` `>=` | left |
| equality | `==` `!=` | left |
| logical AND | `&&` | left |
| logical OR | `\|\|` | left |

Compound assignment is a statement, not an expression: its right
operand is a complete `Expression`, so compound assignments do not
nest. The `else` of an `if` binds to the nearest enclosing `if` that
has no `else` yet.
