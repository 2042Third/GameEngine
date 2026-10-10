#include "UI/MaterialEditor.h"

#include "Editor/AssetEdit.h"
#include "Editor/EditorContext.h"
#include "UI/PropertyWidgets.h"
#include "UI/Theme.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Core/Log.h>
#include <Strata/Renderer/Material.h>

#include <imgui.h>

namespace Strata
{

	namespace
	{

		const PropertyInfo* FindMaterialProperty(std::string_view name)
		{
			for (const PropertyInfo& property : Material::GetPropertyInfos())
			{
				if (property.Name == name)
					return &property;
			}
			return nullptr;
		}

	}

	void MaterialEditor::Draw(EditorContext& context, AssetHandle material, std::string_view probeKey)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		// A drag of another material that never finished is saved before this one is edited.
		if (m_Pending && m_Pending->Material != material)
			CommitPending(context);
		if (IsBuiltinAssetHandle(material))
		{
			ImGui::PushStyleColor(ImGuiCol_Text, colors.TextSecondary);
			ImGui::TextWrapped("The built-in default material cannot be edited: give the mesh a material of the project (New Material).");
			ImGui::PopStyleColor();
			return;
		}
		if (!context.GetAssetManager())
		{
			ImGui::TextColored(colors.TextSecondary, "Open a project to edit its materials");
			return;
		}
		const Ref<Material> loaded = AssetManager::GetAsset<Material>(material);
		if (!loaded)
		{
			if (AssetManager::GetAssetState(material) == AssetState::Failed)
				ImGui::TextColored(colors.Error, "The material could not be loaded (see the Console)");
			else
				ImGui::TextColored(colors.TextSecondary, "Loading...");
			return;
		}

		// A material a model's import made is rewritten by every import: shown, not edited.
		const std::optional<AssetMetadata> metadata = AssetManager::HasActive() ? AssetManager::GetActive()->GetMetadata(material) : std::nullopt;
		const bool imported = metadata && metadata->IsSubAsset();
		if (imported)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, colors.TextSecondary);
			ImGui::TextWrapped("Imported with a model, which makes it again on every import: give the mesh a material of the project (New "
				"Material) to change it.");
			ImGui::PopStyleColor();
		}

		if (!UI::BeginPropertyTable("Material"))
			return;
		for (const PropertyInfo& property : Material::GetPropertyInfos())
		{
			if (property.IsHidden() || property.IsTransient())
				continue;
			PropertyValue value = property.GetValue(&loaded->GetProperties());
			UI::PropertyLabel(property.DisplayName, property.Tooltip);
			const std::string probe = fmt::format("{}.{}", probeKey, property.Name);
			PropertyWidgetOptions options;
			options.ProbeKey = probe;
			ImGui::BeginDisabled(property.IsReadOnly() || imported);
			const PropertyEditResult result = DrawPropertyWidget(property, value, *context.GetActiveScene(), options);
			ImGui::EndDisabled();

			std::string error;
			if (result.Changed && !result.Finished)
			{
				// While dragging: only the loaded material changes, so the viewport follows the value.
				if (AssetEdit::SetMaterialProperty(context, material, property, value, true, &error))
					m_Pending = PendingPreview { material, property.Name };
				else
					ST_WARN("{}: {}", property.Name, error);
			}
			if (result.Finished)
			{
				// The value as the widget left it (a drag's last frame may change nothing): saved, one undo step per edit.
				if (!AssetEdit::SetMaterialProperty(context, material, property, value, false, &error))
					ST_WARN("{}: {}", property.Name, error);
				context.GetUndoStack().BreakMerge();
				m_Pending.reset();
			}
		}
		UI::EndPropertyTable();
	}

	void MaterialEditor::CommitPending(EditorContext& context)
	{
		if (!m_Pending)
			return;
		const PendingPreview pending = *m_Pending;
		m_Pending.reset();
		const PropertyInfo* property = FindMaterialProperty(pending.Property);
		const Ref<Material> loaded = property && context.GetAssetManager() ? AssetManager::GetAsset<Material>(pending.Material) : nullptr;
		if (!loaded)
			return;
		std::string error;
		if (!AssetEdit::SetMaterialProperty(context, pending.Material, *property, property->GetValue(&loaded->GetProperties()), false, &error))
			ST_WARN("{}: {}", property->Name, error);
		context.GetUndoStack().BreakMerge();
	}

}
