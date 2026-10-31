# Generate lexer and parser from grammar files
set(GRAMMAR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/grammar)
set(GENERATED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/generated)
file(MAKE_DIRECTORY ${GENERATED_DIR})

# Require Flex and Bison for grammar generation
if(NOT FLEX_EXE OR NOT BISON_EXE)
    message(FATAL_ERROR
        "Flex and Bison are required to generate the parser/lexer.\n"
        "Set -DFLEX_EXE=<path> and -DBISON_EXE=<path> on Windows, "
        "or ensure they are on PATH.\n"
        "Windows users: download from https://github.com/lexxmark/winflexbison")
endif()

add_custom_command(
    OUTPUT ${GENERATED_DIR}/nlang.tab.cpp ${GENERATED_DIR}/nlang.tab.h
    #v0.9.1: the grammar carries 4 accepted reduce/reduce conflicts, and
    #deterministic parsers cannot use %expect alongside any r/r conflict,
    #so the conflict notices are silenced here; the counts are pinned by
    #the accepted-conflict ledger in nlang.y and its re-measurement gate.
    COMMAND ${BISON_EXE} -d --header=${GENERATED_DIR}/nlang.tab.h
        -Wno-conflicts-sr -Wno-conflicts-rr
        -o ${GENERATED_DIR}/nlang.tab.cpp ${GRAMMAR_DIR}/nlang.y
    DEPENDS ${GRAMMAR_DIR}/nlang.y
    COMMENT "Generating Bison parser"
)

add_custom_command(
    OUTPUT ${GENERATED_DIR}/nlang.lex.cpp ${GENERATED_DIR}/nlang.lex.h
    COMMAND ${FLEX_EXE} --header-file=nlang.lex.h
        -o nlang.lex.cpp ${GRAMMAR_DIR}/nlang.l
    WORKING_DIRECTORY ${GENERATED_DIR}
    DEPENDS ${GRAMMAR_DIR}/nlang.l ${GENERATED_DIR}/nlang.tab.h
    COMMENT "Generating Flex lexer"
)

#The embedded prelude TU is a hard source of nlang_compiler: the build
#generates it from stdlib/builtins.n (the declarations' source of truth),
#so editing that file re-embeds on the next build.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(BUILTINS_SRC ${PROJECT_SOURCE_DIR}/stdlib/builtins.n)
set(BUILTINS_EMBED ${CMAKE_CURRENT_BINARY_DIR}/generated/builtins_embedded.cpp)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_command(
    OUTPUT ${BUILTINS_EMBED}
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/embed_source.py
            ${BUILTINS_SRC} ${BUILTINS_EMBED} kBuiltinPreludeSource
    DEPENDS ${BUILTINS_SRC} ${PROJECT_SOURCE_DIR}/tools/embed_source.py
    COMMENT "Embedding stdlib/builtins.n"
)

