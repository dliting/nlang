/*---
    IntrinsicsThread.cpp — K1 并发内在函数族：
      · thread.* 自由调用（129-133）：OP_CallFunc 内建臂派发，参数自
        callParamBase 槽 0 起（无 this）；
      · Mutex/Channel 方法（134-141）：接收者派发形态（槽 0 = this）。
    Slice 5 仅真实实现 yield/id；其余在切片 6-9 逐个接入，此前统一
    「not implemented yet」基础异常（编译面已注册，行为面未到）。
---*/
#include "VmExecutor.h"
#include <cstring>
#include <stdexcept>

namespace nlang {

//Slices 6-9 land the real implementations; until then every
//not-yet-arrived intrinsic reports its surface name and throws.
[[noreturn]] static void ThrowThreadNotImplemented(const char* surface)
{
    throw std::runtime_error(
        std::string("NLang VM: ") + surface + " is not implemented yet");
}

bool VmExecutor::ExecuteIntrinsicThread(uint16_t intrinsicId,
    uint16_t callParamBase, uint8_t* locals, uint8_t* pResult)
{
    switch (intrinsicId)
    {
    case INTR_Thread_Yield:
        //Explicit cooperative yield: the same Preempted signal as quota
        //preemption — the carrier requeues the context and the resume
        //flag re-enters RunFrames at the advanced pc. [[noreturn]].
        RequestPark(ParkSignal::Reason::Preempted);
    case INTR_Thread_Id:
    {
        int32_t id = m_spScheduler->CurrentContextId();
        std::memcpy(pResult, &id, sizeof(id));
        return true;
    }
    //Registered but not yet implemented — each arrives in its slice
    //(spawn/join Slice 6, sleep Slice 7, Mutex Slice 8, Channel Slice 9).
    case INTR_Thread_Spawn:
        ThrowThreadNotImplemented("thread.spawn");
    case INTR_Thread_Join:
        ThrowThreadNotImplemented("thread.join");
    case INTR_Thread_Sleep:
        ThrowThreadNotImplemented("thread.sleep");
    case INTR_Mutex_Ctor:
        ThrowThreadNotImplemented("Mutex constructor");
    case INTR_Mutex_Lock:
        ThrowThreadNotImplemented("Mutex.lock");
    case INTR_Mutex_Unlock:
        ThrowThreadNotImplemented("Mutex.unlock");
    case INTR_Mutex_TryLock:
        ThrowThreadNotImplemented("Mutex.tryLock");
    case INTR_Channel_Ctor:
        ThrowThreadNotImplemented("Channel constructor");
    case INTR_Channel_Send:
        ThrowThreadNotImplemented("Channel.send");
    case INTR_Channel_Recv:
        ThrowThreadNotImplemented("Channel.recv");
    case INTR_Channel_Close:
        ThrowThreadNotImplemented("Channel.close");
    default:
        return false;   //not ours — ExecuteIntrinsic reaches the
                        //unknown-id throw
    }
}

} // namespace nlang
