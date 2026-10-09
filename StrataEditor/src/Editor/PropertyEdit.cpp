#include "Editor/PropertyEdit.h"

#include "Editor/EditorContext.h"

#include <Strata/Scene/ComponentAccess.h>

namespace Strata
{

	bool SetPropertyWithUndo(EditorContext& context, Entity entity, const ComponentInfo& component, const PropertyInfo& property, const PropertyValue& value,
		std::string* outError)
	{
		if (!entity)
		{
			if (outError)
				*outError = "The entity does not exist";
			return false;
		}
		const std::string mergeKey = fmt::format("Property/{}/{}/{}", entity.GetUUID().ToString(), component.Name, property.Name);
		SceneEditTransaction transaction(*entity.GetScene(), fmt::format("Edit {} {}", component.DisplayName, property.DisplayName), { entity.GetUUID() }, mergeKey);
		if (!ComponentAccess::SetProperty(entity, component, property, value, outError))
		{
			transaction.Rollback();
			return false;
		}
		context.CommitEdit(transaction);
		return true;
	}

}
