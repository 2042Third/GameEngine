#pragma once

#include <cstdint>

namespace Strata
{

	// Paces a check whose cost grows with the size of what it checks (a whole-structure validation in Debug builds), so that
	// calling it every frame costs a bounded amount per call whatever that size: each call earns a budget of
	// `elementsPerCall` elements, and the check is due once the budget earned since the last one covers the elements there
	// are now. Structures of up to `elementsPerCall` elements are checked at every call, larger ones every few calls (one of
	// 10,000 elements with a budget of 500, every 20th), and the first call always checks. Deterministic: it counts calls,
	// never time. Not thread-safe.
	class AmortizedCheck
	{
	public:
		explicit AmortizedCheck(uint64_t elementsPerCall)
			: m_ElementsPerCall(elementsPerCall > 0 ? elementsPerCall : 1)
		{
		}

		// Whether to check `elements` elements at this call; true starts the next budget.
		bool IsDue(uint64_t elements)
		{
			if (!m_Started)
			{
				m_Started = true;
				m_Budget = 0;
				return true;
			}
			m_Budget += m_ElementsPerCall;
			if (m_Budget < elements)
				return false;
			m_Budget = 0;
			return true;
		}

		uint64_t GetElementsPerCall() const { return m_ElementsPerCall; }
	private:
		uint64_t m_ElementsPerCall;
		uint64_t m_Budget = 0;
		bool m_Started = false;
	};

}
