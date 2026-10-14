#include "stpch.h"
#include "Strata/Scene/SceneDocumentReader.h"

#include "Strata/Core/JobSystem.h"
#include "Strata/Core/JsonEvents.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Scene/EntitySink.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/SceneSerializer.h"

#include <algorithm>
#include <atomic>
#include <optional>
#include <utility>
#include <vector>

namespace Strata
{

	namespace
	{

		// The fewest bytes an entity takes in a document ("{}" and a comma): a declared count is reserved only as far as the
		// text could hold it.
		constexpr uint64_t c_MinEntityBytes = 3;

		// Builds the DOM of one value from parse events, as nlohmann's own DOM parser does (a key that appears twice keeps its
		// last value).
		class ValueBuilder
		{
		public:
			void Reset()
			{
				m_Root = nullptr;
				m_Stack.clear();
				m_Element = nullptr;
			}

			// A scalar: true when it is the whole value.
			bool Scalar(nlohmann::json&& value)
			{
				if (m_Stack.empty())
				{
					m_Root = std::move(value);
					return true;
				}
				Insert(std::move(value));
				return false;
			}

			void StartContainer(nlohmann::json&& container)
			{
				if (m_Stack.empty())
				{
					m_Root = std::move(container);
					m_Stack.push_back(&m_Root);
					return;
				}
				m_Stack.push_back(Insert(std::move(container)));
			}

			void Key(std::string& key)
			{
				// The lexer's buffer: it is cleared before the next token, so the key may be moved out of it.
				m_Element = &m_Stack.back()->get_ref<nlohmann::json::object_t&>()[std::move(key)];
			}

			// True when the container ended the value.
			bool EndContainer()
			{
				m_Stack.pop_back();
				return m_Stack.empty();
			}

			nlohmann::json& GetValue() { return m_Root; }
		private:
			nlohmann::json* Insert(nlohmann::json&& value)
			{
				nlohmann::json& container = *m_Stack.back();
				if (container.is_array())
				{
					nlohmann::json::array_t& array = container.get_ref<nlohmann::json::array_t&>();
					array.push_back(std::move(value));
					return &array.back();
				}
				*m_Element = std::move(value);
				return m_Element;
			}
		private:
			nlohmann::json m_Root;
			std::vector<nlohmann::json*> m_Stack;
			nlohmann::json* m_Element = nullptr;
		};

		// How a document kind is named and versioned, and what its errors say (the messages of the DOM readers before it:
		// SceneSerializer::Deserialize and Prefab's and Model's).
		struct DocumentRules
		{
			std::string_view Format;
			uint32_t MaxVersion = 1;
			bool IsScene = false;

			std::string NotDocument() const
			{
				return IsScene ? std::string("Not a Strata scene document (missing 'Strata' or 'Scene')") : fmt::format("Not a Strata {} document", Format);
			}
			std::string WrongFormat() const
			{
				return IsScene ? std::string("Document format is not 'Scene'") : fmt::format("Not a Strata {} document", Format);
			}
			std::string WrongVersion(uint64_t version) const
			{
				return IsScene ? fmt::format("Unsupported scene format version {} (supported: 1-{})", version, MaxVersion)
					: fmt::format("Unsupported {} version {}", Format, version);
			}
			std::string ContentNotObject() const
			{
				return IsScene ? std::string("'Scene' must be an object") : NoEntityList();
			}
			std::string EntitiesNotArray() const
			{
				return IsScene ? std::string("Entity data must be an object with an 'Entities' array") : NoEntityList();
			}
			std::string NoEntityList() const
			{
				return fmt::format("{} document has no entity list", Format);
			}
		};

		DocumentRules GetRules(SceneDocumentKind kind)
		{
			switch (kind)
			{
				case SceneDocumentKind::Scene:  return DocumentRules { "Scene", SceneSerializer::c_FormatVersion, true };
				case SceneDocumentKind::Prefab: return DocumentRules { "Prefab", Prefab::c_FormatVersion, false };
				case SceneDocumentKind::Model:  return DocumentRules { "Model", Model::c_FormatVersion, false };
			}
			return DocumentRules { "Scene", SceneSerializer::c_FormatVersion, true };
		}

