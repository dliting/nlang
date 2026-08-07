const fs = require('fs');
let content = fs.readFileSync('src/compiler/grammar/nlang.y', 'utf-8');

// Remove the constructor rule
const ctorStart = "AccessType TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {";
const ctorIdx = content.indexOf(ctorStart);
if (ctorIdx === -1) {
    console.log('Constructor rule not found');
    process.exit(1);
}

// Find the start of this alternative (the preceding '|')
const beforeCtor = content.substring(0, ctorIdx);
const pipeIdx = beforeCtor.lastIndexOf('|');

// Find the end of this alternative (the next '|')
const afterCtorStart = content.indexOf(ctorIdx);
// Find the next '} |' after the constructor rule
const ctorEnd = content.indexOf('} |', ctorIdx);
if (ctorEnd === -1) {
    console.log('End of constructor rule not found');
    process.exit(1);
}

// Remove from the pipe to the '} |'
content = content.substring(0, pipeIdx) + content.substring(ctorEnd + 2);

fs.writeFileSync('src/compiler/grammar/nlang.y', content, 'utf-8');
console.log('Constructor rule removed');
