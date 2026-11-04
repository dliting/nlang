/*---
    ModuleSaverRecords.h — .ncu 复合记录表序列化器（写入侧内部实现）。
    ModuleSaver.cpp 拆分（源尺寸守卫触发的可维护性重构，零行为变化）：
    WriteCompiledModule 的骨架与标量段留在 ModuleSaver.cpp，复合记录表的
    逐字段序列化按一表一 TU 分文件——interfaces 在
    ModuleSaverRecordsInterfaces.cpp（0.9.12 v2.7 拆分，与读取侧
    ModuleLoaderRecords.h 同构），classes 在 ModuleSaverRecordsClasses.cpp
    （2026-11-02 v2.13 静态块落地时拆分）。仅 vm 模块内部使用，不安装到
    include/。
---*/
#pragma once
#include "nlang/vm/CompiledModule.h"
#include <ostream>

namespace nlang {

//0.9.10 v2.4 + 0.9.12 v2.7: serialize the interface statics section
//(u32 count + per-interface records). Defined in
//ModuleSaverRecordsInterfaces.cpp; the wire layout is documented there.
void WriteInterfaceRecords(std::ostream& fs, const CompiledModule& mod);

//v1.6 (names) + v2.0 (keys) + 0.10.14 v2.9 (member-method tables):
//serialize the enum record table. Defined in
//ModuleSaverRecordsEnums.cpp; the wire layout is documented there.
void WriteEnumRecords(std::ostream& fs, const CompiledModule& mod);

//Class record table (own fields, members, constructors, v2.13 static
//block). Defined in ModuleSaverRecordsClasses.cpp (2026-11-02 split from
//ModuleSaver.cpp when the static block landed); wire layout documented
//there.
void WriteClassRecords(std::ostream& fs, const CompiledModule& mod);

//Stream-level adapters shared by the saver TUs (defined in
//ModuleSaver.cpp): one length-prefixed type descriptor and one fixed
//default-value record.
void WriteLenPrefixedTypeDesc(std::ostream& fs, const TypeDesc& td);
void WriteDefaultValueDesc(std::ostream& fs, const DefaultValueDesc& dv);

} // namespace nlang
