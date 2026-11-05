/*---
    IntrinsicsNetSupport.cpp — the net package's intrinsic family entry,
    the byte helpers (net.toBytes / net.fromBytes), the TcpListener
    arms shipped with the package skeleton, and the shared net helpers
    every protocol TU uses (handle minting, slot lookup, buffer access,
    the IO park). The exception ctors ride the shared Exception chain
    in VmExecutorIntrinsics.cpp; the entry claims their ids so an
    unknown-id throw never masks a missing arm (net library design §6).
---*/
#include "VmExecutor.h"
#include "net/NetPlatform.h"
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace nlang {

//TcpListener accept-queue depth (the backlog every listener API hides
//behind a constant; 64 matches the common library default).
constexpr int kNetListenBacklog = 64;

//Deadline arithmetic for the park loops: timeoutMs <= 0 means "no
//deadline" (the language face's infinite-wait convention) — the
//time_point::max() sentinel keeps the scheduler's deadline-ordered
//waiter containers well-defined.
std::chrono::steady_clock::time_point IoDeadline(int timeoutMs)
{
    if (timeoutMs <= 0)
        return std::chrono::steady_clock::time_point::max();
    return std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeoutMs);
}

//True once now has reached or passed the deadline (max() never does).
bool DeadlinePassed(std::chrono::steady_clock::time_point deadline)
{
    return !(std::chrono::steady_clock::now() < deadline);
}

//Family entry (contract: false when the id is not ours). Claims the
//whole 174..220 block; the exception ctors return false because the
//shared chain in VmExecutorIntrinsics.cpp already handled them — the
//claim only keeps them out of the per-protocol sub-families.
bool VmExecutor::ExecuteIntrinsicNet(uint16_t intrinsicId,
    uint16_t callParamBase, uint8_t* locals, uint8_t* pResult)
{
    if (intrinsicId < INTR_SocketException_Ctor
        || intrinsicId > INTR_IcmpSocket_Close)
        return false;
    //Idempotent through a once flag (ruling R4); every net arm needs
    //the socket subsystem up (Winsock on Windows).
    NetPlatformInit();
    if (intrinsicId <= INTR_TimeoutException_Ctor)
        return false;   //the shared Exception ctor chain owns these arms
    if (intrinsicId <= INTR_Net_Ping)
        return ExecuteIntrinsicNetSupport(intrinsicId, callParamBase,
                                          locals, pResult);
    if (intrinsicId <= INTR_TcpStream_LocalPort)
        return ExecuteIntrinsicNetTcp(intrinsicId, callParamBase,
                                      locals, pResult);
    if (intrinsicId <= INTR_UdpSocket_Close)
        return ExecuteIntrinsicNetUdp(intrinsicId, callParamBase,
                                      locals, pResult);
    return ExecuteIntrinsicNetIcmp(intrinsicId, callParamBase,
                                   locals, pResult);
}

//Support sub-family: the free byte helpers. net.ping joins here in
//task 9 (it runs on the resolver worker, not on the carrier).
bool VmExecutor::ExecuteIntrinsicNetSupport(uint16_t intrinsicId,
    uint16_t callParamBase, uint8_t* locals, uint8_t* pResult)
{
    switch (intrinsicId) {
    case INTR_Net_ToBytes:
        return IntrinsicNetToBytes(callParamBase, locals, pResult);
    case INTR_Net_FromBytes:
        return IntrinsicNetFromBytes(callParamBase, locals, pResult);
    default:
        return false;   //net.ping (task 9)
    }
}

//net.toBytes(string) — free call: the string handle rides slot 0. The
//string is the byte container (UTF-8 bytes pass through untouched,
//bypassing the string method face; net library design §5).
bool VmExecutor::IntrinsicNetToBytes(uint16_t callParamBase,
    uint8_t* locals, uint8_t* pResult)
{
    int32_t strHandle;
    std::memcpy(&strHandle, locals + callParamBase, sizeof(strHandle));
    const std::string& content = StrVal(strHandle);   //handle 0 reads ""
    const uint16_t arrayTypeIdx = FindScalarArrayTypeIdx(RTK_Byte, 1);
    const int32_t len = static_cast<int32_t>(content.size());
    const int32_t arrIdx = AllocArrayOnHeap(arrayTypeIdx, 1, &len);
    //Re-index after the allocation (the AllocArrayOnHeap invalidation
    //invariant): write the payload by index, never through a reference
    //held across the call.
    auto& rec = m_structHeap[static_cast<size_t>(arrIdx)];
    for (int32_t i = 0; i < len; ++i)
        rec[ArrayHeaderCells(1) + static_cast<size_t>(i)] =
            static_cast<uint8_t>(content[static_cast<size_t>(i)]);
    std::memcpy(pResult, &arrIdx, sizeof(arrIdx));
    return true;
}

