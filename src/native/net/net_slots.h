/*---
    net_slots.h — the per-executor socket slot registry (net library
    design §6): __handle values are (generation<<kNetHandleIndexBits
    | (index+1)) so a closed slot's recycled index never aliases a live
    handle (ABA). The fd close is deferred while waiters are parked on
    the slot (plan ruling R5): MarkClosed only flags, the last
    ReleaseWaiter performs the real NetClose.

    The slot returns to the free list the moment it is both closed and
    waiter-free: whichever of MarkClosed / ReleaseWaiter observes that
    combination performs the close and the recycle, so no caller has a
    second step to forget (final review M10).

    Locking: guarded by NetExecState::m_mutex (plan ruling R9) — with
    r1 blocking natives the GIL is released around the syscall, so
    calls run concurrently across carriers and the old "everything
    under the VM global lock" premise (the intrinsic channel) no
    longer holds. The table itself takes no lock.
---*/
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nlang {

//Slot kinds (the __handle carries no kind; the slot does).
constexpr uint8_t kNetKindTcpListener = 1;
constexpr uint8_t kNetKindTcpStream = 2;
constexpr uint8_t kNetKindUdp = 3;
constexpr uint8_t kNetKindIcmp = 4;

//Waiter directions (stored on the slot, not in ContextState — one
//ParkedIo state serves every direction).
enum class NetDir : uint8_t { Read, Write, Accept };

//Handle codec width (net_slots.cpp Mint/SlotOf): the generation lives
//in the bits above this, the slot index+1 below.
constexpr int32_t kNetHandleIndexBits = 24;

struct NetSocketSlot {
    int fd = -1;
    uint8_t kind = 0;            //0 = free
    bool closed = false;
    int generation = 0;          //increments on every free
    int acceptWaiter = 0;        //context id, 0 = none
    int readWaiter = 0;
    int writeWaiter = 0;
    //Bytes of the in-flight write already handed to the kernel: a
    //park/re-entry restarts the native from its first line, so the
    //write offset lives here, not on the rebuilt stack. Only the
    //writeWaiter's call reads or writes it.
    int writeSent = 0;
    //Last recvFrom source (UDP/ICMP): plain text, minted to a string
    //on demand — no GC root.
    std::string lastPeerAddress;
    int lastPeerPort = 0;
};

class NetSocketTable {
public:
    //Takes the fd into a free slot (appends when none free).
    int32_t Mint(int fd, uint8_t kind);
    //The slot for a handle, or null when the handle is stale (index
    //freed/reused) or the slot is closed.
    NetSocketSlot* Lookup(int32_t handle);
    const NetSocketSlot* Lookup(int32_t handle) const;
    void SetWaiter(int32_t handle, NetDir dir, int contextId);
    //Clears the direction's waiter id; when the slot is closed and no
    //waiter remains, performs the deferred real fd close and returns
    //the slot to the free list.
    void ReleaseWaiter(int32_t handle, NetDir dir);
    //Flags closed. With no waiters the fd closes and the slot is
    //recycled immediately; otherwise the last ReleaseWaiter does both
    //(R5). Idempotent: a recycled handle is simply unknown.
    void MarkClosed(int32_t handle);
    //Returns the slot's fd (-1 once really closed). Used by close/
    //option arms before the closed check.
    int FdOf(int32_t handle) const;
    int OpenCount() const;   //slots with a live fd (leak assertions)
    //Closes every still-open fd and clears the slots (executor
    //teardown backstop, design D12 — the script never closed them).
    void CloseAllOpen();

private:
    //Closes the fd and returns the slot to the free list (generation
    //++). Precondition: closed and no waiters (asserted). Called by
    //MarkClosed / ReleaseWaiter at the moment both hold.
    void Recycle(int32_t handle);

    std::vector<NetSocketSlot> m_slots;
    std::vector<int32_t> m_freeList;
};

}  //namespace nlang
