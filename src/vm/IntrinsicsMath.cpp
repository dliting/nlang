/*---
IntrinsicsMath.cpp — math namespace intrinsics.

ABI per StdLib.h: arguments are read from callParamBase slot 0 upward —
no this pointer.

Error model: argument/range errors (clampi lo>hi, randomi min>max) and
int32-overflow guards (floor/ceil/round out of range or NaN, absi of
INT_MIN) raise the BASE Exception class — NLang has no dedicated
argument-exception subclass, and base throws stay catch-compatible if
one is added later. Domain errors of the transcendental family (sqrt of
negatives, log of non-positives, asin outside [-1,1]) deliberately
propagate NaN per C semantics.
---*/
#include "VmExecutor.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace nlang
{

//Q9 PRNG constants: (rng() >> 8) is a 24-bit value; scaling by 2^-24 maps
//it exactly onto [0,1) with no rounding (24-bit mantissa).
constexpr unsigned kRngFloatShift = 8;
constexpr float kRngFloatScale = 1.0f / 16777216.0f;

//Phase 11 error model: argument/range errors raise the BASE Exception.
//Defined here (first stdlib family TU) but declared in the header so the
//string/io/fs TUs reuse the same seam.
[[noreturn]] void VmExecutor::RaiseNlangExceptionBase(const std::string& msg)
{
	RaiseNlangException(m_exceptionClassIdx, msg);
}

bool VmExecutor::ExecuteIntrinsicMath(uint16_t intrinsicId,
	uint16_t callParamBase, uint8_t* locals, uint8_t* pResult)
{
	switch (intrinsicId)
	{
	case INTR_Math_Sqrt:
	{
		float x;
		std::memcpy(&x, locals + callParamBase, sizeof(x));
		float r = std::sqrt(x);
		std::memcpy(pResult, &r, sizeof(r));
		return true;
	}
	case INTR_Math_Sin:
	case INTR_Math_Cos:
	case INTR_Math_Tan:
	case INTR_Math_Asin:
	case INTR_Math_Acos:
	case INTR_Math_Atan:
	case INTR_Math_Exp:
	case INTR_Math_Log:
	{
		float x;
		std::memcpy(&x, locals + callParamBase, sizeof(x));
		float r;
		switch (intrinsicId)
		{
		case INTR_Math_Sin:  r = std::sin(x);  break;
		case INTR_Math_Cos:  r = std::cos(x);  break;
		case INTR_Math_Tan:  r = std::tan(x);  break;
		case INTR_Math_Asin: r = std::asin(x); break;
		case INTR_Math_Acos: r = std::acos(x); break;
		case INTR_Math_Atan: r = std::atan(x); break;
		case INTR_Math_Exp:  r = std::exp(x);  break;
		default:             r = std::log(x);  break; //INTR_Math_Log (ln)
		}
		std::memcpy(pResult, &r, sizeof(r));
		return true;
	}
	case INTR_Math_Atan2:
	{
		float y, x;
		std::memcpy(&y, locals + callParamBase, sizeof(y));
		std::memcpy(&x, locals + callParamBase + 4, sizeof(x));
		float r = std::atan2(y, x);
		std::memcpy(pResult, &r, sizeof(r));
		return true;
	}
	case INTR_Math_Pow:
	{
		float x, e;
		std::memcpy(&x, locals + callParamBase, sizeof(x));
		std::memcpy(&e, locals + callParamBase + 4, sizeof(e));
		float r = std::pow(x, e);
		std::memcpy(pResult, &r, sizeof(r));
		return true;
	}
	case INTR_Math_Absi:
	{
		int32_t v;
		std::memcpy(&v, locals + callParamBase, sizeof(v));
		if (v == INT32_MIN)
			RaiseNlangExceptionBase("math.absi: abs of -2147483648 "
				"is outside the int32 range.");
		int32_t r = (v < 0) ? -v : v;
		std::memcpy(pResult, &r, sizeof(r));
		return true;
	}
	case INTR_Math_Absf:
	{
		float x;
		std::memcpy(&x, locals + callParamBase, sizeof(x));
		float r = std::fabs(x);
		std::memcpy(pResult, &r, sizeof(r));
		return true;
	}
	case INTR_Math_Mini:
	case INTR_Math_Maxi:
	{
		int32_t a, b;
		std::memcpy(&a, locals + callParamBase, sizeof(a));
		std::memcpy(&b, locals + callParamBase + 4, sizeof(b));
		int32_t r = (intrinsicId == INTR_Math_Mini)
			? ((a < b) ? a : b) : ((a > b) ? a : b);
		std::memcpy(pResult, &r, sizeof(r));
		return true;
	}
	case INTR_Math_Minf:
	case INTR_Math_Maxf:
	{
		float a, b;
		std::memcpy(&a, locals + callParamBase, sizeof(a));
		std::memcpy(&b, locals + callParamBase + 4, sizeof(b));
		float r = (intrinsicId == INTR_Math_Minf)
			? ((a < b) ? a : b) : ((a > b) ? a : b);
		std::memcpy(pResult, &r, sizeof(r));
		return true;
	}
	case INTR_Math_Clampi:
	{
		int32_t v, lo, hi;
		std::memcpy(&v, locals + callParamBase, sizeof(v));
		std::memcpy(&lo, locals + callParamBase + 4, sizeof(lo));
		std::memcpy(&hi, locals + callParamBase + 8, sizeof(hi));
		if (lo > hi)
			RaiseNlangExceptionBase("math.clampi: low is greater than high.");
		int32_t r = (v < lo) ? lo : ((v > hi) ? hi : v);
		std::memcpy(pResult, &r, sizeof(r));
		return true;
	}
	case INTR_Math_Clampf:
	{
		float v, lo, hi;
		std::memcpy(&v, locals + callParamBase, sizeof(v));
		std::memcpy(&lo, locals + callParamBase + 4, sizeof(lo));
		std::memcpy(&hi, locals + callParamBase + 8, sizeof(hi));
		if (lo > hi)
			RaiseNlangExceptionBase("math.clampf: low is greater than high.");
		float r = (v < lo) ? lo : ((v > hi) ? hi : v);
		std::memcpy(pResult, &r, sizeof(r));
		return true;
	}
	case INTR_Math_Floor:
	case INTR_Math_Ceil:
	case INTR_Math_Round:
	{
		float x;
		std::memcpy(&x, locals + callParamBase, sizeof(x));
		//double math: the float arg widens exactly, and the range guard
		//must see the double-rounded value (std::round of 2.1e9f etc.).
		//C++ float->int conversion outside int32 is UB (x86 silently
		//yields 0x80000000), so reject loudly before casting. NaN fails
		//the same comparison and lands in the same error.
		double d;
		const char* funcName;
		switch (intrinsicId)
		{
		case INTR_Math_Floor: d = std::floor(x); funcName = "floor"; break;
		case INTR_Math_Ceil:  d = std::ceil(x);  funcName = "ceil";  break;
		default:              d = std::round(x); funcName = "round"; break; //half away from zero
		}
		if (!(d >= -2147483648.0 && d <= 2147483647.0))
		{
			char buf[96];
			std::snprintf(buf, sizeof(buf),
				"math.%s: value %g is outside the int32 range.",
				funcName, x);
			RaiseNlangExceptionBase(buf);
		}
		int32_t r = static_cast<int32_t>(d);
		std::memcpy(pResult, &r, sizeof(r));
		return true;
	}
	case INTR_Math_Random:
	{
		float r = static_cast<float>(m_rng() >> kRngFloatShift)
			* kRngFloatScale;
		std::memcpy(pResult, &r, sizeof(r));
		return true;
	}
	case INTR_Math_Srand:
	{
		int32_t seed;
		std::memcpy(&seed, locals + callParamBase, sizeof(seed));
		m_rng.seed(static_cast<uint32_t>(seed));
		//Void return: leave pResult untouched.
		return true;
	}
	case INTR_Math_Randomi:
	{
		int32_t lo, hi;
		std::memcpy(&lo, locals + callParamBase, sizeof(lo));
		std::memcpy(&hi, locals + callParamBase + 4, sizeof(hi));
		if (lo > hi)
			RaiseNlangExceptionBase("math.randomi: min is greater than max.");
		//Modulo bias exists for ranges not dividing 2^32; documented as
		//acceptable for a scripting PRNG (deterministic, no adapter).
		uint32_t span = static_cast<uint32_t>(hi - lo) + 1u;
		int32_t r = lo + static_cast<int32_t>(m_rng() % span);
		std::memcpy(pResult, &r, sizeof(r));
		return true;
	}
	default:
		return false;
	}
}

} //namespace nlang
