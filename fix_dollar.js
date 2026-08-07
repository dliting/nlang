const fs = require('fs');
let content = fs.readFileSync('src/compiler/grammar/nlang.y', 'utf-8');

// Fix all $ that should be $$ in the newly added sections
// The pattern: standalone $ followed by = or -> (not $1, $2, etc.)
// We need to be careful: $1, $2, $3 etc should stay as-is
// $$ = should be $$ = (not $ =)
// $$-> should be $$-> (not $->)

// Fix: replace "$ = " with "$$ = " and "$->" with "$$->"
// But only where it's not already "$$ = " or "$$->"
content = content.replace(/(?<!\$)\$(?=\$|[= ])/g, function(match, offset, str) {
    // Check if next char is already $ (i.e., it's already $$)
    if (str[offset + 1] === '$') return match;
    // Check if this is $1, $2, etc (bison reference)
    if (/[0-9]/.test(str[offset + 1] || '')) return match;
    // This is a lone $ that should be $$
    return '$$';
});

// Actually, let me just do targeted replacements for the known patterns
// Reset and re-read
content = fs.readFileSync('src/compiler/grammar/nlang.y', 'utf-8');

// Replace all "$ = " with "$$ = " (but not "$$ = " which would become "$$$ = ")
// First replace "$$ = " with a placeholder, then "$ = " with "$$ = ", then restore
content = content.replace(/\$\$ = /g, '___DOLLAR2_EQ___');
content = content.replace(/\$ = /g, '$$ = ');
content = content.replace(/___DOLLAR2_EQ___/g, '$$ = ');

// Same for $$->
content = content.replace(/\$\$->/g, '___DOLLAR2_ARROW___');
content = content.replace(/\$->/g, '$$->');
content = content.replace(/___DOLLAR2_ARROW___/g, '$$->');

fs.writeFileSync('src/compiler/grammar/nlang.y', content, 'utf-8');
console.log('Fixed $$ in grammar file');
