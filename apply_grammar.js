const fs = require('fs');
let content = fs.readFileSync('src/compiler/grammar/nlang.y', 'utf-8');

// 1. Add StructDecl and ClassDecl to NamespaceMember
content = content.replace(
  /NamespaceMember:\tNamespace \{\r?\n\t\t\t\t\$\$ = \$1;\r?\n\t\t\t\} \|\r?\n\t\t\t\tFunction \{\r?\n\t\t\t\t\$\$ = \$1;\r?\n\t\t\t\} \|\r?\n\t\t\t\tEnumDecl \{\r?\n\t\t\t\t\$\$ = \$1;\r?\n\t\t\t\} ;/,
  `NamespaceMember:\tNamespace {\r\n\t\t\t\t$$ = $1;\r\n\t\t\t} |\r\n\t\t\t\tFunction {\r\n\t\t\t\t$$ = $1;\r\n\t\t\t} |\r\n\t\t\t\tEnumDecl {\r\n\t\t\t\t$$ = $1;\r\n\t\t\t} |\r\n\t\t\t\tStructDecl {\r\n\t\t\t\t$$ = $1;\r\n\t\t\t} |\r\n\t\t\t\tClassDecl {\r\n\t\t\t\t$$ = $1;\r\n\t\t\t} ;`
);

// 2. Add StructDecl, ClassDecl, ClassMember, NewExpr, etc. after EnumMember
const structAndClassRules = `

/*
Struct type declaration.
*/
StructDecl:\tKT_Struct TT_Identifier '{' StructFieldList '}' {\r
\t\t\t\t$$ = EnNew(SnStructDecl($2, $4, @1));\r
\t\t\t} ;\r
\r
StructFieldList:\tStructFieldList StructField {\r
\t\t\t\t$1->push_back($2);\r
\t\t\t\t$$ = $1;\r
\t\t\t} |\r
\t\t\t\tStructField {\r
\t\t\t\t$$ = EnNew(PtrList<SnStructField>());\r
\t\t\t\t$$->push_back($1);\r
\t\t\t} ;\r
\r
StructField:\tNameExpr TT_Identifier ';' {\r
\t\t\t\t$$ = EnNew(SnStructField($1, $2, @1));\r
\t\t\t} ;\r
\r
\r
/*
Class type declaration.
*/
ClassDecl:\tKT_Class TT_Identifier ClassInheritOpt '{' ClassMemberList '}' {\r
\t\t\t\t$$ = EnNew(SnClassDecl($2, $3, $5, @1));\r
\t\t\t} ;\r
\r
ClassInheritOpt:\t':' NameExpr { $$ = $2; } |\r
\t\t\t\t{ $$ = nullptr; } ;\r
\r
ClassMemberList:\tClassMemberList ClassMember {\r
\t\t\t\t$1->push_back($2);\r
\t\t\t\t$$ = $1;\r
\t\t\t} |\r
\t\t\t\tClassMember {\r
\t\t\t\t$$ = EnNew(PtrList<SnField>());\r
\t\t\t\t$$->push_back($1);\r
\t\t\t} ;\r
\r
ClassMember:\tAccessType NodeFlags NameExpr TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {\r
\t\t\t\tauto* func = EnNew(SnFunction($1, $2, $3, $4, $6, @2));\r
\t\t\t\tif ($8 == nullptr)\r
\t\t\t\t\tfunc->AddFlags(NF_Abstract);\r
\t\t\t\telse\r
\t\t\t\t\tfunc->Body($8);\r
\t\t\t\t$$ = func;\r
\t\t\t} |\r
\t\t\tAccessType NameExpr TT_Identifier ';' {\r
\t\t\t\t$$ = EnNew(SnClassField($2, $3, $1, @1));\r
\t\t\t} ;\r
\r
FunctionBodyOrSemi:\tParagraph { $$ = $1; } |\r
\t\t\t';' { $$ = nullptr; } ;\r
`;

// Insert after EnumMember rule (before NodeFlags)
content = content.replace(
  /\t\t\t\} ;\r?\n\r?\nNodeFlags:/,
  `\t\t\t} ;${structAndClassRules}\r\nNodeFlags:`
);

// 3. Add NewExpr and KT_This/KT_Null to Expression
content = content.replace(
  /\t\t\t\tIdentifierExpr\t\{ \$\$ = \$1; \} \|/,
  `\t\t\t\tIdentifierExpr\t{ $$ = $1; } |\r\n\t\t\t\tNewExpr\t\t{ $$ = $1; } |`
);

// 4. Add KT_This to IdentifierExpr
content = content.replace(
  /\t\t\t\tKT_String\t\t\{ \$\$ = EnNew\(SnIdentifierExpr\(NK_String, @1\)\);\t\} ;/,
  `\t\t\t\tKT_String\t\t{ $$ = EnNew(SnIdentifierExpr(NK_String, @1));\t} |\r\n\t\t\t\tKT_This\t\t\t{ $$ = EnNew(SnThisExpr(@1)); } ;`
);

// 5. Add KT_Null to Expression (after NewExpr)
content = content.replace(
  /\t\t\t\tNewExpr\t\t\{ \$\$ = \$1; \} \|/,
  `\t\t\t\tNewExpr\t\t{ $$ = $1; } |\r\n\t\t\t\tKT_Null\t\t{ $$ = EnNew(SnLiteralExpr(*RnInt32::Instance(), 0, @1)); } |`
);

// 6. Add NewExpr rule (after IdentifierExpr rule)
const newExprRule = `\r\nNewExpr:\tKT_New IdentifierExpr '(' ConcreteParamList ')' {\r
\t\t\t\t$$ = EnNew(SnNewExpr(EnNew(SnNameExpr($2, @2)), $4, @1));\r
\t\t\t} ;\r
`;

content = content.replace(
  /\t\t\t\tKT_This\t\t\t\{ \$\$ = EnNew\(SnThisExpr\(@1\)\); \} ;/,
  `\t\t\t\tKT_This\t\t\t{ $$ = EnNew(SnThisExpr(@1)); } ;${newExprRule}`
);

// 7. Add MemberExpr to AssignStmt
content = content.replace(
  /AssignStmt: IdentifierExpr '=' Expression ';' \{\r?\n\t\t\t\t\$\$ = EnNew\(SnAssignStmt\(\$1, \$3, @1\)\);\r?\n\t\t\t\} ;/,
  `AssignStmt: IdentifierExpr '=' Expression ';' {\r\n\t\t\t\t$$ = EnNew(SnAssignStmt($1, $3, @1));\r\n\t\t\t} |\r\n\t\t\t\tMemberExpr '=' Expression ';' {\r\n\t\t\t\t$$ = EnNew(SnAssignStmt($1, $3, @1));\r\n\t\t\t} ;`
);

fs.writeFileSync('src/compiler/grammar/nlang.y', content, 'utf-8');
console.log('All grammar changes applied');