		// The parse events of a document, sorted into what the sink hears of: the header and the content's small fields are
		// built as DOMs of their own, each entity of the list as one, other keys are skipped without building anything.
		class DocumentHandler
		{
		public:
			DocumentHandler(const DocumentRules& rules, EntitySink& sink, const SceneReadOptions& options, uint64_t textBytes)
				: m_Rules(rules), m_Sink(sink), m_Options(options), m_TextBytes(textBytes)
			{
			}

			// nlohmann's SAX interface (ParseJsonEvents).
			bool null() { return OnScalar(nlohmann::json(nullptr)); }
			bool boolean(bool value) { return OnScalar(nlohmann::json(value)); }
			bool number_integer(nlohmann::json::number_integer_t value) { return OnScalar(nlohmann::json(value)); }
			bool number_unsigned(nlohmann::json::number_unsigned_t value) { return OnScalar(nlohmann::json(value)); }
			bool number_float(nlohmann::json::number_float_t value, const std::string&) { return OnScalar(nlohmann::json(value)); }
			bool string(std::string& value) { return OnScalar(nlohmann::json(std::move(value))); }
			// JSON text holds no binary values (nlohmann's other input formats do).
			bool binary(nlohmann::json::binary_t&) { return Fail("Invalid JSON: a binary value"); }
			bool start_object(std::size_t) { return OnStart(true); }
			bool start_array(std::size_t) { return OnStart(false); }
			bool end_object() { return OnEnd(); }
			bool end_array() { return OnEnd(); }
			bool key(std::string& key) { return OnKey(key); }
			bool parse_error(std::size_t, const std::string&, const nlohmann::json::exception& exception)
			{
				if (m_Error.empty())
					m_Error = fmt::format("Invalid JSON: {}", exception.what());
				m_SyntaxError = true;
				return false;
			}

			// After a parse that succeeded: what the document must have had.
			bool Complete()
			{
				if (!m_HeaderSeen || !m_ContentSeen)
					return Fail(!m_HeaderSeen || m_Rules.IsScene ? m_Rules.NotDocument() : m_Rules.NoEntityList());
				if (!m_Rules.IsScene && !m_EntitiesSeen)
					return Fail(m_Rules.NoEntityList());
				return true;
			}

			const std::string& GetError() const { return m_Error; }
			uint64_t GetEntityCount() const { return m_Entities; }
			bool IsCancelled() const { return m_Cancelled; }
			bool IsEntityError() const { return m_EntityError; }
			bool IsSyntaxError() const { return m_SyntaxError; }
		private:
			// Where the parse is: the structure the reader knows, or a value it builds or skips.
			enum class Mode : uint8_t
			{
				Document, // Before the document's value
				Top,      // In the document object
				Content,  // In the content object ("Scene", "Prefab", "Model")
				Entities, // In the entity list
				Build,    // Building a value (m_Building), then back to m_ReturnMode
				Skip,     // Skipping a value (m_SkipDepth containers deep), then back to m_ReturnMode
				Done      // After the document object
			};
			// What the next value of the object being read is.
			enum class Slot : uint8_t
			{
				Other = 0,
				Header,
				Content,
				Entities,
				EntityCount,
				Field,
				Entity
			};

			bool Fail(std::string message)
			{
				if (m_Error.empty())
					m_Error = std::move(message);
				return false;
			}

			bool OnKey(std::string& key)
			{
				switch (m_Mode)
				{
					case Mode::Build:
						m_Builder.Key(key);
						return true;
					case Mode::Skip:
						return true;
					case Mode::Top:
						m_Slot = key == "Strata" ? Slot::Header : key == m_Rules.Format ? Slot::Content : Slot::Other;
						return true;
					case Mode::Content:
						if (key == "Entities")
						{
							m_Slot = Slot::Entities;
						}
						else if (key == "EntityCount")
						{
							m_Slot = Slot::EntityCount;
						}
						else if (m_Rules.IsScene && (key == "Name" || key == "Settings"))
						{
							m_Slot = Slot::Field;
							m_FieldKey = key;
						}
						else
						{
							m_Slot = Slot::Other;
						}
						return true;
					default:
						return Fail("Invalid JSON: a key outside an object");
				}
			}

