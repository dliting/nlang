const fs = require('fs');
let content = fs.readFileSync('src/compiler/grammar/nlang.y', 'utf-8');

// Fix the corrupted $$ -> $ in the ClassMember section
// Replace the entire ClassMemberList through FunctionBodyOrSemi section

const oldSection = `ClassMemberList:\tClassMemberList ClassMember {
\t\t\t\t$1->push_back($2);
\t\t\t\t$ = $1;
\t\t\t} |
\t\t\t\tClassMember {
\t\t\t\t$ = EnNew(PtrList<SnField>());
\t\t\t\t$->push_back($1);
\t\t\t} ;

ClassMember:\tAccessType NodeFlags NameExpr TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
\t\t\t\tauto* func = EnNew(SnFunction($1, $2, $3, $4, $6, @2));
\t\t\t\tif ($8 == nullptr)
\t\t\t\t\tfunc->AddFlags(NF_Abstract);
\t\t\t\telse
\t\t\t\t\tfunc->Body($8);
\t\t\t\t$ = func;
\t\t\t} |
\t\t\tAccessType NameExpr TT_Identifier ';' {
\t\t\t\t$ = EnNew(SnClassField($2, $3, $1, @1));
\t\t\t} ;

FunctionBodyOrSemi:\tParagraph { $ = $1; } |
\t\t\t';' { $ = nullptr; } ;`;

const newSection = `ClassMemberList:\tClassMemberList ClassMember {
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

if (content.includes(oldSection)) {
    content = content.replace(oldSection, newSection);
    fs.writeFileSync('src/compiler/grammar/nlang.y', content, 'utf-8');
    console.log('Fix successful');
} else {
    console.log('Old section not found, checking for partial matches...');
    // Just fix all the $ that should be $$ in the ClassMember section
    const lines = content.split('\n');
    let inSection = false;
    let fixed = 0;
    for (let i = 0; i < lines.length; i++) {
        if (lines[i].includes('ClassMemberList:') || lines[i].includes('ClassMember:') || lines[i].includes('FunctionBodyOrSemi:')) {
            inSection = true;
        }
        if (inSection && lines[i].includes('$ = ') && !lines[i].includes('$$ = ')) {
            lines[i] = lines[i].replace(/\$ = /g, '$$ = ');
            lines[i] = lines[i].replace(/\$->/g, '$$->');
            fixed++;
        }
        if (lines[i].trim() === '' && inSection && i > 0 && lines[i-1].includes(';')) {
            inSection = false;
        }
    }
    if (fixed > 0) {
        fs.writeFileSync('src/compiler/grammar/nlang.y', lines.join('\n'), 'utf-8');
        console.log('Fixed ' + fixed + ' lines');
    } else {
        console.log('No fixes needed');
    }
}
