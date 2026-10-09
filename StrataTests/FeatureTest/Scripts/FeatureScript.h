#pragma once

// Shared helpers of the feature test scripts (the script module of StrataTests/FeatureTest).
//
// Result channel: every script that verifies something derives from FeatureScript. Expect() counts a check and keeps
// the first failed one in Failure; Completed is set once the script's scenario ran to its end. The engine-side runner
// (StrataTests/src/FeatureTest) requires Completed and an empty Failure from every live instance after play, and names
// the class, entity and failed check otherwise.
//
// Journal: OnCreate (and selected other callbacks) append "<Class>.<Event>@<Entity>;" to the Text of the scene's
// "Journal" entity, so the runner can check events of instances that no longer exist (destroyed entities, removed
// scripts, OnDestroy when play stops). Every class journals its OnCreate: the runner requires each class of the module
// to have run.
//
// State that spans frames lives in fields: the runner hot reloads the module halfway through play, which recreates every
// instance and only keeps field values.

#include "StrataScript/StrataScript.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace FeatureTest
{

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

	// Appends "<className>.<event>@<entity name>;" to the journal (see above).
	inline void Journal(const Strata::Script& script, std::string_view className, std::string_view event)
	{
		Strata::Entity journal = Strata::Scene::FindEntityByName("Journal");
		if (!journal)
			return;

		std::string text = journal.GetProperty<std::string>("Text", "Text").value_or(std::string());
		text += className;
		text += '.';
		text += event;
		text += '@';
		text += script.GetEntity().GetName();
		text += ';';
		journal.SetProperty("Text", "Text", text);
	}

	// The current frame: 0 during the first update after play started (and during the OnCreate calls of the start).
	inline int32_t GetFrame()
	{
		return static_cast<int32_t>(Strata::Time::GetFrameIndex());
	}

	class FeatureScript : public Strata::Script
	{
	public:
		int32_t Checks = 0;
		std::string Failure;    // The first failed check
		bool Completed = false; // The scenario ran to its end
	protected:
		void Expect(bool condition, std::string_view description)
		{
			Checks++;
			if (!condition && Failure.empty())
				Failure = std::string(description);
		}
	};

}
