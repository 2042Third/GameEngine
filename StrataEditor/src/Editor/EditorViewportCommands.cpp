#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"

#include <Strata/Core/Base64.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Renderer/ImageWriter.h>
#include <Strata/Renderer/Renderer.h>
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

		nlohmann::json RangeSchema(std::string description, double minimum, double maximum)
		{
			return { { "type", "number" }, { "description", std::move(description) }, { "minimum", minimum }, { "maximum", maximum } };
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

		struct CaptureRequest
		{
			glm::uvec2 Size = { 0, 0 };
			ViewportCameraSource Camera = ViewportCameraSource::Automatic;
			std::optional<bool> Overlays;
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
			uint32_t PendingAssets = 0;
			std::chrono::steady_clock::time_point Submitted;
			Ref<EncodedCapture> Encoded; // Set once the pixels are on the CPU
			JobHandle EncodeJob;
			JobHandle SaveJob;
			bool SaveStarted = false;
		};

		// First poll (the frame after the command): renders the image and starts reading it back.
		std::optional<EditorCommandResult> StartCapture(EditorContext& context, const CaptureRequest& request, CaptureProgress& progress)
		{
			EditorViewport& viewport = context.GetViewport();
			ViewportRenderer* renderer = viewport.GetCaptureRenderer();
			if (!renderer)
				return EditorCommandResult::Fail("viewport.capture needs a GPU: the renderer is not running");

			std::string error;
			const float aspectRatio = static_cast<float>(request.Size.x) / static_cast<float>(request.Size.y);
			std::optional<ViewportView> view = ResolveViewportView(context, request.Camera, aspectRatio, &error);
			if (!view)
				return EditorCommandResult::Fail(error);
			progress.Overlays = request.Overlays.value_or(view->EditorOverlays);

			// A capture is a single frame: its exposure is metered from this frame alone, not adapted from an earlier one.
			renderer->GetSceneRenderer().ResetExposureAdaptation();
			if (!renderer->Render(context, request.Size, *view, viewport.GetSettings(), progress.Overlays))
				return EditorCommandResult::Fail("Rendering the capture failed (see the log)");
			progress.Readback = TextureReadback::Create(renderer->GetOutputTexture(), {}, &error);
			progress.PendingAssets = renderer->GetStats().PendingAssets;
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
				{ "camera", progress.View.FromScene ? "scene" : "editor" },
				{ "overlays", progress.Overlays },
				{ "pendingAssets", progress.PendingAssets }
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
			} });

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
			} });

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
			} });

		////////////////////////////////////////////////////////////////////////////////
		// Capture
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "viewport.capture",
			"Renders the scene on the next frame and returns the picture as a PNG image, with its width and height. By default it looks like the viewport: "
			"its size (1280x720 while the viewport is hidden), the scene's primary camera while playing (the editor camera otherwise) and the editor overlays "
			"(grid, selection outline, light/camera/collider shapes) for the editor camera. Also reports how many assets were still loading (call editor.wait "
			"and capture again to see them). Needs a GPU (fails in editors started with --no-gpu).",
			ObjectSchema({
				{ "width", IntegerSchema("Image width in pixels; without height the viewport's aspect ratio is kept", c_MinCaptureSize, c_MaxCaptureSize) },
				{ "height", IntegerSchema("Image height in pixels; without width the viewport's aspect ratio is kept", c_MinCaptureSize, c_MaxCaptureSize) },
				{ "camera", { { "type", "string" }, { "enum", { "editor", "scene" } },
					{ "description", "\"editor\": the editor camera; \"scene\": the scene's primary camera (fails without one)" } } },
				{ "overlays", BoolSchema("Draw the editor overlays enabled in the viewport (default: on for the editor camera, off for the scene camera)") },
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

				// The size: as given, the missing side from the viewport's aspect ratio, or the viewport's own size.
				const float aspectRatio = context.GetViewport().GetAspectRatio();
				const glm::uvec2 viewportSize = context.GetViewport().GetSize();
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
			} });
	}

}
