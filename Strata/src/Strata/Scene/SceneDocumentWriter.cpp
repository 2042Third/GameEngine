#include "stpch.h"
#include "Strata/Scene/SceneDocumentWriter.h"

#include "Strata/Core/Hash.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Reflection/PropertyJson.h"
#include "Strata/Scene/ComponentAccess.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/EntityPrototype.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSerializer.h"
#include "Strata/Scene/UnknownComponents.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <unordered_set>

namespace Strata
{

	namespace
	{

		thread_local uint64_t t_SerializedEntities = 0;

		// Chunks serialized per batch, per thread of the pool: a batch is serialized while the one before is written.
		constexpr size_t c_ChunksPerThread = 2;
		constexpr size_t c_MinBatchChunks = 4;
		constexpr size_t c_MaxBatchChunks = 64;

		// A property as the writer writes it: its key ("\"Name\":") and how to read it.
		struct PropertyPlan
		{
			const PropertyInfo* Property = nullptr;
			std::string Key;
		};

		// A registered component as the writer writes it (the components that are written at all: not NoSerialize).
		struct ComponentPlan
		{
			const ComponentInfo* Info = nullptr;
			std::string Key; // "\"Transform\":"
			// Written through its JSON (ComponentAccess::Serialize): data beyond its properties (SerializeExtra).
			bool Extra = false;
			std::vector<PropertyPlan> Properties;              // Serialized ones, sorted by name as nlohmann sorts keys
			std::vector<const PropertyInfo*> AssetProperties;   // Serialized asset properties in registration order (references)
		};

		std::string MakeKey(std::string_view name)
		{
			std::string key;
			JsonUtils::AppendString(key, name);
			key += ':';
			return key;
		}

		// Sorted by name as nlohmann sorts an object's keys (std::less<std::string>). Components the options leave out are not
		// written.
		std::vector<ComponentPlan> MakeComponentPlans(const SceneWriteOptions& options)
		{
			std::vector<ComponentPlan> plans;
			for (const ComponentInfo* info : ComponentRegistry::GetAll())
			{
				if (HasFlag(info->Flags, ComponentFlags::NoSerialize) || std::find(options.LeaveOut.begin(), options.LeaveOut.end(), info) != options.LeaveOut.end())
					continue;
				ComponentPlan& plan = plans.emplace_back();
				plan.Info = info;
				plan.Key = MakeKey(info->Name);
				plan.Extra = static_cast<bool>(info->SerializeExtra);
				for (const PropertyInfo& property : info->Properties)
				{
					if (property.IsTransient())
						continue;
					plan.Properties.push_back(PropertyPlan { &property, MakeKey(property.Name) });
					if (property.Type == PropertyType::Asset)
						plan.AssetProperties.push_back(&property);
				}
				std::sort(plan.Properties.begin(), plan.Properties.end(), [](const PropertyPlan& a, const PropertyPlan& b)
				{
					return a.Property->Name < b.Property->Name;
				});
			}
			std::sort(plans.begin(), plans.end(), [](const ComponentPlan& a, const ComponentPlan& b) { return a.Info->Name < b.Info->Name; });
			return plans;
		}

		// An entity of the document and the parent it is written with (null: top-level in the document).
		struct EntityToWrite
		{
			entt::entity Handle = entt::null;
			UUID Parent = UUID::Null();
		};

		std::vector<EntityToWrite> CollectEntities(const Scene& scene, const std::vector<UUID>& roots)
		{
			std::vector<EntityToWrite> entities;
			const entt::registry& registry = scene.GetRegistry();
			if (roots.empty())
			{
				const std::vector<Entity> ordered = scene.GetEntitiesInHierarchyOrder();
				entities.reserve(ordered.size());
				for (const Entity entity : ordered)
					entities.push_back(EntityToWrite { entity.GetHandle(), registry.get<RelationshipComponent>(entity.GetHandle()).Parent });
				return entities;
			}

			// The listed roots, each with its subtree, parents first; roots below another listed root come with it.
			std::unordered_set<UUID> listed;
			for (const UUID id : roots)
			{
				if (scene.GetEntityByUUID(id))
					listed.insert(id);
			}
			std::unordered_set<UUID> written;
			std::vector<Entity> stack;
			for (const UUID id : roots)
			{
				const Entity root = scene.GetEntityByUUID(id);
				if (!root || written.contains(id))
					continue;
				bool nested = false;
				for (Entity ancestor = root.GetParent(); ancestor && !nested; ancestor = ancestor.GetParent())
					nested = listed.contains(ancestor.GetUUID());
				if (nested)
					continue;
				stack.assign(1, root);
				while (!stack.empty())
				{
					const Entity entity = stack.back();
					stack.pop_back();
					if (!written.insert(entity.GetUUID()).second)
						continue;
					entities.push_back(EntityToWrite { entity.GetHandle(), registry.get<RelationshipComponent>(entity.GetHandle()).Parent });
					const std::vector<Entity> children = entity.GetChildren();
					stack.insert(stack.end(), children.rbegin(), children.rend());
				}
			}
			// Parents are written only within the subset.
			for (EntityToWrite& entity : entities)
			{
				if (entity.Parent.IsValid() && !written.contains(entity.Parent))
					entity.Parent = UUID::Null();
			}
			return entities;
		}

