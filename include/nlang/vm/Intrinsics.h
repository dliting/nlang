#pragma once
#include <cstdint>

namespace nlang {

//Builtin intrinsic function IDs.
//Used by CompiledFunction::intrinsicId to dispatch VM-side methods.
static constexpr uint16_t INTR_None = 0xFFFF;

//ByteStream intrinsics.
static constexpr uint16_t INTR_BS_Ctor         = 0;
static constexpr uint16_t INTR_BS_WriteInt     = 1;
static constexpr uint16_t INTR_BS_ReadInt      = 2;
static constexpr uint16_t INTR_BS_WriteFloat   = 3;
static constexpr uint16_t INTR_BS_ReadFloat    = 4;
static constexpr uint16_t INTR_BS_WriteString  = 5;
static constexpr uint16_t INTR_BS_ReadString   = 6;
static constexpr uint16_t INTR_BS_Length       = 7;
static constexpr uint16_t INTR_BS_Position     = 8;
static constexpr uint16_t INTR_BS_Reset        = 9;
static constexpr uint16_t INTR_BS_Close        = 10;
static constexpr uint16_t INTR_BS_WriteStruct = 11;
static constexpr uint16_t INTR_BS_ReadStruct  = 12;
static constexpr uint16_t INTR_BS_WriteObject = 13;
static constexpr uint16_t INTR_BS_ReadObject  = 14;
//0.7.5 Task 9: 64-bit scalar family (8-byte wire form, mirroring the
//4-byte writeInt/readInt/writeFloat/readFloat quartet).
static constexpr uint16_t INTR_BS_WriteLong   = 15;
static constexpr uint16_t INTR_BS_ReadLong    = 16;
static constexpr uint16_t INTR_BS_WriteDouble = 17;
static constexpr uint16_t INTR_BS_ReadDouble  = 18;

//FileStream intrinsics.
static constexpr uint16_t INTR_FS_Ctor         = 20;
static constexpr uint16_t INTR_FS_WriteInt     = 21;
static constexpr uint16_t INTR_FS_ReadInt      = 22;
static constexpr uint16_t INTR_FS_WriteFloat   = 23;
static constexpr uint16_t INTR_FS_ReadFloat    = 24;
static constexpr uint16_t INTR_FS_WriteString  = 25;
static constexpr uint16_t INTR_FS_ReadString   = 26;
static constexpr uint16_t INTR_FS_Length       = 27;
static constexpr uint16_t INTR_FS_Position     = 28;
static constexpr uint16_t INTR_FS_Close        = 29;
static constexpr uint16_t INTR_FS_WriteStruct = 30;
static constexpr uint16_t INTR_FS_ReadStruct  = 31;
static constexpr uint16_t INTR_FS_WriteObject = 32;
static constexpr uint16_t INTR_FS_ReadObject  = 33;
//0.7.5 Task 9: 64-bit scalar family (34..37; 30..33 were already taken
//by the struct/object serialization block above).
static constexpr uint16_t INTR_FS_WriteLong   = 34;
static constexpr uint16_t INTR_FS_ReadLong    = 35;
static constexpr uint16_t INTR_FS_WriteDouble = 36;
static constexpr uint16_t INTR_FS_ReadDouble  = 37;

//Object/String intrinsic methods (Phase 8e-1).
static constexpr uint16_t INTR_Object_Equals        = 40;
static constexpr uint16_t INTR_Object_GetHashCode   = 41;
static constexpr uint16_t INTR_String_Equals        = 42;
static constexpr uint16_t INTR_String_GetHashCode   = 43;
//Phase 8e-9b: Object.toString() default intrinsic. ID is 61 because List/Dict
//intrinsics (44-60) were allocated before this phase. Registration order in
//VmBackend remains "紧跟 GetHashCode" (semantic adjacency), only the numeric
//ID is non-contiguous with the 8e-1 Object block.
static constexpr uint16_t INTR_Object_toString      = 61;

//List<T> intrinsic methods (Phase 8e-3, erasure-style — all elements stored as heap idxs).
static constexpr uint16_t INTR_List_Ctor        = 44;
static constexpr uint16_t INTR_List_Add         = 45;
static constexpr uint16_t INTR_List_Get         = 46;
static constexpr uint16_t INTR_List_Set         = 47;
static constexpr uint16_t INTR_List_Length      = 48;
static constexpr uint16_t INTR_List_RemoveAt    = 49;
static constexpr uint16_t INTR_List_IndexOf     = 50;
static constexpr uint16_t INTR_List_Contains    = 51;
static constexpr uint16_t INTR_List_Clear       = 52;

//Dict<K,V> intrinsic methods (Phase 8e-4, erasure-style — keys and values
//stored as heap idxs in a side table, linear-scan lookup).
static constexpr uint16_t INTR_Dict_Ctor        = 53;
static constexpr uint16_t INTR_Dict_Set         = 54;
static constexpr uint16_t INTR_Dict_Get         = 55;
static constexpr uint16_t INTR_Dict_ContainsKey = 56;
static constexpr uint16_t INTR_Dict_Remove      = 57;
static constexpr uint16_t INTR_Dict_Clear       = 58;
static constexpr uint16_t INTR_Dict_Count       = 59;
//Phase 8e-5: Dict.Keys() — returns a new List<K> populated from dict entries' keys.
static constexpr uint16_t INTR_Dict_Keys        = 60;
//Phase 9b-pre: collection toString (Python-style "[a, b, c]" / "{k: v}").
static constexpr uint16_t INTR_List_toString    = 62;
static constexpr uint16_t INTR_Dict_toString    = 63;

//Phase 9d: Exception class ctor intrinsics. All 6 Exception-family ctors
//share the same intrinsic dispatch (single ExecuteIntrinsic case handles
//all 6 IDs); the distinct IDs exist so that user `new MyException("msg")`
//resolves to the correct ctor stub for type-checking. Subclass ctors set
//message + allocate an empty List<string> for backtrace, identical to base.
static constexpr uint16_t INTR_Exception_Ctor                = 64;
static constexpr uint16_t INTR_NullPointerException_Ctor     = 65;
static constexpr uint16_t INTR_DivByZeroException_Ctor       = 66;
static constexpr uint16_t INTR_IndexOutOfBoundsException_Ctor = 67;
static constexpr uint16_t INTR_AssertionException_Ctor       = 68;
static constexpr uint16_t INTR_IOException_Ctor              = 69;
//v0.8.5: sixth Exception ctor; 128 = next fresh id (policy below).
static constexpr uint16_t INTR_InvalidCastException_Ctor     = 128;

//Phase 11 stdlib namespace ids: the math (70-94), io (113-119) and fs
//(120-127) blocks retired with the built-in standard library (io's range
//follows its last allocation on the master line, 0.7.7). The freed ids
//stay unallocated — intrinsic ids are only ever appended, never reused.
//String methods (95-112 — 18 since the 0.7.5 char bridge) survive as
//receiver-dispatched built-ins.

//Phase 11 Step 3: built-in string methods (receiver-dispatched via
//OP_CallIntrinsic — receiver string idx at callParamBase[0], args from
//slot 1). Equals/GetHashCode keep their 8e-1 ids 42/43; only the
//implementation moves to IntrinsicsString.cpp.
static constexpr uint16_t INTR_String_Substring  = 95;
static constexpr uint16_t INTR_String_IndexOf    = 96;
static constexpr uint16_t INTR_String_StartsWith = 97;
static constexpr uint16_t INTR_String_EndsWith   = 98;
static constexpr uint16_t INTR_String_Contains   = 99;
static constexpr uint16_t INTR_String_ToUpper    = 100;
static constexpr uint16_t INTR_String_ToLower    = 101;
static constexpr uint16_t INTR_String_Trim       = 102;
static constexpr uint16_t INTR_String_Split      = 103;
static constexpr uint16_t INTR_String_Replace    = 104;
static constexpr uint16_t INTR_String_ToInt      = 105;
static constexpr uint16_t INTR_String_ToFloat    = 106;
//0.7.5 char bridge (Task 8): the six code-point / strict-parse methods.
static constexpr uint16_t INTR_String_CharAt     = 107;
static constexpr uint16_t INTR_String_CharCount  = 108;
static constexpr uint16_t INTR_String_ToChar     = 109;
static constexpr uint16_t INTR_String_ToLong     = 110;
static constexpr uint16_t INTR_String_ToDouble   = 111;
static constexpr uint16_t INTR_String_ToBool     = 112;
static constexpr uint16_t kStringMethodIntrinsicFirst = 95;
static constexpr uint16_t kStringMethodIntrinsicCount = 18;
static_assert(INTR_String_Substring == kStringMethodIntrinsicFirst,
    "string-method intrinsic block must start at its First constant");
static_assert(INTR_String_ToBool
        == kStringMethodIntrinsicFirst + kStringMethodIntrinsicCount - 1,
    "string-method intrinsic block must be contiguous up to ToBool");

//K1 (v0.10.0) concurrency intrinsics. thread namespace free functions
//(129-133): module-level intrinsic stubs dispatched through the
//OP_CallFunc intrinsic arm — no `this` slot, args start at
//callParamBase. Mutex/Channel builtin class methods (134-141): the
//usual receiver-dispatched shape (slot 0 = this). 128 was the last
//fresh id before this block; ids are only ever appended, never reused.
static constexpr uint16_t INTR_Thread_Spawn = 129;
static constexpr uint16_t INTR_Thread_Join  = 130;
static constexpr uint16_t INTR_Thread_Yield = 131;
static constexpr uint16_t INTR_Thread_Id    = 132;
static constexpr uint16_t INTR_Thread_Sleep = 133;
static constexpr uint16_t INTR_Mutex_Ctor    = 134;
static constexpr uint16_t INTR_Mutex_Lock    = 135;
static constexpr uint16_t INTR_Mutex_Unlock  = 136;
static constexpr uint16_t INTR_Mutex_TryLock = 137;
static constexpr uint16_t INTR_Channel_Ctor  = 138;
static constexpr uint16_t INTR_Channel_Send  = 139;
static constexpr uint16_t INTR_Channel_Recv  = 140;
static constexpr uint16_t INTR_Channel_Close = 141;
//K3 task 4: the Channel ctor's capacity contract — the invoke form takes
//exactly one int (builtin invokes stage exact arity); the init-list `{}`
//arm stages 0 = unbounded; >= 1 = bounded; < 0 raises the catchable
//capacity error. Shared by the emitter and the intrinsic.
static constexpr int32_t kChannelUnboundedCapacity = 0;
//K3 Future methods (142-143): receiver-dispatched like the Mutex/
//Channel family (slot 0 = this, the Future instance).
static constexpr uint16_t INTR_Future_Get     = 142;
static constexpr uint16_t INTR_Future_IsDone  = 143;
//K3 (v0.10.3): ChannelClosedException ctor — the (this, message) shape
//shared by the Exception family (dispatch collapses to the shared body).
static constexpr uint16_t INTR_ChannelClosedException_Ctor = 144;
//P2-1 (v0.10.17): native Semaphore — the receiver-dispatched family
//shape (slot 0 = this, the Semaphore instance; the ctor's slot 1 =
//the initial permit count).
static constexpr uint16_t INTR_Semaphore_Ctor       = 145;
static constexpr uint16_t INTR_Semaphore_Acquire    = 146;
static constexpr uint16_t INTR_Semaphore_Release    = 147;
static constexpr uint16_t INTR_Semaphore_TryAcquire = 148;
//0.10.21: native ReentrantMutex — the Mutex receiver-dispatched family
//shape (slot 0 = this); a per-owner hold count replaces the
//non-reentrant self-relock park.
static constexpr uint16_t INTR_ReentrantMutex_Ctor    = 149;
static constexpr uint16_t INTR_ReentrantMutex_Lock    = 150;
static constexpr uint16_t INTR_ReentrantMutex_Unlock  = 151;
static constexpr uint16_t INTR_ReentrantMutex_TryLock = 152;
//0.10.22: native AtomicInt — the receiver-dispatched family shape
//(slot 0 = this; int args start at callParamBase + kFrameSlotBytes).
//Single ctor: `int initial = 0` rides the default-parameter staging,
//so one id serves both arities.
static constexpr uint16_t INTR_AtomicInt_Ctor            = 153;
static constexpr uint16_t INTR_AtomicInt_Load            = 154;
static constexpr uint16_t INTR_AtomicInt_Store           = 155;
static constexpr uint16_t INTR_AtomicInt_GetAndSet       = 156;
static constexpr uint16_t INTR_AtomicInt_CompareExchange = 157;
static constexpr uint16_t INTR_AtomicInt_GetAndAdd       = 158;
static constexpr uint16_t INTR_AtomicInt_AddAndGet       = 159;
static constexpr uint16_t INTR_AtomicInt_IncrementAndGet = 160;
static constexpr uint16_t INTR_AtomicInt_GetAndIncrement = 161;
//Promise family: receiver-dispatched like the Future family (slot 0 =
//this, the Promise instance). Ctor and future() land with the
//born-parked completion source; complete() and completeExceptionally()
//join them with the completion intrinsics. (Ids follow the
//ReentrantMutex/AtomicInt blocks that landed on master first.)
static constexpr uint16_t INTR_Promise_Ctor                  = 162;
static constexpr uint16_t INTR_Promise_Future                = 163;
static constexpr uint16_t INTR_Promise_Complete              = 164;
static constexpr uint16_t INTR_Promise_CompleteExceptionally = 165;
//Future.selectAny: the first STATIC face on a builtin generic
//type name — no receiver rides slot 0; the wait-set array is the sole
//argument (args-from-slot-0 ABI, the binding row's noReceiver shape).
static constexpr uint16_t INTR_Future_SelectAny = 166;
//P2-2 (v0.11.9): spawn-failure observability — thread-namespace free
//functions (slot 0 = the Thread argument, same shape as join).
static constexpr uint16_t INTR_Thread_IsFailed = 167;
static constexpr uint16_t INTR_Thread_Failure  = 168;
//Phase-2 declared Channel: the zero-arg ctor shape. The dispatch
//signature carries no arity, so the two overloads map to two ids —
//this one never reads the unstaged capacity slot (scratch garbage).
static constexpr uint16_t INTR_Channel_CtorUnbounded = 169;

}  // namespace nlang
