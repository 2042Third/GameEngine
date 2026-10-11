#include "Panels/AssetInspectorPanel.h"

#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
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
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/Texture.h>
#include <Strata/Renderer/TextureStreamer.h>

#include <imgui.h>

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

		if (metadata->Type == AssetType::Material)
		{
			if (UI::SectionHeader("AssetInspector.MaterialSection", (std::string(Icons::Palette) + "  Material").c_str()))
				m_MaterialEditor.Draw(context, handle, "AssetInspector.Material");
		}
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
		if (assets)
			DrawDetails(*assets, *metadata);
	}

	void AssetInspectorPanel::OnHidden(EditorPanelContext& context)
	{
		m_MaterialEditor.CommitPending(context.Context);
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
				auto open = [&context, &commands, asset]() { RunEditorCommand(context, commands, "scene.open", { { "scene", asset } }); };
				if (panelContext.Shell)
					panelContext.Shell->RequestDiscardChanges(open);
				else
					open();
			}
		}
		if (metadata.Type == AssetType::Prefab || metadata.Type == AssetType::Model)
		{
			next();
			if (UI::ToolbarButton("AssetInspector.AddToScene", Icons::Plus, "Add to Scene", "Instantiate it at the top level of the scene"))
			{
				const nlohmann::json created = RunEditorCommand(context, commands, "prefab.instantiate", { { "prefab", asset } });
				const nlohmann::json* entities = created.is_object() ? JsonUtils::Find(created, "entities") : nullptr;
				if (entities && entities->is_array() && !entities->empty())
				{
					if (const std::optional<UUID> root = UUIDFromJson(entities->front()))
						context.Select(*root);
				}
			}
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
				});
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
			});
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

	void AssetInspectorPanel::DrawDetails(EditorAssetManager& assets, const AssetMetadata& metadata)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
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
		if (!import.Error.empty())
			ImGui::TextColored(colors.Error, "Import failed: %s", import.Error.c_str());
		for (const std::string& warning : import.Warnings)
			ImGui::TextColored(colors.Warning, "%s", warning.c_str());
	}

}
