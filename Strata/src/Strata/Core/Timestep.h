#pragma once

namespace Strata
{

	class Timestep
	{
	public:
		constexpr Timestep(float time = 0.0f)
			: m_Time(time)
		{
		}

		constexpr operator float() const { return m_Time; }

		constexpr float GetSeconds() const { return m_Time; }
		constexpr float GetMilliseconds() const { return m_Time * 1000.0f; }
	private:
		float m_Time;
	};

}
