#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/EntityPrototype.h"
#include "Strata/Scene/SceneLoader.h"
#include "Strata/Scene/SubScene.h"

#include <vector>

namespace Strata
{

	// The state of one additive load (Scene's sub-scenes, Scene/SceneSubScenes.cpp): what loads, compiles or is created, and
	// the top-level entities it created. Internal to Scene.
	struct SubSceneLoad
	{
		SubSceneStatus Status;
		// The scene asset and its closure, pinned until every entity exists (null for a prepared scene added directly).
		Scope<SceneLoadRequest> Request;
		// The prepared scene's entities compile on the job system into Compiled->Prototype; the job owns the prepared scene
		// and releases it there, so the main thread never pays for destroying it.
		struct CompileResult
		{
			Ref<EntityPrototype> Prototype;
		};
		Ref<CompileResult> Compiled;
		JobHandle CompileJob;
		// The slices' progress through the prototype (released once every entity exists).
		Scope<PrototypeInstantiation> Instantiation;
		// The top-level entities created so far (each with a SubSceneComponent): what unloading destroys.
		std::vector<Entity> Roots;
	};

}
