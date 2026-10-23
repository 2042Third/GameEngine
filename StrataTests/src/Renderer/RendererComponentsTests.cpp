#include <doctest/doctest.h>

#include "Strata/Renderer/RendererComponents.h"

#include <limits>

using namespace Strata;

namespace
{

	// A Custom component whose tunables hold `settings`, with other values in the fields those settings do not read.
	PostProcessComponent CustomWith(const TemporalAntiAliasingSettings& settings)
	{
		PostProcessComponent postProcess;
		postProcess.TAAPreset = TemporalAntiAliasingPreset::Custom;
		postProcess.TAAClipWidth = 3.0f;
		postProcess.TAADisocclusionThreshold = 0.3f;
		SetTemporalAntiAliasingSettings(postProcess, settings);
		return postProcess;
	}

}

TEST_SUITE("Renderer.Components")
{
	TEST_CASE("The TAA's tunables default to the Sharp preset's table, and the tables are normalized")
	{
		const PostProcessComponent postProcess;
		CHECK(postProcess.TAAPreset == TemporalAntiAliasingPreset::Original);
		PostProcessComponent custom = postProcess;
		custom.TAAPreset = TemporalAntiAliasingPreset::Custom;
		CHECK(GetTemporalAntiAliasingSettings(custom) == c_TAASharpSettings);
		CHECK(postProcess.TAAClipWidth == c_TAASharpSettings.ClipWidth);
		CHECK(postProcess.TAADisocclusionThreshold == c_TAASharpSettings.DisocclusionThreshold);
		// Original's table holds 0 where its modes read nothing; Sharp reads every field.
		CHECK(c_TAAOriginalSettings.ClipWidth == 0.0f);
		CHECK(c_TAAOriginalSettings.DisocclusionThreshold == 0.0f);
		CHECK(c_TAAOriginalSettings.MotionResponse == 1.0f / 32.0f);
		// Both keep the least weight at 1/8, so captures settle after eight frames.
		CHECK(c_TAAOriginalSettings.HistoryFrames == 8);
		CHECK(c_TAASharpSettings.HistoryFrames == 8);
	}

	TEST_CASE("Each preset resolves with its table, and Custom with Original's values equals Original whatever its unused fields hold")
	{
		PostProcessComponent postProcess;
		postProcess.TAAPreset = TemporalAntiAliasingPreset::Original;
		CHECK(GetTemporalAntiAliasingSettings(postProcess) == c_TAAOriginalSettings);
		postProcess.TAAPreset = TemporalAntiAliasingPreset::Sharp;
		CHECK(GetTemporalAntiAliasingSettings(postProcess) == c_TAASharpSettings);

		// The tunables are ignored under a preset.
		postProcess.TAAHistoryFrames = 20;
		postProcess.TAASharpening = 1.0f;
		CHECK(GetTemporalAntiAliasingSettings(postProcess) == c_TAASharpSettings);

		const PostProcessComponent custom = CustomWith(c_TAAOriginalSettings);
		CHECK(custom.TAAClipWidth == 3.0f);
		CHECK(custom.TAADisocclusionThreshold == 0.3f);
		CHECK(GetTemporalAntiAliasingSettings(custom) == c_TAAOriginalSettings);
		CHECK(GetTemporalAntiAliasingSettings(CustomWith(c_TAASharpSettings)) == c_TAASharpSettings);

		// The fields the modes read are kept.
		PostProcessComponent tuned = CustomWith(c_TAASharpSettings);
		tuned.TAAClipWidth = 2.0f;
		tuned.TAADisocclusionThreshold = 0.25f;
		const TemporalAntiAliasingSettings settings = GetTemporalAntiAliasingSettings(tuned);
		CHECK(settings.ClipWidth == 2.0f);
		CHECK(settings.DisocclusionThreshold == 0.25f);
	}

	TEST_CASE("Custom's values are clamped to the tunables' ranges, NaN to the low end")
	{
		PostProcessComponent postProcess;
		postProcess.TAAPreset = TemporalAntiAliasingPreset::Custom;
		postProcess.TAAHistoryFrames = 0;
		postProcess.TAAMotionResponse = 5.0f;
		postProcess.TAAMaxMotionWeight = -1.0f;
		postProcess.TAAResampleWeight = std::numeric_limits<float>::quiet_NaN();
		postProcess.TAAHistoryClamp = static_cast<TemporalHistoryClamp>(7);
		postProcess.TAATransparentWeight = 2.0f;
		postProcess.TAADisocclusionRejection = true;
		postProcess.TAADisocclusionThreshold = 0.0f;
		postProcess.TAASharpening = std::numeric_limits<float>::infinity();
		TemporalAntiAliasingSettings settings = GetTemporalAntiAliasingSettings(postProcess);
		CHECK(settings.HistoryFrames == c_TAAMinHistoryFrames);
		CHECK(settings.MotionResponse == c_TAAMaxMotionResponse);
		CHECK(settings.MaxMotionWeight == 0.0f);
		CHECK(settings.ResampleWeight == 0.0f);
		CHECK(settings.HistoryClamp == TemporalHistoryClamp::MinMax);
		CHECK(settings.ClipWidth == 0.0f);
		CHECK(settings.TransparentWeight == 1.0f);
		CHECK(settings.DisocclusionThreshold == c_TAAMinDisocclusionThreshold);
		CHECK(settings.Sharpening == 1.0f);

		postProcess.TAAHistoryFrames = 1000;
		postProcess.TAAHistoryClamp = TemporalHistoryClamp::Variance;
		postProcess.TAAClipWidth = 100.0f;
		postProcess.TAADisocclusionThreshold = 1.0f;
		settings = GetTemporalAntiAliasingSettings(postProcess);
		CHECK(settings.HistoryFrames == c_TAAMaxHistoryFrames);
		CHECK(settings.ClipWidth == c_TAAMaxClipWidth);
		CHECK(settings.DisocclusionThreshold == c_TAAMaxDisocclusionThreshold);
	}

	TEST_CASE("Custom starts from the preset in effect while the tunables hold a table, and from the tuning otherwise")
	{
		// Untouched tunables (Sharp's table, the defaults) under Original: Custom starts from Original's look.
		PostProcessComponent postProcess;
		postProcess.TAAPreset = TemporalAntiAliasingPreset::Original;
		CHECK(GetTemporalAntiAliasingCustomStart(postProcess) == c_TAAOriginalSettings);
		postProcess.TAAPreset = TemporalAntiAliasingPreset::Sharp;
		CHECK(GetTemporalAntiAliasingCustomStart(postProcess) == c_TAASharpSettings);

		// Tunables holding Original's table (Custom started from Original) under Sharp: Sharp's.
		PostProcessComponent fromOriginal = CustomWith(c_TAAOriginalSettings);
		fromOriginal.TAAPreset = TemporalAntiAliasingPreset::Sharp;
		CHECK(GetTemporalAntiAliasingCustomStart(fromOriginal) == c_TAASharpSettings);
		fromOriginal.TAAPreset = TemporalAntiAliasingPreset::Original;
		CHECK(GetTemporalAntiAliasingCustomStart(fromOriginal) == c_TAAOriginalSettings);

		// Tuned values are kept under every preset, also the fields their modes do not read.
		PostProcessComponent tuned = CustomWith(c_TAAOriginalSettings);
		tuned.TAAHistoryFrames = 12;
		TemporalAntiAliasingSettings expected = c_TAAOriginalSettings;
		expected.HistoryFrames = 12;
		expected.ClipWidth = 3.0f;
		expected.DisocclusionThreshold = 0.3f;
		for (const TemporalAntiAliasingPreset preset : { TemporalAntiAliasingPreset::Sharp, TemporalAntiAliasingPreset::Original, TemporalAntiAliasingPreset::Custom })
		{
			tuned.TAAPreset = preset;
			CHECK(GetTemporalAntiAliasingCustomStart(tuned) == expected);
		}

		// Writing a start back changes nothing it does not set: a table's unused fields keep the stored values.
		PostProcessComponent written;
		SetTemporalAntiAliasingSettings(written, c_TAAOriginalSettings);
		CHECK(written.TAAClipWidth == c_TAASharpSettings.ClipWidth);
		CHECK(written.TAADisocclusionThreshold == c_TAASharpSettings.DisocclusionThreshold);
		CHECK(written.TAAHistoryClamp == TemporalHistoryClamp::MinMax);
		CHECK_FALSE(written.TAADisocclusionRejection);
	}
}