			bool OnScalar(nlohmann::json&& value)
			{
				switch (m_Mode)
				{
					case Mode::Build:
						if (m_Builder.Scalar(std::move(value)))
							return OnBuilt();
						return true;
					case Mode::Skip:
						return true;
					case Mode::Document:
						return Fail(m_Rules.NotDocument());
					case Mode::Entities:
						m_Builder.Reset();
						m_Builder.Scalar(std::move(value));
						return OnEntity();
					case Mode::Top:
					case Mode::Content:
						return OnMemberScalar(std::move(value));
					case Mode::Done:
						break;
				}
				return Fail("Invalid JSON: a value after the document");
			}

			bool OnMemberScalar(nlohmann::json&& value)
			{
				switch (m_Slot)
				{
					case Slot::Header:
						m_Builder.Reset();
						m_Builder.Scalar(std::move(value));
						m_BuiltSlot = Slot::Header;
						return OnBuilt();
					case Slot::Content:
						return Fail(m_Rules.ContentNotObject());
					case Slot::Entities:
						return Fail(m_Rules.EntitiesNotArray());
					case Slot::EntityCount:
						return OnEntityCount(value);
					case Slot::Field:
						m_Sink.ReadField(m_FieldKey, value);
						return true;
					case Slot::Other:
					case Slot::Entity:
						return true;
				}
				return true;
			}

			bool OnStart(bool object)
			{
				switch (m_Mode)
				{
					case Mode::Build:
						m_Builder.StartContainer(object ? nlohmann::json::object() : nlohmann::json::array());
						return true;
					case Mode::Skip:
						m_SkipDepth++;
						return true;
					case Mode::Document:
						if (!object)
							return Fail(m_Rules.NotDocument());
						m_Mode = Mode::Top;
						return true;
					case Mode::Entities:
						return StartBuilding(object, Slot::Entity, Mode::Entities);
					case Mode::Top:
						return OnMemberStart(object, Mode::Top);
					case Mode::Content:
						return OnMemberStart(object, Mode::Content);
					case Mode::Done:
						break;
				}
				return Fail("Invalid JSON: a value after the document");
			}

			bool OnMemberStart(bool object, Mode parent)
			{
				switch (m_Slot)
				{
					case Slot::Header:
						return StartBuilding(object, Slot::Header, parent);
					case Slot::Field:
						return StartBuilding(object, Slot::Field, parent);
					case Slot::Content:
						if (!object)
							return Fail(m_Rules.ContentNotObject());
						if (m_ContentSeen)
							return Fail(fmt::format("The document holds '{}' twice", m_Rules.Format));
						m_ContentSeen = true;
						m_Mode = Mode::Content;
						return true;
					case Slot::Entities:
						if (object)
							return Fail(m_Rules.EntitiesNotArray());
						if (m_EntitiesSeen)
							return Fail("The document holds 'Entities' twice");
						m_EntitiesSeen = true;
						m_Mode = Mode::Entities;
						return true;
					case Slot::EntityCount:
					case Slot::Other:
					case Slot::Entity:
						m_SkipDepth = 1;
						m_ReturnMode = parent;
						m_Mode = Mode::Skip;
						return true;
				}
				return true;
			}

			bool StartBuilding(bool object, Slot slot, Mode returnMode)
			{
				m_Builder.Reset();
				m_Builder.StartContainer(object ? nlohmann::json::object() : nlohmann::json::array());
				m_BuiltSlot = slot;
				m_ReturnMode = returnMode;
				m_Mode = Mode::Build;
				return true;
			}

			bool OnEnd()
			{
				switch (m_Mode)
				{
					case Mode::Build:
						if (m_Builder.EndContainer())
						{
							m_Mode = m_ReturnMode;
							return OnBuilt();
						}
						return true;
					case Mode::Skip:
						if (--m_SkipDepth == 0)
							m_Mode = m_ReturnMode;
						return true;
					case Mode::Entities:
						m_Mode = Mode::Content;
						return true;
					case Mode::Content:
						m_Mode = Mode::Top;
						return true;
					case Mode::Top:
						m_Mode = Mode::Done;
						return true;
					case Mode::Document:
					case Mode::Done:
						break;
				}
				return Fail("Invalid JSON: an unmatched end");
			}

			// A value built for m_BuiltSlot is complete.
			bool OnBuilt()
			{
				switch (m_BuiltSlot)
				{
					case Slot::Header:
						return OnHeader(m_Builder.GetValue());
					case Slot::Field:
						m_Sink.ReadField(m_FieldKey, m_Builder.GetValue());
						return true;
					case Slot::Entity:
						return OnEntity();
					default:
						return true;
				}
			}

