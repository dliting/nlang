const fs = require('fs');
let content = fs.readFileSync('src/compiler/grammar/nlang.y', 'utf-8');

// Find the third alternative of ClassMember (AccessType NameExpr TT_Identifier '(' ...)
// and add a constructor rule before the field rule
const marker = "AccessType NameExpr TT_Identifier ';' {";
const idx = content.indexOf(marker);
if (idx === -1) {
    console.log('Marker not found');
    process.exit(1);
}

// Find the preceding '|'
const before = content.substring(0, idx);
const lastPipe = before.lastIndexOf('|');
if (lastPipe === -1) {
    console.log('Pipe not found');
    process.exit(1);
}

// Insert constructor rule before the field rule
const constructorRule = `AccessType TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
\t\t\t\t\t\tauto* voidType = EnNew(SnNameExpr(EnNew(SnIdentifierExpr(NK_Void, @2)), @2));
\t\t\t\t\t\tauto* func = EnNew(SnFunction($1, NF_NONE, voidType, $2, $4, @2));
\t\t\t\t\t\tif ($6 == nullptr)
\t\t\t\t\t\t\tfunc->AddFlags(NF_Abstract);
\t\t\t\t\t\telse
\t\t\t\t\t\t\tfunc->Body($6);
\t\t\t\t\t\t$$ = func;
\t\t\t\t\t} |
\t\t\t\t\t`;

content = content.substring(0, lastPipe + 1) + '\n\t\t\t\t\t' + constructorRule + content.substring(lastPipe + 1);

fs.writeFileSync('src/compiler/grammar/nlang.y', content, 'utf-8');
console.log('Constructor rule added');
