# Overview

NLang uses a register-based bytecode VM: the compiler back end
(`VmBackend`) translates the AST to bytecode, and the executor
(`VmExecutor`) interprets it instruction by instruction. This chapter is
the VM's implementation documentation, covering the bytecode instruction
set, heap and garbage collection, stack frame layout, module
serialization, and debugging support. Class names, function names, and
`OP_*` opcodes on these pages are real source identifiers you can use as
starting points for reading and searching the code.