			bool OnHeader(const nlohmann::json& header)
			{
				// A key that appears twice keeps its last value, as in a DOM: the last header is the one validated.
				m_HeaderSeen = true;
				if (!header.is_object() || JsonUtils::GetString(header, "Format") != m_Rules.Format)
					return Fail(m_Rules.WrongFormat());
				const uint64_t version = JsonUtils::GetUInt(header, "Version", 0);
				if (version == 0 || version > m_Rules.MaxVersion)
					return Fail(m_Rules.WrongVersion(version));
				return true;
			}

			bool OnEntityCount(const nlohmann::json& count)
			{
				// Only a count that comes before the entities says anything in time; one that is not a count is ignored, as
				// readers before it ignored the key.
				if (m_EntitiesSeen || !count.is_number_unsigned())
					return true;
				const uint64_t declared = count.get<uint64_t>();
				if (declared > m_Options.MaxEntities)
					return Fail(fmt::format("The document holds {} entities, more than the {} that fit (a scene holds at most {})", declared,
						m_Options.MaxEntities, Scene::c_MaxEntities));
				m_Sink.ReserveEntities(std::min(declared, m_TextBytes / c_MinEntityBytes));
				return true;
			}

			bool OnEntity()
			{
				if (m_Entities >= m_Options.MaxEntities)
					return Fail(fmt::format("The document holds more than the {} entities that fit (a scene holds at most {})", m_Options.MaxEntities,
						Scene::c_MaxEntities));
				m_Entities++;
				std::string error;
				if (!m_Sink.ReadEntity(m_Builder.GetValue(), &error))
				{
					m_EntityError = true;
					return Fail(std::move(error));
				}
				const uint32_t interval = std::max<uint32_t>(m_Options.CancelCheckEntities, 1);
				if (m_Options.IsCancelled && m_Entities % interval == 0 && m_Options.IsCancelled())
				{
					m_Cancelled = true;
					return Fail("The read was cancelled");
				}
				return true;
			}
		private:
			const DocumentRules& m_Rules;
			EntitySink& m_Sink;
			const SceneReadOptions& m_Options;
			uint64_t m_TextBytes = 0;

			Mode m_Mode = Mode::Document;
			Mode m_ReturnMode = Mode::Document;
			Slot m_Slot = Slot::Other;
			Slot m_BuiltSlot = Slot::Other;
			uint64_t m_SkipDepth = 0;
			ValueBuilder m_Builder;
			std::string m_FieldKey;

			bool m_HeaderSeen = false;
			bool m_ContentSeen = false;
			bool m_EntitiesSeen = false;
			uint64_t m_Entities = 0;
			bool m_Cancelled = false;
			bool m_EntityError = false;
			bool m_SyntaxError = false;
			std::string m_Error;
		};

		// The entity lines of a document in the writer's layout (SceneDocumentWriter): its first line holds the header and the
		// content's fields up to the entity list's opening bracket, each entity follows on a line of its own (each but the last
		// with a comma), and the last line closes the list and the document. A line may end with a carriage return as well (a
		// file checked out with Windows line ends).
		struct EntityLines
		{
			std::string_view Head;               // The first line
			std::vector<std::string_view> Lines; // Each entity's object, without its comma
		};

		std::string_view TrimLineEnd(std::string_view line)
		{
			if (!line.empty() && line.back() == '\r')
				line.remove_suffix(1);
			return line;
		}

