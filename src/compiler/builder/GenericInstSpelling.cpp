/*--- GenericInstSpelling.cpp — CanonicalSpelling: the canonical
    instantiation-spelling join (class contract see
    include/nlang/core/GenericInstDescriptor.h). Split out of
    src/vm/GenericInstDescriptor.cpp with the library layering split
    (docs/dev/library_layering_design.md §6): the compile backend keys
    descriptor table rows through it, so the pure join lives on the
    compiler side of the residual vm -> compiler link edge and stays
    reachable from compiler-only link targets. Moves to src/core with
    GenericInstDescriptor.cpp in the core split (task 6).
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
