#pragma once
/*---
    CompiledModuleStaticFieldStubs.hpp — 导入类记录staticFields表的
    桩重建（v0.12.6 Task 8）。从CompiledModuleStubMembers.hpp拆出
    （源尺寸守卫），同族支持头文件。
---*/
#include "CompiledModuleDefaultSupport.hpp"
#include "CompiledModuleStubSupport.hpp"
#include "SnMisc.h"
#include "SnExpressions.h"
#include "SyntaxTree.h"
#include "nlang/vm/CompiledModule.h"
#include <stdexcept>
#include <string>

namespace nlang
{

//v0.12.6 Task 8: static fields ride the record's staticFields list so
//consumer code can resolve them (reads, writes, first-use gates).
//A folded static const rebuilds with its literal initializer — the
//consumer's constant-read phase folds it inline, zero machinery
//(D3/D13). A runtime slot field rebuilds initializer-less with the
//producer's slot row index on the rider; StaticSlotIndexOf's
//imported arm mints the placeholder import record from it.
inline void AppendClassStaticFieldStubs(const CompiledClass &cc,
	const CompiledModule &cm, const ISourceLocation &loc,
	PtrList<SnField> *pMembers)
{
	for (const auto &sf : cc.staticFields) {
		if (sf.slotIdx == 0xFFFF) {
			SnExpression *pInit =
				SynthScalarDefault(sf.foldedValue, cm, loc);
			if (!pInit)
				throw std::runtime_error(
					"imported class '" + cc.name
					+ "' has a malformed folded static value record");
			auto *pField = new SnClassField(
				SynthTypeExprFromDesc(sf.type, cm, loc),
				new std::string(sf.name),
				static_cast<FieldAccessType>(sf.access), loc, pInit);
			pField->AddFlags(NF_Static);
			pField->AddFlags(NF_Const);
			pMembers->push_back(pField);
			continue;
		}
		auto *pField = new SnClassField(
			SynthTypeExprFromDesc(sf.type, cm, loc),
			new std::string(sf.name),
			static_cast<FieldAccessType>(sf.access), loc);
		pField->AddFlags(NF_Static);
		if (sf.isConst)
			pField->AddFlags(NF_Const);
		pField->m_iProducerSlotIdx = sf.slotIdx;
		pMembers->push_back(pField);
	}
}

} //namespace nlang