		// The references of a chunk, each once in the order they first appear in it.
		class ReferenceCollector
		{
		public:
			void Add(AssetHandle handle)
			{
				if (handle.IsValid() && m_Seen.insert(handle).second)
					m_References.push_back(handle);
			}

			const std::vector<AssetHandle>& GetReferences() const { return m_References; }

			void Clear()
			{
				m_References.clear();
				m_Seen.clear();
			}
		private:
			std::vector<AssetHandle> m_References;
			std::unordered_set<AssetHandle> m_Seen;
		};

		// The asset properties of a component's JSON, as AssetDependencies reads them.
		void CollectJsonReferences(const ComponentInfo& info, const nlohmann::json& component, ReferenceCollector& references)
		{
			for (const PropertyInfo& property : info.Properties)
			{
				if (property.Type != PropertyType::Asset || property.IsTransient())
					continue;
				const nlohmann::json* value = JsonUtils::Find(component, property.Name);
				if (!value)
					continue;
				const std::optional<PropertyValue> parsed = PropertyValueFromJson(property, *value);
				if (parsed && std::holds_alternative<UUID>(*parsed))
					references.Add(std::get<UUID>(*parsed));
			}
		}

		void AppendComponent(std::string& out, const SceneWriteOptions& options, const ComponentPlan& plan, const void* component, ReferenceCollector& references)
		{
			out += plan.Key;
			if (plan.Extra)
			{
				const nlohmann::json json = ComponentAccess::Serialize(*plan.Info, component);
				out += JsonUtils::Dump(json);
				CollectJsonReferences(*plan.Info, json, references);
				if (options.ExtraReferences)
				{
					std::vector<AssetHandle> extra;
					options.ExtraReferences(*plan.Info, json, extra);
					for (const AssetHandle asset : extra)
						references.Add(asset);
				}
				return;
			}
			out += '{';
			for (size_t index = 0; index < plan.Properties.size(); index++)
			{
				if (index > 0)
					out += ',';
				const PropertyPlan& property = plan.Properties[index];
				out += property.Key;
				AppendPropertyValueJson(out, *property.Property, property.Property->GetValue(component));
			}
			out += '}';
			for (const PropertyInfo* property : plan.AssetProperties)
			{
				const PropertyValue value = property->GetValue(component);
				if (const UUID* asset = std::get_if<UUID>(&value))
					references.Add(*asset);
			}
		}

		// An entity as SceneSerializer's entity object, dumped: {"Components":{...},"ID":"...","Parent":"..."}. `find` says
		// whether the entity has a registered component and gives its value (tag components may give null), `kept` its kept
		// unknown components.
		template<typename FindComponent>
		void AppendEntity(std::string& out, const std::vector<ComponentPlan>& plans, const SceneWriteOptions& options, FindComponent&& find,
			const nlohmann::json* kept, UUID id, UUID parent, ReferenceCollector& references)
		{
			out += "{\"Components\":{";
			// Registered components and the kept unknown ones (UnknownComponentsComponent), merged in name order as one JSON object
			// holds them; an unknown one named like a registered one replaces it, as UnknownComponentsComponent::AppendTo does.
			if (kept && !kept->is_object())
				kept = nullptr;
			nlohmann::json::const_iterator keptIt;
			nlohmann::json::const_iterator keptEnd;
			if (kept)
			{
				keptIt = kept->cbegin();
				keptEnd = kept->cend();
			}
			size_t planIndex = 0;
			const void* component = nullptr;
			bool first = true;
			while (true)
			{
				while (planIndex < plans.size() && !find(*plans[planIndex].Info, component))
					planIndex++;
				const bool hasRegistered = planIndex < plans.size();
				const bool hasKept = kept && keptIt != keptEnd;
				if (!hasRegistered && !hasKept)
					break;
				const int order = hasRegistered && hasKept ? plans[planIndex].Info->Name.compare(keptIt.key()) : (hasRegistered ? -1 : 1);
				if (!first)
					out += ',';
				first = false;
				if (order < 0)
				{
					AppendComponent(out, options, plans[planIndex], component, references);
					planIndex++;
					continue;
				}
				JsonUtils::AppendString(out, keptIt.key());
				out += ':';
				out += JsonUtils::Dump(keptIt.value());
				if (order == 0)
				{
					// What a scan of the document finds there: the kept JSON read as that registered component.
					if (keptIt.value().is_object())
						CollectJsonReferences(*plans[planIndex].Info, keptIt.value(), references);
					planIndex++;
				}
				++keptIt;
			}
			out += "},\"ID\":";
			JsonUtils::AppendString(out, id.ToString());
			if (parent.IsValid())
			{
				out += ",\"Parent\":";
				JsonUtils::AppendString(out, parent.ToString());
			}
			out += '}';
			t_SerializedEntities++;
		}

