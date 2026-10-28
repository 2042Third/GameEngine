#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Renderer/SceneRenderData.h"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Strata
{

	struct SettingsSection;

	// The presets of RendererQualitySettings. The numbers are stored and passed by scripts (graphics options): never
	// renumber them; a new preset takes the next number.
	enum class RendererQualityPreset : uint8_t
	{
		Low = 0,
		Medium = 1, // Strata 0.6.0's costs; SceneRendererSpecification's default
		High = 2,   // The programs' default (the Renderer.Quality section's)
		Ultra = 3,
		Custom = 4  // Values of the section's fields
	};
	constexpr size_t c_RendererQualityPresetCount = 4; // Low to Ultra: the presets with values of their own

	// The resolution of each sun shadow cascade: Auto and AutoFine follow the output's rows
	// (RendererQuality::ResolveShadowMapSize), the others are fixed.
	enum class ShadowMapResolution : uint8_t
	{
		Auto = 0, // 2048 below RendererQuality::c_HighResolutionRows output rows, 4096 from them
		AutoFine, // 4096 below, 8192 from them
		Size1024,
		Size2048,
		Size4096,
		Size8192
	};

	// The motion blur's gather samples (RendererQualitySettings::MotionBlurQuality); Off turns the passes off. File and script
	// values by name.
	enum class MotionBlurSamples : uint8_t
	{
		Off = 0,
		Samples8,
		Samples12,
		Samples16
	};

	// The players' anti-aliasing (RendererQualitySettings::AntiAliasing): the scene's method, or one they prefer.
	enum class AntiAliasingChoice : uint8_t
	{
		Scene = 0, // The Post Process's
		Off,
		FXAA,
		TAA
	};

	// A players' comfort row: the scene's effect, or off (players may turn off, never on, what a scene uses).
	enum class SceneEffectChoice : uint8_t
	{
		Scene = 0,
		Off
	};

	// The costly knobs of a renderer: what a person, project or player chooses for a machine, never a scene's look (that
	// stays on lights, Post Process and the sky). The defaults are Medium, 0.6.0's costs. Docs/DeveloperGuide.md,
	// "Quality settings": a row of the presets per knob, and what adding one involves.
	struct RendererQualitySettings
	{
		RendererQualityPreset Preset = RendererQualityPreset::Medium; // What these values are; reported, never compared
		// Texels each way of each directional shadow cascade.
		ShadowMapResolution ShadowResolution = ShadowMapResolution::Size2048;
		uint32_t ShadowCascades = 4; // 1 to RenderData::c_MaxCascades
		// Point and spot lights with CastShadows: the renderer's depth atlas of this many texels each way (a power of two from
		// 512 to 8192; made when the first such light is drawn, released with the render targets: 64 MiB at 4096). The
		// MaxShadowedLocalLights most relevant of them in view (at most RenderData::c_MaxShadowedLocalLights; ranked as the
		// light clusters rank lights) get tiles of it, a spot light one, a point light six (the faces of a cube), of 128 to
		// LocalShadowMaxTile texels following how large the light's range looks on screen; when they do not fit, the less
		// relevant lights' tiles get smaller first (SceneRendererStats::LocalShadowLights, LocalShadowTileSizes).
		uint32_t LocalShadowAtlasSize = 4096;
		uint32_t LocalShadowMaxTile = 1024; // A power of two from 128 to the atlas size
		uint32_t MaxShadowedLocalLights = RenderData::c_MaxShadowedLocalLights;
		// Levels of detail with errors (MeshLOD::Error, which generated levels have): the most simplified level whose error,
		// projected onto the screen, spans at most this many pixels of the viewport height is drawn. 0 draws full detail.
		float LODErrorPixels = 1.0f;
		// Mesh renderers whose projected size (bounding sphere diameter) is below this many pixels of the viewport height are
		// not drawn in the view, by either path: they add moire and GPU time, not information. 0 draws them all. Shadow
		// cascades leave out casters below a texel (SceneRendererStats::SubTexelCasters).
		float MinProjectedPixels = 0.5f;
		// The motion blur's gather samples, for scenes whose Post Process turns it on (8 at Medium; Off at Low turns it off).
		MotionBlurSamples MotionBlurQuality = MotionBlurSamples::Samples8;
		// The players' rows, the same under every preset (comfort and preference, not cost): the anti-aliasing method (the
		// scene's, or off, FXAA or TAA) and whether the scene's motion blur and its cameras' depth of field show.
		AntiAliasingChoice AntiAliasing = AntiAliasingChoice::Scene;
		SceneEffectChoice MotionBlur = SceneEffectChoice::Scene;
		SceneEffectChoice DepthOfField = SceneEffectChoice::Scene;

		// Every member equal, Preset included.
		bool operator==(const RendererQualitySettings& other) const = default;
		// Every knob equal (Preset is a label): copies whose Preset is made equal, compared with operator==, so a knob a
		// stream appends needs no line here.
		bool HasSameKnobs(const RendererQualitySettings& other) const;
	};

	namespace RendererQuality
	{
		constexpr std::string_view c_SectionId = "Renderer.Quality";
		constexpr uint32_t c_SectionVersion = 2; // Each landing of a row adds one (Docs/DeveloperGuide.md, "Quality settings")
		constexpr uint32_t c_HighResolutionRows = 1440;
		constexpr uint32_t c_MaxShadowMapSize = 8192; // The most texels each way of a cascade (SceneRenderer applies it)

		// A preset's values, labeled with it. Custom: High's values labeled Custom (the section's field defaults).
		RendererQualitySettings GetPreset(RendererQualityPreset preset);
		// The knobs within their ranges (what SceneRenderer applies): cascades clamped, sizes floored to powers of two
		// within their ranges, the tile at most the atlas, non-finite or negative pixels replaced by Medium's.
		RendererQualitySettings Clamp(const RendererQualitySettings& settings);
		// The cascades' texels each way for an output of `outputRows` rows (0: below the threshold).
		uint32_t ResolveShadowMapSize(ShadowMapResolution resolution, uint32_t outputRows);

		const char* PresetToString(RendererQualityPreset preset); // "Low" ... "Custom"
		std::optional<RendererQualityPreset> PresetFromString(std::string_view name, bool ignoreCase = false);
		const char* ShadowResolutionToString(ShadowMapResolution resolution); // "Auto", "AutoFine", "1024" ...
		std::optional<ShadowMapResolution> ShadowResolutionFromString(std::string_view name);
		const char* MotionBlurSamplesToString(MotionBlurSamples samples); // "Off", "Samples8" ...
		std::optional<MotionBlurSamples> MotionBlurSamplesFromString(std::string_view name);
		// The gather's samples of a row value (0 for Off).
		uint32_t GetMotionBlurSampleCount(MotionBlurSamples samples);
		const char* AntiAliasingChoiceToString(AntiAliasingChoice choice); // "Scene", "Off", "FXAA", "TAA"
		std::optional<AntiAliasingChoice> AntiAliasingChoiceFromString(std::string_view name);
		const char* SceneEffectChoiceToString(SceneEffectChoice choice); // "Scene", "Off"
		std::optional<SceneEffectChoice> SceneEffectChoiceFromString(std::string_view name);

		// The Renderer.Quality section (simple; registered by RegisterRendererModule).
		SettingsSection MakeSection();
		// The settings a validated value of the section stands for (null: the section's default, High): a preset's values,
		// or the fields' with Preset Custom.
		RendererQualitySettings FromSectionValue(const nlohmann::json* value);
		// A value of the section for these settings: their Preset and every field (a preset's own fields hold its values).
		nlohmann::json ToSectionValue(const RendererQualitySettings& settings);
		// The project's quality: the section read through IProjectSettings, kept until its version changes (a project that
		// does not set the section: its default, High). Medium, SceneRendererSpecification's default, while no program
		// provides the service: only tests and tools render so (the editor provides it whenever it runs, the game from its
		// manifest). Main thread only.
		const RendererQualitySettings& GetProjectQuality();
	}

}