		std::optional<EntityLines> FindEntityLines(std::string_view text)
		{
			const size_t headEnd = text.find('\n');
			if (headEnd == std::string_view::npos)
				return std::nullopt;
			EntityLines lines;
			lines.Head = TrimLineEnd(text.substr(0, headEnd));
			if (!lines.Head.starts_with('{') || !lines.Head.ends_with("\"Entities\":["))
				return std::nullopt;
			size_t start = headEnd + 1;
			while (start < text.size())
			{
				const size_t end = text.find('\n', start);
				const std::string_view line = TrimLineEnd(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
				if (line == "]}}")
				{
					// Nothing but the line's end may follow.
					if (end != std::string_view::npos && end + 1 != text.size())
						return std::nullopt;
					for (size_t index = 0; index < lines.Lines.size(); index++)
					{
						std::string_view& entity = lines.Lines[index];
						const bool last = index + 1 == lines.Lines.size();
						if (last == entity.ends_with(','))
							return std::nullopt;
						if (!last)
							entity.remove_suffix(1);
						if (!entity.starts_with('{') || !entity.ends_with('}'))
							return std::nullopt;
					}
					return lines;
				}
				if (end == std::string_view::npos)
					return std::nullopt;
				lines.Lines.push_back(line);
				start = end + 1;
			}
			return std::nullopt;
		}

		// Whether the head and every entity line parse alone, checked on the job system's threads before anything is handed to
		// the sink: a document whose entities span lines or share one is read as a stream instead.
		bool LinesParseAlone(const EntityLines& lines, uint32_t chunkEntities, JobPriority priority)
		{
			const std::string head = std::string(lines.Head) + "]}}";
			if (!nlohmann::json::accept(head.begin(), head.end()))
				return false;
			std::atomic<bool> failed = false;
			const uint32_t chunks = static_cast<uint32_t>((lines.Lines.size() + chunkEntities - 1) / chunkEntities);
			JobSystem::ParallelFor(chunks, 1, [&](uint32_t begin, uint32_t end)
			{
				for (uint32_t chunk = begin; chunk < end && !failed.load(std::memory_order_relaxed); chunk++)
				{
					const size_t last = std::min<size_t>(lines.Lines.size(), static_cast<size_t>(chunk + 1) * chunkEntities);
					for (size_t index = static_cast<size_t>(chunk) * chunkEntities; index < last; index++)
					{
						if (!nlohmann::json::accept(lines.Lines[index].begin(), lines.Lines[index].end()))
						{
							failed.store(true, std::memory_order_relaxed);
							return;
						}
					}
				}
			}, priority);
			return !failed.load();
		}

		// One chunk of entity lines on its way to the sink: prepared by a job, handed over in order by the reading thread.
		struct ChunkSlot
		{
			PreparedEntities Entities;
			std::string Error;   // The entity of the chunk that cannot be read (the chunk's entities before it are prepared)
			bool Failed = false;
			bool Complete = false; // The job ran to its end (a job that threw leaves it false)
			JobHandle Job;
		};

		void PrepareChunk(const EntityLines& lines, size_t first, size_t count, ChunkSlot& slot, const std::atomic<bool>& stop)
		{
			slot.Entities = PreparedEntities {};
			slot.Entities.Columns.Reserve(count);
			for (size_t index = first; index < first + count; index++)
			{
				if (stop.load(std::memory_order_relaxed))
					return;
				// Every line parsed alone when the read began (LinesParseAlone).
				const std::optional<nlohmann::json> entity = JsonUtils::Parse(lines.Lines[index], &slot.Error);
				if (!entity || !slot.Entities.Columns.Add(*entity, &slot.Entities.Warnings, slot.Entities.Unknown, &slot.Error))
				{
					slot.Failed = true;
					break;
				}
			}
			slot.Complete = true;
		}

	}