		// A chunk of the entity list, serialized by one job.
		struct Chunk
		{
			std::string Text;
			ReferenceCollector References;
		};

		uint32_t GetFormatVersion(SceneDocumentKind kind)
		{
			switch (kind)
			{
				case SceneDocumentKind::Scene:  return SceneSerializer::c_FormatVersion;
				case SceneDocumentKind::Prefab: return Prefab::c_FormatVersion;
				case SceneDocumentKind::Model:  return Model::c_FormatVersion;
			}
			return 1;
		}

		// The bytes of the document as they go out: counted and hashed.
		class HashedOutput
		{
		public:
			explicit HashedOutput(DocumentOutput& output)
				: m_Output(output)
			{
			}

			bool Write(std::string_view bytes)
			{
				if (!m_Output.Write(bytes))
					return false;
				m_Sha256.Update(bytes);
				m_ContentHash = Hash::FNV1a(bytes, m_ContentHash);
				m_Bytes += bytes.size();
				return true;
			}

			uint64_t GetBytes() const { return m_Bytes; }
			Sha256Digest FinalizeSha256() { return m_Sha256.Finalize(); }
			uint64_t GetContentHash() const { return m_ContentHash; }
		private:
			DocumentOutput& m_Output;
			Sha256 m_Sha256;
			uint64_t m_ContentHash = Hash::FNVOffsetBasis;
			uint64_t m_Bytes = 0;
		};

	}

	const char* SceneDocumentKindToString(SceneDocumentKind kind)
	{
		switch (kind)
		{
			case SceneDocumentKind::Scene:  return "Scene";
			case SceneDocumentKind::Prefab: return "Prefab";
			case SceneDocumentKind::Model:  return "Model";
		}
		return "Unknown";
	}

	uint64_t SceneDocumentWriter::GetThreadEntityCount()
	{
		return t_SerializedEntities;
	}

	namespace
	{

		// The document's head: the header, the content's fields and the entity list's opening (the first line of the layout).
		std::string MakeHead(SceneDocumentKind kind, size_t entities, const Scene* scene, const SceneWriteOptions& options)
		{
			const char* format = SceneDocumentKindToString(kind);
			std::string head = fmt::format("{{\"Strata\":{{\"Format\":\"{}\",\"Version\":{}}},\"{}\":{{", format, GetFormatVersion(kind), format);
			if (kind == SceneDocumentKind::Scene && scene)
			{
				head += "\"Name\":";
				JsonUtils::AppendString(head, options.Name ? *options.Name : scene->GetName());
				head += ',';
				if (options.WriteSettings)
				{
					head += "\"Settings\":";
					head += JsonUtils::Dump(SceneSerializer::SerializeSettings(scene->GetSettings()));
					head += ',';
				}
			}
			head += fmt::format("\"EntityCount\":{},\"Entities\":[", entities);
			return head;
		}

