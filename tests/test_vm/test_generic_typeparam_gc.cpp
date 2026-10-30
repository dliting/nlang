/*---
test_generic_typeparam_gc.cpp - type-parameter slot kind tests.

Unit coverage of the RTK_TypeParam label (v0.12.3 Task 4): the wire
value pin, the placeholder node predicate, the compile-time kind
emission, and the .ncu byte round-trip. The three runtime GC arms
(frame-band mask, param scan, class-field scan) are code-reviewed here
but exercised end-to-end after Task 5 by tests/e2e/generic_gc_tslot_
survives.n — hand-built executor frames are a retired test pattern
(test_vm.cpp main comment), and T slots cannot compile before the
registration flip.
---*/
//Include order: every NLang header BEFORE QtTest — Qt defines `slots`
//as a macro and would eat VmBackend.h's FuncContext::ClaimClassRecord
//member of that name.
#include "VmBackend.h"
#include "ModuleLoader.h"
#include "BytecodeEmitter.h"
#include "BytecodeOps.h"
#include <nlang/vm/CompiledModuleRecords.h>
#include <nlang/vm/CompiledModule.h>
#include <nlang/compiler/GenericInstanceRegistry.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/ScriptLocation.h>
#include <QtTest/QtTest>
#include <filesystem>
#include <fstream>
#include <string>

using namespace nlang;

namespace
{
//A bare class node shaped like a template declaration (the same form
//PlaceholderFor receives from the resolver).
SnClassDecl* NewBareClass(const char* pName)
{
	return new SnClassDecl(new std::string(pName), nullptr,
		new PtrList<SnField>(), ScriptLocation());
}
}

class TestGenericTypeParamGc : public QObject
{
	Q_OBJECT

private slots:
	//The wire value: non-scalar kinds past the scalar block (0/1 and
	//10..19 live in PrimitiveTypes.h). Pinned so renumbering elsewhere
	//cannot silently collide with a future scalar.
	void typeParamKindValuePinned()
	{
		QCOMPARE(RTK_TypeParam, uint8_t(20));
	}

	//The mint marks its placeholder: the context-free node predicate the
	//backend's static kind classification reads (same shape as IsFuncType
	//for func<...>); ordinary classes stay unmarked.
	void placeholderNodeCarriesTypeParamFlag()
	{
		GenericInstanceRegistry registry;
		auto* pTmpl = NewBareClass("Box");
		auto* pPlaceholder = registry.PlaceholderFor(*pTmpl, "T", 0);
		QVERIFY(static_cast<SnClassDecl*>(pPlaceholder)
			->IsTypeParamPlaceholder());

		auto* pPlain = NewBareClass("Plain");
		QVERIFY(!pPlain->IsTypeParamPlaceholder());
	}

	//The compile-time emission: a placeholder type expression classifies
	//as RTK_TypeParam (local descriptors, field kinds and signature
	//leaves all flow through this one source); an ordinary class still
	//classifies as RTK_Class.
	void runtimeTypeKindEmitsTypeParamForPlaceholders()
	{
		GenericInstanceRegistry registry;
		auto* pTmpl = NewBareClass("Box");
		auto* pPlaceholder = registry.PlaceholderFor(*pTmpl, "T", 0);
		QCOMPARE(VmBackend::RuntimeTypeKind(pPlaceholder), RTK_TypeParam);

		auto* pPlain = NewBareClass("Plain");
		QCOMPARE(VmBackend::RuntimeTypeKind(pPlain), RTK_Class);
	}

	//Serialization pin (passes today: both sides move the kind as a raw
	//byte, no whitelist) — guards the label against a future loader-side
	//kind validation rejecting it.
	void typeParamKindSurvivesNcuRoundTrip()
	{
		CompiledModule mod;
		mod.name = "tparam_roundtrip";
		CompiledFunction func;
		func.name = "main";
		func.localsSize = 8;
		func.paramCount = 0;
		func.returnTypeKind = RTK_Void;
		BytecodeEmitter em;
		em.Emit(OpCode::OP_Return);
		func.bytecode = em.TakeBytes();
		LocalDescriptor ld;
		ld.offset = 0;
		ld.size = 4;
		ld.isParam = 1;
		ld.typeKind = RTK_TypeParam;
		ld.name = "v";
		func.locals.push_back(ld);
		//The loader enforces mask-length/frame-layout agreement: one
		//band slot (localsSize 8 / 8-byte slots) needs exactly one mask
		//byte. Class None — this record round-trips bytes, it never runs.
		func.evalAreaHandleMask.assign(1, 0);
		mod.functions.push_back(std::move(func));

		const auto path = std::filesystem::temp_directory_path()
			/ "nlang_test_tparam_kind.ncu";
		{
			std::ofstream fs(path, std::ios::binary);
			QVERIFY2(WriteCompiledModule(fs, mod, "main"),
				"WriteCompiledModule failed");
		}
		const auto loaded = ModuleLoader::Load(path.string());
		std::filesystem::remove(path);
		QCOMPARE(loaded.functions.size(), size_t(1));
		QCOMPARE(loaded.functions[0].locals.size(), size_t(1));
		QCOMPARE(loaded.functions[0].locals[0].typeKind, RTK_TypeParam);
	}
};

QTEST_GUILESS_MAIN(TestGenericTypeParamGc)
#include "test_generic_typeparam_gc.moc"
