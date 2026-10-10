#include "Editor/HierarchyView.h"

#include <Strata/Core/StringUtils.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/SceneHierarchy.h>

#include <algorithm>
#include <cctype>

namespace Strata
{

	namespace
	{

		constexpr uint8_t c_MarkMatch = ST_BIT(0);         // The entity matches the filter
		constexpr uint8_t c_MarkShownDescendant = ST_BIT(1); // A descendant matches: the entity is shown as its ancestor

		char ToLowerChar(char character)
		{
			return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		}

		bool IsSpace(char character)
		{
			return std::isspace(static_cast<unsigned char>(character)) != 0;
		}

		// Whether `text` contains `lowered` (lower case), ignoring case, without allocating.
		bool ContainsLowered(std::string_view text, std::string_view lowered)
		{
			if (lowered.size() > text.size())
				return false;
			return std::search(text.begin(), text.end(), lowered.begin(), lowered.end(),
				[](char character, char wanted) { return ToLowerChar(character) == wanted; }) != text.end();
		}

		size_t GetSlot(entt::entity entity)
		{
			return static_cast<size_t>(entt::to_entity(entity));
		}

		// The first top-level entity's handle, or null for an empty scene.
		entt::entity GetFirstRoot(const Scene& scene)
		{
			const std::vector<UUID>& roots = scene.GetRootEntities();
			if (roots.empty())
				return entt::null;
			const Entity first = scene.GetEntityByUUID(roots.front());
			return first ? first.GetHandle() : entt::null;
		}

	}

