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
#include <nlang/vm/CompiledModule.h>
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

//v0.13.1 Task 2: a hand-created instantiation shell mirroring
//CreateInstanceShell (ExprResolverGenericInstantiation.cpp) — name, erasure
//markers, type arguments. Compile-time only; the test owns the nodes.
SnClassDecl* CreateShell(const char* name, std::vector<SnField*> args)
{
	auto* pShell = new SnClassDecl(new std::string(name), nullptr,
		new PtrList<SnField>(), ScriptLocation());
	pShell->SetGenericInstantiation();
	pShell->SetBaseName(name);
	pShell->SetGenericTypeArgs(std::move(args));
	return pShell;
}

//One builtin type kind and the internal name the runtime tables stringize for
//it. Both vocabulary tests walk this list, which the X-macro generates from
//the runtime's own kind list — a hand-copied kind list is exactly what let the
//producer and the parser drift apart.
struct BuiltinTypeRow
{
	NodeKind kind;
	const char* name;
};

const BuiltinTypeRow* BuiltinTypeRows(size_t& count)
{
	static const BuiltinTypeRow rows[] = {
#define MACRO_IMPL(T) { NK_##T, #T },
		BUILTIN_TYPE_NODE_DECL(MACRO_IMPL)
#undef MACRO_IMPL
	};
	count = sizeof(rows) / sizeof(rows[0]);
	return rows;
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
	//nested unqualified-name rule, and placeholder encoding for T arguments.
	void builderSpellsConcreteInstantiationArgs()
	{
		ModuleRegistry registry;
		auto* pBoxInt = CreateShell("Box",
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
	//the instantiation as runtime-composable (the composition-record detector).
	void builderEncodesTypeParamArgs()
	{
		ModuleRegistry registry;
		auto* pTmpl = CreateShell("Box", {});
		pTmpl->SetTypeParamNames({"T"});
		SnField* pPlaceholder =
			registry.DeclaredGenerics().PlaceholderFor(*pTmpl, "T", 0);
		auto* pBoxT = CreateShell("Box", {pPlaceholder});
		const auto args = BuildTypeArgsOf(registry, *pBoxT);
		QCOMPARE(args.size(), size_t{1});
		QVERIFY(IsTypeParamArg(args[0]));
		QCOMPARE(TypeParamOrdinalOf(args[0]), int32_t(0));
		QVERIFY(HasTypeParamArg(args));
		//A concrete sibling is not composable.
		auto* pBoxInt = CreateShell("Box", {SnBuiltinDataType::InstanceOf(NK_Int32)});
		QVERIFY(!HasTypeParamArg(BuildTypeArgsOf(registry, *pBoxInt)));
	}

	//Nested instantiations: the outer argument carries the inner tree —
	//spelling through CanonicalArgSpelling stays the intern key.
	void builderNestsInstantiationArgs()
	{
		ModuleRegistry registry;
		auto* pInner = CreateShell("Box", {SnBuiltinDataType::InstanceOf(NK_Int32)});
		auto* pOuter = CreateShell("Wrap", {pInner});
		const auto args = BuildTypeArgsOf(registry, *pOuter);
		QCOMPARE(args.size(), size_t{1});
		QCOMPARE(args[0].spelling, std::string("Box"));
		QCOMPARE(CanonicalArgSpelling(args[0]),
			std::string("Box<int>"));
		auto* pDesc = InternDescriptor(pOuter->BaseName(), args);
		QCOMPARE(pDesc->Spelling(), std::string("Wrap<Box<int>>"));
	}

	//A func type argument spells its return slot through the same argument
	//builder as any other instantiation, and `void` — the only type that
	//names no value — lives in that slot. The leaf must spell the keyword:
	//degrading it left "func<?,int>", an intern key the load-side parser
	//could not resolve, so the whole image was refused as corrupt.
	void builderSpellsVoidReturnLeaf()
	{
		ModuleRegistry registry;
		auto* pFunc = CreateShell("func",
			{SnBuiltinDataType::InstanceOf(NK_Void), SnBuiltinDataType::InstanceOf(NK_Int32)});
		auto* pListOfFunc = CreateShell("List", {pFunc});
		const auto args = BuildTypeArgsOf(registry, *pListOfFunc);
		QCOMPARE(args.size(), size_t{1});
		QCOMPARE(args[0].nested.size(), size_t{2});
		QCOMPARE(args[0].nested[0].spelling, std::string("void"));
		QCOMPARE(args[0].nested[0].resolvedKind, uint8_t(RTK_Void));
		QCOMPARE(CanonicalArgSpelling(args[0]),
			std::string("func<void,int>"));
		auto* pDesc = InternDescriptor(pListOfFunc->BaseName(), args);
		QCOMPARE(pDesc->Spelling(), std::string("List<func<void,int>>"));
	}

	//The whole keyword-leaf vocabulary, pinned in BOTH directions from one
	//expected list. The producer and the load-side parser each kept their
	//own copy of the keyword set and the copies drifted: `void` degraded to
	//"?" on the producer side while the parser did not know the word
	//either, so a `func<void,int>` argument emitted a row the image loader
	//refused as corrupt. One vocabulary row now serves both directions, and
	//this test is the gate that keeps them a pair (a keyword leaf resolves
	//without any module table, so an empty module is the whole parser).
	void keywordLeafVocabularyRoundTripsBothWays()
	{
		struct LeafRow
		{
			NodeKind kind;
			const char* keyword;
			uint8_t rtk;
		};
		const LeafRow rows[] = {
			{ NK_Int32,  "int",    RTK_Int32  },
			{ NK_Float,  "float",  RTK_Float  },
			{ NK_Byte,   "byte",   RTK_Byte   },
			{ NK_UByte,  "ubyte",  RTK_UByte  },
			{ NK_Short,  "short",  RTK_Short  },
			{ NK_UShort, "ushort", RTK_UShort },
			{ NK_UInt32, "uint",   RTK_UInt32 },
			{ NK_Long,   "long",   RTK_Long   },
			{ NK_ULong,  "ulong",  RTK_ULong  },
			{ NK_Double, "double", RTK_Double },
			{ NK_Bool,   "bool",   RTK_Bool   },
			{ NK_Char,   "char",   RTK_Char   },
			{ NK_String, "string", RTK_String },
			{ NK_Void,   "void",   RTK_Void   },
		};
		ModuleRegistry registry;
		const CompiledModule empty;
		QStringList problems;
		for (const LeafRow& row : rows)
		{
			const std::string spelling =
				std::string("Box<") + row.keyword + ">";
			SnField* pType = SnBuiltinDataType::InstanceOf(row.kind);
			if (!pType)
			{
				problems << QString(spelling.c_str())
					+ ": the AST has no builtin node for the kind";
				continue;
			}
			//Producer direction: the resolved type spells its keyword and
			//carries the kind the dispatch paths read — never the "?"
			//degrade, which is what the parser then could not resolve.
			const TypeArgDesc arg = BuildTypeArgOf(registry, *pType);
			if (arg.spelling != row.keyword)
			{
				problems << QString(spelling.c_str()) + ": the producer spelled \""
					+ QString::fromLatin1(arg.spelling.c_str()) + "\"";
			}
			if (arg.resolvedKind != row.rtk)
			{
				problems << QString(spelling.c_str()) + ": resolved kind "
					+ QString::number(arg.resolvedKind) + " expected "
					+ QString::number(row.rtk);
			}
			//Load direction: the same spelling parses back to the one
			//canonical key. A throw here is the parser refusing a leaf
			//the producer emitted.
			try
			{
				auto* pDesc = InternDescriptorFromSpelling(spelling, empty);
				if (pDesc->Spelling() != spelling)
				{
					problems << QString(spelling.c_str())
						+ ": the parser interned \""
						+ QString::fromLatin1(pDesc->Spelling().c_str())
						+ "\"";
				}
			}
			catch (const std::exception& e)
			{
				problems << QString(spelling.c_str()) + ": "
					+ QString::fromLatin1(e.what());
			}
		}
		QVERIFY2(problems.isEmpty(),
			qPrintable(problems.join(" | ")));
	}

	//Completeness of the vocabulary table, measured against the runtime's own
	//builtin type list rather than against this test's expectations: every
	//builtin kind is either a keyword leaf or the one kind that names no value
	//(`Type`, which has no keyword and therefore no spelling here). The
	//producer and the parser each used to keep a private copy of the keyword
	//set, so a kind missing from one copy surfaced only as a refused image;
	//walking the X-macro list turns the next builtin type kind into a failure
	//named by kind.
	void keywordLeafTableMatchesEveryBuiltinType()
	{
		size_t rowCount = 0;
		const BuiltinTypeRow* rows = BuiltinTypeRows(rowCount);
		QStringList problems;
		for (size_t i = 0; i < rowCount; ++i)
		{
			const BuiltinTypeRow& row = rows[i];
			const TypeLeafKeyword* pLeaf = FindTypeLeafKeyword(row.kind);
			if (!pLeaf)
			{
				if (row.kind != NK_Type)
					problems << QString::fromLatin1(row.name)
						+ ": no keyword leaf row, so the producer spells no"
						+ " leaf the parser can resolve";
				continue;
			}
			//The row must pair back from its keyword — the parser reads the
			//table in that direction, and a keyword reaching no row is the
			//refused-image failure again.
			if (FindTypeLeafKeyword(std::string(pLeaf->keyword)) != pLeaf)
			{
				problems << QString::fromLatin1(row.name) + ": the keyword \""
					+ QString::fromLatin1(pLeaf->keyword)
					+ "\" does not pair back to the same row";
			}
		}
		//A keyword row must name a builtin type kind: the table is the whole
		//leaf vocabulary, and a row for anything else claims a keyword the
		//parser would take for a link key.
		for (const TypeLeafKeyword& leafRow : kTypeLeafKeywords)
		{
			if (!IsBuiltinType(leafRow.kind))
			{
				problems << "the keyword row \""
					+ QString::fromLatin1(leafRow.keyword)
					+ "\" names a kind that is not a builtin type";
			}
		}
		QVERIFY2(problems.isEmpty(),
			qPrintable(problems.join(" | ")));
	}

	//The internal names the runtime tables stringize for the builtin types are
	//not type spellings: a builtin data type mirror gives up its unqualified
	//name as the AST is built from the runtime namespace, so `Int32`, `Void`
	//and `Type` resolve nowhere — the spellings of the language are its
	//keywords, its numeric aliases and the declared names (manual chapter 1).
	//Indexed, they resolved silently: `Void v;` declared a type with no value
	//representation, and `Type t;` reached the instantiation descriptor layer,
	//which defines no spelling for it. The mirror nodes stay attached, since
	//the keyword path resolves through them — every test above proves that.
	void builtinTypeMirrorNamesAreNotResolvable()
	{
		size_t rowCount = 0;
		const BuiltinTypeRow* rows = BuiltinTypeRows(rowCount);
		QStringList problems;
		for (size_t i = 0; i < rowCount; ++i)
		{
			if (TheAST().Root()->Members().find(std::string(rows[i].name)))
			{
				problems << "the root namespace indexes the mirror name \""
					+ QString::fromLatin1(rows[i].name)
					+ "\", so an internal type name resolves as a spelling";
			}
		}
		QVERIFY2(problems.isEmpty(), qPrintable(problems.join(" | ")));
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
