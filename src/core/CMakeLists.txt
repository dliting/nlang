#nlang_core — the shared vocabulary between the compiler and the VM
#(library layering split, docs/dev/library_layering_design.md §6): the
#module-image wire format (loader/saver/NCU package/linker), the
#bytecode opcode tables and disassembler, and the generic-instantiation
#descriptor spelling layer. Depends only on nlang_runtime; both
#nlang_compiler and nlang_vm depend on it, never on each other.
add_library(nlang_core
    OpCodeTable.cpp
    GenericInstDescriptor.cpp
    CanonicalArgSpelling.cpp
    GenericInstSpelling.cpp
    TypeDesc.cpp
    TypeDescIndices.cpp
    ModuleLoader.cpp
    ModuleLoaderRecords.cpp
    ModuleLoaderRecordsInterfaces.cpp
    ModuleLoaderRecordsClasses.cpp
    ModuleSaver.cpp
    ModuleSaverRecordsFuncs.cpp
    ModuleSaverRecordsClasses.cpp
    ModuleSaverRecordsInterfaces.cpp
    ModuleSaverRecordsEnums.cpp
    NcuPackage.cpp
    NcuLoader.cpp
    NcuLinker.cpp
    NcuLinkerRemap.cpp
    NcuLinkerSlots.cpp
    NcuLinkerInterfaces.cpp
    Disassembler.cpp
    DisassemblerPrim.cpp
    DisassemblerWalk.cpp
    DisassemblerFormat.cpp
    DisassemblerCalls.cpp
)

target_include_directories(nlang_core
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(nlang_core
    PUBLIC nlang_runtime
)
