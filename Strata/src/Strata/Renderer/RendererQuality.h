#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Renderer/SceneRenderData.h"

#include <glm/vec2.hpp>
#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
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
		TAA,
		Morphological
	};

	// A players' comfort row: the scene's effect, or off (players may turn off, never on, what a scene uses).
	enum class SceneEffectChoice : uint8_t
	{
		Scene = 0,
		Off
	};

	// Ambient occlusion's samples (Effects/GTAO.comp: slices x steps per side): Low 1 x 4, Medium 2 x 6 (Strata 0.6.0's), High
	// 3 x 8, Ultra 4 x 12. The values never change meaning (the Renderer.Quality section stores their names).
	enum class AmbientOcclusionLevel : uint8_t
	{
		Low = 0,
		Medium = 1,
		High = 2,
		Ultra = 3
	};

	struct AmbientOcclusionSamples
	{
		uint32_t Slices = 2;
		uint32_t StepsPerSide = 6;
	};

	// Screen-space reflections (PostProcessComponent::ScreenSpaceReflections asks for them; the row scales or turns off what
	// a scene asks): Off; Checkerboard, Strata 0.6.0's trace (half resolution, a pixel of each 2x2 block per frame, the TAA's
	// history as its color: only with TAA); Spatial and SpatialHigh, every pixel at full resolution with a spatial resolve and
	// the previous frame's color (with or without TAA). The values never change meaning (the section stores their names).
	enum class ReflectionsQuality : uint8_t
	{
		Off = 0,
		Checkerboard = 1,
		Spatial = 2,
		SpatialHigh = 3
	};

	// The march of a screen-space reflection ray (Effects/ScreenSpaceReflections.comp, ScreenSpaceReflectionsSpatial.comp):
	// its steps along the ray and the bisection's refinements of a hit.
	struct ScreenReflectionSteps
	{
		uint32_t Steps = 24;
		uint32_t RefineSteps = 6;
		uint32_t MaxCandidates = 1; // Crossings Spatial's march may refine and reject before a ray finds nothing (Checkerboard's takes the first)
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
		// Taps of the sun's shadow edges where their penumbra is wider than a shadow texel (a Vogel disk turned by a fixed
		// blue-noise tile, Include/Shadows.slang): 8, 16, 24 or 32. More taps, less grain.
		uint32_t ShadowFilterQuality = 16;
		// Steps of the sun's screen-space contact shadows (0 off; else 4 to 32), which ground objects below the cascades' detail.
		// 0 in every preset (they shimmer in motion); Custom turns them on.
		uint32_t ContactShadows = 0;
		// Ambient occlusion's samples per pixel (GetAmbientOcclusionSamples); the scene's Post Process asks for the occlusion.
		AmbientOcclusionLevel AmbientOcclusionQuality = AmbientOcclusionLevel::Medium;
		// The prefiltered sky's texels each way per face, 256 or 512 (Clamp takes any other value to 256; the environment cube it
		// is filtered from stays 512): 512 keeps mirror-like reflections of the sky sharp at 4K. Six levels at both sizes, RGBA16F:
		// 4 MiB at 256, 16 MiB at 512 per set of maps (the atmosphere keeps two).
		uint32_t EnvironmentResolution = 256;
		// Screen-space reflections' mode (ReflectionsQuality): Spatial and SpatialHigh add a full-resolution trace, resolve and
		// color copy (about 150 MiB more than Checkerboard at 3840x2160; DG "Reflections").
		ReflectionsQuality Reflections = ReflectionsQuality::Checkerboard;
		// The motion blur's gather samples, for scenes whose Post Process turns it on (8 at Medium; Off at Low turns it off).
		MotionBlurSamples MotionBlurQuality = MotionBlurSamples::Samples8;
		// The reflection probes' texels each way per face, 128 or 256 (Clamp takes any other value to 128): 256 keeps polished
		// metal's reflections of nearby things sharp. Six levels at both sizes, RGBA16F, for RenderData::c_MaxReflectionProbes
		// slots and a scratch cube: 17 MiB at 128, 68 MiB at 256, allocated only by renderers that show a probe. Captures are
		// 1024x512 from 256-texel faces, so 256 needs no new bake.
		uint32_t ReflectionProbeResolution = 128;
		// The players' rows, the same under every preset (comfort and preference, not cost): the anti-aliasing method (the
		// scene's, or off, FXAA, TAA or the morphological anti-aliasing) and whether the scene's motion blur, its cameras' depth
		// of field, its film grain and its chromatic aberration show.
		AntiAliasingChoice AntiAliasing = AntiAliasingChoice::Scene;
		SceneEffectChoice MotionBlur = SceneEffectChoice::Scene;
		SceneEffectChoice DepthOfField = SceneEffectChoice::Scene;
		SceneEffectChoice FilmGrain = SceneEffectChoice::Scene;
		SceneEffectChoice ChromaticAberration = SceneEffectChoice::Scene;

		// Every member equal, Preset included.
		bool operator==(const RendererQualitySettings& other) const = default;
		// Every knob equal (Preset is a label): copies whose Preset is made equal, compared with operator==, so a knob a
		// stream appends needs no line here.
		bool HasSameKnobs(const RendererQualitySettings& other) const;
	};

	namespace RendererQuality
	{
		constexpr std::string_view c_SectionId = "Renderer.Quality";
		constexpr uint32_t c_SectionVersion = 5; // Each landing of a row adds one (Docs/DeveloperGuide.md, "Quality settings")
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
		const char* AntiAliasingChoiceToString(AntiAliasingChoice choice); // "Scene", "Off", "FXAA", "TAA", "Morphological"
		std::optional<AntiAliasingChoice> AntiAliasingChoiceFromString(std::string_view name);
		const char* SceneEffectChoiceToString(SceneEffectChoice choice); // "Scene", "Off"
		std::optional<SceneEffectChoice> SceneEffectChoiceFromString(std::string_view name);
		const char* AmbientOcclusionLevelToString(AmbientOcclusionLevel level); // "Low" ... "Ultra"
		std::optional<AmbientOcclusionLevel> AmbientOcclusionLevelFromString(std::string_view name);
		// Slices and steps per side of a level: { 1, 4 }, { 2, 6 }, { 3, 8 }, { 4, 12 }.
		AmbientOcclusionSamples GetAmbientOcclusionSamples(AmbientOcclusionLevel level);
		const char* ReflectionsQualityToString(ReflectionsQuality quality); // "Off", "Checkerboard", "Spatial", "SpatialHigh"
		std::optional<ReflectionsQuality> ReflectionsQualityFromString(std::string_view name);
		// The march of a mode: Checkerboard 24 + 6, Spatial 32 + 6, SpatialHigh 48 + 8 (Off: Checkerboard's, unused).
		ScreenReflectionSteps GetScreenReflectionSteps(ReflectionsQuality quality);
		// Whether a mode traces every pixel at full resolution with the previous frame's color (Spatial, SpatialHigh).
		bool IsSpatialReflections(ReflectionsQuality quality);
		// The samples per texel that filter level `mip` of a prefiltered sky of `size` texels each way (IBL/Prefilter.comp): 512
		// at every level of 256 (Strata 0.6.0's); at 512, levels 1 and 2 take 128 (their narrow lobes read finer source levels)
		// and the others 512, so no level costs more than 0.6.0's largest (size^2 x samples).
		uint32_t GetPrefilterSamples(uint32_t size, uint32_t mip);
		// The same for a reflection probe's cube of `size` texels (ReflectionProbeResolution): 256 at every level of 128 (0.7.0's);
		// at 256, levels 1 and 2 take 128 (the sky's rule at 512), so a conversion at 256 takes 2.2 times the samples of one at 128,
		// not 4.
		uint32_t GetProbePrefilterSamples(uint32_t size, uint32_t mip);

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

		// What graphics options (the Graphics Quality page, the players' options) know of a field of Renderer.Quality besides
		// the field itself. RendererQuality.cpp's c_RowTraits holds one line per field but Preset, in field order, beside the
		// knob's PresetRow line (Docs/DeveloperGuide.md, "Quality settings").
		struct RowTraits
		{
			std::string_view Field;    // The section field ("ShadowResolution")
			bool FollowsPreset = true; // False: a player's choice the presets leave alone (comfort rows), read under every preset
			std::string_view GPUStage; // The GPU stage whose time this row changes ("GPU.Shadows"); empty: none of its own
			// Bytes of GPU memory this row's resources take in one renderer for these (unresolved) settings at an output size
			// (Auto and AutoFine resolved with ResolveShadowMapSize(resolution, output.y)); null when the row allocates nothing of
			// its own or another row counts the resource (ShadowCascades is counted with ShadowResolution).
			uint64_t (*MemoryBytes)(const RendererQualitySettings& settings, glm::uvec2 output) = nullptr;
		};
		// The engine's rows: one per field of MakeSection() but Preset, in field order.
		std::span<const RowTraits> GetRowTraits();
		// The section values of Low, Medium, High and Ultra, every field spelled out: ToSectionValue(GetPreset(p)), made once.
		const std::array<nlohmann::json, c_RendererQualityPresetCount>& GetPresetValues();

		// What the option logic works over: a simple quality section (its first field Preset, an Enum of Low, Medium, High,
		// Ultra and Custom), its rows' traits and its four presets' values. The engine's is GetOptionModel(section); the
		// page's and the players' options' tests build one over a test section of their own (for example with a row that does
		// not follow the preset).
		struct OptionModel
		{
			const SettingsSection* Section = nullptr;
			std::span<const RowTraits> Rows; // One per field but Preset
			// Low, Medium, High, Ultra: c_RendererQualityPresetCount values, or null for none. A pointer, not a span: this header
			// only declares JSON (Docs/DeveloperGuide.md, "Precompiled headers").
			const nlohmann::json* PresetValues = nullptr;
		};
		// The engine's rows and presets over `section` (the registered Renderer.Quality section).
		OptionModel GetOptionModel(const SettingsSection& section);

		// The row of a field, ignoring case (null for Preset and unknown names).
		const RowTraits* FindRow(std::string_view field, const OptionModel& model);

		// The value with every field present and, under a named preset, every row that follows the preset set to the preset's
		// value (PresetValues): what is in effect, spelled out. `value` must be valid at the section's version (Validate's
		// result). Idempotent; for the engine's model FromSectionValue(&Normalize(v)) has the same knobs as FromSectionValue(&v).
		nlohmann::json Normalize(const nlohmann::json& value, const OptionModel& model);
		// The Custom value a switch to Custom starts from (the rule of the TAA preset's Custom start): the rows that follow the
		// preset, taken together, are someone's tuning when they equal neither the fields' defaults nor any preset's values; a
		// tuning is kept, otherwise they become the values in effect (Normalize). Rows that do not follow the preset keep
		// their values. Preset becomes "Custom". `value` as for Normalize.
		nlohmann::json MakeCustomStart(const nlohmann::json& value, const OptionModel& model);
	}

}
