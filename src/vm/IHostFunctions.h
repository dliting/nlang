/*---
    IHostFunctions.h — 扩展点⑤（spec §8）：宿主函数直派钩。
    仿 IHostIo 的可空接缝；CallNative 未命中内建注册表时先问此钩
    ——宿主注册优先于同名 DLL native，且宿主注册名不触发 DLL 载入
    尝试。回调禁止让 C++ 异常穿透 VM 执行栈（实现内须翻译为 NLang
    异常，见 embed/HostFunctionTable）。
---*/
#pragma once
#include <cstdint>

namespace nlang {

struct CompiledFunction;   //内部类型前向声明（VmExecutor.h 定义）

class IHostFunctions {
public:
    virtual ~IHostFunctions() = default;
    //callee：被调声明（编组的声明源——形参/返回 RTK）。argCells：
    //paramCount 个 8 字节单元。resultCell：8 字节出参。
    //返回 false＝不受理，落回既有 m_natives/惰性 DLL 路径。
    //string 结果的受理契约（裁定 D27，与 DLL native 同一道派发后检
    //查）：写 resultCell 的句柄必须是活动字符串——句柄 0＝"没有产出
    //字符串"，属契约违例，抛 host 级错误"returned null string"；
    //string 形参单元则一律是活动字符串，句柄 0＝空串（裁定 D26）。
    //embed 宿主经 MintHostString 铸柄，与 newString 同规永不铸 0。
    virtual bool Call(const CompiledFunction& callee,
                      const uint8_t* argCells, uint32_t argc,
                      uint8_t* resultCell) = 0;
};

}  // namespace nlang
