/*---
    BuiltinClasses.cpp — the builtin class registry, fully retired
    (v0.12.7 Task 8.5). Every user-visible builtin class now comes from
    a standard-library .n declaration whose native records stamp onto
    their intrinsic ids through the compiler-side binding table
    (compiler/builder/BindingTable.cpp): Object/Exception family/String/
    Array faces in stdlib/lang.n, containers in stdlib/containers.n,
    thread/concurrency classes in stdlib/thread.n +
    stdlib/concurrent.n, and the stream classes in stdlib/io.n (the
    final two rows this file used to synthesize). The registration
    entry point stays so callers need no change; the table is empty.
---*/
#include "VmBackend.h"
#include <nlang/runtime/NodeConsts.h>
#include <iterator>
#include <utility>

namespace nlang {


namespace {

//No registration table remains (a zero-sized array is ill-formed on
//MSVC, and an empty shape would guard nothing). The "every builtin
//class needs a .n declaration face" invariant lives in the coverage
//gate (tests/test_compiler/test_builtin_surface_coverage.cpp), which
//fails by name when a C++-only builtin surface appears.

} //namespace

void VmBackend::RegisterBuiltinClasses() {
    //Empty since the stream classes moved to stdlib/io.n (Task 8.5);
    //kept as the stable registration entry point.
}

} //namespace nlang
