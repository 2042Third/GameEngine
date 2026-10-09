#include "Editor/EditorViewport.h"

#include "Editor/EditorContext.h"
#include "Editor/SceneBounds.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Renderer/Renderer.h>
#include <Strata/Scene/Scene.h>

#include <cmath>
#include <cstring>
#include <tuple>
#include <utility>

namespace Strata
{

	namespace
	{

		constexpr const char* c_StateFormat = "EditorViewport";
		constexpr int64_t c_StateVersion = 1;

	}

	////////////////////////////////////////////////////////////////////////////////
	// ViewportSettings
	////////////////////////////////////////////////////////////////////////////////

	nlohmann::json ViewportSettings::ToJson() const
	{
		return {
			{ "ShowGrid", ShowGrid },
			{ "ShowSelectionOutline", ShowSelectionOutline },
			{ "ShowSceneGizmos", ShowSceneGizmos },
			{ "ShowStats", ShowStats },
			{ "Gizmo", GizmoOperationToString(Gizmo) },
			{ "Space", GizmoSpaceToString(Space) },
			{ "TranslateSnap", TranslateSnap },
			{ "RotateSnap", RotateSnap },
			{ "ScaleSnap", ScaleSnap }
		};
	}

	bool ViewportSettings::FromJson(const nlohmann::json& json, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};
		if (!json.is_object())
			return fail("the viewport settings must be an object");