	bool SceneDocumentReader::Read(std::string_view text, SceneDocumentKind kind, EntitySink& sink, const SceneReadOptions& options,
		SceneReadResult* outResult, std::string* outError)
	{
		ST_PROFILE_FUNCTION();
		SceneReadResult result;
		auto finish = [&](bool success, std::string error)
		{
			if (outResult)
				*outResult = result;
			if (!success && outError)
				*outError = std::move(error);
			return success;
		};
		if (options.IsCancelled && options.IsCancelled())
		{
			result.Cancelled = true;
			return finish(false, "The read was cancelled");
		}

		const DocumentRules rules = GetRules(kind);
		DocumentHandler handler(rules, sink, options, text.size());

		// Large documents in the writer's layout: the entities parsed on the job system's threads, chunk by chunk, handed over
		// in order.
		const uint32_t chunkEntities = std::max<uint32_t>(options.ParallelChunkEntities, 1);
		if (options.Parallel && sink.TakesPreparedEntities() && JobSystem::IsInitialized())
		{
			std::optional<EntityLines> lines = FindEntityLines(text);
			if (lines && lines->Lines.size() >= options.ParallelMinEntities && LinesParseAlone(*lines, chunkEntities, options.Priority))
			{
				result.Parallel = true;
				// The head with an empty list is the document without its entities: its header and fields reach the sink as a
				// stream's would (and a declared count over the limit is refused there).
				const std::string head = std::string(lines->Head) + "]}}";
				if (!ParseJsonEvents(head, handler))
				{
					result.SyntaxError = handler.IsSyntaxError();
					return finish(false, handler.GetError());
				}
				const size_t total = lines->Lines.size();
				if (total > options.MaxEntities)
					return finish(false, fmt::format("The document holds more than the {} entities that fit (a scene holds at most {})", options.MaxEntities,
						Scene::c_MaxEntities));

				const size_t chunks = (total + chunkEntities - 1) / chunkEntities;
				// Enough chunks under way to keep the threads busy while the reading thread hands them over, and no more: what is
				// prepared waits in memory.
				const size_t window = std::min(chunks, 2 * (static_cast<size_t>(JobSystem::GetWorkerThreadCount()) + 1));
				std::vector<ChunkSlot> slots(window);
				std::atomic<bool> stop = false;
				auto submit = [&](size_t chunk)
				{
					ChunkSlot& slot = slots[chunk % window];
					const size_t first = chunk * chunkEntities;
					const size_t count = std::min<size_t>(chunkEntities, total - first);
					slot.Failed = false;
					slot.Complete = false;
					slot.Error.clear();
					slot.Job = JobSystem::Submit([&lines, &slot, &stop, first, count]() { PrepareChunk(*lines, first, count, slot, stop); }, options.Priority);
				};
				// Ends the jobs under way (they stop at their next entity) before the lines and slots they use go.
				auto abandon = [&](std::string error)
				{
					stop.store(true);
					for (ChunkSlot& slot : slots)
					{
						if (slot.Job.IsValid())
							JobSystem::Wait(slot.Job);
					}
					return finish(false, std::move(error));
				};

				for (size_t chunk = 0; chunk < window; chunk++)
					submit(chunk);
				for (size_t chunk = 0; chunk < chunks; chunk++)
				{
					ChunkSlot& slot = slots[chunk % window];
					JobSystem::Wait(slot.Job);
					slot.Job = JobHandle();
					if (!slot.Complete)
						return abandon("A chunk of entities could not be read");
					const size_t prepared = slot.Entities.Columns.GetEntityCount();
					std::string error;
					if (!sink.ReadPrepared(slot.Entities, &error))
					{
						result.EntityError = true;
						return abandon(std::move(error));
					}
					result.Entities += prepared;
					if (slot.Failed)
					{
						result.EntityError = true;
						return abandon(slot.Error);
					}
					slot.Entities = PreparedEntities {};
					if (chunk + window < chunks)
						submit(chunk + window);
					if (options.Progress)
					{
						const size_t next = std::min(total, (chunk + 1) * chunkEntities);
						const size_t parsed = next < total ? static_cast<size_t>(lines->Lines[next].data() - text.data()) : text.size();
						options.Progress(static_cast<float>(static_cast<double>(parsed) / static_cast<double>(std::max<size_t>(text.size(), 1))));
					}
					if (options.IsCancelled && options.IsCancelled())
					{
						result.Cancelled = true;
						return abandon("The read was cancelled");
					}
				}
				if (!handler.Complete())
					return finish(false, handler.GetError());
				std::string error;
				if (!sink.Finish(&error))
				{
					result.EntityError = true;
					return finish(false, std::move(error));
				}
				return finish(true, {});
			}
		}

		JsonParseProgress progress;
		if (options.Progress)
		{
			progress.Callback = [&options, size = std::max<size_t>(text.size(), 1)](size_t parsedBytes)
			{
				options.Progress(static_cast<float>(static_cast<double>(parsedBytes) / static_cast<double>(size)));
			};
		}
		const bool parsed = ParseJsonEvents(text, handler, &progress);
		result.Entities = handler.GetEntityCount();
		result.Cancelled = handler.IsCancelled();
		result.EntityError = handler.IsEntityError();
		result.SyntaxError = handler.IsSyntaxError();
		if (!parsed)
			return finish(false, handler.GetError().empty() ? std::string("Invalid JSON") : handler.GetError());
		if (!handler.Complete())
			return finish(false, handler.GetError());

		std::string error;
		if (!sink.Finish(&error))
		{
			result.EntityError = true;
			return finish(false, std::move(error));
		}
		return finish(true, {});
	}

}