//net.fromBytes(byte[]) — free call: the array handle rides slot 0.
//Views are legal arguments (slice syntax): ReadNetBufferArg resolves
//the payload base/count. Cells are int32 each; the low byte is the
//byte value (the array store's narrow-cell convention).
bool VmExecutor::IntrinsicNetFromBytes(uint16_t callParamBase,
    uint8_t* locals, uint8_t* pResult)
{
    int32_t arrIdx = 0;
    size_t base = 0;
    int32_t count = 0;
    ReadNetBufferArg(0, callParamBase, locals, arrIdx, base, count);
    auto& rec = m_structHeap[static_cast<size_t>(arrIdx)];
    std::string content;
    content.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i)
        content.push_back(
            static_cast<char>(rec[base + static_cast<size_t>(i)] & 0xFF));
    const int32_t strIdx = MintNewString(std::move(content));
    std::memcpy(pResult, &strIdx, sizeof(strIdx));
    return true;
}

//Tcp sub-family: the TcpListener arms shipped with the skeleton.
//accept and the TcpStream arms arrive in task 5.
bool VmExecutor::ExecuteIntrinsicNetTcp(uint16_t intrinsicId,
    uint16_t callParamBase, uint8_t* locals, uint8_t* pResult)
{
    switch (intrinsicId) {
    case INTR_TcpListener_Bind:
        return IntrinsicTcpListenerBind(callParamBase, locals, pResult);
    case INTR_TcpListener_LocalAddress: {
        int32_t handle = 0;
        NetSocketSlot* slot =
            NetSlotFromReceiver(callParamBase, locals, handle);
        const int32_t strIdx = MintNewString(NetLocalAddress(slot->fd));
        std::memcpy(pResult, &strIdx, sizeof(strIdx));
        return true;
    }
    case INTR_TcpListener_LocalPort: {
        int32_t handle = 0;
        NetSocketSlot* slot =
            NetSlotFromReceiver(callParamBase, locals, handle);
        WriteNetResultInt(pResult, NetLocalPort(slot->fd));
        return true;
    }
    case INTR_TcpListener_Close: {
        //close is idempotent: the handle cell is read directly, not
        //through NetSlotFromReceiver (which raises on a closed slot).
        //The fd is captured before MarkClosed — with no waiters the
        //close is immediate and FdOf reads -1 afterwards. The order
        //matters for the parked-waiter case (task 5): MarkClosed only
        //flags while waiters remain, RemoveIoWaitsForFd drops their
        //interest entries, and the last ReleaseWaiter closes the fd
        //(ruling R5).
        int32_t thisIdx;
        std::memcpy(&thisIdx, locals + callParamBase, sizeof(thisIdx));
        if (thisIdx <= 0
            || static_cast<size_t>(thisIdx) >= m_structHeap.size())
            throw std::runtime_error(
                "NLang VM: net call on null reference");
        const int32_t handle =
            m_structHeap[static_cast<size_t>(thisIdx)][1];
        const int fd = m_netSlots.FdOf(handle);
        m_netSlots.MarkClosed(handle);
        if (fd >= 0)
            m_spScheduler->RemoveIoWaitsForFd(fd);
        return true;
    }
    default:
        return false;   //accept + the TcpStream arms (task 5)
    }
}

