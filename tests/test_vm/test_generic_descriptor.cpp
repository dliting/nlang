/*---
test_generic_descriptor.cpp - generic instantiation descriptor tests.

Pure unit coverage of GenericInstDescriptor (v0.12.3 Task 3): canonical
no-space spelling, intern identity by full spelling (nested arguments
included), structure passthrough, and array-argument rank marks.
---*/
#include <QtTest/QtTest>
#include "GenericInstDescriptor.h"
#include <nlang/compiler/TypeArgSpelling.h>
#include <nlang/runtime/PrimitiveTypes.h>
#include <nlang/runtime/Runtime.h>
#include <nlang/compiler/SnTypes.h>
#include <nlang/vm/CompiledModuleRecords.h>
#include <nlang/compiler/GenericInstanceRegistry.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/ScriptLocation.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <nlang/compiler/SyntaxTree.h>
#include "builder/ModuleRegistry.h"
#include <memory>
#include <string>
#include <vector>

using namespace nlang;

namespace
{
//C++17-compatible argument builders (no designated initializers).
TypeArgDesc PrimArg(const char* spelling, uint8_t kind)
{
	TypeArgDesc arg;
	arg.spelling = spelling;
	arg.resolvedKind = kind;
	return arg;
}

TypeArgDesc ClassArg(const char* spelling, int32_t rowRef,
	std::vector<TypeArgDesc> nested)
{
	TypeArgDesc arg;
	arg.spelling = spelling;
	arg.resolvedKind = RTK_Class;
	arg.rowRef = rowRef;
	arg.nested = std::move(nested);
	return arg;
}

//v0.13.1 Task 2: a hand-minted instantiation shell mirroring
//MintInstanceShell (ExprResolverGenericMint.cpp) — name, erasure
//markers, type arguments. Compile-time only; the test owns the nodes.
SnClassDecl* MintShell(const char* name, std::vector<SnField*> args)
{
	auto* pShell = new SnClassDecl(new std::string(name), nullptr,
		new PtrList<SnField>(), ScriptLocation());
	pShell->SetGenericInstantiation();
	pShell->SetBaseName(name);
	pShell->SetGenericTypeArgs(std::move(args));
	return pShell;
}

//Silent logger for the environment setup (no diagnostics expected).
class QuietLogger : public CompileLogger
{
protected:
	void WriteLog(CompileLogLevel, const ISourceLocation*,
		const char*) override
	{
	}
};
}

class TestGenericDescriptor : public QObject
{
	Q_OBJECT

private slots:
	//The hand-built AST nodes below need the runtime type singletons
	//AND the AST's builtin type nodes (SnBuiltinDataType::InstanceOf
	//resolves through TheAST) — the same two-step init ModuleBuilder
	//runs before resolving anything.
	void initTestCase() {
		Runtime::StaticInit();
		BuildParams params;
		QuietLogger logger;
		m_upEnv = std::make_unique<BuildEnvironment>(params, logger);
		TheAST().Clear();
		TheAST().BuildFromRuntime(*m_upEnv);
	}

	//Intern identity: the same instantiation asked for twice yields the
	//same descriptor, and the spelling is the canonical no-space form
	//(the diagnostic render joins with ", " — this must not).
	void internReturnsSameDescriptorForSameSpelling()
	{
		auto* pFirst = InternDescriptor("Box", {PrimArg("int", RTK_Int32)});
		auto* pSecond =
			InternDescriptor("Box", {PrimArg("int", RTK_Int32)});
		QVERIFY(pFirst == pSecond);
		QCOMPARE(pFirst->Spelling(), std::string("Box<int>"));
		QCOMPARE(pFirst->TemplateName(), std::string("Box"));
	}

	//Multi-argument join: commas without spaces, argument order kept.
	void spellingJoinsArgumentsWithoutSpaces()
	{
		auto* pDict = InternDescriptor("Dict",
			{PrimArg("string", RTK_String), PrimArg("int", RTK_Int32)});
		QCOMPARE(pDict->Spelling(), std::string("Dict<string,int>"));
	}

	//Nested instantiations intern by FULL spelling: List<Box<int>> and
	//List<Box<string>> are distinct, while two separately constructed
	//List<Box<int>> descriptors are one interned record; the nested
	//structure and classRowRef ride along untouched.
	void nestedInstantiationsInternByFullSpelling()
	{
		const std::vector<TypeArgDesc> boxInt{
			ClassArg("Box", 7, {PrimArg("int", RTK_Int32)})};
		const std::vector<TypeArgDesc> boxStr{
			ClassArg("Box", 7, {PrimArg("string", RTK_String)})};

		auto* pListBoxInt = InternDescriptor("List", boxInt);
		auto* pListBoxIntAgain = InternDescriptor("List", boxInt);
		auto* pListBoxStr = InternDescriptor("List", boxStr);

		QVERIFY(pListBoxInt == pListBoxIntAgain);
		QVERIFY(pListBoxInt != pListBoxStr);
		QCOMPARE(pListBoxInt->Spelling(),
			std::string("List<Box<int>>"));
		QCOMPARE(pListBoxStr->Spelling(),
			std::string("List<Box<string>>"));
		QCOMPARE(pListBoxInt->TypeArgs().at(0).rowRef, 7);
		QCOMPARE(pListBoxInt->TypeArgs().at(0).nested.at(0).spelling,
			std::string("int"));
	}

