#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"

#include <Strata/Core/Base64.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Renderer/ImageWriter.h>
#include <Strata/Renderer/Renderer.h>
#include <Strata/Renderer/SceneRenderer.h>
#include <Strata/Renderer/TextureReadback.h>
#include <Strata/Scene/Scene.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		constexpr int64_t c_MinCaptureSize = 16;
		constexpr int64_t c_MaxCaptureSize = 4096;
		// A capture whose GPU readback has not finished after this long fails (a lost or hung device).
		constexpr std::chrono::seconds c_CaptureReadbackTimeout { 30 };
		// Frames a capture may render again while glyphs of its text are still being rasterized (a budget per render, see
		// TextRenderer::c_FrameRasterBudget); enough for thousands of distinct glyphs.
		constexpr uint32_t c_MaxCaptureTextFrames = 120;
		// Frames a capture may render again while meshes, materials or textures of its view are still loading (waitForAssets):
		// about five seconds at the editor's 60 frames per second, enough for a scene's first loads after opening or playing;
		// what still loads then is reported (pendingAssets).
		constexpr uint32_t c_MaxCaptureAssetFrames = 300;
		// Frames a capture may render again (waitForAssets) while textures it draws are coarser than the level its view wants:
		// a texture counts as loaded with its mip tail, and the finer levels stream in over the next frames. A texture budget
		// may keep a view from ever getting them: then the picture shows what fits after about two seconds.
		constexpr uint32_t c_MaxCaptureMipFrames = 120;
		// Frames a capture may render again (waitForAssets) while animators it draws show a pose older than their time: an
		// animator that animates only while seen (WhenVisible) and that no renderer drew before skipped its poses, and the
		// capture's report poses it in the next frame (also while the game is paused).
		constexpr uint32_t c_MaxCapturePoseFrames = 8;

		nlohmann::json ToJson(const glm::vec3& value)
		{
			return { value.x, value.y, value.z };
		}

		nlohmann::json DescribeCamera(const EditorCamera& camera)
		{
			return {
				{ "position", ToJson(camera.GetPosition()) },
				{ "target", ToJson(camera.GetTarget()) },
				{ "forward", ToJson(camera.GetForward()) },
				{ "yaw", camera.GetYaw() },
				{ "pitch", camera.GetPitch() },
				{ "distance", camera.GetDistance() },
				{ "fov", camera.GetFOV() },
				{ "near", camera.GetNear() },
				{ "far", camera.GetFar() },
				{ "flySpeed", camera.GetFlySpeed() }
			};
		}

		// Where a capture looked from: position, forward direction and vertical field of view in degrees (null for orthographic
		// cameras).
		nlohmann::json DescribeView(const SceneCamera& camera)
		{
			// The view matrix maps the camera's forward direction to -Z: its third row, negated.
			const glm::vec3 forward = -glm::vec3(camera.View[0][2], camera.View[1][2], camera.View[2][2]);
			return { { "position", ToJson(camera.Position) }, { "forward", ToJson(forward) },
				{ "fov", camera.Orthographic ? nlohmann::json(nullptr) : nlohmann::json(glm::degrees(camera.VerticalFOV)) } };
		}

		nlohmann::json RangeSchema(std::string description, double minimum, double maximum)
		{
			return { { "type", "number" }, { "description", std::move(description) }, { "minimum", minimum }, { "maximum", maximum } };
		}

		// viewport.setSettings parameters and the ViewportSettings keys they set.
		constexpr std::pair<const char*, const char*> c_ViewportSettingNames[] = {
			{ "grid", "ShowGrid" },
			{ "selectionOutline", "ShowSelectionOutline" },
			{ "sceneGizmos", "ShowSceneGizmos" },
			{ "allColliders", "AllColliders" },
			{ "stats", "ShowStats" },
			{ "previewLighting", "PreviewLighting" },
			{ "gameUI", "ShowGameUI" },
			{ "gizmo", "Gizmo" },
			{ "space", "Space" },
			{ "snap", "Snap" },
			{ "translateSnap", "TranslateSnap" },
			{ "rotateSnap", "RotateSnap" },
			{ "scaleSnap", "ScaleSnap" },
			{ "debugView", "DebugView" },
			{ "pilotGameCamera", "PilotGameCamera" }
		};

		nlohmann::json DebugViewSchema(std::string description)
		{
			return { { "type", "string" }, { "enum", { "none", "streaming", "lod", "mip" } }, { "description", std::move(description) } };
		}

		// What the debug view of a render found.
		nlohmann::json DescribeDebugView(ViewportDebugView view, const StreamingViewStats& stats, uint32_t tintedEntities)
		{
			return {
				{ "view", ViewportDebugViewToString(view) },
				{ "tintedEntities", tintedEntities },
				{ "pendingEntities", stats.PendingEntities },
				{ "failedEntities", stats.FailedEntities },
				{ "placeholderBoxes", stats.PlaceholderBoxes },
				{ "entitiesPerLOD", stats.EntitiesPerLOD },
				{ "entitiesPerMipShortfall", stats.EntitiesPerMipShortfall } };
		}

		nlohmann::json DescribeViewportSettings(const ViewportSettings& settings)
		{
			const nlohmann::json values = settings.ToJson();
			nlohmann::json result = nlohmann::json::object();
			for (const auto& [parameter, key] : c_ViewportSettingNames)
				result[parameter] = values[key];
			result["gameView"] = { { "mode", GameViewModeToString(settings.GameView.Mode) }, { "width", settings.GameView.Width },
				{ "height", settings.GameView.Height } };
			return result;
		}

		// An optional number parameter within [minimum, maximum]; problems are recorded in `arguments`.
		std::optional<float> ReadNumber(const nlohmann::json& parameters, CommandArguments& arguments, const char* name, float minimum, float maximum)
		{
			const auto it = parameters.find(name);
			if (it == parameters.end() || it->is_null())
				return std::nullopt;
			const double value = it->is_number() ? it->get<double>() : std::nan("");
			if (!std::isfinite(value) || value < minimum || value > maximum)
			{
				arguments.SetError(fmt::format("Parameter '{}' must be a number between {} and {}", name, minimum, maximum));
				return std::nullopt;
			}
			return static_cast<float>(value);
		}

		std::optional<glm::vec3> ReadVec3(const nlohmann::json& parameters, CommandArguments& arguments, const char* name)
		{
			const auto it = parameters.find(name);
			if (it == parameters.end() || it->is_null())
				return std::nullopt;
			if (it->is_array() && it->size() == 3 && (*it)[0].is_number() && (*it)[1].is_number() && (*it)[2].is_number())
			{
				const glm::dvec3 value((*it)[0].get<double>(), (*it)[1].get<double>(), (*it)[2].get<double>());
				const double limit = EditorCamera::c_MaxCoordinate;
				if (std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && glm::all(glm::lessThanEqual(glm::abs(value), glm::dvec3(limit))))
					return glm::vec3(value);
			}
			arguments.SetError(fmt::format("Parameter '{}' must be an array of three finite numbers within +-{}", name, EditorCamera::c_MaxCoordinate));
			return std::nullopt;
		}

		// A camera of the capture's own: the editor camera with these changes, which the person's camera never sees.
		struct CaptureCamera
		{
			std::optional<glm::vec3> Position;
			std::optional<glm::vec3> Target;
			std::optional<float> FOV;

			bool IsSet() const { return Position || Target || FOV; }

			// Applies the changes to a copy of the editor camera; false when position and target are too close or too far apart.
			bool Apply(EditorCamera& camera) const
			{
				if (FOV)
					camera.SetFOV(*FOV);
				if (Position && Target)
					return camera.LookAt(*Position, *Target);
				if (Position)
					camera.SetPosition(*Position);
				else if (Target)
					camera.SetTarget(*Target);
				return true;
			}
		};

		struct CaptureRequest
		{
			glm::uvec2 Size = { 0, 0 };
			ViewportCameraSource Camera = ViewportCameraSource::Automatic;
			CaptureCamera OwnCamera; // Set: renders through it instead of the viewport's cameras
			bool WaitForAssets = true;
			std::optional<bool> Overlays;
			std::optional<ViewportDebugView> DebugView; // Unset: the viewport's
			std::filesystem::path Path; // Empty: the image is only returned
			bool Overwrite = false;
		};

		// The image and what the jobs make of it. Jobs write it; the main thread reads it once a job has completed.
		struct EncodedCapture
		{
			ReadbackImage Image;
			std::vector<uint8_t> Png;
			std::string Base64;
			std::string Error;     // Encoding failed when set
			std::string SaveError; // Saving failed when set
		};

		struct CaptureProgress
		{
			Scope<TextureReadback> Readback;
			ViewportView View;
			bool Overlays = false;
			ViewportDebugView DebugView = ViewportDebugView::None;
			StreamingViewStats DebugViewStats;
			uint32_t TintedEntities = 0;
			uint32_t PendingAssets = 0;
			uint32_t PendingTextGlyphs = 0;
			bool PreviewLighting = false;
			uint32_t ScreenSpaceTexts = 0;
			uint32_t HiddenScreenSpaceTexts = 0;
			uint32_t TextFrames = 0;  // Renders repeated so far to complete the text
			uint32_t AssetFrames = 0; // Renders repeated so far while assets of the view were loading
			uint32_t MipFrames = 0;   // Renders repeated so far while textures were coarser than the view wants
			uint32_t PoseFrames = 0;  // Renders repeated so far while animators showed poses older than their time
			uint32_t TexturesBelowWanted = 0;
			uint32_t StalePoses = 0;
			std::chrono::steady_clock::time_point Submitted;
			Ref<EncodedCapture> Encoded; // Set once the pixels are on the CPU
			JobHandle EncodeJob;
			JobHandle SaveJob;
			bool SaveStarted = false;
		};

		// The view of a capture: the viewport's choice of camera, or the capture's own camera (an editor view while the game
		// is not playing, like the editor camera it starts from).
		std::optional<ViewportView> ResolveCaptureView(EditorContext& context, const CaptureRequest& request, float aspectRatio, std::string* outError)
		{
			if (!request.OwnCamera.IsSet())
				return ResolveViewportView(context, request.Camera, aspectRatio, outError);
			EditorCamera camera = context.GetViewport().GetCamera();
			if (!request.OwnCamera.Apply(camera))
			{
				if (outError)
					*outError = fmt::format("position and target must be between {} and {} apart", EditorCamera::c_MinDistance, EditorCamera::c_MaxDistance);
				return std::nullopt;
			}
			ViewportView view;
			view.Camera = camera.GetSceneCamera(aspectRatio);
			view.EditorOverlays = true;
			view.EditorView = context.GetSceneState() != SceneState::Play;
			return view;
		}

		// First poll (the frame after the command): renders the image and starts reading it back. While glyphs of the
		// scene's text are still being rasterized, or (waitForAssets) assets of the view are still loading or textures it draws
		// lack the levels the view wants, the image would lack them: the next polls render again until it is complete, or
		// c_MaxCaptureTextFrames, c_MaxCaptureAssetFrames and c_MaxCaptureMipFrames ran out.
		std::optional<EditorCommandResult> StartCapture(EditorContext& context, const CaptureRequest& request, CaptureProgress& progress)
		{
			EditorViewport& viewport = context.GetViewport();
			ViewportRenderer* renderer = viewport.GetCaptureRenderer();
			if (!renderer)
				return EditorCommandResult::Fail("viewport.capture needs a GPU: the renderer is not running");

			std::string error;
			const float aspectRatio = static_cast<float>(request.Size.x) / static_cast<float>(request.Size.y);
			std::optional<ViewportView> view = ResolveCaptureView(context, request, aspectRatio, &error);
			if (!view)
			{
				renderer->ReleaseTargets(); // An earlier render of this capture may have created them
				return EditorCommandResult::Fail(error);
			}
			progress.Overlays = request.Overlays.value_or(view->EditorOverlays);
			ViewportSettings settings = viewport.GetSettings();
			if (request.DebugView)
				settings.DebugView = *request.DebugView;
			progress.DebugView = settings.DebugView;

			// A capture is a single frame: its exposure is metered from this frame alone, not adapted from an earlier one.
			renderer->GetSceneRenderer().ResetExposureAdaptation();
			if (!renderer->Render(context, request.Size, *view, settings, progress.Overlays))
			{
				renderer->ReleaseTargets();
				return EditorCommandResult::Fail("Rendering the capture failed (see the log)");
			}
			const SceneRendererStats& stats = renderer->GetStats();
			progress.DebugViewStats = renderer->GetDebugViewStats();
			progress.TintedEntities = stats.TintedEntities;
			progress.PendingAssets = stats.PendingAssets;
			progress.PendingTextGlyphs = stats.PendingTextGlyphs;
			progress.PreviewLighting = stats.PreviewLighting;
			progress.ScreenSpaceTexts = stats.ScreenSpaceTexts;
			progress.HiddenScreenSpaceTexts = stats.HiddenScreenSpaceTexts;
			progress.TexturesBelowWanted = renderer->CountTexturesBelowWanted();
			progress.StalePoses = stats.StalePoses;
			const bool textPending = progress.PendingTextGlyphs > 0 && progress.TextFrames < c_MaxCaptureTextFrames;
			const bool assetsPending = request.WaitForAssets && progress.PendingAssets > 0 && progress.AssetFrames < c_MaxCaptureAssetFrames;
			const bool mipsPending = request.WaitForAssets && progress.TexturesBelowWanted > 0 && progress.MipFrames < c_MaxCaptureMipFrames;
			const bool posesPending = request.WaitForAssets && progress.StalePoses > 0 && progress.PoseFrames < c_MaxCapturePoseFrames;
			if (textPending || assetsPending || mipsPending || posesPending)
			{
				progress.TextFrames += textPending ? 1 : 0;
				progress.AssetFrames += assetsPending ? 1 : 0;
				progress.MipFrames += mipsPending ? 1 : 0;
				progress.PoseFrames += posesPending ? 1 : 0;
				return std::nullopt;
			}
			progress.Readback = TextureReadback::Create(renderer->GetOutputTexture(), {}, &error);
			// Captures are occasional and may be large: their render targets are not kept between them.
			renderer->ReleaseTargets();
			if (!progress.Readback)
				return EditorCommandResult::Fail(fmt::format("Reading the capture back failed: {}", error));
			progress.View = std::move(*view);
			progress.Submitted = std::chrono::steady_clock::now();
			return std::nullopt;
		}

		// Later polls: wait for the GPU copy, encode the image on a worker thread (PNG compression and Base64 of a large
		// image take long enough to hitch the editor), save it on an I/O thread when asked to, then report.
		std::optional<EditorCommandResult> ContinueCapture(const CaptureRequest& request, CaptureProgress& progress)
		{
			if (!progress.Encoded)
			{
				if (!progress.Readback->IsReady())
				{
					if (std::chrono::steady_clock::now() - progress.Submitted > c_CaptureReadbackTimeout)
						return EditorCommandResult::Fail(fmt::format("The GPU did not finish the capture within {} seconds", c_CaptureReadbackTimeout.count()));
					return std::nullopt;
				}
				Ref<EncodedCapture> encoded = CreateRef<EncodedCapture>();
				if (!progress.Readback->GetResult(encoded->Image))
					return EditorCommandResult::Fail("Mapping the captured image failed");
				progress.Readback.reset();
				progress.Encoded = encoded;
				progress.EncodeJob = JobSystem::Submit([encoded]()
				{
					std::string error;
					std::optional<std::vector<uint8_t>> png = ImageWriter::EncodePNG(encoded->Image, true, &error);
					if (!png)
					{
						encoded->Error = fmt::format("Encoding the capture as PNG failed: {}", error);
						return;
					}
					encoded->Base64 = Base64::Encode(*png);
					encoded->Png = std::move(*png);
				});
				return std::nullopt;
			}

			if (!progress.EncodeJob.IsComplete())
				return std::nullopt;
			EncodedCapture& encoded = *progress.Encoded;
			if (!encoded.Error.empty())
				return EditorCommandResult::Fail(encoded.Error);
			if (!request.Path.empty() && !progress.SaveStarted)
			{
				progress.SaveStarted = true;
				progress.SaveJob = JobSystem::SubmitIO([encodedRef = progress.Encoded, path = request.Path, overwrite = request.Overwrite]()
				{
					// Checked again: the file may have appeared since the command was given.
					if (!overwrite && FileSystem::Exists(path))
						encodedRef->SaveError = fmt::format("'{}' already exists (pass overwrite: true to replace it)", FileSystem::ToUTF8(path));
					else if (!FileSystem::CreateDirectories(path.parent_path()) || !FileSystem::WriteBytes(path, encodedRef->Png))
						encodedRef->SaveError = fmt::format("Cannot write the capture to '{}'", FileSystem::ToUTF8(path));
				});
				return std::nullopt;
			}
			if (!progress.SaveJob.IsComplete())
				return std::nullopt;
			if (!encoded.SaveError.empty())
				return EditorCommandResult::Fail(encoded.SaveError);

			nlohmann::json result = {
				{ "Image", { { "MimeType", "image/png" }, { "Data", std::move(encoded.Base64) } } },
				{ "width", encoded.Image.Width },
				{ "height", encoded.Image.Height },
				{ "camera", request.OwnCamera.IsSet() ? "custom" : (progress.View.FromScene ? "scene" : "editor") },
				{ "view", DescribeView(progress.View.Camera) },
				{ "waitedFrames", std::max({ progress.TextFrames, progress.AssetFrames, progress.MipFrames, progress.PoseFrames }) },
				{ "overlays", progress.Overlays },
				{ "pendingAssets", progress.PendingAssets },
				{ "texturesBelowWanted", progress.TexturesBelowWanted },
				{ "stalePoses", progress.StalePoses },
				{ "pendingTextGlyphs", progress.PendingTextGlyphs },
				{ "previewLighting", progress.PreviewLighting },
				{ "screenSpaceTexts", progress.ScreenSpaceTexts },
				{ "hiddenScreenSpaceTexts", progress.HiddenScreenSpaceTexts },
				{ "debugView", DescribeDebugView(progress.DebugView, progress.DebugViewStats, progress.TintedEntities) }
			};
			if (!progress.View.Notice.empty())
				result["notice"] = progress.View.Notice;
			if (!request.Path.empty())
				result["path"] = FileSystem::ToUTF8(request.Path);
			return EditorCommandResult::Ok(std::move(result));
		}

	}

	void RegisterViewportCommands(EditorCommandRegistry& registry)
	{
		////////////////////////////////////////////////////////////////////////////////
		// Editor camera
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "camera.get",
			"The editor camera: position, target (orbit pivot), forward direction, yaw and pitch in degrees, distance to the target, vertical field of view, clip planes and fly speed.",
			ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				return EditorCommandResult::Ok(DescribeCamera(context.GetViewport().GetCamera()));
			}, EditorCommandKind::Query, EditorCommandResults::Session });

		registry.Register({ "camera.set",
			"Moves or configures the editor camera (a view change: no undo step, the scene is unchanged). position + target looks from one point at the other; "
			"otherwise yaw/pitch orbit around the target, distance sets how far the camera is from it, and position or target moves the camera keeping its orientation. "
			"Returns the camera like camera.get.",
			ObjectSchema({
				{ "position", Vec3Schema("Camera position in world space") },
				{ "target", Vec3Schema("Point the camera looks at and orbits around") },
				{ "yaw", RangeSchema("Degrees around world +Y; 0 looks along -Z, 90 along -X", -1.0e6, 1.0e6) },
				{ "pitch", RangeSchema("Degrees up (positive) or down (negative)", -EditorCamera::c_MaxPitch, EditorCamera::c_MaxPitch) },
				{ "distance", RangeSchema("Distance from the camera to the target", EditorCamera::c_MinDistance, EditorCamera::c_MaxDistance) },
				{ "fov", RangeSchema("Vertical field of view in degrees", EditorCamera::c_MinFOV, EditorCamera::c_MaxFOV) },
				{ "near", RangeSchema("Near clip plane distance", EditorCamera::c_MinNear, EditorCamera::c_MaxFar) },
				{ "far", RangeSchema("Far clip plane distance (above near)", EditorCamera::c_MinNear, EditorCamera::c_MaxFar) },
				{ "flySpeed", RangeSchema("Fly mode speed in units per second", EditorCamera::c_MinFlySpeed, EditorCamera::c_MaxFlySpeed) } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::optional<glm::vec3> position = ReadVec3(parameters, arguments, "position");
				const std::optional<glm::vec3> target = ReadVec3(parameters, arguments, "target");
				const std::optional<float> yaw = ReadNumber(parameters, arguments, "yaw", -1.0e6f, 1.0e6f);
				const std::optional<float> pitch = ReadNumber(parameters, arguments, "pitch", -EditorCamera::c_MaxPitch, EditorCamera::c_MaxPitch);
				const std::optional<float> distance = ReadNumber(parameters, arguments, "distance", EditorCamera::c_MinDistance, EditorCamera::c_MaxDistance);
				const std::optional<float> fov = ReadNumber(parameters, arguments, "fov", EditorCamera::c_MinFOV, EditorCamera::c_MaxFOV);
				const std::optional<float> nearClip = ReadNumber(parameters, arguments, "near", EditorCamera::c_MinNear, EditorCamera::c_MaxFar);
				const std::optional<float> farClip = ReadNumber(parameters, arguments, "far", EditorCamera::c_MinNear, EditorCamera::c_MaxFar);
				const std::optional<float> flySpeed = ReadNumber(parameters, arguments, "flySpeed", EditorCamera::c_MinFlySpeed, EditorCamera::c_MaxFlySpeed);
				if (!arguments.IsValid())
					return arguments.Fail();
				if (position && target && (yaw || pitch || distance))
					return EditorCommandResult::Fail("position and target define the orientation and distance: give either them or yaw, pitch and distance");

				// Applied to a copy, so a rejected combination changes nothing.
				EditorCamera camera = context.GetViewport().GetCamera();
				if ((nearClip || farClip) && !camera.SetClipPlanes(nearClip.value_or(camera.GetNear()), farClip.value_or(camera.GetFar())))
				{
					return EditorCommandResult::Fail(fmt::format("near ({}) must be below far ({})", nearClip.value_or(camera.GetNear()),
						farClip.value_or(camera.GetFar())));
				}
				if (fov)
					camera.SetFOV(*fov);
				if (flySpeed)
					camera.SetFlySpeed(*flySpeed);
				if (position && target)
				{
					if (!camera.LookAt(*position, *target))
					{
						return EditorCommandResult::Fail(fmt::format("position and target must be between {} and {} apart", EditorCamera::c_MinDistance,
							EditorCamera::c_MaxDistance));
					}
				}
				else
				{
					if (yaw || pitch)
						camera.SetOrientation(yaw.value_or(camera.GetYaw()), pitch.value_or(camera.GetPitch()));
					if (distance)
						camera.SetDistance(*distance);
					if (position)
						camera.SetPosition(*position);
					else if (target)
						camera.SetTarget(*target);
				}
				context.GetViewport().GetCamera() = camera;
				return EditorCommandResult::Ok(DescribeCamera(camera));
			}, EditorCommandKind::Session });

		registry.Register({ "camera.focus",
			"Points the editor camera at entities (with their descendants) and backs it off until their bounds fit the view, keeping its orientation; without entities it frames the whole scene. No undo step. Returns the camera like camera.get.",
			ObjectSchema({ { "entities", EntityArraySchema("Entities to frame (default: every active entity)") } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				std::vector<Entity> entities;
				if (arguments.Has("entities"))
					entities = arguments.GetEntities(*context.GetActiveScene(), "entities");
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!context.GetViewport().Focus(context, entities))
					return EditorCommandResult::Fail("Nothing to frame: the scene has no active entities");
				return EditorCommandResult::Ok(DescribeCamera(context.GetViewport().GetCamera()));
			}, EditorCommandKind::Session });

		////////////////////////////////////////////////////////////////////////////////
		// Settings
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "viewport.getSettings",
			"The viewport's view settings (saved with the project's editor state): which editor overlays are drawn (grid, selectionOutline, sceneGizmos, "
			"stats; allColliders: collider shapes of every entity instead of only the selection's), previewLighting (editor views light scenes without any light of their own with a preview sun and sky), gameUI (editor views draw the "
			"game's screen-space text), the gizmo mode and space, whether gizmo drags snap, the snap steps, debugView (\"none\", or \"streaming\" / "
			"\"lod\" / \"mip\": the image colored by streaming state, level of detail or the mip levels textures lack), gameView (the Game view's image: "
			"{mode, width, height}, width and height for mode \"custom\") and pilotGameCamera (the Scene view looks through the scene's primary camera).",
			ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				return EditorCommandResult::Ok(DescribeViewportSettings(context.GetViewport().GetSettings()));
			}, EditorCommandKind::Query, EditorCommandResults::Session });

		registry.Register({ "viewport.setSettings",
			"Changes view settings of the viewport (a view change: no undo step, the scene is unchanged) and returns all of them like viewport.getSettings. "
			"Editor views are the Scene view's editor camera (also while the game plays); the Game view and the scene's camera always show the scene as "
			"the game does.",
			ObjectSchema({
				{ "grid", BoolSchema("Draw the ground grid") },
				{ "selectionOutline", BoolSchema("Outline the selected entities") },
				{ "sceneGizmos", BoolSchema("Draw light, camera and collider shapes (far away lights and cameras as small icons)") },
				{ "allColliders", BoolSchema("Draw the collider shapes of every entity; off (the default), only those of the selection and its descendants") },
				{ "stats", BoolSchema("Show frame time, draw calls and loading assets over the image") },
				{ "previewLighting", BoolSchema("Editor views light scenes without any light of their own (no directional, point, spot or sky light) "
					"with a preview sun and sky (never part of the scene or the game)") },
				{ "gameUI", BoolSchema("Editor views draw the game's screen-space text (its HUD); off by default so the HUD does not cover the scene") },
				{ "gizmo", { { "type", "string" }, { "enum", { "None", "Translate", "Rotate", "Scale" } }, { "description", "Transform gizmo of the viewport" } } },
				{ "space", { { "type", "string" }, { "enum", { "Local", "World" } }, { "description", "Space the gizmo works in" } } },
				{ "snap", BoolSchema("Snap gizmo drags to the steps below (holding Ctrl inverts it)") },
				{ "translateSnap", RangeSchema("Move snapping step in world units", 0.0, ViewportSettings::c_MaxTranslateSnap) },
				{ "rotateSnap", RangeSchema("Rotation snapping step in degrees", 0.0, ViewportSettings::c_MaxRotateSnap) },
				{ "scaleSnap", RangeSchema("Scale snapping step", 0.0, ViewportSettings::c_MaxScaleSnap) },
				{ "debugView", DebugViewSchema("Color every view of the viewport: \"streaming\" tints entities whose mesh, material or textures are still "
					"loading (Sulfur; failed ones Cinnabar; entities whose mesh is not there are drawn as boxes), \"lod\" tints entities by the level of "
					"detail they are drawn at (0 to 3+), \"mip\" tints entities with textures by the mip levels their worst texture lacks against what "
					"their view needs (none Malachite, 1 Azurite, 2 Sulfur, 3 or more or still loading Cinnabar), \"none\" shows the image as rendered") },
				{ "gameView", ObjectSchema({
					{ "mode", { { "type", "string" }, { "enum", { "free", "16:9", "16:10", "4:3", "21:9", "1920x1080", "1280x720", "custom", "game" } },
						{ "description", "The Game view's image: free fills the panel, an aspect ratio is the largest image of it inside the panel "
							"(letterboxed), a size renders at exactly those pixels scaled to fit, custom uses width and height, game the resolution the "
							"running game asked for (free without a request)" } } },
					{ "width", IntegerSchema("Width of the custom size", GameViewSettings::c_MinSize, GameViewSettings::c_MaxSize) },
					{ "height", IntegerSchema("Height of the custom size", GameViewSettings::c_MinSize, GameViewSettings::c_MaxSize) } }) },
				{ "pilotGameCamera", BoolSchema("The Scene view looks through the scene's primary camera, with the editor's overlays (F8)") } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				nlohmann::json changes = nlohmann::json::object();
				for (const auto& [parameter, key] : c_ViewportSettingNames)
				{
					if (const auto it = parameters.find(parameter); it != parameters.end())
						changes[key] = *it;
				}
				if (const auto gameView = parameters.find("gameView"); gameView != parameters.end())
				{
					if (!gameView->is_object())
						return EditorCommandResult::InvalidParameters("Parameter 'gameView' must be an object {mode, width, height}");
					nlohmann::json values = nlohmann::json::object();
					for (const auto& [name, value] : gameView->items())
					{
						if (name != "mode" && name != "width" && name != "height")
							return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'gameView' has no '{}' (mode, width, height)", name));
						values[name == "mode" ? "Mode" : (name == "width" ? "Width" : "Height")] = value;
					}
					changes["GameView"] = std::move(values);
				}
				// Applied to a copy, so an invalid value changes nothing.
				ViewportSettings settings = context.GetViewport().GetSettings();
				std::string error;
				if (!settings.FromJson(changes, &error))
				{
					// The settings name their values like the saved state does; the parameters are camelCase.
					for (const auto& [parameter, key] : c_ViewportSettingNames)
					{
						const std::string quoted = fmt::format("'{}'", key);
						if (const size_t position = error.find(quoted); position != std::string::npos)
							error.replace(position, quoted.size(), fmt::format("Parameter '{}'", parameter));
					}
					for (const auto& [saved, parameter] : { std::pair<const char*, const char*> { "'GameView.Mode'", "Parameter 'gameView.mode'" },
						{ "'GameView.Width'", "Parameter 'gameView.width'" }, { "'GameView.Height'", "Parameter 'gameView.height'" } })
					{
						if (const size_t position = error.find(saved); position != std::string::npos)
							error.replace(position, std::string_view(saved).size(), parameter);
					}
					return EditorCommandResult::InvalidParameters(error);
				}
				context.GetViewport().GetSettings() = settings;
				return EditorCommandResult::Ok(DescribeViewportSettings(settings));
			}, EditorCommandKind::Session });

		////////////////////////////////////////////////////////////////////////////////
		// Capture
		////////////////////////////////////////////////////////////////////////////////

		registry.Register(MayDefer({ "viewport.capture",
			"Renders the scene on the next frame and returns the picture as a PNG image, with its width and height. By default it looks like the viewport: "
			"its size (1280x720 while the viewport is hidden), the scene's primary camera while playing (the editor camera otherwise) and the editor overlays "
			"(grid, selection outline, light/camera/collider shapes) for the editor camera; camera \"scene\" takes the Game view's size when it keeps an "
			"aspect ratio or a fixed size (viewport.setSettings gameView). position, target and fov look through a camera of the "
			"capture's own (the editor camera with those changes; camera.get stays as it was): position + target looks from one at the other, either "
			"alone moves the editor camera's view keeping its direction; the result's camera is then \"custom\", and view has the position, forward "
			"direction and fov used. With the editor camera outside play mode, a scene without any light "
			"of its own is lit by preview lighting (previewLighting: true in the result; the game will be unlit) and the game's screen-space text is hidden "
			"(hiddenScreenSpaceTexts counts it): see viewport.setSettings, or use camera \"scene\" to see what the game shows. Text whose glyphs are still being "
			"prepared delays the picture by a few frames, so it is complete, and so do meshes, materials and textures of the view that are still loading "
			"(waitForAssets, default true: up to about five seconds of frames; waitedFrames counts the extra renders), and textures it draws that still "
			"lack the finer mip levels the view wants (up to about two seconds, as a texture budget may keep them out), and animators that animate only "
			"while seen (WhenVisible) until they show the pose of their time (a few frames, also while paused). Reports what was still "
			"loading when the picture was taken (pendingAssets), the textures drawn coarser than the view wants (texturesBelowWanted), animators "
			"drawn with an older pose (stalePoses) and glyphs that could not be prepared in time (pendingTextGlyphs). debugView colors the picture "
			"by streaming state, level of detail or mip levels (default: the viewport's, viewport.setSettings) and the result's debugView says what it "
			"found: {view, tintedEntities, pendingEntities, failedEntities, placeholderBoxes, entitiesPerLOD: [LOD0, LOD1, LOD2, LOD3+], "
			"entitiesPerMipShortfall: [at the wanted level, 1 level coarser, 2, 3+ or loading]} among the entities in view. "
			"Needs a GPU (fails in editors started with --no-gpu).",
			ObjectSchema({
				{ "width", IntegerSchema("Image width in pixels; without height the viewport's aspect ratio is kept", c_MinCaptureSize, c_MaxCaptureSize) },
				{ "height", IntegerSchema("Image height in pixels; without width the viewport's aspect ratio is kept", c_MinCaptureSize, c_MaxCaptureSize) },
				{ "camera", { { "type", "string" }, { "enum", { "editor", "scene" } },
					{ "description", "\"editor\": the editor camera; \"scene\": the scene's primary camera (fails without one)" } } },
				{ "overlays", BoolSchema("Draw the editor overlays enabled in the viewport (default: on for the editor camera, off for the scene camera)") },
				{ "position", Vec3Schema("Look from this point (a camera of the capture's own; the editor camera does not move)") },
				{ "target", Vec3Schema("Look at this point (a camera of the capture's own; the editor camera does not move)") },
				{ "fov", RangeSchema("Vertical field of view in degrees of the capture's own camera", EditorCamera::c_MinFOV, EditorCamera::c_MaxFOV) },
				{ "waitForAssets", BoolSchema("Render again on the following frames while assets of the view are still loading, up to about five seconds, "
					"and while textures lack the mip levels the view wants, up to about two (default true); false takes the picture at once") },
				{ "debugView", DebugViewSchema("\"streaming\" tints entities by streaming state (pending Sulfur, failed Cinnabar, missing meshes as boxes), "
					"\"lod\" by level of detail, \"mip\" by the mip levels their textures lack, \"none\" not at all (default: the viewport's debug view)") },
				{ "path", StringSchema("Also save the PNG to this .png file: relative to the project directory (needs an open project) or absolute; folders are "
					"created, network and device paths are refused") },
				{ "overwrite", BoolSchema("Replace an existing file at path (default false: an existing file is an error)") } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const int64_t width = arguments.GetInt("width", 0, c_MinCaptureSize, c_MaxCaptureSize);
				const int64_t height = arguments.GetInt("height", 0, c_MinCaptureSize, c_MaxCaptureSize);
				CaptureRequest request;
				const std::string camera = arguments.GetString("camera", "");
				if (camera == "editor")
					request.Camera = ViewportCameraSource::Editor;
				else if (camera == "scene")
					request.Camera = ViewportCameraSource::Scene;
				else if (arguments.Has("camera"))
					arguments.SetError("Parameter 'camera' must be \"editor\" or \"scene\"");
				if (arguments.Has("overlays"))
					request.Overlays = arguments.GetBool("overlays", true);
				request.OwnCamera.Position = ReadVec3(parameters, arguments, "position");
				request.OwnCamera.Target = ReadVec3(parameters, arguments, "target");
				request.OwnCamera.FOV = ReadNumber(parameters, arguments, "fov", EditorCamera::c_MinFOV, EditorCamera::c_MaxFOV);
				request.WaitForAssets = arguments.GetBool("waitForAssets", true);
				if (arguments.IsValid() && request.OwnCamera.IsSet())
				{
					EditorCamera check = context.GetViewport().GetCamera();
					if (request.Camera == ViewportCameraSource::Scene)
						arguments.SetError("position, target and fov set a camera of the capture's own: they cannot be combined with camera \"scene\"");
					else if (!request.OwnCamera.Apply(check))
						arguments.SetError(fmt::format("Parameters 'position' and 'target' must be between {} and {} apart", EditorCamera::c_MinDistance, EditorCamera::c_MaxDistance));
				}
				if (arguments.Has("debugView"))
				{
					request.DebugView = ViewportDebugViewFromString(arguments.GetString("debugView"));
					if (!request.DebugView && arguments.IsValid())
						arguments.SetError("Parameter 'debugView' must be \"none\", \"streaming\", \"lod\" or \"mip\"");
				}
				request.Overwrite = arguments.GetBool("overwrite", false);
				if (arguments.Has("path"))
				{
					const std::string path = arguments.GetString("path");
					std::string error;
					if (arguments.IsValid())
					{
						if (std::optional<std::filesystem::path> resolved = ResolveOutputPath(context, path, ".png", request.Overwrite, &error))
							request.Path = std::move(*resolved);
						else
							arguments.SetError(fmt::format("Parameter 'path': {}", error));
					}
				}
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!Renderer::IsInitialized())
					return EditorCommandResult::Fail("viewport.capture needs a GPU, but this editor runs without a graphics device (--no-gpu or no usable GPU)");

				// The size: as given, the missing side from the viewport's aspect ratio, or the viewport's own size. The scene's
				// camera takes the Game view's when it keeps an aspect ratio or a size: its fixed size, else its image's (or the
				// default width at its aspect ratio while it is hidden).
				float aspectRatio = context.GetViewport().GetAspectRatio();
				glm::uvec2 viewportSize = context.GetViewport().GetSize();
				if (request.Camera == ViewportCameraSource::Scene)
				{
					const GameViewSettings& gameView = context.GetViewport().GetSettings().GameView;
					const std::optional<glm::uvec2> gameRequest = context.GetGameWindow().GetRequestedResolution();
					if (const std::optional<float> gameAspect = GetGameViewAspectRatio(gameView, gameRequest))
					{
						aspectRatio = *gameAspect;
						const glm::uvec2 shown = context.GetViewport().GetGameViewSize();
						const std::optional<glm::uvec2> fixed = GetGameViewFixedSize(gameView, gameRequest);
						viewportSize = fixed ? *fixed : (shown.x > 0 && shown.y > 0 ? shown
							: glm::uvec2(EditorViewport::c_DefaultWidth, static_cast<uint32_t>(std::lround(EditorViewport::c_DefaultWidth / *gameAspect))));
					}
				}
				int64_t captureWidth = width;
				int64_t captureHeight = height;
				if (width == 0 && height == 0)
				{
					captureWidth = viewportSize.x > 0 ? viewportSize.x : EditorViewport::c_DefaultWidth;
					captureHeight = viewportSize.y > 0 ? viewportSize.y : EditorViewport::c_DefaultHeight;
				}
				else if (height == 0)
				{
					captureHeight = static_cast<int64_t>(std::lround(static_cast<double>(width) / aspectRatio));
				}
				else if (width == 0)
				{
					captureWidth = static_cast<int64_t>(std::lround(static_cast<double>(height) * aspectRatio));
				}
				const int64_t deviceLimit = static_cast<int64_t>(Renderer::GetGraphicsDevice().GetInfo().MaxTextureDimension2D);
				const int64_t limit = std::min(c_MaxCaptureSize, deviceLimit);
				// Larger images (a viewport on a very large display) shrink to the limit, keeping their shape.
				const int64_t largest = std::max(captureWidth, captureHeight);
				if (largest > limit)
				{
					const double scale = static_cast<double>(limit) / static_cast<double>(largest);
					captureWidth = static_cast<int64_t>(std::lround(static_cast<double>(captureWidth) * scale));
					captureHeight = static_cast<int64_t>(std::lround(static_cast<double>(captureHeight) * scale));
				}
				request.Size = glm::uvec2(static_cast<uint32_t>(std::clamp<int64_t>(captureWidth, 1, limit)), static_cast<uint32_t>(std::clamp<int64_t>(captureHeight, 1, limit)));

				// Polled from the next frame on: the first poll renders, the later ones wait for the readback and the jobs.
				auto progress = std::make_shared<CaptureProgress>();
				return EditorCommandResult::Defer([request, progress](EditorContext& context) -> std::optional<EditorCommandResult>
				{
					if (!progress->Readback && !progress->Encoded)
						return StartCapture(context, request, *progress);
					return ContinueCapture(request, *progress);
				});
			}, EditorCommandKind::Query }));
	}

}