//TcpListener.bind(host, port): static face — host at slot 0, port at
//slot 1 (no `this`). The first resolved endpoint picks the family
//(host "::" opens a dual-stack listener: NetBind clears IPV6_V6ONLY
//for it); port 0 is system-assigned and read back through localPort().
bool VmExecutor::IntrinsicTcpListenerBind(uint16_t callParamBase,
    uint8_t* locals, uint8_t* pResult)
{
    int32_t hostHandle;
    std::memcpy(&hostHandle, locals + callParamBase, sizeof(hostHandle));
    const std::string host = StrVal(hostHandle);   //copy: minting follows
    const int port = ReadNetIntArg(1, callParamBase, locals);
    std::vector<NetEndpoint> endpoints;
    if (!NetResolve(host, port, endpoints) || endpoints.empty())
        RaiseNetSocketError("bind " + host + ":" + std::to_string(port)
            + " failed: " + NetLastErrorText());
    const NetEndpoint& ep = endpoints.front();
    const int fd = NetOpenTcpStream(ep.family);
    if (fd < 0 || NetBind(fd, ep.address, port) != 0
        || NetListen(fd, kNetListenBacklog) != 0) {
        if (fd >= 0)
            NetClose(fd);
        RaiseNetSocketError("bind " + host + ":" + std::to_string(port)
            + " failed: " + NetLastErrorText());
    }
    const int32_t handle = m_netSlots.Mint(fd, kNetKindTcpListener);
    const int32_t objIdx = MintNetHandle(m_tcpListenerClassIdx, handle);
    std::memcpy(pResult, &objIdx, sizeof(objIdx));
    return true;
}

//Udp/Icmp sub-families: the arms arrive with their tasks (8/9). The
//entry routes their ids here meanwhile; false falls through to the
//unknown-id throw.
bool VmExecutor::ExecuteIntrinsicNetUdp(uint16_t, uint16_t, uint8_t*,
    uint8_t*)
{
    return false;
}

bool VmExecutor::ExecuteIntrinsicNetIcmp(uint16_t, uint16_t, uint8_t*,
    uint8_t*)
{
    return false;
}

//--- shared net helpers (every net TU uses these) -------------------------

//Reads and validates the byte-array argument at a slot frame. Raises
//SocketException (the user-visible error) on a null or wrong-shaped
//argument; the API surface is byte[]/ubyte[], rank 1, unit stride.
void VmExecutor::ReadNetBufferArg(uint16_t slotOffset,
    uint16_t callParamBase, uint8_t* locals, int32_t& outArrIdx,
    size_t& outBase, int32_t& outCount)
{
    int32_t handle;
    std::memcpy(&handle,
        locals + callParamBase + slotOffset * kFrameSlotBytes,
        sizeof(handle));
    if (handle <= 0
        || static_cast<size_t>(handle) >= m_slotKinds.size())
        RaiseNetSocketError("buffer argument is null");
    const size_t idx = static_cast<size_t>(handle);
    const uint8_t kind = m_slotKinds[idx];
    if (kind != RTK_Array && kind != RTK_ArrayView)
        RaiseNetSocketError("buffer argument is not a byte array");
    auto& rec = m_structHeap[idx];
    if (rec[2] != 1)
        RaiseNetSocketError("buffer argument must be a rank-1 array");
    const int32_t typeIdx = rec[1];
    if (typeIdx < 0
        || static_cast<size_t>(typeIdx) >= m_currModule->arrayTypes.size())
        RaiseNetSocketError("buffer argument has a corrupt array type");
    const uint8_t elemKind =
        m_currModule->arrayTypes[static_cast<size_t>(typeIdx)].elemKind;
    if (elemKind != RTK_Byte && elemKind != RTK_UByte)
        RaiseNetSocketError("buffer element kind must be byte or ubyte");
    if (kind == RTK_Array) {
        outArrIdx = handle;
        outBase = ArrayPayloadBase(rec);
        outCount = rec[3];
        return;
    }
    //View argument (post-merge slice syntax): the payload lives in the
    //target record at the recorded element offset (consumer pattern
    //VmExecutorNativeCallbacks.cpp:141); a non-unit stride cannot be
    //streamed into a net buffer.
    if (rec[ArrayViewStridesBase(1)] != 1)
        RaiseNetSocketError("view stride must be 1");
    const int32_t target = rec[ArrayViewTargetCell(1)];
    if (target <= 0
        || static_cast<size_t>(target) >= m_slotKinds.size()
        || m_slotKinds[static_cast<size_t>(target)] != RTK_Array)
        RaiseNetSocketError("view target is not a live array");
    auto& owner = m_structHeap[static_cast<size_t>(target)];
    outArrIdx = target;
    outBase = ArrayPayloadBase(owner)
        + static_cast<size_t>(rec[ArrayViewOffsetCell(1)]);
    outCount = rec[3];
}