		// Writes the head, then the entities (appendEntity(out, index, references) serializes one), serialized in parallel chunks
		// and written in order while the next chunks are serialized, then the document's end.
		template<typename AppendEntityFunction>
		bool WriteEntities(DocumentOutput& output, const std::string& head, size_t entityCount, const SceneWriteOptions& options,
			AppendEntityFunction&& appendEntity, SceneWriteResult& result, std::string& error)
		{
			result.Entities = entityCount;
			HashedOutput hashed(output);
			if (!hashed.Write(head))
			{
				error = "Could not write the document";
				return false;
			}

			const size_t chunkEntities = std::max<uint32_t>(options.ChunkEntities, 1);
			const size_t chunkCount = (entityCount + chunkEntities - 1) / chunkEntities;
			const uint32_t threads = options.Priority == JobPriority::Background ? JobSystem::GetBackgroundThreadCount() : JobSystem::GetWorkerThreadCount();
			const size_t batchChunks = std::clamp<size_t>(c_ChunksPerThread * (static_cast<size_t>(threads) + 1), c_MinBatchChunks, c_MaxBatchChunks);

			std::atomic<bool> cancelled = false;
			auto isCancelled = [&]()
			{
				if (!cancelled.load(std::memory_order_relaxed) && options.Cancel && options.Cancel())
					cancelled.store(true, std::memory_order_relaxed);
				return cancelled.load(std::memory_order_relaxed);
			};

			// Two batches in flight: one serialized on the job system while the batch before it is written (and hashed) by a job
			// of its own.
			std::array<std::vector<Chunk>, 2> batches;
			JobHandle writing;
			std::atomic<bool> writeFailed = false;
			std::atomic<size_t> entitiesWritten = 0;
			// Progress counts serializing and writing alike (each about half of the work), so that it moves with every chunk: a
			// batch of a million-entity scene takes long enough to show. Reported one call at a time, and only when it grew.
			std::atomic<size_t> entitiesSerialized = 0;
			std::mutex progressMutex;
			float reportedProgress = 0.0f;
			const auto reportProgress = [&]()
			{
				if (!options.Progress)
					return;
				const double done = static_cast<double>(entitiesSerialized.load(std::memory_order_relaxed) + entitiesWritten.load(std::memory_order_relaxed));
				const float share = static_cast<float>(std::min(1.0, done / (2.0 * static_cast<double>(entityCount))));
				std::lock_guard lock(progressMutex);
				if (share <= reportedProgress)
					return;
				reportedProgress = share;
				options.Progress(share);
			};
			std::unordered_set<AssetHandle> seen;
			for (size_t firstChunk = 0, batchIndex = 0; firstChunk < chunkCount; firstChunk += batchChunks, batchIndex++)
			{
				if (isCancelled())
					break;
				std::vector<Chunk>& batch = batches[batchIndex % 2];
				const size_t count = std::min(batchChunks, chunkCount - firstChunk);
				batch.resize(count);
				JobSystem::ParallelFor(static_cast<uint32_t>(count), 1, [&](uint32_t begin, uint32_t end)
				{
					for (uint32_t index = begin; index < end; index++)
					{
						Chunk& chunk = batch[index];
						chunk.Text.clear();
						chunk.References.Clear();
						if (isCancelled())
							return;
						const size_t firstEntity = (firstChunk + index) * chunkEntities;
						const size_t lastEntity = std::min(entityCount, firstEntity + chunkEntities);
						for (size_t entity = firstEntity; entity < lastEntity; entity++)
						{
							chunk.Text += entity == 0 ? "\n" : ",\n";
							appendEntity(chunk.Text, entity, chunk.References);
						}
						entitiesSerialized.fetch_add(lastEntity - firstEntity, std::memory_order_relaxed);
						reportProgress();
					}
				}, options.Priority);

				// The batch before is written by now; then this one's references join the document's, in order.
				JobSystem::Wait(writing);
				if (writeFailed.load() || isCancelled())
					break;
				for (const Chunk& chunk : batch)
				{
					for (const AssetHandle handle : chunk.References.GetReferences())
					{
						if (seen.insert(handle).second)
							result.References.push_back(handle);
					}
				}
				const size_t batchEnd = std::min(entityCount, (firstChunk + count) * chunkEntities);
				writing = JobSystem::Submit([&hashed, &batch, &writeFailed, &entitiesWritten, &reportProgress, batchEnd]()
				{
					for (const Chunk& chunk : batch)
					{
						if (!hashed.Write(chunk.Text))
						{
							writeFailed.store(true);
							return;
						}
					}
					entitiesWritten.store(batchEnd, std::memory_order_relaxed);
					reportProgress();
				}, options.Priority);
			}
			JobSystem::Wait(writing);

			if (writeFailed.load())
			{
				error = "Could not write the document";
				return false;
			}
			if (cancelled.load() || entitiesWritten.load() != entityCount)
			{
				result.Cancelled = true;
				error = "The write was cancelled";
				return false;
			}
			if (!hashed.Write(entityCount == 0 ? "]}}\n" : "\n]}}\n"))
			{
				error = "Could not write the document";
				return false;
			}
			if (entityCount == 0 && options.Progress)
				options.Progress(1.0f);
			result.Bytes = hashed.GetBytes();
			result.Sha256 = hashed.FinalizeSha256();
			result.ContentHash = hashed.GetContentHash();
			return true;
		}

	}

