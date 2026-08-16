import sys
with open('src/compiler/builder/StatementResolver.hpp', 'r', encoding='utf-8') as f:
    content = f.read()

old = '''\t\t\t//Like EN: a method that overrides a parent virtual method
\t\t\t//is also virtual (implicit virtual propagation).
\t\t\tauto *pSuper = sn.SuperClass();
\t\t\tif (pSuper)
\t\t\t{
\t\t\t\tfor (auto &field : sn.Members())
\t\t\t\t{
\t\t\t\t\tif (field.Kind() != NK_Function)
\t\t\t\t\t\tcontinue;
\t\t\t\t\tif (field.ContainFlags(NF_Virtual))
\t\t\t\t\t\tcontinue;
\t\t\t\t\tauto *pAncestor = pSuper;
\t\t\t\t\twhile (pAncestor)
\t\t\t\t\t{
\t\t\t\t\t\tauto *pParentMethod = pAncestor->FindField(field.Name());
\t\t\t\t\t\tif (pParentMethod && pParentMethod->Kind() == NK_Function
\t\t\t\t\t\t\t&& pParentMethod->ContainFlags(NF_Virtual))
\t\t\t\t\t\t{
\t\t\t\t\t\t\tfield.AddFlags(NF_Virtual);
\t\t\t\t\t\t\tbreak;
\t\t\t\t\t\t}
\t\t\t\t\t\tpAncestor = pAncestor->SuperClass();
\t\t\t\t\t}
\t\t\t\t}
\t\t\t}'''

new = '''\t\t\t//Like EN: a method that overrides a parent virtual method
\t\t\t//is also virtual (implicit virtual propagation).
\t\t\t//Check both name and parameter count to avoid false matches.
\t\t\tauto *pSuper = sn.SuperClass();
\t\t\tif (pSuper)
\t\t\t{
\t\t\t\tfor (auto &field : sn.Members())
\t\t\t\t{
\t\t\t\t\tif (field.Kind() != NK_Function)
\t\t\t\t\t\tcontinue;
\t\t\t\t\tif (field.ContainFlags(NF_Virtual))
\t\t\t\t\t\tcontinue;
\t\t\t\t\tauto &childFunc = static_cast<SnFunction&>(field);
\t\t\t\t\tauto *pAncestor = pSuper;
\t\t\t\t\twhile (pAncestor)
\t\t\t\t\t{
\t\t\t\t\t\tauto *pParentMethod = pAncestor->FindField(field.Name());
\t\t\t\t\t\tif (pParentMethod && pParentMethod->Kind() == NK_Function
\t\t\t\t\t\t\t&& pParentMethod->ContainFlags(NF_Virtual))
\t\t\t\t\t\t{
\t\t\t\t\t\t\tauto &parentFunc = static_cast<SnFunction&>(*pParentMethod);
\t\t\t\t\t\t\tif (childFunc.Params().size() == parentFunc.Params().size())
\t\t\t\t\t\t\t{
\t\t\t\t\t\t\t\tfield.AddFlags(NF_Virtual);
\t\t\t\t\t\t\t\tbreak;
\t\t\t\t\t\t\t}
\t\t\t\t\t\t}
\t\t\t\t\t\tpAncestor = pAncestor->SuperClass();
\t\t\t\t\t}
\t\t\t\t}
\t\t\t}'''

if old not in content:
    print('ERROR: old text not found')
    sys.exit(1)
content = content.replace(old, new)
with open('src/compiler/builder/StatementResolver.hpp', 'w', encoding='utf-8') as f:
    f.write(content)
print('OK')
