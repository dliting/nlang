import sys
with open('src/compiler/builder/StatementResolver.hpp', 'rb') as f:
    data = f.read()
content = data.decode('utf-8')

old = '\t\t\tif (pParentMethod && pParentMethod->Kind() == NK_Function\r\n\t\t\t\t\t&& pParentMethod->ContainFlags(NF_Virtual))\r\n\t\t\t\t\t{\r\n\t\t\t\t\t\tfield.AddFlags(NF_Virtual);\r\n\t\t\t\t\t\tbreak;\r\n\t\t\t\t\t}'

new = '\t\t\tif (pParentMethod && pParentMethod->Kind() == NK_Function\r\n\t\t\t\t\t&& pParentMethod->ContainFlags(NF_Virtual))\r\n\t\t\t\t\t{\r\n\t\t\t\t\t\tauto &parentFunc = static_cast<SnFunction&>(*pParentMethod);\r\n\t\t\t\t\t\tif (childFunc.Params().size() == parentFunc.Params().size())\r\n\t\t\t\t\t\t{\r\n\t\t\t\t\t\t\tfield.AddFlags(NF_Virtual);\r\n\t\t\t\t\t\t\tbreak;\r\n\t\t\t\t\t\t}\r\n\t\t\t\t\t}'

# Also add childFunc declaration
old2 = '\t\t\t\tif (field.ContainFlags(NF_Virtual))\r\n\t\t\t\t\tcontinue;\r\n\t\t\t\tauto *pAncestor = pSuper;'
new2 = '\t\t\t\tif (field.ContainFlags(NF_Virtual))\r\n\t\t\t\t\tcontinue;\r\n\t\t\t\tauto &childFunc = static_cast<SnFunction&>(field);\r\n\t\t\t\tauto *pAncestor = pSuper;'

if old not in content:
    print('ERROR: old text not found')
    sys.exit(1)
if old2 not in content:
    print('ERROR: old2 text not found')
    sys.exit(1)

content = content.replace(old, new)
content = content.replace(old2, new2)

with open('src/compiler/builder/StatementResolver.hpp', 'wb') as f:
    f.write(content.encode('utf-8'))
print('OK')
