add_library(nlang_vm
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
    VmExecutorDeviceArray.cpp
    VmClassLayout.cpp
    VmStaticSlots.cpp
    VmDefaultMaterializer.cpp
    VmExecutorContainers.cpp
    VmExecutorFormat.cpp
    VmExecutorFormatArray.cpp
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
    CrashReporter.cpp
    NativeLibraryLoader.cpp
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
)

target_link_libraries(nlang_vm
    PUBLIC nlang_core
    PRIVATE ${CMAKE_DL_LIBS}
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
