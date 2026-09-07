#include "ModuleLoader.h"
#include "Disassembler.h"
#include <nlang_version.h>  // generated from the repo VERSION file
#include <iostream>
#include <iterator>  //std::size for s_typeKindNames bound
#include <string>

using namespace nlang;

static const char* s_typeKindNames[] = {
    "i32",    // NK_Int32 = 0 (also default for void-like functions)
    "f32",    // NK_Float = 1
    "str",    // NK_String = 2
    "struct", // RTK_Struct = 3
    "class",  // RTK_Class = 4
    "array",  // RTK_Array = 5 (serialized array return types, Phase 11 Step 0)
    "boxed"   // RTK_Boxed = 6 (Phase 8e-1 boxed primitive)
};

static const char* TypeKindName(uint16_t kind) {
    if (kind < std::size(s_typeKindNames))
        return s_typeKindNames[kind];
    return "unknown";
}

static void DisassembleFunction(const CompiledFunction& func,
                                 const CompiledModule& module) {
    std::cout << "function " << func.name
              << " (frameSize=" << func.localsSize
              << ", params=" << func.paramCount
              << ", returnType=" << TypeKindName(func.returnTypeKind);
    if (func.isNative)
        std::cout << ", native";
    if (func.intrinsicId != 0xFFFF)
        std::cout << ", intrinsic=" << func.intrinsicId;
    if (!func.sourceFile.empty())
        std::cout << ", file=" << func.sourceFile;
    std::cout << ")\n";

    if (func.bytecode.empty()) {
        std::cout << "  (no bytecode)\n\n";
        return;
    }

    std::cout << "  bytecode:\n";
    for (const auto& line : DisassembleCode(func, module))
        std::cout << "    " << line.text << "\n";
    std::cout << DisassembleTryBlocks(func);
    std::cout << "\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: ndisasm <module.nmod>\n"
                  << "       ndisasm -func <name> <module.nmod>\n"
                  << "       ndisasm --version\n";
        return 1;
    }

    //Version gate: before any module loading, so no runtime init runs.
    if (std::string(argv[1]) == "--version") {
        std::cout << "ndisasm (NLang) " << NLANG_VERSION << "\n";
        return 0;
    }

    std::string funcFilter;
    std::string modulePath;

    if (std::string(argv[1]) == "-func" && argc >= 4) {
        funcFilter = argv[2];
        modulePath = argv[3];
    } else {
        modulePath = argv[1];
    }

    CompiledModule module;
    try {
        module = ModuleLoader::Load(modulePath);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    std::cout << "module: " << module.name << "\n";

    //Struct descriptors
    if (module.structs.empty()) {
        std::cout << "structs: (none)\n";
    } else {
        std::cout << "structs:\n";
        for (size_t i = 0; i < module.structs.size(); ++i) {
            auto& st = module.structs[i];
            std::cout << "  [" << i << "] " << st.name
                      << " (fields=" << st.fieldCount << ")\n";
            for (uint16_t j = 0; j < st.fieldCount; ++j) {
                std::cout << "    " << st.fieldNames[j]
                          << ": " << TypeKindName(st.fieldTypeKinds[j]);
                if (st.fieldTypeKinds[j] == RTK_Struct
                    && st.fieldStructIndices[j] != 0xFFFF)
                    std::cout << " [" << st.fieldStructIndices[j] << "]";
                std::cout << "\n";
            }
        }
    }
    std::cout << "\n";

    //Class descriptors
    if (module.classes.empty()) {
        std::cout << "classes: (none)\n";
    } else {
        std::cout << "classes:\n";
        for (size_t i = 0; i < module.classes.size(); ++i) {
            auto& cc = module.classes[i];
            std::cout << "  [" << i << "] " << cc.name
                      << " (fields=" << cc.fieldCount
                      << ", super=" << cc.superClassIdx << ")\n";
            for (uint16_t j = 0; j < cc.fieldCount; ++j) {
                std::cout << "    " << cc.fieldNames[j]
                          << ": " << TypeKindName(cc.fieldTypeKinds[j]);
                if (cc.fieldTypeKinds[j] == RTK_Struct
                    && cc.fieldStructIndices[j] != 0xFFFF)
                    std::cout << " [" << cc.fieldStructIndices[j] << "]";
                if (cc.fieldTypeKinds[j] == RTK_Class
                    && cc.fieldClassIndices[j] != 0xFFFF)
                    std::cout << " [" << cc.fieldClassIndices[j] << "]";
                std::cout << "\n";
            }
        }
    }
    std::cout << "\n";

    //String constants
    if (module.stringConstants.empty()) {
        std::cout << "string constants: (none)\n";
    } else {
        std::cout << "string constants:\n";
        for (size_t i = 0; i < module.stringConstants.size(); ++i) {
            std::cout << "  [" << i << "] \"" << module.stringConstants[i]
                      << "\"\n";
        }
    }
    std::cout << "\n";

    //Functions
    for (auto& func : module.functions) {
        if (!funcFilter.empty() && func.name != funcFilter)
            continue;
        DisassembleFunction(func, module);
    }

    return 0;
}