	HierarchyFilter HierarchyFilter::Parse(std::string_view text)
	{
		HierarchyFilter filter;
		size_t position = 0;
		while (position < text.size())
		{
			while (position < text.size() && IsSpace(text[position]))
				position++;
			size_t end = position;
			while (end < text.size() && !IsSpace(text[end]))
				end++;
			if (end == position)
				break;
			std::string term = StringUtils::ToLower(text.substr(position, end - position));
			position = end;
			// A prefix alone ("c:" while it is being typed) asks for nothing yet.
			if (term.starts_with("c:"))
			{
				if (term.size() > 2)
					filter.Components.push_back(term.substr(2));
			}
			else if (term.starts_with("t:"))
			{
				if (term.size() > 2)
					filter.Tags.push_back(term.substr(2));
			}
			else
			{
				filter.Words.push_back(std::move(term));
			}
		}
		return filter;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Rows
	////////////////////////////////////////////////////////////////////////////////

	bool HierarchyView::Update(const Ref<Scene>& scene)
	{
		if (m_Scene.lock() != scene)
			Bind(scene);
		if (!scene)
		{
			if (m_Rows.empty())
				return false;
			m_Rows.clear();
			m_RowIndexValid = false;
			m_RebuildCount++;
			return true;
		}
		if (!m_Dirty && scene->GetHierarchyVersion() == m_HierarchyVersion)
			return false;
		Rebuild(*scene);
		return true;
	}

	HierarchyView::~HierarchyView()
	{
		Disconnect();
	}

	void HierarchyView::Bind(const Ref<Scene>& scene)
	{
		Disconnect();
		m_Scene = scene;
		m_Dirty = true;
		if (scene && IsFiltering())
			ConnectFilterSignals();
	}

	void HierarchyView::Disconnect()
	{
		// The scene owns the registry whose signals the connections belong to (m_Scene is what they were made with).
		if (!m_Scene.expired())
		{
			for (entt::connection& connection : m_Connections)
				connection.release();
		}
		m_Connections.clear();
	}

	void HierarchyView::ConnectFilterSignals()
	{
		Disconnect();
		const Ref<Scene> scene = m_Scene.lock();
		if (!scene)
			return;
		entt::registry& registry = scene->GetRegistry();
		auto observe = [this, &registry](const ComponentInfo* info, bool updates)
		{
			if (!info)
				return;
			for (const entt::connection& connection : info->Observe(registry, *this, updates))
				m_Connections.push_back(connection);
		};
		// Names and tags change by value; for "c:" only which entities have the component counts.
		if (!m_Filter.Words.empty())
			observe(ComponentRegistry::Find<NameComponent>(), true);
		if (!m_Filter.Tags.empty())
			observe(ComponentRegistry::Find<TagComponent>(), true);
		for (const ComponentInfo* info : m_FilterComponents)
			observe(info, false);
	}

	void HierarchyView::OnComponentChanged(entt::registry&, entt::entity)
	{
		m_Dirty = true;
	}

	void HierarchyView::Rebuild(const Scene& scene)
	{
		const size_t previous = m_Rows.size();
		m_Rows.clear();
		m_Rows.reserve(previous);
		if (IsFiltering())
			BuildFiltered(scene);
		else
			BuildTree(scene);
		m_HierarchyVersion = scene.GetHierarchyVersion();
		m_Dirty = false;
		m_RowIndexValid = false;
		m_RebuildCount++;
	}

	void HierarchyView::BuildTree(const Scene& scene)
	{
		// Depth first along the scene's sibling links, into expanded entities only: the cost is the visible rows.
		const entt::registry& registry = scene.GetRegistry();
		std::vector<entt::entity> resume; // Where to continue once a subtree is done: the next sibling of each expanded ancestor
		entt::entity current = GetFirstRoot(scene);
		while (current != entt::null)
		{
			const HierarchyComponent& hierarchy = registry.get<HierarchyComponent>(current);
			HierarchyRow row;
			row.Handle = current;
			row.ID = registry.get<IDComponent>(current).ID;
			row.Depth = hierarchy.Depth;
			const bool hasChildren = hierarchy.FirstChild != entt::null;
			const bool expanded = hasChildren && m_Expanded.contains(row.ID);
			if (hasChildren)
				row.Flags |= HierarchyRowFlags::HasChildren;
			if (expanded)
				row.Flags |= HierarchyRowFlags::Expanded;
			m_Rows.push_back(row);

			if (expanded)
			{
				resume.push_back(hierarchy.NextSibling);
				current = hierarchy.FirstChild;
				continue;
			}
			current = hierarchy.NextSibling;
			while (current == entt::null && !resume.empty())
			{
				current = resume.back();
				resume.pop_back();
			}
		}
	}

	void HierarchyView::BuildFiltered(const Scene& scene)
	{
		const entt::registry& registry = scene.GetRegistry();
		// The matches, then their ancestors: each ancestor is marked once (a walk stops at one marked before), so marking
		// is linear in the scene.
		std::vector<entt::entity> matches;
		size_t slots = 0;
		for (const entt::entity entity : registry.view<HierarchyComponent>())
		{
			slots = std::max(slots, GetSlot(entity) + 1);
			if (Matches(registry, entity))
				matches.push_back(entity);
		}
		m_Marks.assign(slots, 0);
		for (const entt::entity match : matches)
		{
			m_Marks[GetSlot(match)] |= c_MarkMatch;
			for (entt::entity parent = registry.get<HierarchyComponent>(match).Parent; parent != entt::null;
				parent = registry.get<HierarchyComponent>(parent).Parent)
			{
				uint8_t& mark = m_Marks[GetSlot(parent)];
				if (mark & c_MarkShownDescendant)
					break;
				mark |= c_MarkShownDescendant;
			}
		}

		// Depth first over the marked entities, in hierarchy order; ancestors of matches are expanded to show them.
		std::vector<entt::entity> resume;
		entt::entity current = matches.empty() ? entt::null : GetFirstRoot(scene);
		while (current != entt::null)
		{
			const HierarchyComponent& hierarchy = registry.get<HierarchyComponent>(current);
			const uint8_t mark = m_Marks[GetSlot(current)];
			if (mark != 0)
			{
				HierarchyRow row;
				row.Handle = current;
				row.ID = registry.get<IDComponent>(current).ID;
				row.Depth = hierarchy.Depth;
				if (hierarchy.FirstChild != entt::null)
					row.Flags |= HierarchyRowFlags::HasChildren;
				if (mark & c_MarkShownDescendant)
					row.Flags |= HierarchyRowFlags::Expanded;
				if (mark & c_MarkMatch)
					row.Flags |= HierarchyRowFlags::Match;
				m_Rows.push_back(row);
				if (mark & c_MarkShownDescendant)
				{
					resume.push_back(hierarchy.NextSibling);
					current = hierarchy.FirstChild;
					continue;
				}
			}
			current = hierarchy.NextSibling;
			while (current == entt::null && !resume.empty())
			{
				current = resume.back();
				resume.pop_back();
			}
		}
	}

	bool HierarchyView::Matches(const entt::registry& registry, entt::entity entity) const
	{
		if (!m_Filter.Words.empty())
		{
			const NameComponent* name = registry.try_get<NameComponent>(entity);
			const std::string_view text = name ? std::string_view(name->Name) : std::string_view();
			for (const std::string& word : m_Filter.Words)
			{
				if (!ContainsLowered(text, word))
					return false;
			}
		}
		if (!m_Filter.Tags.empty())
		{
			const TagComponent* tag = registry.try_get<TagComponent>(entity);
			if (!tag)
				return false;
			for (const std::string& term : m_Filter.Tags)
			{
				if (!ContainsLowered(tag->Tag, term))
					return false;
			}
		}
		if (!m_Filter.Components.empty())
		{
			// A term that names no component matches nothing.
			if (!m_UnknownComponents.empty())
				return false;
			for (size_t term = 0; term < m_Filter.Components.size(); term++)
			{
				bool has = false;
				for (size_t index = 0; index < m_FilterComponents.size() && !has; index++)
					has = m_FilterComponentTerms[index] == term && m_FilterComponents[index]->Has(registry, entity);
				if (!has)
					return false;
			}
		}
		return true;
	}

	std::optional<size_t> HierarchyView::FindRow(UUID entity) const
	{
		if (!m_RowIndexValid)
		{
			m_RowIndex.clear();
			m_RowIndex.reserve(m_Rows.size());
			for (size_t index = 0; index < m_Rows.size(); index++)
				m_RowIndex.emplace(m_Rows[index].ID, index);
			m_RowIndexValid = true;
		}
		const auto it = m_RowIndex.find(entity);
		return it != m_RowIndex.end() ? std::optional<size_t>(it->second) : std::nullopt;
	}

	std::vector<UUID> HierarchyView::GetRowRange(size_t first, size_t last) const
	{
		std::vector<UUID> entities;
		if (m_Rows.empty())
			return entities;
		const size_t begin = std::min(std::min(first, last), m_Rows.size() - 1);
		const size_t end = std::min(std::max(first, last), m_Rows.size() - 1);
		entities.reserve(end - begin + 1);
		for (size_t index = begin; index <= end; index++)
			entities.push_back(m_Rows[index].ID);
		return entities;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Expansion and filter
	////////////////////////////////////////////////////////////////////////////////

	void HierarchyView::SetExpanded(UUID entity, bool expanded)
	{
		const bool changed = expanded ? m_Expanded.insert(entity).second : m_Expanded.erase(entity) > 0;
		// While filtering, the rows show the matches whatever is expanded.
		if (changed && !IsFiltering())
			m_Dirty = true;
	}

	void HierarchyView::SetSubtreeExpanded(const Scene& scene, UUID entity, bool expanded)
	{
		const Entity root = scene.GetEntityByUUID(entity);
		if (!root)
			return;
		const entt::registry& registry = scene.GetRegistry();
		std::vector<entt::entity> stack = { root.GetHandle() };
		while (!stack.empty())
		{
			const entt::entity current = stack.back();
			stack.pop_back();
			const HierarchyComponent& hierarchy = registry.get<HierarchyComponent>(current);
			if (hierarchy.FirstChild == entt::null)
				continue; // Leaves need no expansion state
			SetExpanded(registry.get<IDComponent>(current).ID, expanded);
			for (entt::entity child = hierarchy.FirstChild; child != entt::null; child = registry.get<HierarchyComponent>(child).NextSibling)
				stack.push_back(child);
		}
	}

	void HierarchyView::CollapseAll()
	{
		if (m_Expanded.empty())
			return;
		m_Expanded.clear();
		if (!IsFiltering())
			m_Dirty = true;
	}

	void HierarchyView::SetFilter(std::string_view text)
	{
		if (text == m_FilterText)
			return;
		m_FilterText = text;
		m_Filter = HierarchyFilter::Parse(text);
		m_FilterComponents.clear();
		m_FilterComponentTerms.clear();
		m_UnknownComponents.clear();
		for (size_t term = 0; term < m_Filter.Components.size(); term++)
		{
			bool known = false;
			for (const ComponentInfo* info : ComponentRegistry::GetAll())
			{
				if (info->IsHidden() || HasFlag(info->Flags, ComponentFlags::NoSerialize))
					continue;
				if (ContainsLowered(info->Name, m_Filter.Components[term]) || ContainsLowered(info->DisplayName, m_Filter.Components[term]))
				{
					m_FilterComponents.push_back(info);
					m_FilterComponentTerms.push_back(term);
					known = true;
				}
			}
			if (!known)
				m_UnknownComponents.push_back(m_Filter.Components[term]);
		}
		Disconnect();
		if (IsFiltering())
			ConnectFilterSignals();
		m_Dirty = true;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Reveal and drops
	////////////////////////////////////////////////////////////////////////////////

	HierarchyView::RevealResult HierarchyView::Reveal(const Ref<Scene>& scene, UUID entity)
	{
		RevealResult result;
		const Entity target = scene ? scene->GetEntityByUUID(entity) : Entity();
		if (!target)
			return result;
		if (!IsFiltering())
		{
			const entt::registry& registry = scene->GetRegistry();
			std::vector<UUID> ancestors;
			for (entt::entity parent = registry.get<HierarchyComponent>(target.GetHandle()).Parent; parent != entt::null;
				parent = registry.get<HierarchyComponent>(parent).Parent)
			{
				ancestors.push_back(registry.get<IDComponent>(parent).ID);
			}
			for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it)
			{
				if (m_Expanded.insert(*it).second)
				{
					result.Expanded.push_back(*it);
					m_Dirty = true;
				}
			}
		}
		Update(scene);
		result.Row = FindRow(entity);
		return result;
	}

	HierarchyDropZone HierarchyView::GetDropZone(float fraction)
	{
		if (fraction < 0.25f)
			return HierarchyDropZone::Before;
		if (fraction > 0.75f)
			return HierarchyDropZone::After;
		return HierarchyDropZone::Inside;
	}

	std::optional<HierarchyDropTarget> HierarchyView::GetDropTarget(const Scene& scene, size_t row, float fraction, std::span<const UUID> dragged) const
	{
		if (row >= m_Rows.size())
			return std::nullopt;
		const Entity target = scene.GetEntityByUUID(m_Rows[row].ID);
		if (!target)
			return std::nullopt;

		HierarchyDropTarget result;
		result.Zone = GetDropZone(fraction);
		Entity parent;
		size_t insertion = 0; // Position in the parent's list before the dragged entities leave it
		const bool showsChildren = m_Rows[row].IsExpanded() && row + 1 < m_Rows.size() && m_Rows[row + 1].Depth > m_Rows[row].Depth;
		if (result.Zone == HierarchyDropZone::Inside)
		{
			parent = target;
			insertion = scene.GetRegistry().get<HierarchyComponent>(target.GetHandle()).ChildCount;
		}
		else if (result.Zone == HierarchyDropZone::After && showsChildren)
		{
			// The line after an expanded entity lies above its first shown child: the entities go in front of that child.
			parent = target;
			const Entity next = scene.GetEntityByUUID(m_Rows[row + 1].ID);
			insertion = next ? scene.GetSiblingIndex(next) : 0;
		}
		else
		{
			parent = target.GetParent();
			insertion = scene.GetSiblingIndex(target) + (result.Zone == HierarchyDropZone::After ? 1 : 0);
		}

		// The entities that move: none may become its own ancestor, and those that leave the list in front of the
		// insertion point move it forward.
		const std::vector<UUID> moving = GetTopmostInHierarchyOrder(scene, dragged);
		size_t leaving = 0;
		for (const UUID id : moving)
		{
			const Entity entity = scene.GetEntityByUUID(id);
			if (parent && (parent == entity || scene.IsDescendantOf(parent, entity)))
				return std::nullopt;
			if (entity.GetParent() == parent && scene.GetSiblingIndex(entity) < insertion)
				leaving++;
		}
		result.Parent = parent ? parent.GetUUID() : UUID::Null();
		result.Index = insertion - leaving;
		return result;
	}

	std::vector<UUID> HierarchyView::GetTopmostInHierarchyOrder(const Scene& scene, std::span<const UUID> entities)
	{
		std::unordered_set<UUID> listed;
		for (const UUID id : entities)
		{
			if (scene.GetEntityByUUID(id))
				listed.insert(id);
		}
		std::vector<Entity> topmost;
		std::unordered_set<UUID> seen;
		for (const UUID id : entities)
		{
			if (!listed.contains(id) || !seen.insert(id).second)
				continue;
			const Entity entity = scene.GetEntityByUUID(id);
			bool nested = false;
			for (Entity ancestor = entity.GetParent(); ancestor && !nested; ancestor = ancestor.GetParent())
				nested = listed.contains(ancestor.GetUUID());
			if (!nested)
				topmost.push_back(entity);
		}
		std::sort(topmost.begin(), topmost.end(), [&scene](Entity a, Entity b) { return scene.CompareHierarchyOrder(a, b) < 0; });
		std::vector<UUID> ids;
		ids.reserve(topmost.size());
		for (const Entity entity : topmost)
			ids.push_back(entity.GetUUID());
		return ids;
	}

}