	//Array-argument rank marks flow into the spelling and stay
	//distinguishable from the element type alone.
	void arrayArgumentRankMarksFlowIntoSpelling()
	{
		auto* pMatrixOfArrays =
			InternDescriptor("Matrix", {PrimArg("int[]", RTK_Array)});
		auto* pMatrixOfScalars =
			InternDescriptor("Matrix", {PrimArg("int", RTK_Int32)});
		QCOMPARE(pMatrixOfArrays->Spelling(),
			std::string("Matrix<int[]>"));
		QVERIFY(pMatrixOfArrays != pMatrixOfScalars);
	}

	//v0.13.1 Task 2: the AST-side builder spells concrete instantiations
	//the same way the intern pool does — scalar canonical names, the
	//nested bare-name rule, and placeholder encoding for T arguments.
	void builderSpellsConcreteInstantiationArgs()
	{
		ModuleRegistry registry;
		auto* pBoxInt = MintShell("Box",
			{SnBuiltinDataType::InstanceOf(NK_Int32), SnBuiltinDataType::InstanceOf(NK_String)});
		const auto args = BuildTypeArgsOf(registry, *pBoxInt);
		QCOMPARE(args.size(), size_t{2});
		QCOMPARE(args[0].spelling, std::string("int"));
		QCOMPARE(args[0].resolvedKind, uint8_t(RTK_Int32));
		QCOMPARE(args[1].spelling, std::string("string"));
		QCOMPARE(args[1].resolvedKind, uint8_t(RTK_String));
		auto* pDesc = InternDescriptor(pBoxInt->BaseName(), args);
		QCOMPARE(pDesc->Spelling(), std::string("Box<int,string>"));
	}

	//A type-parameter argument encodes its parameter position and marks
	//the instantiation as runtime-composable (the recipe detector).
	void builderEncodesTypeParamArgs()
	{
		ModuleRegistry registry;
		auto* pTmpl = MintShell("Box", {});
		pTmpl->SetTypeParamNames({"T"});
		SnField* pPlaceholder =
			registry.DeclaredGenerics().PlaceholderFor(*pTmpl, "T", 0);
		auto* pBoxT = MintShell("Box", {pPlaceholder});
		const auto args = BuildTypeArgsOf(registry, *pBoxT);
		QCOMPARE(args.size(), size_t{1});
		QVERIFY(IsTypeParamArg(args[0]));
		QCOMPARE(TypeParamOrdinalOf(args[0]), int32_t(0));
		QVERIFY(HasTypeParamArg(args));
		//A concrete sibling is not composable.
		auto* pBoxInt = MintShell("Box", {SnBuiltinDataType::InstanceOf(NK_Int32)});
		QVERIFY(!HasTypeParamArg(BuildTypeArgsOf(registry, *pBoxInt)));
	}

	//Nested instantiations: the outer argument carries the inner tree —
	//spelling through CanonicalArgSpelling stays the intern key.
	void builderNestsInstantiationArgs()
	{
		ModuleRegistry registry;
		auto* pInner = MintShell("Box", {SnBuiltinDataType::InstanceOf(NK_Int32)});
		auto* pOuter = MintShell("Wrap", {pInner});
		const auto args = BuildTypeArgsOf(registry, *pOuter);
		QCOMPARE(args.size(), size_t{1});
		QCOMPARE(args[0].spelling, std::string("Box"));
		QCOMPARE(CanonicalArgSpelling(args[0]),
			std::string("Box<int>"));
		auto* pDesc = InternDescriptor(pOuter->BaseName(), args);
		QCOMPARE(pDesc->Spelling(), std::string("Wrap<Box<int>>"));
	}

	//Array-argument rank rides the descriptor arg (the spelling carries
	//the same information for display; rank feeds runtime array checks).
	void arrayArgCarriesRankField()
	{
		TypeArgDesc rank2;
		rank2.spelling = "int[,]";
		rank2.resolvedKind = RTK_Array;
		rank2.rank = 2;
		auto* pDesc = InternDescriptor("Grid", {rank2});
		QCOMPARE(pDesc->Spelling(), std::string("Grid<int[,]>"));
		QCOMPARE(pDesc->TypeArgs().at(0).rank, uint8_t(2));
	}

private:
	std::unique_ptr<BuildEnvironment> m_upEnv;
};

QTEST_GUILESS_MAIN(TestGenericDescriptor)
#include "test_generic_descriptor.moc"