		ViewportSettings result = *this;
		for (const auto& [key, flag] : { std::pair<const char*, bool*> { "ShowGrid", &result.ShowGrid }, { "ShowSelectionOutline", &result.ShowSelectionOutline },
			{ "ShowSceneGizmos", &result.ShowSceneGizmos }, { "ShowStats", &result.ShowStats } })
		{
			if (const auto it = json.find(key); it != json.end())
			{
				if (!it->is_boolean())
					return fail(fmt::format("'{}' must be true or false", key));
				*flag = it->get<bool>();
			}
		}
		if (const auto it = json.find("Gizmo"); it != json.end())
		{
			const std::optional<GizmoOperation> operation = it->is_string() ? GizmoOperationFromString(it->get<std::string>()) : std::nullopt;
			if (!operation)
				return fail("'Gizmo' must be \"None\", \"Translate\", \"Rotate\" or \"Scale\"");
			result.Gizmo = *operation;
		}
		if (const auto it = json.find("Space"); it != json.end())
		{
			const std::optional<GizmoSpace> space = it->is_string() ? GizmoSpaceFromString(it->get<std::string>()) : std::nullopt;
			if (!space)
				return fail("'Space' must be \"Local\" or \"World\"");
			result.Space = *space;
		}
		for (const auto& [key, value, maximum] : { std::tuple<const char*, float*, float> { "TranslateSnap", &result.TranslateSnap, c_MaxTranslateSnap },
			{ "RotateSnap", &result.RotateSnap, c_MaxRotateSnap }, { "ScaleSnap", &result.ScaleSnap, c_MaxScaleSnap } })
		{
			if (const auto it = json.find(key); it != json.end())
			{
				// Read as double: converting a number beyond the float range to float is undefined.
				const double number = it->is_number() ? it->get<double>() : -1.0;
				if (!std::isfinite(number) || !(number > 0.0) || number > maximum)
					return fail(fmt::format("'{}' must be a number above 0 and at most {}", key, maximum));
				*value = static_cast<float>(number);
			}
		}
		*this = result;
		return true;
	}

	////////////////////////////////////////////////////////////////////////////////
	// ViewportImageArea
	////////////////////////////////////////////////////////////////////////////////

	glm::uvec2 ViewportImageArea::GetPixelSize() const
	{
		const glm::vec2 pixels = glm::max(Size * PixelScale, glm::vec2(0.0f));
		if (!std::isfinite(pixels.x) || !std::isfinite(pixels.y))
			return glm::uvec2(0);
		return glm::uvec2(pixels);
	}

	glm::vec2 ViewportImageArea::ChoosePixelScale(const glm::vec2& viewportScale, const glm::vec2& displayScale)
	{
		auto valid = [](const glm::vec2& scale) { return scale.x > 0.0f && scale.y > 0.0f && std::isfinite(scale.x) && std::isfinite(scale.y); };
		if (valid(viewportScale))
			return viewportScale;
		if (valid(displayScale))
			return displayScale;
		return glm::vec2(1.0f);
	}

	std::optional<glm::uvec2> ViewportImageArea::ToPixel(const glm::vec2& position) const
	{
		const glm::uvec2 size = GetPixelSize();
		const glm::vec2 local = position - Min;
		if (size.x == 0 || size.y == 0 || !(local.x >= 0.0f && local.y >= 0.0f && local.x < Size.x && local.y < Size.y))
			return std::nullopt;
		// Pixels are cut off at the image's last full pixel, so positions in the remainder land on it.
		return glm::min(glm::uvec2(local * PixelScale), size - glm::uvec2(1));
	}

	////////////////////////////////////////////////////////////////////////////////
	// EditorViewport
	////////////////////////////////////////////////////////////////////////////////

	EditorViewport::~EditorViewport() = default;

	float EditorViewport::GetAspectRatio() const
	{
		if (m_Size.x == 0 || m_Size.y == 0)
			return static_cast<float>(c_DefaultWidth) / static_cast<float>(c_DefaultHeight);
		return static_cast<float>(m_Size.x) / static_cast<float>(m_Size.y);
	}

	ViewportRenderer* EditorViewport::GetRenderer()
	{
		if (!m_Renderer && Renderer::IsInitialized())
			m_Renderer = CreateScope<ViewportRenderer>("Viewport");
		return m_Renderer.get();
	}

	ViewportRenderer* EditorViewport::GetCaptureRenderer()
	{
		if (!m_CaptureRenderer && Renderer::IsInitialized())
			m_CaptureRenderer = CreateScope<ViewportRenderer>("ViewportCapture");
		return m_CaptureRenderer.get();
	}

	bool EditorViewport::Focus(EditorContext& context, std::span<const Entity> entities)
	{
		Scene& scene = *context.GetActiveScene();
		const AABB bounds = entities.empty() ? SceneBounds::GetSceneBounds(scene) : SceneBounds::GetEntitiesBounds(scene, entities);
		return m_Camera.Focus(bounds, GetAspectRatio());
	}

	////////////////////////////////////////////////////////////////////////////////
	// Picking
	////////////////////////////////////////////////////////////////////////////////

	bool EditorViewport::RequestPick(const glm::uvec2& pixel, const glm::uvec2& imageSize, ViewportPickMode mode)
	{
		if (!m_Renderer || m_Renderer->GetSize() != imageSize)
			return false;
		Scope<TextureReadback> readback = m_Renderer->GetSceneRenderer().ReadEntityIDAsync(pixel.x, pixel.y);
		if (!readback)
			return false;
		m_Pick = CreateScope<PendingPick>();
		m_Pick->Readback = std::move(readback);
		m_Pick->PickedScene = m_Renderer->GetRenderedScene();
		m_Pick->Mode = mode;
		m_Pick->Requested = std::chrono::steady_clock::now();
		return true;
	}

	void EditorViewport::UpdatePicking(EditorContext& context)
	{
		if (!m_Pick)
			return;
		if (!m_Pick->Readback->IsReady())
		{
			if (std::chrono::steady_clock::now() - m_Pick->Requested > c_PickTimeout)
			{
				ST_WARN("Picking in the viewport timed out: the GPU did not finish the readback");
				m_Pick.reset();
			}
			return;
		}

		const Scope<PendingPick> pick = std::move(m_Pick);
		ReadbackImage image;
		if (!pick->Readback->GetResult(image) || image.Pixels.size() != sizeof(uint32_t))
			return;
		const Ref<Scene> scene = pick->PickedScene.lock();
		if (!scene || scene != context.GetActiveScene())
			return; // The clicked image showed another scene
		uint32_t id = 0;
		std::memcpy(&id, image.Pixels.data(), sizeof(id));
		ApplyPick(context, SceneRenderer::GetEntityFromID(*scene, id), pick->Mode);
	}

	void EditorViewport::ApplyPick(EditorContext& context, Entity entity, ViewportPickMode mode)
	{
		if (entity && entity.GetScene() != context.GetActiveScene().get())
			entity = {};
		switch (mode)
		{
			case ViewportPickMode::Replace:
				if (entity)
					context.Select(entity.GetUUID());
				else
					context.ClearSelection();
				break;
			case ViewportPickMode::Toggle:
				if (entity && context.IsSelected(entity.GetUUID()))
					context.Deselect(entity.GetUUID());
				else if (entity)
					context.Select(entity.GetUUID(), true);
				break;
			case ViewportPickMode::Add:
				if (entity)
					context.Select(entity.GetUUID(), true);
				break;
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// Persistence
	////////////////////////////////////////////////////////////////////////////////

	nlohmann::json EditorViewport::ToJson() const
	{
		return {
			{ "Strata", { { "Format", c_StateFormat }, { "Version", c_StateVersion } } },
			{ "Camera", m_Camera.ToJson() },
			{ "Settings", m_Settings.ToJson() }
		};
	}

	bool EditorViewport::FromJson(const nlohmann::json& json, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};
		const nlohmann::json* header = json.is_object() ? JsonUtils::Find(json, "Strata") : nullptr;
		if (!header || !header->is_object() || JsonUtils::GetString(*header, "Format") != c_StateFormat)
			return fail(fmt::format("not an editor viewport state (expected the format \"{}\")", c_StateFormat));
		const int64_t version = JsonUtils::GetInt(*header, "Version", 0);
		if (version < 1 || version > c_StateVersion)
			return fail(fmt::format("unsupported version {} (supported: 1 to {})", version, c_StateVersion));

		// Both parts are validated before either changes.
		EditorCamera camera = m_Camera;
		ViewportSettings settings = m_Settings;
		std::string error;
		if (const nlohmann::json* cameraJson = JsonUtils::Find(json, "Camera"); cameraJson && !camera.FromJson(*cameraJson, &error))
			return fail(fmt::format("camera: {}", error));
		if (const nlohmann::json* settingsJson = JsonUtils::Find(json, "Settings"); settingsJson && !settings.FromJson(*settingsJson, &error))
			return fail(fmt::format("settings: {}", error));
		m_Camera = camera;
		m_Settings = settings;
		return true;
	}

	bool EditorViewport::Save(const std::filesystem::path& path, std::string* outError) const
	{
		if (!FileSystem::CreateDirectories(path.parent_path()) || !FileSystem::WriteText(path, JsonUtils::Dump(ToJson(), 1, '\t') + "\n"))
		{
			if (outError)
				*outError = fmt::format("cannot write '{}'", FileSystem::ToUTF8(path));
			return false;
		}
		return true;
	}

	bool EditorViewport::Load(const std::filesystem::path& path, std::string* outError)
	{
		const std::optional<std::string> text = FileSystem::ReadText(path);
		if (!text)
		{
			if (outError)
				*outError = fmt::format("cannot read '{}'", FileSystem::ToUTF8(path));
			return false;
		}
		std::string error;
		const std::optional<nlohmann::json> json = JsonUtils::Parse(*text, &error);
		if (!json)
		{
			if (outError)
				*outError = fmt::format("'{}' is not valid JSON: {}", FileSystem::ToUTF8(path), error);
			return false;
		}
		if (!FromJson(*json, &error))
		{
			if (outError)
				*outError = fmt::format("'{}': {}", FileSystem::ToUTF8(path), error);
			return false;
		}
		return true;
	}

	void EditorViewport::ResetState()
	{
		m_Camera.Reset();
		m_Settings = ViewportSettings();
		m_Pick.reset();
	}

}
