#pragma once

// Shared helpers of the scripting API test module. The engine-side tests (StrataTests/src/Scripting) read the results
// through script fields and the scene.

#include "StrataScript/StrataScript.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace ScriptTests
{

	// Appends "<entity>.<script>.<event>;" to the Text of the entity named "Log", the tests' event recorder.
	inline void Record(const Strata::Script& script, std::string_view className, std::string_view event)
	{
		Strata::Entity log = Strata::Scene::FindEntityByName("Log");
		if (!log)
			return;

		std::string text = log.GetProperty<std::string>("Text", "Text").value_or(std::string());
		text += script.GetEntity().GetName();
		text += '.';
		text += className;
		text += '.';
		text += event;
		text += ';';
		log.SetProperty("Text", "Text", text);
	}

	inline bool Near(float a, float b, float epsilon = 1e-4f)
	{
		return std::abs(a - b) <= epsilon;
	}

	template<glm::length_t Length>
	bool Near(const glm::vec<Length, float, glm::defaultp>& a, const glm::vec<Length, float, glm::defaultp>& b, float epsilon = 1e-4f)
	{
		for (glm::length_t index = 0; index < Length; index++)
		{
			if (!Near(a[index], b[index], epsilon))
				return false;
		}
		return true;
	}

	// Rotations are equal when they are the same up to sign.
	inline bool Near(const glm::quat& a, const glm::quat& b, float epsilon = 1e-4f)
	{
		return std::abs(std::abs(glm::dot(a, b)) - 1.0f) <= epsilon;
	}

	// Base of scripts that verify API behavior: Checks counts the checks, Failure names the first one that failed.
	class CheckingScript : public Strata::Script
	{
	public:
		int32_t Checks = 0;
		std::string Failure;
	protected:
		void Expect(bool condition, const char* description)
		{
			Checks++;
			if (!condition && Failure.empty())
				Failure = description;
		}
	};

}

// Records every callback in the scene's "Log" entity and counts them in fields.
class Lifecycle : public Strata::Script
{
public:
	int32_t Creates = 0;
	int32_t Updates = 0;
	int32_t FixedUpdates = 0;
	int32_t LateUpdates = 0;
	bool RecordUpdates = true;

	void OnCreate() override
	{
		Creates++;
		ScriptTests::Record(*this, GetClassName(), "Create");
	}

	void OnUpdate(float) override
	{
		Updates++;
		if (RecordUpdates)
			ScriptTests::Record(*this, GetClassName(), "Update");
	}

	void OnFixedUpdate(float) override
	{
		FixedUpdates++;
		if (RecordUpdates)
			ScriptTests::Record(*this, GetClassName(), "Fixed");
	}

	void OnLateUpdate(float) override
	{
		LateUpdates++;
		if (RecordUpdates)
			ScriptTests::Record(*this, GetClassName(), "Late");
	}

	void OnDestroy() override
	{
		ScriptTests::Record(*this, GetClassName(), "Destroy");
	}
protected:
	virtual const char* GetClassName() const { return "Lifecycle"; }
};
