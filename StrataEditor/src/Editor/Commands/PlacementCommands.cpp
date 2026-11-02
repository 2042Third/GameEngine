#include "Editor/AssetBounds.h"
#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorPlacement.h"
#include "Editor/EditorViewport.h"
#include "Editor/SceneEdit.h"
#include "Editor/SceneRaycast.h"
#include "Editor/ViewportRenderer.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Math/Math.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Prefab.h>
#include <Strata/Scene/Scene.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <chrono>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// scene.raycast and asset.place: what a ray or a pixel of the Scene view hits, and placing assets where it lands
// (EditorPlacement::Place), which the Scene view's drop runs too.

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		constexpr int64_t c_MaxViewportPixels = 16384;
		constexpr float c_MaxPlaceScale = 1000.0f;
		// asset.place waits this long for a prefab or model to load (an import may run first), then fails.
		constexpr double c_PlaceWaitSeconds = 30.0;
		// Then this long for what its size needs (asset.info's give-up, by the clock), then places by the size known so far.
		constexpr double c_PlaceBoundsSeconds = 5.0;

		nlohmann::json PositiveNumberSchema(std::string description, std::optional<double> maximum = std::nullopt)
		{
			nlohmann::json schema = NumberSchema(std::move(description));
			schema["exclusiveMinimum"] = 0;
			if (maximum)
				schema["maximum"] = *maximum;
			return schema;
		}

		nlohmann::json ViewportPointSchema(std::string description)
		{
			nlohmann::json schema = ObjectSchema({
				{ "x", NumberSchema("Pixels from the image's left edge, continuous: the middle of the first column is 0.5") },
				{ "y", NumberSchema("Pixels from the image's top edge, continuous") },
				{ "width", IntegerSchema("The width of the image the pixel refers to (default: the Scene view's, or 1280 while it has none)", 1, c_MaxViewportPixels) },
				{ "height", IntegerSchema("The height of that image (default: the Scene view's, or 720)", 1, c_MaxViewportPixels) } }, { "x", "y" });
			schema["description"] = std::move(description);
			return schema;
		}

		nlohmann::json RaySchema(std::string description)
		{
			nlohmann::json schema = ObjectSchema({
				{ "origin", Vec3Schema("Where the ray starts, in world space") },
				{ "direction", Vec3Schema("Where it points; any length but zero") } }, { "origin", "direction" });
			schema["description"] = std::move(description);
			return schema;
		}

		nlohmann::json ToJson(const glm::vec3& value)
		{
			return nlohmann::json::array({ value.x, value.y, value.z });
		}

		nlohmann::json DescribeRay(const Ray& ray)
		{
			return { { "origin", ToJson(ray.Origin) }, { "direction", ToJson(ray.Direction) } };
		}

		// The members of an object parameter are among `allowed`: the registry checks only the top level's names.
		bool CheckMembers(const nlohmann::json& object, std::string_view name, std::initializer_list<std::string_view> allowed, std::string* outError)
		{
			if (!object.is_object())
			{
				*outError = fmt::format("'{}' must be an object", name);
				return false;
			}
			for (const auto& member : object.items())
			{
				bool known = false;
				for (const std::string_view allowedName : allowed)
					known = known || member.key() == allowedName;
				if (!known)
				{
					*outError = fmt::format("'{}' has no member '{}'", name, member.key());
					return false;
				}
			}
			return true;
		}

		// An optional number parameter within (or from) `minimum` to `maximum`: absent leaves `value` as it is.
		bool ReadNumber(const nlohmann::json& parameters, std::string_view name, double minimum, bool exclusiveMinimum, double maximum, float& value,
			std::string* outError)
		{
			const auto found = parameters.find(name);
			if (found == parameters.end() || found->is_null())
				return true;
			if (!found->is_number() || !std::isfinite(found->get<double>()))
			{
				*outError = fmt::format("'{}' must be a number", name);
				return false;
			}
			const double number = found->get<double>();
			if ((exclusiveMinimum ? number <= minimum : number < minimum) || number > maximum)
			{
				*outError = fmt::format("'{}' must be {} {} and at most {}", name, exclusiveMinimum ? "above" : "at least", minimum, maximum);
				return false;
			}
			value = static_cast<float>(number);
			return true;
		}

		// A direction or normal: three finite numbers, not all (nearly) zero.
		std::optional<glm::vec3> ReadDirection(const nlohmann::json& value, std::string_view name, std::string* outError)
		{
			const std::optional<glm::vec3> direction = ReadVec3(value);
			if (!direction)
			{
				*outError = fmt::format("'{}' must be three finite numbers", name);
				return std::nullopt;
			}
			const float length = glm::length(*direction);
			if (!std::isfinite(length) || length <= 1.0e-6f)
			{
				*outError = fmt::format("'{}' must not be zero", name);
				return std::nullopt;
			}
			return direction;
		}

		// ray {origin, direction}: its origin and direction as given (the direction not normalized).
		std::optional<std::pair<glm::vec3, glm::vec3>> ReadRay(const nlohmann::json& value, std::string* outError)
		{
			if (!CheckMembers(value, "ray", { "origin", "direction" }, outError))
				return std::nullopt;
			if (!value.contains("origin") || !value.contains("direction"))
			{
				*outError = "'ray' needs origin and direction";
				return std::nullopt;
			}
			const std::optional<glm::vec3> origin = ReadVec3(value["origin"]);
			if (!origin)
			{
				*outError = "'ray.origin' must be three finite numbers";
				return std::nullopt;
			}
			const std::optional<glm::vec3> direction = ReadDirection(value["direction"], "ray.direction", outError);
			if (!direction)
				return std::nullopt;
			return std::make_pair(*origin, *direction);
		}

		struct ViewRay
		{
			Ray CastRay;
			float Far = 0.0f;       // The view camera's far plane
			bool FromScene = false; // Through the scene's camera (Game camera), not the editor camera
		};

		// viewport {x, y, width?, height?}: the ray through that pixel of the Scene view's image, through its camera.
		std::optional<ViewRay> ReadViewportRay(EditorContext& context, const nlohmann::json& value, std::string* outError)
		{
			if (!CheckMembers(value, "viewport", { "x", "y", "width", "height" }, outError))
				return std::nullopt;
			const glm::uvec2 sceneView = context.GetViewport().GetSize();
			const bool hasSize = sceneView.x > 0 && sceneView.y > 0;
			int64_t size[2] = { hasSize ? sceneView.x : EditorViewport::c_DefaultWidth, hasSize ? sceneView.y : EditorViewport::c_DefaultHeight };
			const char* sizeNames[2] = { "width", "height" };
			for (int axis = 0; axis < 2; axis++)
			{
				const auto found = value.find(sizeNames[axis]);
				if (found == value.end())
					continue;
				if (!found->is_number_integer() || found->get<int64_t>() < 1 || found->get<int64_t>() > c_MaxViewportPixels)
				{
					*outError = fmt::format("'viewport.{}' must be an integer from 1 to {}", sizeNames[axis], c_MaxViewportPixels);
					return std::nullopt;
				}
				size[axis] = found->get<int64_t>();
			}
			float pixel[2] = { 0.0f, 0.0f };
			const char* pixelNames[2] = { "x", "y" };
			for (int axis = 0; axis < 2; axis++)
			{
				const auto found = value.find(pixelNames[axis]);
				if (found == value.end() || !found->is_number() || !std::isfinite(found->get<double>()))
				{
					*outError = fmt::format("'viewport.{}' must be a number", pixelNames[axis]);
					return std::nullopt;
				}
				const double coordinate = found->get<double>();
				if (coordinate < 0.0 || coordinate > static_cast<double>(size[axis]))
				{
					*outError = fmt::format("'viewport.{}' ({}) lies outside the image (0 to {})", pixelNames[axis], coordinate, size[axis]);
					return std::nullopt;
				}
				pixel[axis] = static_cast<float>(coordinate);
			}
			const float width = static_cast<float>(size[0]);
			const float height = static_cast<float>(size[1]);
			const std::optional<ViewportView> view = ResolveSceneView(context, width / height);
			if (!view)
			{
				*outError = "The Scene view has no camera to cast through";
				return std::nullopt;
			}
			// The panel's own mapping: NDC +Y is up, the image's rows go down.
			const glm::vec2 ndc(pixel[0] / width * 2.0f - 1.0f, 1.0f - pixel[1] / height * 2.0f);
			return ViewRay { EditorPlacement::MakeViewRay(view->Camera, ndc), view->Camera.Far, view->FromScene };
		}

		// The display name of a mesh asset, for the entity made from it.
		std::string GetMeshName(const EditorAssetManager& assets, AssetHandle mesh)
		{
			const std::optional<AssetMetadata> metadata = assets.GetMetadata(mesh);
			return metadata && !metadata->Name.empty() ? metadata->Name : std::string("Mesh");
		}

		// What asset.place waits on while its asset loads: copies of everything, so its poll owns its data.
		struct PlaceJob
		{
			EditorPlacement::PlacementRequest Request;
			AssetHandle Asset;
			AssetType Type = AssetType::None;
			UUID Parent = UUID::Null();
			std::string Reference; // The asset as the call named it, for messages
			std::weak_ptr<Scene> TargetScene;
			std::chrono::steady_clock::time_point Start;
			std::optional<std::chrono::steady_clock::time_point> TemplateResident;
		};

		// Places the job's asset once it is ready: a result, or nullopt while it waits (requesting the asset again).
		std::optional<EditorCommandResult> TryPlace(EditorContext& context, PlaceJob& job)
		{
			EditorAssetManager* assets = context.GetAssetManager();
			if (!assets)
				return EditorCommandResult::Fail(fmt::format("The project was closed before '{}' was placed", job.Reference), EditorCommandError::Cancelled);
			const Ref<Scene> scene = job.TargetScene.lock();
			if (!scene || scene != context.GetActiveScene())
			{
				return EditorCommandResult::Fail(fmt::format("The active scene changed (play started or stopped, or another scene opened) before '{}' was ready: "
					"nothing was placed", job.Reference));
			}
			if (!context.CheckSceneEditable())
				return EditorCommandResult::Fail("The scene is being opened: nothing was placed");
			if (assets->GetAssetState(job.Asset) == AssetState::Failed)
			{
				return EditorCommandResult::Fail(fmt::format("'{}' could not be loaded: {}: nothing was placed", job.Reference, assets->GetAssetError(job.Asset)));
			}

			const auto now = std::chrono::steady_clock::now();
			Ref<EntityTemplate> entityTemplate;
			if (job.Type != AssetType::Mesh)
			{
				// Requested every frame it waits, so that it stays the most recently requested and is not evicted.
				assets->RequestLoad(job.Asset);
				entityTemplate = std::dynamic_pointer_cast<EntityTemplate>(assets->PeekLoadedAsset(job.Asset));
				if (!entityTemplate)
				{
					if (now - job.Start >= std::chrono::duration<double>(c_PlaceWaitSeconds))
						return EditorCommandResult::Fail(fmt::format("'{}' did not load within {} s (still importing?)", job.Reference, c_PlaceWaitSeconds));
					return std::nullopt;
				}
				if (!job.TemplateResident)
					job.TemplateResident = now;
			}
			const AssetBounds::Result measured = context.GetAssetBounds().Measure(*assets, job.Asset);
			if (measured.Pending && now - job.TemplateResident.value_or(job.Start) < std::chrono::duration<double>(c_PlaceBoundsSeconds))
				return std::nullopt;

			Scene& target = *scene;
			Entity parent;
			if (job.Parent.IsValid())
			{
				parent = target.GetEntityByUUID(job.Parent);
				if (!parent)
					return EditorCommandResult::Fail(fmt::format("The parent was deleted before '{}' was ready: nothing was placed", job.Reference));
			}
			const std::optional<EditorPlacement::Placement> placement = EditorPlacement::Place(target, job.Request, measured.Bounds);
			if (!placement)
			{
				if (job.Request.MaxDistance >= std::numeric_limits<float>::max())
					return EditorCommandResult::Fail("Nothing under the ray: nothing was placed");
				return EditorCommandResult::Fail(fmt::format("Nothing under the ray within {} m: nothing was placed", job.Request.MaxDistance));
			}

			SceneEditTransaction transaction(target, "Place Asset", {});
			std::vector<Entity> roots;
			if (entityTemplate)
			{
				std::string error;
				roots = InstantiateTemplate(context, target, transaction, *entityTemplate, job.Asset, parent, &error);
				if (!error.empty())
					return RollBackEdit(transaction, fmt::format("Instantiating '{}' failed: {}", job.Reference, error), EditorCommandError::Failed);
				if (roots.empty())
					return RollBackEdit(transaction, fmt::format("'{}' holds no entity to place", job.Reference), EditorCommandError::Failed);
			}
			else
			{
				const std::string name = GetMeshName(*assets, job.Asset);
				const Entity entity = parent ? target.CreateChildEntity(parent, name) : target.CreateEntity(name);
				if (!entity)
					return RollBackEdit(transaction, fmt::format("The scene is full: it holds at most {} entities", Scene::c_MaxEntities), EditorCommandError::Failed);
				transaction.TrackCreated(entity.GetUUID());
				std::string error;
				if (!ApplyComponents(entity, { { "MeshRenderer", { { "Mesh", UUIDToJson(job.Asset) } } } }, &error))
					return RollBackEdit(transaction, error, EditorCommandError::Failed);
				roots.push_back(entity);
			}

			// The first root where Place put it, turned and scaled over its own; the others move rigidly with it, so that the
			// bounds measured around the first root's origin hold for all of them.
			Entity root = roots.front();
			std::vector<glm::mat4> asMade;
			asMade.reserve(roots.size());
			for (const Entity made : roots)
				asMade.push_back(target.GetWorldTransform(made));
			TransformComponent& transform = root.GetComponent<TransformComponent>();
			const glm::quat rotation = job.Request.Rotation * transform.Rotation;
			const glm::vec3 scale = job.Request.Scale * transform.Scale;
			if (!parent)
			{
				// No decomposition: a placement without rotation or scale keeps the root's own values bit for bit.
				transform.Translation = placement->Translation;
				transform.Rotation = rotation;
				transform.Scale = scale;
				root.MarkModified<TransformComponent>();
			}
			else
			{
				const glm::mat4 world = glm::translate(glm::mat4(1.0f), placement->Translation) * glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.0f), scale);
				if (!target.SetWorldTransform(root, world))
					return RollBackEdit(transaction, "The parent's transform cannot hold the placed transform", EditorCommandError::Failed);
			}
			if (roots.size() > 1)
			{
				const glm::mat4 moved = target.GetWorldTransform(root) * glm::inverse(asMade.front());
				for (size_t index = 1; index < roots.size(); index++)
				{
					if (!target.SetWorldTransform(roots[index], moved * asMade[index]))
						return RollBackEdit(transaction, "The parent's transform cannot hold the placed transform", EditorCommandError::Failed);
				}
			}

			SceneEdit::NameNumbers numbers;
			nlohmann::json ids = nlohmann::json::array();
			for (const Entity placed : roots)
			{
				SceneEdit::MakeNameUnique(target, placed, &numbers);
				ids.push_back(UUIDToJson(placed.GetUUID()));
			}
			transaction.SetName(fmt::format("Place {}", SceneEdit::DescribeEntities(roots)));
			const AABB& placedBounds = placement->PlacedBounds;
			const AABB worldBounds = placedBounds.IsValid() ? AABB(placedBounds.Min + placement->Translation, placedBounds.Max + placement->Translation) : AABB();
			nlohmann::json result = {
				{ "id", UUIDToJson(root.GetUUID()) },
				{ "entities", std::move(ids) },
				{ "placedOn", EditorPlacement::PlacementSourceToString(placement->Source) },
				{ "hitEntity", placement->HitEntity ? UUIDToJson(placement->HitEntity.GetUUID()) : nlohmann::json(nullptr) },
				{ "point", ToJson(placement->RestingOn.Point) },
				{ "normal", ToJson(placement->RestingOn.Normal) },
				{ "translation", ToJson(placement->Translation) },
				{ "bounds", DescribeBounds(worldBounds, measured.Pending) },
				{ "snapped", placement->Snapped } };
			return FinishEdit(context, transaction, std::move(result));
		}

	}

	void RegisterPlacementCommands(EditorCommandRegistry& registry)
	{
		registry.Register({ "scene.raycast",
			"What a ray, or a pixel of the Scene view, hits among what the active scene draws: its mesh renderers, on their triangles (their bounds "
			"while a mesh loads). Give origin and direction, or viewport {x, y, width?, height?} (a pixel of the Scene view's image, through its "
			"camera: the editor camera, or the scene's primary camera with Game camera on). maxDistance limits the ray (default: unlimited for "
			"origin and direction, the view's far plane for a pixel); ignore leaves entities out with their descendants. Returns {hit: true, entity, "
			"name, point, normal (facing the ray's origin), distance, onTriangle (false: a loading mesh's bounds were hit), ray: {origin, direction}} "
			"or {hit: false, ray}; ray is what asset.place {ray} takes. Colliders, gizmos and UI are not hit. While playing it casts into the running "
			"game.",
			ObjectSchema({
				{ "origin", Vec3Schema("Where the ray starts, in world space (with direction)") },
				{ "direction", Vec3Schema("Where the ray points, any length but zero (with origin)") },
				{ "viewport", ViewportPointSchema("A pixel of the Scene view's image, cast through its camera (instead of origin and direction)") },
				{ "maxDistance", PositiveNumberSchema("How far along the ray to look, in meters (default: unlimited for a ray, the view's far plane for a pixel)") },
				{ "ignore", EntityArraySchema("Entities left out, with their descendants") } }),
			[](EditorContext& context, const nlohmann::json& parameters) -> EditorCommandResult
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				std::vector<Entity> ignore;
				if (arguments.Has("ignore"))
					ignore = arguments.GetEntities(scene, "ignore");
				if (!arguments.IsValid())
					return arguments.Fail();
				const bool hasRay = arguments.Has("origin") || arguments.Has("direction");
				const bool hasViewport = arguments.Has("viewport");
				if (hasRay == hasViewport)
					return EditorCommandResult::InvalidParameters("Give either origin and direction, or viewport");

				std::string error;
				Ray ray;
				float maxDistance = std::numeric_limits<float>::max();
				if (hasViewport)
				{
					const std::optional<ViewRay> view = ReadViewportRay(context, parameters["viewport"], &error);
					if (!view)
						return EditorCommandResult::InvalidParameters(error);
					ray = view->CastRay;
					maxDistance = view->Far;
				}
				else
				{
					if (!arguments.Has("origin") || !arguments.Has("direction"))
						return EditorCommandResult::InvalidParameters("Give origin and direction together");
					const std::optional<glm::vec3> origin = ReadVec3(parameters["origin"]);
					if (!origin)
						return EditorCommandResult::InvalidParameters("'origin' must be three finite numbers");
					const std::optional<glm::vec3> direction = ReadDirection(parameters["direction"], "direction", &error);
					if (!direction)
						return EditorCommandResult::InvalidParameters(error);
					ray = Ray(*origin, *direction);
				}
				if (!ReadNumber(parameters, "maxDistance", 0.0, true, std::numeric_limits<float>::max(), maxDistance, &error))
					return EditorCommandResult::InvalidParameters(error);

				const std::optional<SceneRayHit> hit = SceneRaycast::Cast(scene, ray, maxDistance, ignore);
				if (!hit)
					return EditorCommandResult::Ok({ { "hit", false }, { "ray", DescribeRay(ray) } });
				return EditorCommandResult::Ok({
					{ "hit", true },
					{ "entity", UUIDToJson(hit->HitEntity.GetUUID()) },
					{ "name", hit->HitEntity.GetName() },
					{ "point", ToJson(hit->Point) },
					{ "normal", ToJson(hit->Normal) },
					{ "distance", hit->Distance },
					{ "onTriangle", hit->OnTriangle },
					{ "ray", DescribeRay(ray) } });
			}, EditorCommandKind::Query });

		nlohmann::json fallbackDistance = NumberSchema("With ray: where on the ray an asset lands when nothing is hit and the grid is beyond 500 m (default 10)");
		fallbackDistance["minimum"] = EditorPlacement::c_MinFallbackDistance;
		fallbackDistance["maximum"] = EditorPlacement::c_GridFallbackDistance;
		registry.Register(MayDefer({ "asset.place",
			"Places a prefab, model or mesh where a ray, a Scene view pixel or a point lands, resting on the surface there, as a drop into the Scene "
			"view does. One undo step. Exactly one of: ray {origin, direction}; viewport {x, y, width?, height?} (a pixel of the Scene view's image "
			"through its camera, as scene.raycast); point [x, y, z] with normal (default [0, 1, 0]), e.g. a scene.raycast hit. A ray or pixel that "
			"hits nothing lands on the grid plane y = 0 within 500 m, else on the ray at fallbackDistance (default 10 m; for a pixel the editor "
			"camera's distance to its target), unless requireSurface: then it fails. snap (meters) puts the point on that grid along the surface "
			"and finds the height there; rotation ([x, y, z] degrees) and scale (uniform) apply over the asset's own. A mesh becomes an entity with "
			"a Mesh Renderer, named after it. While the prefab or model loads it waits (at most 30 s), then up to 5 s for what its size needs "
			"(after that it places by the size known so far: bounds.pending true). Returns {id (the root), entities (every root), placedOn: "
			"surface | grid | inFront | point, hitEntity (null unless surface), point, normal, translation, bounds {min, max, size, center, "
			"pending}, snapped}; while playing it changes the running copy only.",
			ObjectSchema({
				{ "asset", AssetSchema("The prefab, model or mesh to place") },
				{ "ray", RaySchema("Place where this ray lands (one of ray, viewport, point)") },
				{ "viewport", ViewportPointSchema("Place where this pixel of the Scene view lands, through its camera (one of ray, viewport, point)") },
				{ "point", Vec3Schema("Place resting on this point, e.g. a scene.raycast hit (one of ray, viewport, point)") },
				{ "normal", Vec3Schema("With point: the surface's normal there (default [0, 1, 0])") },
				{ "parent", OptionalEntitySchema("Parent of the placed roots; null or omitted for the top level") },
				{ "rotation", Vec3Schema("Euler angles in degrees over the asset's own rotation (default [0, 0, 0])") },
				{ "scale", PositiveNumberSchema("Uniform scale over the asset's own (default 1)", c_MaxPlaceScale) },
				{ "snap", PositiveNumberSchema("Grid step in meters: the point snaps to it along the surface, and the height is found there",
					ViewportSettings::c_MaxTranslateSnap) },
				{ "maxDistance", PositiveNumberSchema("With ray or viewport: how far the ray looks for a surface, in meters (default: unlimited, or the view's far plane)") },
				{ "fallbackDistance", std::move(fallbackDistance) },
				{ "requireSurface", BoolSchema("Fail instead of falling back to the grid or the ray when nothing is hit (default false)") } }, { "asset" }),
			[](EditorContext& context, const nlohmann::json& parameters) -> EditorCommandResult
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				const AssetHandle asset = ResolveAsset(context, arguments, "asset", AssetType::None);
				const Entity parent = arguments.FindEntity(scene, "parent");
				const bool requireSurface = arguments.GetBool("requireSurface", false);
				if (!arguments.IsValid())
					return arguments.Fail();
				const std::string reference = parameters["asset"].get<std::string>();
				const std::optional<FoundAsset> found = FindAsset(reference);
				const AssetType type = found ? found->Type : AssetType::None;
				if (type != AssetType::Prefab && type != AssetType::Model && type != AssetType::Mesh)
				{
					return EditorCommandResult::InvalidParameters(fmt::format("'{}' is a {}: asset.place places prefabs, models and meshes (component.set gives a "
						"mesh renderer a material)", reference, AssetTypeToString(type)));
				}

				// Exactly one place to put it, and the options that go with it.
				const bool hasRay = arguments.Has("ray");
				const bool hasViewport = arguments.Has("viewport");
				const bool hasPoint = arguments.Has("point");
				if (static_cast<int>(hasRay) + static_cast<int>(hasViewport) + static_cast<int>(hasPoint) != 1)
					return EditorCommandResult::InvalidParameters("Give exactly one of ray, viewport and point");
				if (arguments.Has("normal") && !hasPoint)
					return EditorCommandResult::InvalidParameters("'normal' goes with point only");
				if (arguments.Has("fallbackDistance") && !hasRay)
					return EditorCommandResult::InvalidParameters("'fallbackDistance' goes with ray only (a pixel takes the view's)");
				if (arguments.Has("maxDistance") && hasPoint)
					return EditorCommandResult::InvalidParameters("'maxDistance' goes with ray or viewport only");

				std::string error;
				float maxDistance = std::numeric_limits<float>::max();
				float fallbackDistance = EditorPlacement::c_DefaultFallbackDistance;
				float scale = 1.0f;
				float snap = 0.0f;
				if (!ReadNumber(parameters, "maxDistance", 0.0, true, std::numeric_limits<float>::max(), maxDistance, &error)
					|| !ReadNumber(parameters, "fallbackDistance", EditorPlacement::c_MinFallbackDistance, false, EditorPlacement::c_GridFallbackDistance,
						fallbackDistance, &error)
					|| !ReadNumber(parameters, "scale", 0.0, true, c_MaxPlaceScale, scale, &error)
					|| !ReadNumber(parameters, "snap", 0.0, true, ViewportSettings::c_MaxTranslateSnap, snap, &error))
				{
					return EditorCommandResult::InvalidParameters(error);
				}
				const std::optional<float> snapStep = snap > 0.0f ? std::optional<float>(snap) : std::nullopt;
				glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
				if (arguments.Has("rotation"))
				{
					const std::optional<glm::vec3> degrees = ReadVec3(parameters["rotation"]);
					if (!degrees)
						return EditorCommandResult::InvalidParameters("'rotation' must be three finite numbers (degrees)");
					rotation = Math::EulerDegreesToQuat(*degrees);
				}

				// The request, resolved now: a placement that waits for its asset lands where the call asked, wherever the
				// camera goes meanwhile.
				EditorPlacement::PlacementRequest request;
				if (hasRay)
				{
					const std::optional<std::pair<glm::vec3, glm::vec3>> ray = ReadRay(parameters["ray"], &error);
					if (!ray)
						return EditorCommandResult::InvalidParameters(error);
					request = EditorPlacement::MakeRayRequest(ray->first, ray->second, maxDistance, fallbackDistance, snapStep);
				}
				else if (hasViewport)
				{
					const std::optional<ViewRay> view = ReadViewportRay(context, parameters["viewport"], &error);
					if (!view)
						return EditorCommandResult::InvalidParameters(error);
					const float viewFallback = EditorPlacement::GetFallbackDistance(view->FromScene ? nullptr : &context.GetViewport().GetCamera());
					request = EditorPlacement::MakeRayRequest(view->CastRay.Origin, view->CastRay.Direction, arguments.Has("maxDistance") ? maxDistance : view->Far,
						viewFallback, snapStep);
				}
				else
				{
					const std::optional<glm::vec3> point = ReadVec3(parameters["point"]);
					if (!point)
						return EditorCommandResult::InvalidParameters("'point' must be three finite numbers");
					glm::vec3 normal(0.0f, 1.0f, 0.0f);
					if (arguments.Has("normal"))
					{
						const std::optional<glm::vec3> given = ReadDirection(parameters["normal"], "normal", &error);
						if (!given)
							return EditorCommandResult::InvalidParameters(error);
						normal = *given;
					}
					request.At = EditorPlacement::Surface { *point, normal, true };
					request.Snap = snapStep;
				}
				request.RequireSurface = requireSurface;
				request.Rotation = rotation;
				request.Scale = scale;

				if (!context.CheckSceneEditable())
					return EditorCommandResult::Fail("The scene is being opened: nothing was placed");
				if (!context.GetAssetManager())
					return EditorCommandResult::Fail("No project is open: asset.place places the project's assets");

				PlaceJob job;
				job.Request = request;
				job.Asset = asset;
				job.Type = type;
				job.Parent = parent ? parent.GetUUID() : UUID::Null();
				job.Reference = reference;
				job.TargetScene = context.GetActiveScene();
				job.Start = std::chrono::steady_clock::now();
				if (std::optional<EditorCommandResult> placed = TryPlace(context, job))
					return std::move(*placed);
				return EditorCommandResult::Defer([job = std::move(job)](EditorContext& context) mutable -> std::optional<EditorCommandResult>
				{
					return TryPlace(context, job);
				});
			} }));
	}

}
