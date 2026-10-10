#pragma once

#include "Strata/Asset/Asset.h"
#include "Strata/Renderer/GraphicsDevice.h"
#include "Strata/Renderer/SceneRenderer.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

// The streaming stress project: a generated game project whose textures far exceed a residency budget, and a scripted
// camera sweep over it (the port of the streaming audit's generator). The world is a square of c_StressWorldSize units
// split into districts, one material per district, with the entities spread evenly over it and a terrain grid under
// them; a camera flying low over the districts sees a few of them at a time, so what it needs changes as it moves.
// Used by PerfGPU.Streaming (the full size, in a helper process) and GPU.Assets.Streaming (a small one, validated).
namespace Strata::Tests
{

	struct StressProjectSpec
	{
		uint32_t Textures2k = 16;     // Textures of SmallTextureSize^2
		uint32_t Textures4k = 1;      // Textures of LargeTextureSize^2
		uint32_t Materials = 17;      // One per district; material i uses texture i modulo the texture count
		uint32_t Entities = 2000;     // Cubes and spheres, spread evenly over the world
		uint32_t GridResolution = 512; // Vertices per side of the terrain grid (a glTF model)
		uint32_t SmallTextureSize = 2048;
		uint32_t LargeTextureSize = 4096;
	};

	constexpr float c_StressWorldSize = 2000.0f; // World units per side, centered on the origin

	// What WriteStressProject wrote. Handles are fixed, so every run produces the same project.
	struct StressProject
	{
		std::filesystem::path Directory;   // Holds Stress.stproj and Assets/
		AssetHandle WorldScene = UUID::Null();
		AssetHandle GridModel = UUID::Null();
		AssetHandle GridMesh = UUID::Null(); // The model's mesh (sub-asset), drawn as the terrain
		std::vector<AssetHandle> Textures;   // The small textures, then the large ones
		std::vector<AssetHandle> Materials;  // Material i belongs to district i
		uint64_t TextureBytes = 0;           // Every texture with its mip chain in RGBA8, as the GPU holds it
		uint64_t LargestTextureBytes = 0;
	};

	// Writes the project into `directory` (created if needed; it must not hold a project yet): noise-plus-gradient PNGs
	// (stb_image_write), one material per district, the terrain grid as glTF with an external buffer, the scene 'World'
	// (start scene) and the project file, every asset with its .meta. Nothing is imported. nullopt with the reason in
	// error on failure.
	std::optional<StressProject> WriteStressProject(const std::filesystem::path& directory, const StressProjectSpec& spec, std::string& error);

	// Imports the project like the editor does (EditorAssetManager::Scan, in parallel when the job system runs) and builds
	// its asset pack (BuildAssetPack). Fails when an asset does not import cleanly: an error, or a warning.
	bool BuildStressPack(const StressProject& project, const std::filesystem::path& packPath, std::string& error);

	// GPU bytes of a square RGBA8 texture of `size` with its full mip chain.
	uint64_t GetStressTextureBytes(uint32_t size);

	// The camera sweep: the camera holds above the first stop, then repeatedly moves to the next stop (MoveFrames,
	// easing in and out) and holds there (HoldFrames). Stops are district centers spread along a serpentine walk over the
	// district grid; the camera looks down at them at PitchDegrees from Height above the ground. Moves cross districts
	// that are seen for a few frames only.
	struct StressSweepPath
	{
		uint32_t Stops = 6;
		uint32_t MoveFrames = 40;
		uint32_t HoldFrames = 60;
		float Height = 150.0f;
		float PitchDegrees = 60.0f;
		float VerticalFOVDegrees = 60.0f;
		float FarClip = 1000.0f;

		uint32_t GetFrameCount() const { return Stops * (MoveFrames + HoldFrames); }
	};

	struct StressSweepFrame
	{
		SceneCamera Camera;
		uint32_t Stop = 0;          // The stop the camera moves to or holds at
		bool Holding = false;       // At rest at the stop
		uint32_t FramesHolding = 0; // Frames at rest at the stop before this one
	};

	// The camera of a frame of the sweep (frames past the end hold at the last stop).
	StressSweepFrame GetStressSweepFrame(const StressProjectSpec& spec, const StressSweepPath& path, uint32_t frame, float aspectRatio);

