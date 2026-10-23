#include "Editor/AssetFileAction.h"
#include "Editor/AssetReferences.h"
#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/ProbeBake.h"
#include "Editor/SceneEdit.h"
#include "Editor/UndoStack.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Project/Project.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/ImageWriter.h>
#include <Strata/Renderer/Renderer.h>
#include <Strata/Renderer/RendererComponents.h>
#include <Strata/Renderer/SceneRenderer.h>
#include <Strata/Renderer/TextureReadback.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Scene.h>

#include <glm/gtc/packing.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

// lighting.bakeProbes: reflection probes' captures rendered from the edited scene (ProbeBake.h), written as HDR textures of
// the project and set as the probes' Capture, in one undo step.

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		// Frames a probe's views may render again while what they show is still loading (every level of every texture: the
		// bake's renderer streams nothing), shadow views or other probes' captures are pending, or animators lag their pose.
		constexpr uint32_t c_MaxBakeWaitFrames = 300;
		// A bake whose GPU readbacks have not finished after this long fails (a lost or hung device).
		constexpr std::chrono::seconds c_BakeReadbackTimeout { 30 };

		// A probe's capture on its way from the GPU to a file (shared with the jobs that make it).
		struct BakeFile
		{
			std::array<ReadbackImage, 6> Faces;
			std::vector<uint8_t> Bytes;
			std::filesystem::path Staging; // The file the import copies from
			std::string Error;
		};

		struct BakeProbe
		{
			UUID Entity;
			std::string Name;
			glm::vec3 Position = glm::vec3(0.0f);
			bool Complete = true; // Its views waited for nothing beyond c_MaxBakeWaitFrames
			std::array<Scope<TextureReadback>, 6> Readbacks;
			Ref<BakeFile> File;
			JobHandle EncodeJob;
			AssetHandle Capture = 0; // The imported texture
			std::string Path;
		};

		enum class BakeStage : uint8_t
		{
			Render = 0, // A probe's six views per frame, until each probe's are complete
			Encode,     // Readbacks, then the equirectangular images assembled and encoded on jobs
			Write,      // The staging files written on an I/O job
			Import,     // Copied into the project and imported in the background (EditorAssetManager::ImportExternal)
		};

		struct BakeState
		{
			std::weak_ptr<Scene> EditScene; // The scene the bake renders and changes
			std::weak_ptr<AssetManagerBase> Assets;
			std::string TargetDirectory; // "Lighting/<scene>"
			std::filesystem::path StagingDirectory;
			Scope<SceneRenderer> Renderer;
			float PreExposure = 1.0f;
			glm::vec4 ClearColor = glm::vec4(0.0f);
			std::vector<BakeProbe> Probes;
			size_t NextProbe = 0;
			uint32_t WaitFrames = 0;
			BakeStage Stage = BakeStage::Render;
			std::chrono::steady_clock::time_point Submitted;
			JobHandle WriteJob;
			Ref<ExternalImport> Import;

			BakeState() = default;
			BakeState(const BakeState&) = delete;
			BakeState& operator=(const BakeState&) = delete;
			// Whatever ends the bake: its renderer and its staging files go with it (ReleaseBake).
			~BakeState();
		};

		// Lets go of the bake's renderer (its targets once the GPU is done with them) and of its staging files, on an I/O job.
		void ReleaseBake(BakeState& state)
		{
			if (state.Renderer)
				state.Renderer->SetViewportSize(0, 0);
			state.Renderer.reset();
			if (!state.StagingDirectory.empty())
			{
				JobSystem::SubmitIO([directory = state.StagingDirectory]()
				{
					std::error_code error;
					std::filesystem::remove_all(directory, error); // A leftover staging folder holds nothing the project uses
				});
				state.StagingDirectory.clear();
			}
		}

		BakeState::~BakeState()
		{
			ReleaseBake(*this);
		}

		// The folder of the scene's captures: "Lighting/<the scene file's name>", or its name until it is saved.
		std::string GetCaptureDirectory(EditorContext& context, const Scene& scene)
		{
			std::string name;
			if (const std::optional<AssetMetadata> metadata = context.GetAssetManager()->GetMetadata(context.GetSceneHandle()))
				name = FileSystem::ToUTF8(FileSystem::FromUTF8(metadata->Path).stem());
			if (name.empty())
			{
				for (const char character : scene.GetName())
				{
					const bool safe = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9')
						|| character == ' ' || character == '-' || character == '_';
					name.push_back(safe ? character : '_');
				}
			}
			return "Lighting/" + (name.empty() ? std::string("Scene") : name);
		}

		// Whether `path` is a capture an earlier bake made for the probe: "<directory>/<id>.hdr" or "<directory>/<id> (n).hdr".
		bool IsBakedCaptureOf(const std::string& path, const std::string& directory, const UUID& probe)
		{
			const std::string prefix = directory + "/" + probe.ToString();
			if (!path.starts_with(prefix) || !path.ends_with(".hdr"))
				return false;
			const std::string rest = path.substr(prefix.size(), path.size() - prefix.size() - 4);
			if (rest.empty())
				return true;
			if (rest.size() < 4 || !rest.starts_with(" (") || !rest.ends_with(")"))
				return false;
			for (size_t index = 2; index + 1 < rest.size(); index++)
			{
				if (rest[index] < '0' || rest[index] > '9')
					return false;
			}
			return true;
		}

		// The six views of the next probe: complete, or rendered again on the next frame while what they show loads.
		std::optional<EditorCommandResult> RenderNextProbe(BakeState& state, Scene& scene)
		{
			BakeProbe& probe = state.Probes[state.NextProbe];
			SceneRenderOptions options;
			options.HDROutput = true;
			options.PreExposure = state.PreExposure;
			bool complete = true;
			for (uint32_t face = 0; face < 6; face++)
			{
				SceneCamera camera = ProbeBake::GetFaceCamera(probe.Position, face);
				camera.ClearColor = state.ClearColor;
				if (!state.Renderer->Render(scene, camera, nullptr, options))
					return EditorCommandResult::Fail(fmt::format("Rendering what '{}' sees failed (the log says why)", probe.Name));
				const SceneRendererStats& stats = state.Renderer->GetStats();
				complete = complete && stats.PendingAssets == 0 && stats.DeferredProxies == 0 && stats.LocalShadowViewsPending == 0
					&& stats.ReflectionProbesPending == 0 && stats.StalePoses == 0;
				std::string error;
				probe.Readbacks[face] = TextureReadback::Create(state.Renderer->GetHDRTexture(), {}, &error);
				if (!probe.Readbacks[face])
					return EditorCommandResult::Fail(fmt::format("Reading what '{}' sees back failed: {}", probe.Name, error));
			}
			if (!complete && ++state.WaitFrames < c_MaxBakeWaitFrames)
			{
				probe.Readbacks = {};
				return std::nullopt;
			}
			probe.Complete = complete;
			state.WaitFrames = 0;
			if (++state.NextProbe == state.Probes.size())
			{
				// Every view is on its way back: the renderer is done.
				state.Renderer->SetViewportSize(0, 0);
				state.Renderer.reset();
				state.Stage = BakeStage::Encode;
				state.Submitted = std::chrono::steady_clock::now();
			}
			return std::nullopt;
		}

		// The readbacks, then each probe's image assembled and encoded on a job (ProbeBake::AssembleEquirect, ImageWriter::EncodeHDR).
		std::optional<EditorCommandResult> EncodeCaptures(BakeState& state)
		{
			bool done = true;
			for (BakeProbe& probe : state.Probes)
			{
				if (probe.File)
				{
					done = done && probe.EncodeJob.IsComplete();
					continue;
				}
				done = false;
				bool ready = true;
				for (const Scope<TextureReadback>& readback : probe.Readbacks)
					ready = ready && readback->IsReady();
				if (!ready)
				{
					if (std::chrono::steady_clock::now() - state.Submitted > c_BakeReadbackTimeout)
						return EditorCommandResult::Fail(fmt::format("The GPU did not finish the bake within {} seconds", c_BakeReadbackTimeout.count()));
					continue;
				}
				Ref<BakeFile> file = CreateRef<BakeFile>();
				for (uint32_t face = 0; face < 6; face++)
				{
					if (!probe.Readbacks[face]->GetResult(file->Faces[face]))
						return EditorCommandResult::Fail(fmt::format("Reading what '{}' sees back failed: the staging memory could not be mapped", probe.Name));
				}
				probe.Readbacks = {};
				probe.File = file;
				const float scale = 1.0f / state.PreExposure;
				probe.EncodeJob = JobSystem::Submit([file, scale]()
				{
					std::array<std::vector<float>, 6> faces;
					for (uint32_t face = 0; face < 6; face++)
					{
						const ReadbackImage& image = file->Faces[face];
						if (image.Format != nvrhi::Format::RGBA16_FLOAT || image.Width != ProbeBake::c_FaceSize || image.Height != ProbeBake::c_FaceSize)
						{
							file->Error = "a view came back in another format or size";
							return;
						}
						faces[face].resize(static_cast<size_t>(image.Width) * image.Height * 3);
						for (size_t texel = 0; texel < static_cast<size_t>(image.Width) * image.Height; texel++)
						{
							uint16_t half[4];
							std::memcpy(half, image.Pixels.data() + texel * sizeof(half), sizeof(half));
							for (int channel = 0; channel < 3; channel++)
								faces[face][texel * 3 + channel] = glm::unpackHalf1x16(half[channel]);
						}
						file->Faces[face] = {};
					}
					const std::vector<float> image = ProbeBake::AssembleEquirect(faces, scale);
					std::string error;
					std::optional<std::vector<uint8_t>> bytes = ImageWriter::EncodeHDR(ProbeBake::c_CaptureWidth, ProbeBake::c_CaptureHeight, image, &error);
					if (!bytes)
						file->Error = error;
					else
						file->Bytes = std::move(*bytes);
				});
			}
			if (!done)
				return std::nullopt;
			for (const BakeProbe& probe : state.Probes)
			{
				if (!probe.File->Error.empty())
					return EditorCommandResult::Fail(fmt::format("Encoding the capture of '{}' failed: {}", probe.Name, probe.File->Error));
			}
			// The staging files, named after the probes, on an I/O job.
			std::vector<Ref<BakeFile>> files;
			for (BakeProbe& probe : state.Probes)
			{
				probe.File->Staging = state.StagingDirectory / FileSystem::FromUTF8(probe.Entity.ToString() + ".hdr");
				files.push_back(probe.File);
			}
			state.WriteJob = JobSystem::SubmitIO([files, directory = state.StagingDirectory]()
			{
				if (!FileSystem::CreateDirectories(directory))
				{
					for (const Ref<BakeFile>& file : files)
						file->Error = fmt::format("the folder '{}' could not be created", FileSystem::ToUTF8(directory));
					return;
				}
				for (const Ref<BakeFile>& file : files)
				{
					if (!FileSystem::WriteBytes(file->Staging, file->Bytes))
						file->Error = fmt::format("'{}' could not be written", FileSystem::ToUTF8(file->Staging));
					file->Bytes = {};
				}
			});
			state.Stage = BakeStage::Write;
			return std::nullopt;
		}

		// Moves the files the import brought to the trash again (a bake that cannot be applied leaves nothing behind).
		void DiscardImported(EditorAssetManager& assets, const ExternalImport& import)
		{
			for (const ExternalImportItem& item : import.GetItems())
			{
				if (!item.Error.empty() || item.Path.empty() || !assets.GetMetadata(assets.FindAssetByPath(item.Path)))
					continue;
				TrashedAssets trashed;
				std::string error;
				if (!assets.MoveToTrash(item.Path, trashed, &error))
					ST_WARN("lighting.bakeProbes: '{}' stays in the project: {}", item.Path, error);
			}
		}

		// The new captures become the probes' Capture, the files earlier bakes made for them go to the trash when nothing else
		// uses them: one undo step.
		EditorCommandResult ApplyBake(EditorContext& context, BakeState& state, Scene& scene, EditorAssetManager& assets)
		{
			Scope<EditorActionGroup> step = CreateScope<EditorActionGroup>("Bake Reflection Probes");
			for (const BakeProbe& probe : state.Probes)
				step->Add(CreateScope<AssetCreateAction>(assets, fmt::format("Create {}", probe.Path), probe.Capture, probe.Path));

			SceneEditTransaction transaction(scene, "Bake Reflection Probes", {});
			std::vector<AssetHandle> previous(state.Probes.size(), UUID::Null());
			for (size_t index = 0; index < state.Probes.size(); index++)
			{
				const BakeProbe& probe = state.Probes[index];
				Entity entity = scene.GetEntityByUUID(probe.Entity);
				if (!entity || !entity.HasComponent<ReflectionProbeComponent>())
					continue; // Deleted while it baked: its file stays, as any imported texture
				transaction.Track(probe.Entity);
				ReflectionProbeComponent& component = entity.GetComponent<ReflectionProbeComponent>();
				previous[index] = component.Capture;
				component.Capture = probe.Capture;
				entity.MarkModified<ReflectionProbeComponent>();
			}
			context.UpdatePrefabOverrides(transaction);
			step->Add(transaction.TakeAction());

			nlohmann::json probes = nlohmann::json::array();
			for (size_t index = 0; index < state.Probes.size(); index++)
			{
				const BakeProbe& probe = state.Probes[index];
				nlohmann::json entry = { { "entity", UUIDToJson(probe.Entity) }, { "name", probe.Name }, { "capture", UUIDToJson(probe.Capture) },
					{ "path", probe.Path }, { "complete", probe.Complete }, { "previous", nullptr }, { "trashed", false } };
				const AssetHandle old = previous[index];
				const std::optional<AssetMetadata> oldMetadata = old.IsValid() ? assets.GetMetadata(old) : std::nullopt;
				if (oldMetadata)
				{
					entry["previous"] = UUIDToJson(old);
					// Only a file an earlier bake made for this probe, and only once nothing uses it any more: the open scene counts
					// as it is in memory, which names the new capture now (its saved file names the earlier one until its next save);
					// any other texture the probe named stays as it is.
					std::vector<AssetReferencer> users;
					const bool baked = IsBakedCaptureOf(oldMetadata->Path, state.TargetDirectory, probe.Entity);
					if (baked)
					{
						users = AssetReferences::Find(context, AssetReferences::WithSubAssets(context, { old }), {});
						AssetReferences::IgnoreSavedOpenScene(users);
					}
					if (baked && users.empty())
					{
						TrashedAssets trashed;
						std::string error;
						if (assets.MoveToTrash(oldMetadata->Path, trashed, &error))
						{
							step->Add(CreateScope<AssetTrashAction>(context, assets, fmt::format("Delete {}", oldMetadata->Path), trashed, false, true));
							entry["trashed"] = true;
						}
						else
						{
							ST_WARN("lighting.bakeProbes: the previous capture '{}' stays: {}", oldMetadata->Path, error);
						}
					}
				}
				probes.push_back(std::move(entry));
			}
			context.GetUndoStack().Record(std::move(step));
			return EditorCommandResult::Ok({ { "directory", state.TargetDirectory }, { "probes", std::move(probes) } });
		}

		std::optional<EditorCommandResult> PollBake(EditorContext& context, BakeState& state)
		{
			const Ref<Scene> scene = state.EditScene.lock();
			const Ref<AssetManagerBase> manager = state.Assets.lock();
			EditorAssetManager* assets = context.GetAssetManager();
			const bool sameScene = scene && scene == context.GetEditScene() && manager && manager.get() == assets;
			// Until the import, nothing is in the project: a changed scene or Play ends the bake without a trace.
			if (state.Stage != BakeStage::Import && (!sameScene || context.IsPlaying()))
			{
				return EditorCommandResult::Fail(!sameScene ? "The scene changed before the bake finished; nothing was changed"
					: "Play started before the bake finished; nothing was changed (bake again once stopped)");
			}

			switch (state.Stage)
			{
				case BakeStage::Render:
					return RenderNextProbe(state, *scene);
				case BakeStage::Encode:
					return EncodeCaptures(state);
				case BakeStage::Write:
				{
					if (!state.WriteJob.IsComplete())
						return std::nullopt;
					ExternalImportRequest request;
					for (const BakeProbe& probe : state.Probes)
					{
						if (!probe.File->Error.empty())
							return EditorCommandResult::Fail(fmt::format("Writing the capture of '{}' failed: {}", probe.Name, probe.File->Error));
						request.Files.push_back(probe.File->Staging);
					}
					request.TargetDirectory = state.TargetDirectory;
					std::string error;
					state.Import = assets->ImportExternal(request, &error);
					if (!state.Import)
						return EditorCommandResult::Fail(fmt::format("Importing the captures failed: {}", error));
					state.Stage = BakeStage::Import;
					return std::nullopt;
				}
				case BakeStage::Import:
				{
					if (!manager || manager.get() != assets)
						return EditorCommandResult::Fail("The project was closed before the bake finished");
					if (!state.Import->IsFinished())
						return std::nullopt;
					if (!state.Import->GetError().empty())
						return EditorCommandResult::Fail(fmt::format("Importing the captures failed: {}", state.Import->GetError()));
					const std::vector<ExternalImportItem>& items = state.Import->GetItems();
					std::string failure;
					for (size_t index = 0; index < items.size() && failure.empty(); index++)
					{
						if (!items[index].Error.empty() || items[index].Assets.empty())
							failure = fmt::format("Importing the capture of '{}' failed: {}", state.Probes[index].Name, items[index].Error);
					}
					std::string editable;
					if (failure.empty() && (!sameScene || context.IsPlaying() || !context.CheckSceneEditable(&editable)))
					{
						failure = !sameScene ? "The scene changed before the bake finished; nothing was changed"
							: (context.IsPlaying() ? "Play started before the bake finished; nothing was changed (bake again once stopped)" : editable);
					}
					if (!failure.empty())
					{
						DiscardImported(*assets, *state.Import);
						return EditorCommandResult::Fail(failure);
					}
					for (size_t index = 0; index < items.size(); index++)
					{
						state.Probes[index].Capture = items[index].Assets.front();
						state.Probes[index].Path = items[index].Path;
					}
					return ApplyBake(context, state, *scene, *assets);
				}
			}
			return EditorCommandResult::Fail("The bake is in an unknown state");
		}

	}

	void RegisterLightingCommands(EditorCommandRegistry& registry)
	{
		registry.Register(MayDefer({ "lighting.bakeProbes",
			"Bakes reflection probes: renders what each one sees from its position (the edited scene as it is: lights, sky, fog and the "
			"other probes' current captures, so baking twice adds a second bounce) into a new equirectangular HDR texture, "
			"Assets/Lighting/<scene>/<probe id>.hdr (' (n)' added when the name is taken), and sets the probe's Capture to it. The capture "
			"an earlier bake made for the probe goes to the trash once nothing else uses it (the open scene as it is in memory, even while its "
			"saved file still names it); any other texture Capture named stays. One undo "
			"step for the files and the captures. Answers once the textures are imported (a few frames; the editor keeps running). Needs a "
			"GPU; refused while playing (it renders and writes from the edited scene). Returns {directory, probes: [{entity, name, capture, "
			"path, complete (false when something it sees was still loading after 300 frames), previous (the capture before, or null), "
			"trashed}]}.",
			ObjectSchema({ { "entities", EntityArraySchema("Probes to bake (entities with a ReflectionProbe component); default: every active "
				"probe of the scene") } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				std::string error;
				EditorAssetManager* assets = context.GetAssetManager();
				if (!assets)
					return EditorCommandResult::Fail("No project is open (project.open or project.create)");
				if (context.IsPlaying())
					return EditorCommandResult::Fail("Stop playing first: lighting.bakeProbes renders and writes from the edited scene");
				if (!context.CheckSceneEditable(&error))
					return EditorCommandResult::Fail(std::move(error));
				const Ref<Scene>& scene = context.GetEditScene();
				CommandArguments arguments(parameters);
				std::vector<Entity> entities;
				if (arguments.Has("entities"))
				{
					// A probe named twice is baked once (twice would import its capture twice and trash the first copy in the step).
					std::unordered_set<UUID> named;
					for (const Entity entity : arguments.GetEntities(*scene, "entities"))
					{
						if (arguments.IsValid() && !entity.HasComponent<ReflectionProbeComponent>())
							arguments.SetError(fmt::format("Parameter 'entities': '{}' has no ReflectionProbe component", entity.GetName()));
						if (named.insert(entity.GetUUID()).second)
							entities.push_back(entity);
					}
				}
				else
				{
					for (auto [handle, probe, world] : scene->GetRegistry().view<ReflectionProbeComponent, WorldTransformComponent>().each())
					{
						if (world.ActiveInHierarchy)
							entities.push_back(Entity(handle, scene.get()));
					}
				}
				if (!arguments.IsValid())
					return arguments.Fail();
				if (entities.empty())
					return EditorCommandResult::Fail("The scene has no active reflection probe (add a ReflectionProbe component where a viewer stands)");
				if (!Renderer::IsInitialized())
					return EditorCommandResult::Fail("lighting.bakeProbes needs a GPU: the renderer is not running");

				Ref<BakeState> state = CreateRef<BakeState>();
				state->EditScene = scene;
				state->Assets = assets->weak_from_this();
				state->TargetDirectory = GetCaptureDirectory(context, *scene);
				state->StagingDirectory = context.GetProject()->GetIntermediateDirectory() / "Bake" / FileSystem::FromUTF8(UUID().ToString());
				state->PreExposure = ProbeBake::ChoosePreExposure(*scene);
				// The background where nothing is drawn and no sky shows: the scene camera's, as the game shows it.
				const Entity camera = scene->GetPrimaryCameraEntity();
				state->ClearColor = camera ? SceneCamera::FromEntity(*scene, camera, 1.0f).ClearColor : SceneCamera().ClearColor;
				for (const Entity entity : entities)
				{
					BakeProbe probe;
					probe.Entity = entity.GetUUID();
					probe.Name = entity.GetName();
					probe.Position = glm::vec3(entity.GetComponent<WorldTransformComponent>().Matrix[3]);
					state->Probes.push_back(std::move(probe));
				}
				state->Renderer = CreateScope<SceneRenderer>(ProbeBake::GetRendererSpecification());
				state->Renderer->SetViewportSize(ProbeBake::c_FaceSize, ProbeBake::c_FaceSize);
				// The poll owns the state: the bake's renderer and staging files go with it, whatever ends it.
				return EditorCommandResult::Defer([state](EditorContext& pollContext) -> std::optional<EditorCommandResult>
				{
					return PollBake(pollContext, *state);
				});
			} }));
	}

}
