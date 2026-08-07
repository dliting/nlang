import os

filepath = r'E:\cases\nlang\src\compiler\grammar\nlang.y'

with open(filepath, 'rb') as f:
    content = f.read()

# Add StructDecl grammar rules after EnumMember rules
old = b"""\t\t\t} ;\r\n\r\nNodeFlags:"""
new = b"""\t\t\t} ;\r\n\r\n/*\r\nStruct type declaration.\r\n*/\r\nStructDecl:\tKT_Struct TT_Identifier '{' StructFieldList '}' {\r\n\t\t\t\t$$ = EnNew(SnStructDecl($2, $4, @1));\r\n\t\t\t} ;\r\n\r\nStructFieldList:\tStructFieldList StructField {\r\n\t\t\t\t$1->push_back($2);\r\n\t\t\t\t$$ = $1;\r\n\t\t\t} |\r\n\t\t\t\tStructField {\r\n\t\t\t\t$$ = EnNew(PtrList<SnStructField>());\r\n\t\t\t\t$$->push_back($1);\r\n\t\t\t} ;\r\n\r\nStructField:\tNameExpr TT_Identifier ';' {\r\n\t\t\t\t$$ = EnNew(SnStructField($1, $2, @1));\r\n\t\t\t} ;\r\n\r\nNodeFlags:"""

if old in content:
    content = content.replace(old, new, 1)
    print('Added StructDecl grammar rules')
else:
    print('ERROR: Could not find insertion point for StructDecl')

# Add MemberExpr alternatives to InitFor and FiniFor
old_initfor = b"\t\t\tIdentifierExpr '=' Expression {\r\n\t\t\t\t$$ = EnNew(SnAssignStmt($1, $3, @1));\r\n\t\t\t} ;\r\n\r\nFiniFor:"
new_initfor = b"""\t\t\tIdentifierExpr '=' Expression {\r\n\t\t\t\t$$ = EnNew(SnAssignStmt($1, $3, @1));\r\n\t\t\t} |\r\n\t\t\tMemberExpr '=' Expression {\r\n\t\t\t\t$$ = EnNew(SnAssignStmt($1, $3, @1));\r\n\t\t\t} ;\r\n\r\nFiniFor:"""

if old_initfor in content:
    content = content.replace(old_initfor, new_initfor, 1)
    print('Added MemberExpr to InitFor')
else:
    print('ERROR: Could not find InitFor insertion point')

old_finifor = b"""\t\t\tIdentifierExpr '=' Expression {\r\n\t\t\t\t$$ = EnNew(SnAssignStmt($1, $3, @1));\r\n\t\t\t} ;\r\n\r\n/*\r\nBreak statement."""
new_finifor = b"""\t\t\tIdentifierExpr '=' Expression {\r\n\t\t\t\t$$ = EnNew(SnAssignStmt($1, $3, @1));\r\n\t\t\t} |\r\n\t\t\tMemberExpr '=' Expression {\r\n\t\t\t\t$$ = EnNew(SnAssignStmt($1, $3, @1));\r\n\t\t\t} ;\r\n\r\n/*\r\nBreak statement."""

if old_finifor in content:
    content = content.replace(old_finifor, new_finifor, 1)
    print('Added MemberExpr to FiniFor')
else:
    print('ERROR: Could not find FiniFor insertion point')

with open(filepath, 'wb') as f:
    f.write(content)
