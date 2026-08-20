/*---
IntrinsicsMath.cpp — math namespace intrinsics.

ABI per StdLib.h: arguments are read from callParamBase slot 0 upward —
no this pointer.
---*/
#include "VmExecutor.h"
#include <cmath>
#include <cstring>

namespace nlang
{

bool VmExecutor::ExecuteIntrinsicMath(uint16_t intrinsicId,
	uint16_t callParamBase, uint8_t* locals, uint8_t* pResult)
{
	if (intrinsicId == INTR_Math_Sqrt)
	{
		float x;
		std::memcpy(&x, locals + callParamBase, sizeof(x));
		float r = std::sqrt(x);
		std::memcpy(pResult, &r, sizeof(r));
		return true;
	}
	return false;
}

} //namespace nlang
