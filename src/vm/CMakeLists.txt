add_library(nlang_vm
    OpCodeTable.cpp
    VmScheduler.cpp
    GcWorldStop.cpp
    VmSchedulerTimer.cpp
    VmSchedulerIo.cpp
    VmSchedulerSync.cpp
    VmWorkPool.cpp
    VmSchedulerJoin.cpp
    VmExecutor.cpp
    VmExecutorRaise.cpp
    VmExecutorPump.cpp
    VmCarrierPool.cpp
    VmExecutorHostBridge.cpp
    VmExecutorHostProxyBridge.cpp
    VmExecutorStrings.cpp
    VmExecutorDebug.cpp
    VmExecutorDebugArray.cpp
    VmExecutorDebugContainers.cpp
    VmExecutorDebugStatics.cpp
    VmExecutorDebugSlots.cpp
    VmExecutorFreeze.cpp
    VmExecutorOps.cpp
    VmExecutorFrames.cpp
    VmExecutorOpsArith.cpp
    VmExecutorOpsPrim.cpp
    VmExecutorOpsBit.cpp
    VmExecutorOpsControl.cpp
    VmExecutorOpsCalls.cpp
    VmExecutorOpsStrings.cpp
    VmExecutorOpsArrayN.cpp
    VmExecutorOpsAssemble.cpp
    VmExecutorOpsSlice.cpp
    VmExecutorOpsArrayCheck.cpp
    VmExecutorOpsObjects.cpp
    VmExecutorOpsRef.cpp
    VmExecutorOpsDescriptor.cpp
    VmExecutorOpsStaticInit.cpp
    VmExecutorGC.cpp
    VmExecutorGCContainers.cpp
    VmExecutorGCRoots.cpp
    VmExecutorGCSweep.cpp
    VmExecutorGCMarks.cpp
    VmExecutorAlloc.cpp
    VmClassLayout.cpp
    VmStaticSlots.cpp
    VmDefaultMaterializer.cpp
    VmExecutorContainers.cpp
    VmExecutorFormat.cpp
    VmExecutorIntrinsics.cpp
    VmExecutorNativeHost.cpp
    VmExecutorNativeHostText.cpp
    VmExecutorNativeLoader.cpp
    VmExecutorNativeCallbacks.cpp
    VmExecutorNativeArray.cpp
    VmExecutorBlockingNative.cpp
    VmExecutorFailure.cpp
    InputLineSource.cpp
    TokenView.cpp
    DebugSessionController.cpp
    IntrinsicsContainer.cpp
    IntrinsicsString.cpp
    IntrinsicsByteStream.cpp
    IntrinsicsFileStream.cpp
    IntrinsicsThread.cpp
    IntrinsicsFuture.cpp
    IntrinsicsPromise.cpp
    IntrinsicsChannel.cpp
    IntrinsicsMutex.cpp
    IntrinsicsSemaphore.cpp
    IntrinsicsAtomic.cpp
    GenericInstDescriptor.cpp
    CrashReporter.cpp
    Disassembler.cpp
    DisassemblerPrim.cpp
    DisassemblerWalk.cpp
    DisassemblerFormat.cpp
    DisassemblerCalls.cpp
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
    NcuLinker.cpp
    NcuLinkerRemap.cpp
    NcuLinkerSlots.cpp
    NcuLinkerInterfaces.cpp
    NcuLoader.cpp
    NativeLibraryLoader.cpp
    TypeDesc.cpp
    TypeDescIndices.cpp
)

#The IO poller abstraction (IoPoller.h) is a VM facility — the net
#module's IO parking (ABI v11 parkForIo) is its first consumer. The
#NetPlatform implementations moved to the nlang_net native module
#(src/native/net).
if(WIN32)
    target_sources(nlang_vm PRIVATE
        IoPollerWindows.cpp)
else()
    target_sources(nlang_vm PRIVATE
        IoPollerPosix.cpp)
endif()

target_include_directories(nlang_vm
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        #Residual edge of the pre-split vm <-> compiler cycle (library
        #layering split, docs/dev/library_layering_design.md §6): no vm
        #source includes compiler headers anymore; kept until the core
        #split rules on the link edge. Its twin: src/compiler/CMakeLists.txt
        #lists src/vm.
        ${PROJECT_SOURCE_DIR}/src/compiler
)

target_link_libraries(nlang_vm
    PUBLIC nlang_runtime
    PRIVATE nlang_compiler ${CMAKE_DL_LIBS}
)

if(WIN32)
    #Vendored wepoll (IOCP-backed epoll-compatible poller, BSD-2-Clause —
    #src/3rdparty/wepoll/LICENSE.txt) needs winsock2; iphlpapi
    #(IcmpSendEcho2) moved to nlang_net with the ICMP platform TU.
    target_sources(nlang_vm PRIVATE
        ${PROJECT_SOURCE_DIR}/src/3rdparty/wepoll/wepoll.c)
    target_include_directories(nlang_vm PRIVATE
        ${PROJECT_SOURCE_DIR}/src/3rdparty/wepoll)
    target_link_libraries(nlang_vm PRIVATE ws2_32)
endif()
