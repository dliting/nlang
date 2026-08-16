const fs = require('fs');
let content = fs.readFileSync('src/compiler/grammar/nlang.y', 'utf-8');

const old = `ClassMemberList:\tClassMemberList ClassField {
\t\t\t\t$1->push_back($2);
\t\t\t\t$$ = $1;
\t\t\t} |
\t\t\t\tClassMemberList Function {
\t\t\t\t$1->push_back($2);
\t\t\t\t$$ = $1;
\t\t\t} |
\t\t\t\tClassField {
\t\t\t\t$$ = EnNew(PtrList<SnField>());
\t\t\t\t$$->push_back($1);
\t\t\t} |
\t\t\t\tFunction {
\t\t\t\t$$ = EnNew(PtrList<SnField>());
\t\t\t\t$$->push_back($1);
\t\t\t} ;

ClassField:\tAccessType NameExpr TT_Identifier ';' {
\t\t\t\t$$ = EnNew(SnClassField($2, $3, $1, @1));
\t\t\t} ;`;

const newStr = `ClassMemberList:\tClassMemberList ClassMember {
\t\t\t\t$1->push_back($2);
\t\t\t\t$$ = $1;
\t\t\t} |
\t\t\t\tClassMember {
\t\t\t\t$$ = EnNew(PtrList<SnField>());
\t\t\t\t$$->push_back($1);
\t\t\t} ;

ClassMember:\tAccessType NodeFlags NameExpr TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
\t\t\t\tauto* func = EnNew(SnFunction($1, $2, $3, $4, $6, @2));
\t\t\t\tif ($8 == nullptr)
\t\t\t\t\tfunc->AddFlags(NF_Abstract);
\t\t\t\telse
\t\t\t\t\tfunc->Body($8);
\t\t\t\t$$ = func;
\t\t\t} |
\t\t\tAccessType NameExpr TT_Identifier ';' {
\t\t\t\t$$ = EnNew(SnClassField($2, $3, $1, @1));
\t\t\t} ;

FunctionBodyOrSemi:\tParagraph { $$ = $1; } |
\t\t\t';' { $$ = nullptr; } ;`;

if (content.includes(old)) {
    content = content.replace(old, newStr);
    fs.writeFileSync('src/compiler/grammar/nlang.y', content, 'utf-8');
    console.log('Replacement successful');
} else {
    console.log('Old text not found');
    const lines = content.split('\n');
    for (let i = 663; i < 684; i++) {
        console.log((i+1) + ': ' + JSON.stringify(lines[i]));
    }
}
