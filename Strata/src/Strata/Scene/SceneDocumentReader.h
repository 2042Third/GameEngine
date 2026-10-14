#pragma once

#include "Strata/Core/JobSystem.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneDocumentWriter.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace Strata
{

	class EntitySink;

	struct SceneReadOptions
	{
		// Asked before the read and every CancelCheckEntities entities: true ends the read, which fails as cancelled
		// (SceneReadResult::Cancelled). Loaders pass AssetLoadData::IsCancelled.
		std::function<bool()> IsCancelled;
		uint32_t CancelCheckEntities = 256;
		// The most entities the document may hold: one that declares more (EntityCount, before its entities) is refused before
		// its first entity, one that does not at its first entity beyond the limit. Scene loads: Scene::c_MaxEntities less the
		// entities the scene has.
		uint64_t MaxEntities = Scene::c_MaxEntities;
		// The share of the text parsed so far, in [0, 1], on the reading thread: about every megabyte, and 1 at the end of the
		// parse (the sink's Finish follows).
		std::function<void(float share)> Progress;
		// Documents in the writer's layout (an entity per line) of at least ParallelMinEntities entities are parsed on the job
		// system's threads, ParallelChunkEntities lines per job, when the sink takes prepared entities
		// (EntitySink::TakesPreparedEntities): the reading thread hands the chunks to the sink in order, asking IsCancelled
		// after each. Every line is checked to parse alone first; a document whose lines do not is read as a stream, as are
		// documents in other layouts. The result does not depend on the path.
		bool Parallel = true;
		uint32_t ParallelChunkEntities = 2048;
		uint32_t ParallelMinEntities = 16384;
		// Of the jobs that parse chunks (from background work they are background jobs).
		JobPriority Priority = JobPriority::Normal;
	};

	struct SceneReadResult
	{
		uint64_t Entities = 0;   // Handed to the sink
		bool Cancelled = false;  // SceneReadOptions::IsCancelled ended the read
		bool EntityError = false; // The sink refused the entity data (the error is the sink's)
		bool SyntaxError = false; // The text is not valid JSON
		bool Parallel = false;    // The entities were parsed in parallel (SceneReadOptions::Parallel)
	};

	// Reads scene, prefab and model documents (SceneDocumentKind) as a stream of parse events, handing the content to an
	// EntitySink (SceneBuilder, EntityPrototypeBuilder, ...) one entity at a time: what a read holds at once is the text, one
	// entity's DOM and what the sink keeps, never a DOM of the whole document. Accepts any key order and whitespace: the
	// streamed layout SceneDocumentWriter writes, and older files (tab-indented or compact, with their keys sorted, so the
	// header comes after the entities): the header is validated when it arrives, and a document without a valid one fails,
	// also after its entities were handed over. Keys a reader does not know are skipped.
	//
	// Any thread; the sink decides what may run alongside (SceneBuilder: nothing else touches its scene).
	class SceneDocumentReader
	{
	public:
		// Reads the text, which must be a document of `kind` (its header names the kind's format, at a version the engine
		// reads). False, with the reason, when it is no such document, holds more entities than the limit, the sink refuses it
		// (outResult->EntityError) or the read is cancelled (outResult->Cancelled).
		static bool Read(std::string_view text, SceneDocumentKind kind, EntitySink& sink, const SceneReadOptions& options = {},
			SceneReadResult* outResult = nullptr, std::string* outError = nullptr);
	};

}