//Copies count payload bytes into a net buffer's cells at base (the
//read direction fills them after recv); base already carries the view
//offset when the argument was a view.
void VmExecutor::WriteNetBufferCells(int32_t arrIdx, size_t base,
    const uint8_t* src, int count)
{
    auto& rec = m_structHeap[static_cast<size_t>(arrIdx)];
    for (int i = 0; i < count; ++i)
        rec[base + static_cast<size_t>(i)] = src[i];
}

//Mints a net handle object: the class instance with __handle (field 0
//under the uniform 2-cell stride, cell 1) set to the slot handle —
//the MintThreadObject shape (IntrinsicsThread.cpp:265). Direct write:
//AllocClassOnHeap grew the store before this line.
int32_t VmExecutor::MintNetHandle(int16_t classIdx, int32_t slotHandle)
{
    const int32_t objIdx =
        AllocClassOnHeap(static_cast<uint16_t>(classIdx));
    m_structHeap[static_cast<size_t>(objIdx)][1] = slotHandle;
    return objIdx;
}

//Reads the receiver's slot handle (slot 0 = this) and looks the slot
//up. A null/stale reference is a VM invariant violation (the script
//layer's null checks run before the call); a closed slot is the
//user-visible "socket closed" error.
NetSocketSlot* VmExecutor::NetSlotFromReceiver(uint16_t callParamBase,
    uint8_t* locals, int32_t& outHandle)
{
    int32_t thisIdx;
    std::memcpy(&thisIdx, locals + callParamBase, sizeof(thisIdx));
    if (thisIdx <= 0
        || static_cast<size_t>(thisIdx) >= m_structHeap.size())
        throw std::runtime_error("NLang VM: net call on null reference");
    const int32_t handle =
        m_structHeap[static_cast<size_t>(thisIdx)][1];
    outHandle = handle;
    NetSocketSlot* slot = m_netSlots.Lookup(handle);
    if (!slot)
        RaiseNetSocketError("socket closed");
    return slot;
}

//True when the slot handle no longer resolves to a live slot (the
//close-race check inside the park loops).
bool VmExecutor::IsNetSlotClosed(int32_t handle)
{
    return m_netSlots.Lookup(handle) == nullptr;
}

//User-visible net failure: SocketException through the by-name class
//cache (RaiseNlangException handles the -1 fallback itself).
void VmExecutor::RaiseNetSocketError(const std::string& msg)
{
    RaiseNlangException(m_socketExcClassIdx, "net: " + msg);
}

//Reads the int argument at slot offset n (stride kFrameSlotBytes from
//callParamBase; the memcpy pattern of the string intrinsics).
int VmExecutor::ReadNetIntArg(int slotOffset, uint16_t callParamBase,
    uint8_t* locals)
{
    int32_t v;
    std::memcpy(&v,
        locals + callParamBase + slotOffset * kFrameSlotBytes, sizeof(v));
    return v;
}

//Writes an int result into pResult (memcpy pattern of the host
//natives).
void VmExecutor::WriteNetResultInt(uint8_t* pResult, int value)
{
    const int32_t v = value;
    std::memcpy(pResult, &v, sizeof(v));
}

//Parks the current context on fd+events until ready/deadline/close:
//the sleep park shape (IntrinsicsThread.cpp:383) — the suspension
//gate runs BEFORE mutating scheduler state, and RequestPark never
//returns (the wake-before-steal window is closed by the same K7
//framesParked handshake). On the wake the intrinsic re-enters right
//after this call and re-looks-up the slot.
void VmExecutor::ParkForCurrentIo(int fd, uint32_t events,
    std::chrono::steady_clock::time_point deadline, bool hasDeadline)
{
    EnsureSuspensionAllowed();
    m_spScheduler->ParkForIo(m_spScheduler->CurrentContextId(), fd,
                             events, deadline, hasDeadline);
    RequestPark(ParkSignal::Reason::Io);   //[[noreturn]]
}

} // namespace nlang
