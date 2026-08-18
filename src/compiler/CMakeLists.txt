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
    COMMAND ${BISON_EXE} -d --header=${GENERATED_DIR}/nlang.tab.h
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

add_library(nlang_compiler
    ${GENERATED_DIR}/nlang.tab.cpp
    ${GENERATED_DIR}/nlang.lex.cpp
    SyntaxNode.cpp
    SyntaxTree.cpp
    TranslationUnit.cpp
    ScriptParser.cpp
    ScriptScanner.cpp
    ScriptLocation.cpp
    BuildEnvironment.cpp
    ModuleBuilder.cpp
    Logger.cpp
    CastInfo.cpp
    SnTypes.cpp
    SnData.cpp
    SnExpressions.cpp
    SnStatements.cpp
    SnExtraTypes.cpp
    SnMisc.cpp
    Utils.cpp
    builder/ExprResolver.cpp
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
