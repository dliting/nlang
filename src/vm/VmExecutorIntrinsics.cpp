/*---
    VmExecutorIntrinsics.cpp — 内建函数分派（ExecuteIntrinsic）与 native 桥
    从 VmExecutor.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
---*/
#include "VmExecutor.h"
#include "VmExecutorSer.h"
#include <nlang/runtime/PrimitiveTypes.h>
#include <cstring>
#include <cstdio>

namespace nlang {

//K2 slice 4: the native-call bridge (registration, CallNative dispatch
//and the blocking-native protocol) lives in VmExecutorNativeHost.cpp
//with the rest of the native ABI surface. This file keeps the VM-side
//intrinsic dispatch.

void VmExecutor::ExecuteIntrinsic(uint16_t intrinsicId, uint16_t callParamBase,
    uint8_t* locals, uint8_t* pResult)
{
    //K8: no whole-dispatch guard — each family owns its domain lock
    //(Container/Stream in their TUs; string arms self-lock via mint
    //flatten); the thread/sync family still holds the store lock
    //wholly (see IntrinsicsThread.cpp) until the scheduler lock
    //restatement lands alongside its Sync domain.
    //ByteStream intrinsics (0-18): family delegation to
    //IntrinsicsByteStream.cpp (same shape as the string family below).
    if (ExecuteIntrinsicByteStream(intrinsicId, callParamBase, locals, pResult))
        return;

    //FileStream intrinsics (20-37): family delegation to
    //IntrinsicsFileStream.cpp.
    if (ExecuteIntrinsicFileStream(intrinsicId, callParamBase, locals, pResult))
        return;

    //K1 concurrency intrinsics (129-141): thread.* free calls and the
    //Mutex/Channel methods — family delegation to IntrinsicsThread.cpp
    //(same contract: false when the id is not ours).
    if (ExecuteIntrinsicThread(intrinsicId, callParamBase, locals, pResult))
        return;

    //Phase 8e-1: Object protocol intrinsics.
    //Object.Equals(this, other) → 0 or 1 (identity comparison on heap idx).
    //Object.GetHashCode(this) → heap idx as int32 (identity-based hash).
    //Both handle null: two nulls are equal, null has hash 0.
    if (intrinsicId == INTR_Object_Equals) {
        int32_t thisHeapIdx, otherHeapIdx;
        std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
        std::memcpy(&otherHeapIdx, locals + callParamBase + kFrameSlotBytes,
                    sizeof(otherHeapIdx));
        int32_t result;
        if (thisHeapIdx == 0 && otherHeapIdx == 0)
            result = 1;  //two nulls are equal
        else if (thisHeapIdx == 0 || otherHeapIdx == 0)
            result = 0;  //one null, one non-null
        else
            result = (thisHeapIdx == otherHeapIdx) ? 1 : 0;
        std::memcpy(pResult, &result, sizeof(result));
        return;
    }
    if (intrinsicId == INTR_Object_GetHashCode) {
        int32_t thisHeapIdx;
        std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
        int32_t result = (thisHeapIdx > 0) ? thisHeapIdx : 0;
        std::memcpy(pResult, &result, sizeof(result));
        return;
    }

    //Phase 8e-1: String.Equals/GetHashCode migrated to
    //IntrinsicsString.cpp (ids unchanged); the Phase 11 Step 3 string
    //methods dispatch there too — see the family chain below.

    //Phase 8e-9b: Object.toString() default intrinsic.
    //Returns "TypeName@hex(heapIdx)" — Java-compat (lowercase, no padding).
    //Null receiver throws NPE. The hex uses heap idx directly (not user
    //override of getHashCode) — documented divergence from Java.
    if (intrinsicId == INTR_Object_toString) {
        int32_t thisHeapIdx;
        std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
        if (thisHeapIdx <= 0)
            RaiseNlangException(m_nullPtrExcClassIdx,
                                "NLang VM: NullPointerException");
        if (static_cast<size_t>(thisHeapIdx) >= m_structHeap.size())
            throw std::runtime_error("NLang VM: toString on invalid heap idx");
        //Boxed receiver: the payload IS the value — format it like the
        //matching primitive-to-string conversion (a boxed string's
        //toString shares the payload handle: string objects are
        //immutable, and handle 0 reads as "" everywhere).
        if (m_slotKinds[static_cast<size_t>(thisHeapIdx)] == RTK_Boxed) {
            const auto& rec = m_structHeap[static_cast<size_t>(thisHeapIdx)];
            int32_t handle;
            if (rec[0] == RTK_String) {
                handle = rec[1] > 0 ? rec[1] : MintNewString(std::string());
            } else if (ScalarPrimIndexOfRtk(
                           static_cast<uint8_t>(rec[0])) >= 0) {
                //0.7.5: the full scalar family (narrow rows, uint,
                //long/ulong) renders through the canonical registry
                //renderer — records are {tag, lo, hi}.
                handle = MintNewString(FormatScalarValue(
                    static_cast<uint8_t>(rec[0]), rec[1], rec[2]));
            } else {
                throw std::runtime_error(
                    "NLang VM: toString on unsupported boxed type tag");
            }
            std::memcpy(pResult, &handle, sizeof(handle));
            return;
        }
        int32_t classIdx = m_structHeap[static_cast<size_t>(thisHeapIdx)][0];
        if (classIdx < 0
            || static_cast<size_t>(classIdx) >= m_currModule->classes.size())
            throw std::runtime_error("NLang VM: toString on invalid class idx");
        const std::string& className =
            m_currModule->classes[static_cast<size_t>(classIdx)].name;
        char buf[64];
        //%x yields lowercase no-padding (Java Integer.toHexString-compatible).
        //Leaf name: the table key is package-qualified linkage identity;
        //toString names the class as source wrote it (Java style).
        std::snprintf(buf, sizeof(buf), "%s@%x",
                      LeafNameOfKey(className).c_str(),
                      static_cast<unsigned>(thisHeapIdx));
        int32_t handle = MintNewString(buf);
        std::memcpy(pResult, &handle, sizeof(handle));
        return;
    }

    //Phase 9d: Exception ctor intrinsic. Shared by all 6 built-in
    //Exception subclasses (INTR_Exception_Ctor, INTR_NullPointer..,
    //INTR_DivByZero.., INTR_IndexOutOfBounds.., INTR_Assertion..).
    //Formals: (this, message). Writes message idx to cell[1], allocates
    //an empty List<string> and stores its heap idx in cell[3] (backtrace
    //— field 1 under the uniform 2-cell stride).
    //Direct writes (no held references) because AllocClassOnHeap may
    //reallocate m_structHeap mid-execution.
    if (intrinsicId == INTR_Exception_Ctor
        || intrinsicId == INTR_NullPointerException_Ctor
        || intrinsicId == INTR_DivByZeroException_Ctor
        || intrinsicId == INTR_IndexOutOfBoundsException_Ctor
        || intrinsicId == INTR_AssertionException_Ctor
        || intrinsicId == INTR_IOException_Ctor
        || intrinsicId == INTR_InvalidCastException_Ctor
        || intrinsicId == INTR_ChannelClosedException_Ctor) {
        int32_t thisHeapIdx;
        std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
        if (thisHeapIdx <= 0)
            throw std::runtime_error(
                "NLang VM: Exception ctor on null instance");
        if (static_cast<size_t>(thisHeapIdx) >= m_structHeap.size())
            throw std::runtime_error(
                "NLang VM: Exception ctor on stale reference");

        //Read message formal (string handle).
        int32_t msgHandle;
        std::memcpy(&msgHandle, locals + callParamBase + kFrameSlotBytes,
                    sizeof(msgHandle));

        //cell[1] = message (string handle). Direct write — safe because
        //no allocation between read and write.
        m_structHeap[static_cast<size_t>(thisHeapIdx)][1] = msgHandle;

        //cell[3] = backtrace. Allocate a List<string>, set __handle = 0
        //(empty), then write its heap idx to cell[3]. The allocation may
        //reallocate m_structHeap, so we don't hold any references across
        //the AllocClassOnHeap call.
        int32_t listHeapIdx = AllocClassOnHeap(
            static_cast<uint16_t>(m_listClassIdx));
        if (listHeapIdx > 0) {
            //__handle field (cell[1] of List instance) = 0 = empty
            m_structHeap[static_cast<size_t>(listHeapIdx)][1] = 0;
            m_structHeap[static_cast<size_t>(thisHeapIdx)][3] = listHeapIdx;
        }
        return;
    }

    //Container intrinsics (List/Dict, Phase 8e-3/8e-4): family
    //delegation to IntrinsicsContainer.cpp — the Container domain
    //lock lives there (structural arms under it, toString arms
    //snapshot-and-release so user overrides re-enter freely).
    if (ExecuteIntrinsicContainer(intrinsicId, callParamBase, locals,
                                  pResult))
        return;

    //String methods (receiver at callParamBase slot 0, args from slot 1).
    //Chained before the unknown-id throw so each family TU stays
    //independently extensible. math/io/fs are not here: they are library
    //sources whose native members run from nlang_<ns>.dll.
    if (ExecuteIntrinsicString(intrinsicId, callParamBase, locals, pResult))
        return;

    throw std::runtime_error("NLang VM: unknown intrinsic id " + std::to_string(intrinsicId));
}

} // namespace nlang
