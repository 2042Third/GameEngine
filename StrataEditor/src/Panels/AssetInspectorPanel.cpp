#include "Panels/AssetInspectorPanel.h"

#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/ThumbnailCache.h"
#include "UI/EditorActions.h"
#include "UI/EditorFonts.h"
#include "UI/EditorShell.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/PropertyWidgets.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Asset/AssetImporter.h>
#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Audio/AudioClip.h>
#include <Strata/Audio/AudioClipAsset.h>
#include <Strata/Audio/AudioEngine.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/Mesh.h>
#include <Strata/Renderer/Texture.h>
#include <Strata/Renderer/TextureStreamer.h>
#include <Strata/World/WorldAsset.h>

#include <imgui.h>
#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace Strata
{

	namespace
	{

		// The texture preview is at most this many text heights tall.
		constexpr float c_PreviewMaxHeightInFontSizes = 22.0f;
		// The thumbnail of a material, mesh, model or prefab is at most this many text heights on a side.
		constexpr float c_ThumbnailMaxSizeInFontSizes = 16.0f;
		// The waveform: this many text heights tall, with this many columns.
		constexpr float c_WaveformHeightInFontSizes = 4.0f;
		constexpr uint32_t c_WaveformColumns = 256;

		// "1:05.3" for 65.3 seconds.
		std::string FormatDuration(float seconds)
		{
			const float clamped = std::max(seconds, 0.0f);
			const int minutes = static_cast<int>(clamped / 60.0f);
			return fmt::format("{}:{:04.1f}", minutes, clamped - static_cast<float>(minutes) * 60.0f);
		}

		std::string FormatBytes(uint64_t bytes)
		{
			if (bytes >= 1024ull * 1024ull)
				return fmt::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
			if (bytes >= 1024ull)
				return fmt::format("{:.1f} KB", static_cast<double>(bytes) / 1024.0);
			return fmt::format("{} bytes", bytes);
		}

		// "BC7 sRGB" for TextureFormat::BC7SRGB: the format's name with its color space apart.
		std::string GetTextureFormatDisplayName(TextureFormat format)
		{
			std::string name = TextureFormatToString(format);
			constexpr std::string_view c_SRGBSuffix = "SRGB";
			if (GetTextureFormatInfo(format).IsSRGB && name.size() > c_SRGBSuffix.size() && name.ends_with(c_SRGBSuffix))
				name = name.substr(0, name.size() - c_SRGBSuffix.size()) + " sRGB";
			return name;
		}

		// The preview's caption: size, the stored format (what Compression and Quality chose), mips and what the texture
		// takes on the GPU once uploaded.
		std::string DescribeTexture(const Texture& texture)
		{
			std::string caption = fmt::format("{} x {} \xC2\xB7 {} \xC2\xB7 {} mips", texture.GetWidth(), texture.GetHeight(),
				GetTextureFormatDisplayName(texture.GetSpecification().Format), texture.GetMipCount());
			if (texture.GetSpecification().Grayscale)
				caption += " \xC2\xB7 grayscale";
			if (const uint64_t gpuBytes = texture.GetMemoryUsage().GpuTextures; gpuBytes > 0)
				caption += fmt::format(" \xC2\xB7 {} on the GPU", FormatBytes(gpuBytes));
			return caption;
		}

		// An icon, a title and a hint, all centered (the hint's lines each).
		void DrawEmptyState(const char* icon, const char* title, const char* text)
		{
			const UI::ThemeColors& colors = UI::GetThemeColors();
			ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize()));
			UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Display);
			const float iconWidth = ImGui::CalcTextSize(icon).x;
			ImGui::SetCursorPosX(std::max((ImGui::GetContentRegionAvail().x - iconWidth) * 0.5f, 0.0f) + ImGui::GetCursorPosX());
			ImGui::TextColored(colors.TextDisabled, "%s", icon);
			ImGui::PopFont();
			UI::PushFont(UI::EditorFont::SemiBold, UI::TextSize::Body);
			const float titleWidth = ImGui::CalcTextSize(title).x;
			ImGui::SetCursorPosX(std::max((ImGui::GetContentRegionAvail().x - titleWidth) * 0.5f, 0.0f) + ImGui::GetCursorPosX());
			ImGui::TextUnformatted(title);
			ImGui::PopFont();
			UI::CenteredText("AssetInspector.Empty", text, colors.TextSecondary);
		}

	}

	void AssetInspectorPanel::OnImGuiRender(EditorPanelContext& panelContext)
	{
		EditorContext& context = panelContext.Context;
		const AssetHandle handle = context.GetInspectedAsset();
		// What the preview voice plays stops with the asset it belongs to.
		if (handle != m_Shown)
		{
			m_AudioPreview.Stop();
			m_Shown = handle;
		}
		if (!handle.IsValid())
		{
			m_MaterialEditor.CommitPending(context);
			DrawEmptyState(Icons::FileBox, "No asset inspected", "Click an asset in the Content Browser to see it here: a material's properties, how a "
				"texture, model or sound is imported, a texture's image.");
			return;
		}
		const std::optional<AssetMetadata> metadata = AssetManager::HasActive() ? AssetManager::GetActive()->GetMetadata(handle) : std::nullopt;
		if (!metadata)
		{
			DrawEmptyState(Icons::FileBox, "The asset is gone", "It was deleted or its project closed.");
			return;
		}

		EditorAssetManager* assets = context.GetAssetManager();
		const AssetImporter* importer = assets ? assets->GetImporter(handle) : nullptr;
		DrawHeader(*metadata);
		DrawActions(panelContext, *metadata, importer);
		if (assets && importer)
			DrawImportOutcome(*assets, *metadata);

		if (metadata->Type == AssetType::Material)
		{
			if (UI::SectionHeader("AssetInspector.MaterialSection", (std::string(Icons::Palette) + "  Material").c_str()))
				m_MaterialEditor.Draw(panelContext, handle, "AssetInspector.Material");
		}
		if (metadata->Type == AssetType::AnimationController)
			m_AnimationViews.DrawController(context, handle);
		else
			m_AnimationViews.Reset();
		if (metadata->Type == AssetType::Skeleton && UI::SectionHeader("AssetInspector.SkeletonSection", (std::string(Icons::Bone) + "  Joints").c_str()))
			m_AnimationViews.DrawSkeleton(handle);
		if (metadata->Type == AssetType::AnimationClip && UI::SectionHeader("AssetInspector.ClipSection", (std::string(Icons::Film) + "  Clip").c_str()))
			m_AnimationViews.DrawClip(handle);
		if (assets && importer)
		{
			if (metadata->IsSubAsset())
			{
				ImGui::Spacing();
				ImGui::PushStyleColor(ImGuiCol_Text, UI::GetThemeColors().TextSecondary);
				ImGui::TextWrapped("Imported with the file it comes from: inspect that asset to change how it is imported.");
				ImGui::PopStyleColor();
				if (UI::LinkButton("AssetInspector.InspectParent", "Inspect the source file", Icons::ArrowRight))
					context.SetInspectedAsset(metadata->Parent);
			}
			else if (!importer->GetSettingsProperties().empty())
			{
				DrawImportSettings(panelContext, *assets, *metadata, *importer);
			}
		}
		if (metadata->Type == AssetType::Texture)
			DrawTexturePreview(handle);
		else if (ThumbnailCache::HasThumbnail(metadata->Type))
			DrawThumbnail(context, handle);
		if (metadata->Type == AssetType::Model && assets)
			DrawModelSummary(*assets, *metadata);
		if (metadata->Type == AssetType::AudioClip)
			DrawAudioPreview(handle);
		if (metadata->Type == AssetType::World && assets)
			DrawWorldSummary(*assets, *metadata);
		if (assets)
			DrawDetails(*assets, *metadata);
	}

	void AssetInspectorPanel::OnHidden(EditorPanelContext& context)
	{
		m_MaterialEditor.CommitPending(context.Context);
		m_AudioPreview.Stop();
	}

	void AssetInspectorPanel::DrawHeader(const AssetMetadata& metadata)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const ImGuiStyle& style = ImGui::GetStyle();

		// The type's icon on a tinted square, as tall as the title and the line under it.
		const float badge = std::round(ImGui::GetFrameHeight() * 1.75f);
		const ImVec2 min = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(badge, badge));
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(min, ImVec2(min.x + badge, min.y + badge), ImGui::GetColorU32(UI::WithAlpha(colors.Accent, 0.14f)), style.FrameRounding * 2.0f);
		drawList->AddRect(min, ImVec2(min.x + badge, min.y + badge), ImGui::GetColorU32(UI::WithAlpha(colors.Accent, 0.4f)), style.FrameRounding * 2.0f);
		const char* icon = UI::GetAssetTypeIcon(metadata.Type);
		UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Title);
		const ImVec2 iconSize = ImGui::CalcTextSize(icon);
		drawList->AddText(ImVec2(min.x + (badge - iconSize.x) * 0.5f, min.y + (badge - iconSize.y) * 0.5f), ImGui::GetColorU32(colors.Accent), icon);
		ImGui::PopFont();

		ImGui::SameLine();
		ImGui::BeginGroup();
		UI::Heading(metadata.Name.empty() ? metadata.Path : metadata.Name, UI::TextSize::Title);
		UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Caption);
		const std::string where = metadata.IsBuiltin() ? std::string("Built-in") : metadata.Path;
		const std::string subtitle = fmt::format("{} \xC2\xB7 {}", UI::GetAssetTypeDisplayName(metadata.Type), where);
		ImGui::TextColored(colors.TextSecondary, "%s", subtitle.c_str());
		ImGui::PopFont();
		ImGui::EndGroup();
		UI::ItemProbe::Record("AssetInspector.Header", true, 0, subtitle);

		const std::string id = UUIDToJson(metadata.Handle).get<std::string>();
		UI::PushFont(UI::EditorFont::Mono, UI::TextSize::Caption);
		ImGui::AlignTextToFramePadding();
		ImGui::TextColored(colors.TextSecondary, "Handle %s", id.c_str());
		ImGui::PopFont();
		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
		if (UI::IconButton("AssetInspector.CopyHandle", Icons::Copy, "Copy the handle (as commands and scripts take it)"))
			ImGui::SetClipboardText(id.c_str());
	}

	void AssetInspectorPanel::DrawActions(EditorPanelContext& panelContext, const AssetMetadata& metadata, const AssetImporter* importer)
	{
		EditorContext& context = panelContext.Context;
		const EditorCommandRegistry& commands = panelContext.Commands;
		const nlohmann::json asset = UUIDToJson(metadata.Handle);
		bool any = false;
		auto next = [&any]()
		{
			if (any)
				ImGui::SameLine();
			any = true;
		};
		if (metadata.Type == AssetType::Scene)
		{
			next();
			UI::ButtonStyle style;
			style.Enabled = metadata.Handle != context.GetSceneHandle() && !context.IsPlaying();
			const char* tooltip = style.Enabled ? "Open this scene for editing" : "The scene is open (or the editor is playing)";
			if (UI::ToolbarButton("AssetInspector.OpenScene", Icons::FolderOpen, "Open", tooltip, style))
			{
				// With a shell, the person has answered the question about unsaved changes by the time the scene opens; without one,
				// unsaved changes make the command fail.
				auto open = [&runner = panelContext.Runner, &context, &commands, asset](bool force)
				{
					RunEditorCommandDeferred(runner, context, commands, "scene.open", { { "scene", asset }, { "force", force } });
				};
				if (panelContext.Shell)
					panelContext.Shell->RequestDiscardChanges([open]() { open(true); });
				else
					open(false);
			}
		}
		if (metadata.Type == AssetType::World && context.GetAssetManager())
		{
			// Open edits the world: its persistent scene, with the cells around the camera.
			const Ref<WorldAsset> world = context.GetAssetManager()->GetAsset<WorldAsset>(metadata.Handle);
			const AssetHandle persistent = world ? world->GetData().PersistentScene : UUID::Null();
			next();
			UI::ButtonStyle openStyle;
			openStyle.Enabled = persistent.IsValid() && persistent != context.GetSceneHandle() && !context.IsPlaying();
			const char* openTooltip = !world ? "The world is loading" : !persistent.IsValid() ? "The world names no persistent scene"
				: openStyle.Enabled ? "Open the world for editing: its persistent scene, with the cells around the camera" : "The world is open (or the editor is playing)";
			if (UI::ToolbarButton("AssetInspector.OpenWorld", Icons::FolderOpen, "Open", openTooltip, openStyle))
			{
				auto open = [&runner = panelContext.Runner, &context, &commands, scene = UUIDToJson(persistent)](bool force)
				{
					RunEditorCommandDeferred(runner, context, commands, "scene.open", { { "scene", scene }, { "force", force } });
				};
				if (panelContext.Shell)
					panelContext.Shell->RequestDiscardChanges([open]() { open(true); });
				else
					open(false);
			}
			// Cook measures the changed cells off the frame (world.cook); with the world open it also sizes its physics.
			next();
			UI::ButtonStyle cookStyle;
			cookStyle.Enabled = !m_Cook->Running && !context.IsPlaying();
			if (UI::ToolbarButton("AssetInspector.CookWorld", Icons::Gauge, "Cook", "Measure the cells that changed (entities, bodies, bounds) into the "
				"world's index, and with the world open size its physics for the busiest place a source can stand (world.cook)", cookStyle))
			{
				m_Cook->Running = true;
				m_Cook->World = metadata.Handle;
				m_Cook->Outcome.clear();
				panelContext.Runner.Run(context, commands, "world.cook", { { "world", asset } }, [state = m_Cook](const EditorCommandResult& result)
				{
					state->Running = false;
					state->Failed = !result.Success;
					if (result.Success)
					{
						state->Outcome = fmt::format("Cooked {} cells ({} read again): at most {} bodies in reach, a capacity of {} recommended",
							result.Value.value("cells", 0), result.Value.value("cellsRead", 0), result.Value.value("maxBodiesInRadius", 0),
							result.Value.value("recommendedMaxBodies", 0));
					}
					else
					{
						state->Outcome = result.Error;
						ST_ERROR("Cook: {}", result.Error);
					}
				}, CommandOrigin { CommandSource::UI });
			}
		}
		if (metadata.Type == AssetType::Prefab || metadata.Type == AssetType::Model)
		{
			next();
			if (UI::ToolbarButton("AssetInspector.AddToScene", Icons::Plus, "Add to Scene", "Instantiate it at the top level of the scene, where the view looks"))
				EditorActions::InstantiatePrefab(context, commands, metadata.Handle);
		}
		if (importer && !metadata.IsSubAsset())
		{
			next();
			UI::ButtonStyle style;
			style.Enabled = !m_Apply->Running;
			if (UI::ToolbarButton("AssetInspector.Reimport", Icons::RotateCcw, "Reimport", "Import the file again (in the background)", style))
			{
				m_Apply->Running = true;
				panelContext.Runner.Run(context, commands, "asset.reimport", { { "asset", asset } }, [state = m_Apply](const EditorCommandResult& result)
				{
					state->Running = false;
					state->Error = result.Success ? std::string() : result.Error;
					if (!result.Success)
						ST_ERROR("Reimport: {}", result.Error);
				}, CommandOrigin { CommandSource::UI });
			}
		}
		if (any)
			ImGui::Spacing();
	}

	void AssetInspectorPanel::DrawImportSettings(EditorPanelContext& panelContext, EditorAssetManager& assets, const AssetMetadata& metadata, const AssetImporter& importer)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const ImGuiStyle& style = ImGui::GetStyle();
		EditorContext& context = panelContext.Context;

		// The draft follows the asset's settings while it has no edits of its own; an applied draft becomes the settings.
		const nlohmann::json current = assets.GetImportSettings(metadata.Handle);
		if (m_Draft.Asset != metadata.Handle || (m_Draft.Edited == m_Draft.Saved && m_Draft.Saved != current))
		{
			m_Draft = ImportDraft { metadata.Handle, current, current };
			m_Apply->Error.clear();
		}
		else if (m_Draft.Edited == current)
		{
			m_Draft.Saved = current;
		}

		if (!UI::SectionHeader("AssetInspector.ImportSettings", (std::string(Icons::Settings2) + "  Import Settings").c_str()))
			return;
		const bool importing = m_Apply->Running || assets.IsImportPending(metadata.Handle);
		if (UI::BeginPropertyTable("Settings"))
		{
			ImGui::BeginDisabled(importing);
			for (const PropertyInfo& property : importer.GetSettingsProperties())
			{
				if (property.IsHidden())
					continue;
				PropertyValue value = property.GetValue(&m_Draft.Edited);
				const std::string probe = "AssetInspector.Import." + property.Name;
				// A setting the other settings make moot (Quality of an uncompressed texture) is shown disabled, with why.
				const std::string inactive = importer.GetInactiveReason(property.Name, m_Draft.Edited);
				const std::string tooltip = inactive.empty() ? property.Tooltip : fmt::format("{}\n\nNo effect now: {}.", property.Tooltip, inactive);
				UI::PropertyLabel(property.DisplayName, tooltip, probe);
				PropertyWidgetOptions options;
				options.ProbeKey = probe;
				ImGui::BeginDisabled(!inactive.empty());
				if (DrawPropertyWidget(property, value, *context.GetActiveScene(), options).Changed)
				{
					std::string error;
					if (!property.SetValue(&m_Draft.Edited, value, &error))
						ST_WARN("{}: {}", property.Name, error);
				}
				ImGui::EndDisabled();
				UI::ItemProbe::Record(probe + ".Active", inactive.empty(), 0, inactive);
			}
			ImGui::EndDisabled();
			UI::EndPropertyTable();
		}

		// Revert and Apply at the right; what the import does at the left.
		const bool edited = m_Draft.Edited != m_Draft.Saved;
		ImGui::Spacing();
		if (importing)
		{
			ImGui::AlignTextToFramePadding();
			ImGui::TextColored(colors.Info, "%s  Importing...", Icons::LoaderCircle);
		}
		else if (!m_Apply->Error.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, colors.Error);
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x * 0.5f);
			ImGui::TextUnformatted(m_Apply->Error.c_str());
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
		}
		else if (edited)
		{
			ImGui::AlignTextToFramePadding();
			ImGui::TextColored(colors.Warning, "Not applied");
		}
		const float revertWidth = UI::DialogButtonWidth("Revert");
		const float applyWidth = UI::DialogButtonWidth("Apply");
		ImGui::SameLine();
		ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - revertWidth - applyWidth - style.ItemSpacing.x));
		if (UI::DialogButton("AssetInspector.Revert", "Revert", false, edited && !importing))
		{
			m_Draft.Edited = m_Draft.Saved;
			m_Apply->Error.clear();
		}
		ImGui::SameLine();
		if (UI::DialogButton("AssetInspector.Apply", "Apply", true, edited && !importing))
		{
			// Only the settings the draft changed (asset.setImportSettings merges them into the others): a setting the
			// importer does not describe, e.g. from a .meta of another engine version, stays as it is instead of making the
			// command refuse every Apply.
			nlohmann::json changes = nlohmann::json::object();
			for (const auto& [key, value] : m_Draft.Edited.items())
			{
				const auto saved = m_Draft.Saved.find(key);
				if (saved == m_Draft.Saved.end() || *saved != value)
					changes[key] = value;
			}
			m_Apply->Running = true;
			m_Apply->Error.clear();
			panelContext.Runner.Run(context, panelContext.Commands, "asset.setImportSettings",
				{ { "asset", UUIDToJson(metadata.Handle) }, { "settings", std::move(changes) } }, [state = m_Apply](const EditorCommandResult& result)
			{
				state->Running = false;
				state->Error = result.Success ? std::string() : result.Error;
				if (!result.Success)
					ST_ERROR("Import settings: {}", result.Error);
			}, CommandOrigin { CommandSource::UI });
		}
		ImGui::Spacing();
	}

	void AssetInspectorPanel::DrawTexturePreview(AssetHandle handle)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		if (!UI::SectionHeader("AssetInspector.Preview", (std::string(Icons::Image) + "  Preview").c_str()))
			return;
		// Requested every frame it is shown, like everything that draws assets (it may be evicted otherwise).
		const Ref<Texture> texture = AssetManager::GetAsset<Texture>(handle);
		if (!texture || texture->GetWidth() == 0 || texture->GetHeight() == 0)
		{
			ImGui::TextColored(colors.TextSecondary, "%s", AssetManager::GetAssetState(handle) == AssetState::Failed ? "The texture could not be loaded" : "Loading...");
			return;
		}
		const float width = std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight());
		const float aspect = static_cast<float>(texture->GetHeight()) / static_cast<float>(texture->GetWidth());
		const float height = std::min(width * aspect, ImGui::GetFontSize() * c_PreviewMaxHeightInFontSizes);
		const float shownWidth = height / aspect;
		// The mip level the preview's pixels need streams in like a view's would (TextureStreamer).
		if (TextureStreamer* streamer = TextureStreamer::Find(*AssetManager::GetActive()))
		{
			const float pixels = height * ImGui::GetIO().DisplayFramebufferScale.y;
			streamer->RequestLevel(handle, TextureStreamingPolicy::ComputeWantedLevel(texture->GetHeight(), texture->GetMipCount(), 1.0f, pixels));
		}
		const ImVec2 min(ImGui::GetCursorScreenPos().x + (width - shownWidth) * 0.5f, ImGui::GetCursorScreenPos().y);
		ImGui::Dummy(ImVec2(width, height));
		const ImVec2 max(min.x + shownWidth, min.y + height);
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(min, max, ImGui::GetColorU32(colors.Chrome));
		// As its content looks (sRGB, grayscale and two-channel normal maps each shown as materials read them), sampled the
		// way the texture is set to be (pixel art stays crisp).
		if (nvrhi::ITexture* gpuTexture = texture->GetGPUTexture())
			UI::TextureImage(drawList, static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(gpuTexture)), texture->GetSpecification(), min, max);
		drawList->AddRect(min, max, ImGui::GetColorU32(colors.Border));
		UI::PushFont(UI::EditorFont::Mono, UI::TextSize::Caption);
		const std::string caption = DescribeTexture(*texture);
		ImGui::TextColored(colors.TextSecondary, "%s", caption.c_str());
		UI::ItemProbe::Record("AssetInspector.Preview.Caption", true, 0, caption);
		ImGui::PopFont();
	}

	void AssetInspectorPanel::DrawThumbnail(EditorContext& context, AssetHandle handle)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		if (!UI::SectionHeader("AssetInspector.Preview", (std::string(Icons::Image) + "  Preview").c_str()))
			return;
		ThumbnailCache* thumbnails = context.GetThumbnails();
		const float width = std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight());
		const float side = std::round(std::min(width, ImGui::GetFontSize() * c_ThumbnailMaxSizeInFontSizes));
		// Rendered at the pixels it covers (the thumbnail shows until then).
		nvrhi::ITexture* thumbnail = thumbnails ? thumbnails->RequestPreview(handle, static_cast<uint32_t>(side)) : nullptr;
		const ImVec2 min(std::round(ImGui::GetCursorScreenPos().x + (width - side) * 0.5f), ImGui::GetCursorScreenPos().y);
		ImGui::Dummy(ImVec2(width, side));
		const ImVec2 max(min.x + side, min.y + side);
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(min, max, ImGui::GetColorU32(colors.Chrome));
		if (thumbnail)
		{
			// Display colors, drawn as they are (ImGuiTextureDisplay::Direct).
			drawList->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(thumbnail)), min, max);
		}
		else
		{
			const char* text = !thumbnails ? "No preview without a GPU"
				: thumbnails->GetState(handle) == ThumbnailState::Unavailable ? "No preview: it could not be rendered" : "Rendering the preview...";
			const ImVec2 size = ImGui::CalcTextSize(text);
			drawList->AddText(ImVec2(std::round((min.x + max.x - size.x) * 0.5f), std::round((min.y + max.y - size.y) * 0.5f)), ImGui::GetColorU32(colors.TextSecondary), text);
		}
		drawList->AddRect(min, max, ImGui::GetColorU32(colors.Border));
		UI::ItemProbe::RecordRect("AssetInspector.Thumbnail", min, max, thumbnail != nullptr, 0, thumbnail ? std::to_string(thumbnail->getDesc().width) : std::string());
	}

	void AssetInspectorPanel::DrawModelSummary(EditorAssetManager& assets, const AssetMetadata& metadata)
	{
		if (!UI::SectionHeader("AssetInspector.Model", (std::string(Icons::Boxes) + "  Model").c_str()))
			return;
		// The import's sub-assets by type; the meshes' triangles and levels of detail once they are loaded (requested while
		// the summary shows, like everything that draws assets).
		const AssetImportInfo import = assets.GetImportInfo(metadata.Handle);
		uint32_t meshes = 0;
		uint32_t materials = 0;
		uint32_t textures = 0;
		uint32_t skeletons = 0;
		uint32_t animations = 0;
		uint32_t others = 0;
		uint64_t triangles = 0;
		size_t levels = 0;
		uint32_t loading = 0;
		for (AssetHandle subAsset : import.SubAssets)
		{
			switch (assets.GetAssetType(subAsset))
			{
				case AssetType::Mesh:
				{
					meshes++;
					const Ref<Mesh> mesh = std::static_pointer_cast<Mesh>(assets.GetAsset(subAsset, AssetPriority::Low));
					if (!mesh)
					{
						loading += assets.GetAssetState(subAsset) == AssetState::Failed ? 0 : 1;
						break;
					}
					triangles += mesh->GetTriangleCount();
					for (const Submesh& submesh : mesh->GetSubmeshes())
						levels = std::max(levels, submesh.LODs.size());
					break;
				}
				case AssetType::Material:
					materials++;
					break;
				case AssetType::Texture:
					textures++;
					break;
				case AssetType::Skeleton:
					skeletons++;
					break;
				case AssetType::AnimationClip:
					animations++;
					break;
				default:
					others++;
					break;
			}
		}
		if (!UI::BeginPropertyTable("Model"))
			return;
		auto row = [](const char* label, const char* tooltip, const std::string& value, const char* probe)
		{
			UI::PropertyLabel(label, tooltip);
			ImGui::TextUnformatted(value.c_str());
			UI::ItemProbe::Record(probe, true, 0, value);
		};
		row("Meshes", "Meshes the import made (one per mesh of the file)", std::to_string(meshes), "AssetInspector.Model.Meshes");
		const std::string triangleText = loading > 0 ? fmt::format("{} ({} meshes loading)", triangles, loading) : std::to_string(triangles);
		row("Triangles", "Of every mesh at full detail", triangleText, "AssetInspector.Model.Triangles");
		row("Levels of Detail", "The most levels a mesh has, full detail included", std::to_string(std::max<size_t>(levels, meshes > 0 ? 1 : 0)),
			"AssetInspector.Model.Levels");
		row("Materials", "Materials the import made", std::to_string(materials), "AssetInspector.Model.Materials");
		row("Textures", "Images the file embeds or names", std::to_string(textures), "AssetInspector.Model.Textures");
		// Only models with skins or animations have these, so static models keep their short summary.
		if (skeletons > 0)
			row("Skeletons", "Joint hierarchies of the file's skins (an Animator's Skeleton)", std::to_string(skeletons), "AssetInspector.Model.Skeletons");
		if (animations > 0)
			row("Animations", "Clips of the file's animations (states of an animation controller play them)", std::to_string(animations),
				"AssetInspector.Model.Animations");
		if (others > 0)
			row("Other", "Further sub-assets", std::to_string(others), "AssetInspector.Model.Other");
		UI::EndPropertyTable();
	}

	void AssetInspectorPanel::DrawAudioPreview(AssetHandle handle)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		if (!UI::SectionHeader("AssetInspector.Audio", (std::string(Icons::AudioLines) + "  Preview").c_str()))
			return;
		// Requested every frame it is shown (it may be evicted otherwise).
		const Ref<AudioClipAsset> asset = AssetManager::GetAsset<AudioClipAsset>(handle);
		const Ref<AudioClip> clip = asset ? asset->GetClip() : nullptr;
		if (!clip)
		{
			ImGui::TextColored(colors.TextSecondary, "%s", AssetManager::GetAssetState(handle) == AssetState::Failed ? "The clip could not be loaded" : "Loading...");
			return;
		}

		const bool playing = m_AudioPreview.IsPlaying(handle);
		if (playing)
		{
			if (UI::ToolbarButton("AssetInspector.Audio.Stop", Icons::Square, "Stop", "Stop the preview"))
				m_AudioPreview.Stop();
		}
		else
		{
			UI::ButtonStyle style;
			style.Enabled = AudioEngine::IsInitialized();
			if (UI::ToolbarButton("AssetInspector.Audio.Play", Icons::Play, "Play", style.Enabled ? "Listen to the clip (not part of the scene)" : "No audio output", style))
				m_AudioPreview.Play(handle, clip);
		}
		ImGui::SameLine();
		UI::PushFont(UI::EditorFont::Mono, UI::TextSize::Caption);
		ImGui::AlignTextToFramePadding();
		const std::string channels = clip->GetChannels() == 1 ? std::string("mono") : clip->GetChannels() == 2 ? std::string("stereo")
			: fmt::format("{} channels", clip->GetChannels());
		const std::string time = playing ? fmt::format("{} / {}", FormatDuration(m_AudioPreview.GetPosition()), FormatDuration(clip->GetLength()))
			: fmt::format("{} \xC2\xB7 {} Hz \xC2\xB7 {}", FormatDuration(clip->GetLength()), clip->GetSampleRate(), channels);
		ImGui::TextColored(colors.TextSecondary, "%s", time.c_str());
		UI::ItemProbe::Record("AssetInspector.Audio.Time", true, 0, time);
		ImGui::PopFont();

		// The waveform, with the voice's position over it while it plays.
		const float width = std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight());
		const float height = std::round(ImGui::GetFontSize() * c_WaveformHeightInFontSizes);
		const ImVec2 min = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(width, height));
		const ImVec2 max(min.x + width, min.y + height);
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(min, max, ImGui::GetColorU32(colors.Chrome));
		const float middle = std::round((min.y + max.y) * 0.5f);
		if (const AudioWaveform* waveform = m_AudioPreview.RequestWaveform(handle, clip, c_WaveformColumns))
		{
			const float columnWidth = width / static_cast<float>(waveform->Min.size());
			const float halfHeight = height * 0.5f - 1.0f;
			const ImU32 color = ImGui::GetColorU32(colors.Accent);
			for (size_t column = 0; column < waveform->Min.size(); column++)
			{
				const float x = min.x + (static_cast<float>(column) + 0.5f) * columnWidth;
				drawList->AddLine(ImVec2(x, middle - waveform->Max[column] * halfHeight), ImVec2(x, middle - waveform->Min[column] * halfHeight + 1.0f), color,
					std::max(1.0f, columnWidth * 0.75f));
			}
			UI::ItemProbe::RecordRect("AssetInspector.Audio.Waveform", min, max, true, 0, std::to_string(waveform->Min.size()));
		}
		else
		{
			drawList->AddLine(ImVec2(min.x, middle), ImVec2(max.x, middle), ImGui::GetColorU32(colors.Border));
		}
		if (playing && clip->GetLength() > 0.0f)
		{
			const float x = std::round(min.x + width * std::clamp(m_AudioPreview.GetPosition() / clip->GetLength(), 0.0f, 1.0f));
			drawList->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), ImGui::GetColorU32(colors.Text));
		}
		drawList->AddRect(min, max, ImGui::GetColorU32(colors.Border));
	}

	void AssetInspectorPanel::DrawWorldSummary(EditorAssetManager& assets, const AssetMetadata& metadata)
	{
		if (!UI::SectionHeader("AssetInspector.WorldSection", (std::string(Icons::Earth) + "  World").c_str()))
			return;
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const Ref<WorldAsset> world = assets.GetAsset<WorldAsset>(metadata.Handle);
		if (!world)
		{
			ImGui::TextColored(colors.TextSecondary, "%s", assets.GetAssetState(metadata.Handle) == AssetState::Failed ? "The world cannot be loaded" : "Loading...");
			return;
		}
		const WorldData& data = world->GetData();
		ImGui::Text("%zu cells on %zu layers", data.Cells.size(), data.Layers.size());
		UI::ItemProbe::Record("AssetInspector.WorldCells");
		if (ImGui::BeginTable("AssetInspector.WorldLayers", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Layer");
			ImGui::TableSetupColumn("Cell");
			ImGui::TableSetupColumn("Streams within");
			ImGui::TableSetupColumn("Cells");
			ImGui::TableSetupColumn("At start");
			ImGui::TableHeadersRow();
			for (const WorldLayer& layer : data.Layers)
			{
				size_t cells = 0;
				for (const WorldCell& cell : data.Cells)
					cells += cell.Layer == layer.ID ? 1 : 0;
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextColored(layer.EnabledByDefault ? colors.Text : colors.TextSecondary, "%s", layer.Name.c_str());
				if (!layer.EnabledByDefault && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
					ImGui::SetTooltip("Off until a game enables it (World::SetLayerEnabled)");
				ImGui::TableNextColumn();
				ImGui::Text("%g m", static_cast<double>(layer.CellSize));
				ImGui::TableNextColumn();
				ImGui::Text("%g m", static_cast<double>(layer.StreamingDistance));
				ImGui::TableNextColumn();
				ImGui::Text("%zu", cells);
				ImGui::TableNextColumn();
				ImGui::TextColored(layer.LoadBeforeStart ? colors.Text : colors.TextSecondary, "%s", layer.LoadBeforeStart ? "Loaded" : "Streams");
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
				{
					ImGui::SetTooltip("%s", layer.LoadBeforeStart ? "A game starts once these cells around its sources are in, with their collision"
						: "These cells stream in after the game starts");
				}
			}
			ImGui::EndTable();
		}
		UI::ItemProbe::Record("AssetInspector.WorldLayers");
		ImGui::PushStyleColor(ImGuiCol_Text, colors.TextSecondary);
		if (data.Index)
		{
			ImGui::TextWrapped("Cooked: %zu cells measured, at most %llu bodies within any source's reach, a physics capacity of %u recommended",
				data.Index->Cells.size(), static_cast<unsigned long long>(data.Index->MaxBodiesInRadius), data.Index->GetRecommendedMaxBodies());
		}
		else
		{
			ImGui::TextWrapped("Not cooked yet: Cook measures the cells (exports cook every world)");
		}
		ImGui::PopStyleColor();
		UI::ItemProbe::Record("AssetInspector.WorldIndex");
		if (m_Cook->World == metadata.Handle && (m_Cook->Running || !m_Cook->Outcome.empty()))
		{
			ImGui::PushStyleColor(ImGuiCol_Text, m_Cook->Failed ? colors.Error : colors.TextSecondary);
			ImGui::TextWrapped("%s", m_Cook->Running ? "Cooking..." : m_Cook->Outcome.c_str());
			ImGui::PopStyleColor();
			UI::ItemProbe::Record("AssetInspector.WorldCook", true, 0, m_Cook->Running ? std::string_view("Cooking...") : std::string_view(m_Cook->Outcome));
		}
	}

	void AssetInspectorPanel::DrawDetails(EditorAssetManager& assets, const AssetMetadata& metadata)
	{
		if (!UI::SectionHeader("AssetInspector.Details", (std::string(Icons::Info) + "  Details").c_str(), ImGuiTreeNodeFlags_None))
			return;
		const AssetImportInfo import = assets.GetImportInfo(metadata.IsSubAsset() ? metadata.Parent : metadata.Handle);
		if (UI::BeginPropertyTable("Details"))
		{
			UI::PropertyLabel("State");
			ImGui::TextUnformatted(AssetStateToString(assets.GetAssetState(metadata.Handle)));
			UI::PropertyLabel("Stored Size", "What a load reads: the imported data, or the file for assets stored as they are");
			ImGui::TextUnformatted(metadata.StoredSize > 0 ? FormatBytes(metadata.StoredSize).c_str() : "Unknown");
			if (!import.SubAssets.empty())
			{
				UI::PropertyLabel("Sub-assets", "Meshes, materials and textures the import made");
				ImGui::Text("%zu", import.SubAssets.size());
			}
			UI::EndPropertyTable();
		}
	}

	void AssetInspectorPanel::DrawImportOutcome(EditorAssetManager& assets, const AssetMetadata& metadata)
	{
		const AssetImportInfo import = assets.GetImportInfo(metadata.IsSubAsset() ? metadata.Parent : metadata.Handle);
		if (import.Error.empty() && import.Warnings.empty())
			return;
		const UI::ThemeColors& colors = UI::GetThemeColors();
		ImGui::Spacing();
		ImGui::PushTextWrapPos(0.0f);
		if (!import.Error.empty())
		{
			const std::string text = fmt::format("{}  Import failed: {}", Icons::CircleX, import.Error);
			ImGui::TextColored(colors.Error, "%s", text.c_str());
			UI::ItemProbe::Record("AssetInspector.ImportError", true, 0, import.Error);
		}
		for (const std::string& warning : import.Warnings)
			ImGui::TextColored(colors.Warning, "%s  %s", Icons::TriangleAlert, warning.c_str());
		ImGui::PopTextWrapPos();
		ImGui::Spacing();
	}

}
