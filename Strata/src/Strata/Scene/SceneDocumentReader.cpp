#include "stpch.h"
#include "Strata/Scene/SceneDocumentReader.h"

#include "Strata/Core/JsonEvents.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Scene/EntitySink.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/SceneSerializer.h"

#include <algorithm>
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
