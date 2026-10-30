/*---
test_generic_descriptor.cpp - generic instantiation descriptor tests.

Pure unit coverage of GenericInstDescriptor (v0.12.3 Task 3): canonical
no-space spelling, intern identity by full spelling (nested arguments
included), structure passthrough, and array-argument rank marks.
---*/
#include <QtTest/QtTest>
#include "GenericInstDescriptor.h"
#include <nlang/runtime/PrimitiveTypes.h>
#include <nlang/vm/CompiledModuleRecords.h>
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
	arg.classRowRef = rowRef;
	arg.nested = std::move(nested);
	return arg;
}
}

class TestGenericDescriptor : public QObject
{
	Q_OBJECT

private slots:
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
		QCOMPARE(pListBoxInt->TypeArgs().at(0).classRowRef, 7);
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
};

QTEST_GUILESS_MAIN(TestGenericDescriptor)
#include "test_generic_descriptor.moc"
