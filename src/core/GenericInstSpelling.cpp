/*--- GenericInstSpelling.cpp — CanonicalSpelling: the canonical
    instantiation-spelling join (class contract see
    include/nlang/core/GenericInstDescriptor.h). Split out of
    src/vm/GenericInstDescriptor.cpp and moved to src/core with it in
    the library layering split (docs/dev/library_layering_design.md §6):
    the compile backend keys descriptor table rows through the join,
    which now reaches the compiler and the vm through nlang_core — no
    vm -> compiler link edge remains.
---*/
#include <nlang/core/GenericInstDescriptor.h>
#include <nlang/core/TypeArgSpelling.h>
#include <string>
#include <vector>

namespace nlang
{

//Canonical no-space join: "Base<Arg1,Arg2>". Distinct from the
//diagnostic render (ExprResolverGenerics.cpp joins with ", ") — the
//canonical form is the intern/serialization key, so it never carries
//presentation spaces. Arguments spell through CanonicalArgSpelling so
//nested instantiations stay distinguishable (the raw spelling field of
//a nested argument is only its bare template name).
std::string CanonicalSpelling(const std::string& templateName,
	const std::vector<TypeArgDesc>& typeArgs)
{
	std::string spelling = templateName;
	spelling += "<";
	for (size_t i = 0; i < typeArgs.size(); ++i)
	{
		if (i)
			spelling += ",";
		spelling += CanonicalArgSpelling(typeArgs[i]);
	}
	spelling += ">";
	return spelling;
}

} //namespace nlang