	bool SceneDocumentWriter::Write(const Scene& scene, DocumentOutput& output, const SceneWriteOptions& options, SceneWriteResult* outResult,
		std::string* outError)
	{
		ST_PROFILE_FUNCTION();
		const std::vector<ComponentPlan> plans = MakeComponentPlans(options);
		const std::vector<EntityToWrite> entities = CollectEntities(scene, options.Roots);
		const entt::registry& registry = scene.GetRegistry();
		auto appendEntity = [&](std::string& out, size_t index, ReferenceCollector& references)
		{
			const EntityToWrite& entity = entities[index];
			const UnknownComponentsComponent* unknown = registry.try_get<UnknownComponentsComponent>(entity.Handle);
			AppendEntity(out, plans, options, [&](const ComponentInfo& info, const void*& outValue)
			{
				if (!info.Has(registry, entity.Handle))
					return false;
				// ComponentInfo::Get takes the registry as mutable; with the component present (Has) it only reads, so serializing
				// threads share the snapshot's registry without a race.
				outValue = info.Get(const_cast<entt::registry&>(registry), entity.Handle);
				return true;
			}, unknown ? &unknown->Components : nullptr, registry.get<IDComponent>(entity.Handle).ID, entity.Parent, references);
		};
		SceneWriteResult result;
		std::string error;
		const bool written = WriteEntities(output, MakeHead(options.Kind, entities.size(), &scene, options), entities.size(), options, appendEntity, result, error);
		if (outResult)
			*outResult = std::move(result);
		if (!written && outError)
			*outError = error;
		return written;
	}

	bool SceneDocumentWriter::WritePrototype(const EntityPrototype& prototype, DocumentOutput& output, const SceneWriteOptions& options,
		SceneWriteResult* outResult, std::string* outError)
	{
		ST_PROFILE_FUNCTION();
		if (options.Kind == SceneDocumentKind::Scene)
		{
			if (outError)
				*outError = "A prototype is written as a prefab or a model document";
			return false;
		}
		const std::vector<ComponentPlan> plans = MakeComponentPlans(options);
		auto appendEntity = [&](std::string& out, size_t index, ReferenceCollector& references)
		{
			const uint32_t parent = prototype.GetParentIndices()[index];
			AppendEntity(out, plans, options, [&](const ComponentInfo& info, const void*& outValue)
			{
				outValue = prototype.FindComponent(index, info);
				return outValue != nullptr;
			}, prototype.GetUnknownComponents(index), prototype.GetEntityID(index),
				parent != EntityPrototype::c_NoParent ? prototype.GetEntityID(parent) : UUID::Null(), references);
		};
		SceneWriteResult result;
		std::string error;
		const size_t count = prototype.GetEntityCount();
		const bool written = WriteEntities(output, MakeHead(options.Kind, count, nullptr, options), count, options, appendEntity, result, error);
		if (outResult)
			*outResult = std::move(result);
		if (!written && outError)
			*outError = error;
		return written;
	}

	bool SceneDocumentWriter::WriteJson(const nlohmann::json& document, SceneDocumentKind kind, DocumentOutput& output, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};
		const char* format = SceneDocumentKindToString(kind);
		const nlohmann::json* header = JsonUtils::Find(document, "Strata");
		if (kind == SceneDocumentKind::Scene || !header || JsonUtils::GetString(*header, "Format") != format)
			return fail(fmt::format("Not a Strata {} document", kind == SceneDocumentKind::Scene ? "prefab or model" : format));
		const nlohmann::json* content = JsonUtils::Find(document, format);
		const nlohmann::json* entities = content ? JsonUtils::Find(*content, "Entities") : nullptr;
		if (!entities || !entities->is_array())
			return fail(fmt::format("{} document has no entity list", format));

		std::string text = fmt::format("{{\"Strata\":{},\"{}\":{{\"EntityCount\":{},\"Entities\":[", JsonUtils::Dump(*header), format, entities->size());
		for (size_t index = 0; index < entities->size(); index++)
		{
			text += index == 0 ? "\n" : ",\n";
			text += JsonUtils::Dump((*entities)[index]);
		}
		text += entities->empty() ? "]}}\n" : "\n]}}\n";
		if (!output.Write(text))
			return fail("Could not write the document");
		return true;
	}

}
