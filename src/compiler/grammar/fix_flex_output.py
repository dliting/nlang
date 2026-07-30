"""Post-process flex-generated files to fix known issues on Windows/MSVC.

NOTE: This script is currently NOT used in the build. The same issues are
now handled more elegantly:
  - unistd.h: YY_NO_UNISTD_H compile definition in CMakeLists.txt
  - INT8_MIN redefinitions: %top{ #include <stdint.h> } in nlang.l

This script is kept as a fallback for environments where those approaches
don't work, or for post-processing pre-generated files manually.

Fixes:
1. Remove #include <unistd.h> (flex 2.6 ignores %option nounistd in .h files)
2. Comment out INT8_MIN/MAX etc. macro definitions (flex defines them in
   the non-C99 branch, but MSVC stdint.h also defines them, causing C4005)
"""

import sys
import re

def fix_unistd(text):
    text = text.replace('#include <unistd.h>', '/* #include <unistd.h> */')
    return text

def fix_int_macros(text):
    macro_names = [
        'INT8_MIN', 'INT16_MIN', 'INT32_MIN',
        'INT8_MAX', 'INT16_MAX', 'INT32_MAX',
        'UINT8_MAX', 'UINT16_MAX', 'UINT32_MAX',
    ]
    for name in macro_names:
        text = re.sub(
            r'^(#define\s+' + name + r'\s+\S+.*)$',
            r'/* \1 */',
            text,
            flags=re.MULTILINE,
        )
    return text

def process_file(path):
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        text = f.read()
    text = fix_unistd(text)
    text = fix_int_macros(text)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)

if __name__ == '__main__':
    for path in sys.argv[1:]:
        process_file(path)