	struct StreamingSweepSettings
	{
		std::filesystem::path Pack;      // Built from the stress project (EditorAssetManager::BuildAssetPack)
		AssetHandle Scene = UUID::Null(); // The world scene
		StressProjectSpec Spec;          // The project's spec (district layout of the camera path)
		StressSweepPath Path;
		uint64_t TextureBudget = 0;      // AssetResidencyBudgets::GpuTextures of the run
		uint32_t Width = 1280;
		uint32_t Height = 720;
		// Frames per second the sweep runs at, like a game with vsync (0: as fast as possible). Loads take time, not frames,
		// so how many frames a stop takes to settle depends on it.
		uint32_t FrameRate = 60;
	};

	// StreamingSweepResult::FramesToSettle of a stop where the camera never drew without pending assets.
	constexpr uint32_t c_StopNeverSettled = std::numeric_limits<uint32_t>::max();

	// What a sweep measured. Frames run like the application's: the device's frame, the asset manager's Update (finalizing
	// and evicting), then the scene renders from the sweep's camera, requesting what it shows.
	struct StreamingSweepResult
	{
		bool Completed = false;          // Every frame ran (else Error says why)
		std::string Error;
		bool ValidationEnabled = false;  // The device ran the validation layers
		uint32_t Frames = 0;
		uint64_t TextureBudget = 0;
		uint64_t MaxResidentTextureBytes = 0; // Resident GPU texture bytes after the manager's Update, at most
		uint64_t MaxInFlightBytes = 0;        // The streaming queue's high-water mark
		uint64_t MaxUploadedBytes = 0;        // GPU bytes finalized in one frame, at most
		float MaxFinalizeMs = 0.0f;           // Finalization time of one frame, at most
		// Per stop: frames from the first frame at rest until the first frame that drew without pending assets (0 when that
		// was the first frame at rest), or c_StopNeverSettled when no frame of the stop's hold did.
		std::vector<uint32_t> FramesToSettle;
		uint32_t FailedAssets = 0;
		uint64_t LoadsCompleted = 0;
		uint64_t Evictions = 0;
		uint64_t StagingReleases = 0;
		uint64_t PeakPrivateBytes = 0;   // The process's peak (Platform::GetProcessMemory) at the end of the sweep; 0 if unknown
		uint64_t PeakWorkingSetBytes = 0; // The process's peak working set (resident RAM) at the end of the sweep; 0 if unknown
		uint32_t NewErrors = 0;          // Errors the device reported during the sweep (validation messages among them)
		// Per frame, for diagnostics: resident GPU texture bytes, pending assets drawn, finalization time, private bytes and
		// device memory in use (GraphicsDevice::GetMemoryBudget().Usage).
		std::vector<uint64_t> ResidentTextureBytes;
		std::vector<uint32_t> PendingAssets;
		std::vector<float> FinalizeMs;
		std::vector<uint64_t> PrivateBytes;
		std::vector<uint64_t> DeviceBytes;
	};

	// Runs the sweep on the device (whose renderer must be initialized) with a RuntimeAssetManager of the pack, made the
	// active manager for the duration. Main thread; the caller decides whether the job system runs.
	StreamingSweepResult RunStreamingSweep(GraphicsDevice& device, const StreamingSweepSettings& settings);

	// The settings and the result cross the process boundary of the helper process as JSON. The readers return nullopt with
	// the reason in error for anything malformed.
	nlohmann::json ToJson(const StreamingSweepSettings& settings);
	std::optional<StreamingSweepSettings> StreamingSweepSettingsFromJson(const nlohmann::json& json, std::string& error);
	nlohmann::json ToJson(const StreamingSweepResult& result);
	std::optional<StreamingSweepResult> StreamingSweepResultFromJson(const nlohmann::json& json, std::string& error);

	// The test executable as a helper process (--strata-test-helper=streaming-sweep <settings file> <result file>):
	// initializes the job system and a device like an application, runs the sweep the settings (ToJson) describe and writes
	// the result as JSON. Peak memory is a property of the whole process, so the perf test measures it here, apart from the
	// import and pack build that precede it. Returns the exit code (0 when the result file was written).
	int RunStreamingSweepProcess(int argc, char** argv);

}