add_library(nlang_compiler
    ${GENERATED_DIR}/nlang.tab.cpp
    ${GENERATED_DIR}/nlang.lex.cpp
    ${BUILTINS_EMBED}
    SyntaxNode.cpp
    SyntaxTree.cpp
    TranslationUnit.cpp
    ScriptParser.cpp
    ScriptScanner.cpp
    ScriptLocation.cpp
    Utf8.cpp
    BuildEnvironment.cpp
    ModuleBuilder.cpp
    ModuleBuilderImports.cpp
    ModuleBuilderChecks.cpp
    ModuleBuilderMethodContracts.cpp
    GenericDeclContracts.cpp
    GenericInstanceRegistry.cpp
    builder/ExprResolverTypeArgs.cpp
    builder/ExprResolverGenericUse.cpp
    ModuleBuilderStaticNamespace.cpp
    ModuleBuilderUnits.cpp
    Logger.cpp
    CastInfo.cpp
    SnArrayTypeToken.cpp
    SnTypes.cpp
    SnData.cpp
    SnExpressions.cpp
    SnExpressionsValues.cpp
    SnStatements.cpp
    SnStatementsFlow.cpp
    SnExtraTypes.cpp
    SnMisc.cpp
    SnTypeDecls.cpp
    Utils.cpp
    builder/ExprResolver.cpp
    builder/ExprResolverStdLib.cpp
    builder/ExprResolverPackageAdmission.cpp
    builder/ExprResolverMember.cpp
    builder/ExprResolverMemberBuiltins.cpp
    builder/ExprResolverStreamBuiltins.cpp
    builder/ExprResolverMemberProtocol.cpp
    builder/ExprResolverMemberFunc.cpp
    builder/ExprResolverMemberFields.cpp
    builder/ExprResolverTypes.cpp
    builder/ExprResolverGenerics.cpp
    builder/ExprResolverGenericMint.cpp
    builder/ExprResolverGenericDefaults.cpp
    builder/ExprResolverBuiltinClasses.cpp
    builder/ExprResolverStaticFace.cpp
    builder/ExprResolverThreadNs.cpp
    builder/ExprResolverTypedSpawn.cpp
    builder/ExprResolverFuncRef.cpp
    builder/ExprResolverValues.cpp
    builder/ExprResolverSubscript.cpp
    builder/ExprResolverAlias.cpp
    builder/ExprResolverBinary.cpp
    builder/ExprResolverTypeGates.cpp
    builder/ExprResolverNew.cpp
    builder/ExprResolverNewCtor.cpp
    builder/ExprResolverInitList.cpp
    builder/ExprResolverInitListTarget.cpp
    builder/ExprResolverInitListCtor.cpp
    builder/ExprResolverInitListForm.cpp
    builder/ExprResolverInvoke.cpp
    builder/ExprResolverInvokeDiag.cpp
    builder/ExprResolverInvokeMatch.cpp
    builder/ExprResolverInvokeReceiver.cpp
    builder/ExprResolverDelegate.cpp
    builder/ExprResolverCast.cpp
    builder/ExprResolverBinding.cpp
    builder/ExprResolverCastFit.cpp
    builder/StatementResolver.cpp
    builder/StatementResolverDecls.cpp
    builder/StatementResolverDeclCtor.cpp
    builder/StatementResolverDeclEnum.cpp
    builder/StatementResolverDeclArray.cpp
    builder/StatementResolverTypes.cpp
    builder/StatementResolverAssign.cpp
    builder/StatementResolverCompound.cpp
    builder/StatementResolverFlow.cpp
    builder/StatementResolverSwitchTry.cpp
    builder/StatementResolverSuper.cpp
    builder/StatementResolverStoreGates.cpp
    builder/ClassFieldInitializerInjection.cpp
    builder/ModuleRegistry.cpp
    builder/ModuleRegistryGate.cpp
)

target_include_directories(nlang_compiler
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        #nlang.tab.h (yytokentype) is the token vocabulary behind
        #ScriptScanner::NextToken -- tool consumers (IDE highlighting)
        #need it. Its prologue includes ScriptLocation.h/SyntaxTree.h by
        #bare name, so the compiler header subdir must be public too.
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/generated>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/nlang/compiler>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/builder
        ${PROJECT_SOURCE_DIR}/src/vm
)

target_link_libraries(nlang_compiler
    PUBLIC nlang_runtime
    PRIVATE nlang_langservice
)

target_compile_definitions(nlang_compiler
    PRIVATE NLANG_COMPILER_EXPORT
    PRIVATE YY_NO_UNISTD_H     # Prevent flex-generated .h from including <unistd.h>
)

if(NLANG_ENABLE_LLVM)
    target_compile_definitions(nlang_compiler PRIVATE NLANG_ENABLE_LLVM)
    target_include_directories(nlang_compiler PRIVATE ${LLVM_INCLUDE_DIRS})
    target_link_libraries(nlang_compiler PRIVATE ${LLVM_LIBRARIES})
endif()
