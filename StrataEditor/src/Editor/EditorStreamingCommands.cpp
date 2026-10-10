#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Reflection/PropertyJson.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		constexpr double c_BytesPerMB = 1024.0 * 1024.0;
		constexpr int64_t c_DefaultAssetRows = 100;
		constexpr int64_t c_MaxAssetRows = 1'000'000;

		// Budgets in bytes; null for an unlimited one.
		nlohmann::json DescribeBudget(uint64_t bytes)
		{
			return bytes == AssetResidencyBudgets::c_Unlimited ? nlohmann::json(nullptr) : nlohmann::json(bytes);
		}

		nlohmann::json DescribeBudgets(const AssetResidencyBudgets& budgets)
		{
			return {
				{ "gpuTexturesBytes", DescribeBudget(budgets.GpuTextures) },
				{ "gpuBuffersBytes", DescribeBudget(budgets.GpuBuffers) },
				{ "cpuBytes", DescribeBudget(budgets.Cpu) },
				{ "inFlightBytes", budgets.InFlightBytes },
				{ "uploadBytesPerFrame", budgets.UploadBytesPerFrame },
				{ "finalizeMsPerFrame", budgets.FinalizeMsPerFrame } };
		}

		nlohmann::json DescribeResidency(const AssetResidencyInfo& asset)
		{
			return {
				{ "handle", UUIDToJson(asset.Handle) },
				{ "path", asset.Path },
				{ "name", asset.Name },
				{ "type", AssetTypeToString(asset.Type) },
				{ "state", AssetStateToString(asset.State) },
				{ "cpuBytes", asset.Usage.Cpu },
				{ "gpuBytes", asset.Usage.GetGpu() },
				{ "lastUsedFrame", asset.LastRequestedFrame },
				{ "pins", asset.PinCount },
				{ "memoryAsset", asset.IsMemoryAsset } };
		}

		// A budget parameter (megabytes or milliseconds); nullopt when absent. Values that are not numbers greater than 0, or
		// that exceed `maximum`, are errors.
		std::optional<double> ReadPositive(const nlohmann::json& parameters, const char* name, double maximum, CommandArguments& arguments)
		{
			auto it = parameters.find(name);
			if (it == parameters.end() || it->is_null())
				return std::nullopt;
			if (!it->is_number())
			{
				arguments.SetError(fmt::format("Parameter '{}' must be a number", name));
				return std::nullopt;
			}
			const double value = it->get<double>();
			if (!std::isfinite(value) || value <= 0.0 || value > maximum)
			{
				arguments.SetError(fmt::format("Parameter '{}' must be greater than 0 and at most {}", name, maximum));
				return std::nullopt;
			}
			return value;
		}

	}

	namespace CommandUtils
	{

		nlohmann::json DescribeAssetStats(const AssetManagerStats& stats)
		{
			nlohmann::json pools = nlohmann::json::object();
			for (AssetMemoryPool pool : c_AssetMemoryPools)
			{
				const char* name = pool == AssetMemoryPool::Cpu ? "cpu" : (pool == AssetMemoryPool::GpuTextures ? "gpuTextures" : "gpuBuffers");
				pools[name] = { { "residentBytes", GetPoolBytes(stats.Resident, pool) }, { "budgetBytes", DescribeBudget(stats.Budgets.GetPoolBudget(pool)) } };
			}
			return {
				{ "frame", stats.Frame },
				{ "registered", stats.RegisteredAssets },
				{ "loaded", stats.LoadedAssets },
				{ "loading", stats.LoadingAssets },
				{ "failed", stats.FailedAssets },
				{ "pinned", stats.PinnedAssets },
				{ "loadsCompleted", stats.TotalLoadsCompleted },
				{ "loadedBytes", stats.LoadedMemory },
				{ "pools", std::move(pools) },
				{ "streaming", {
					{ "queued", { { "high", stats.QueuedLoads[0] }, { "normal", stats.QueuedLoads[1] }, { "low", stats.QueuedLoads[2] } } },
					{ "inFlightLoads", stats.InFlightLoads },
					{ "outstandingReads", stats.OutstandingReads },
					{ "inFlightBytes", stats.InFlightBytes },
					{ "inFlightBytesHighWater", stats.InFlightBytesHighWater },
					{ "inFlightBudgetBytes", stats.Budgets.InFlightBytes } } },
				{ "finalization", {
					{ "uploadedBytesLastFrame", stats.UploadedBytesLastFrame },
					{ "uploadedBytesWindowMax", stats.UploadedBytesWindowMax },
					{ "uploadBudgetBytesPerFrame", stats.Budgets.UploadBytesPerFrame },
					{ "finalizeMsLastFrame", stats.FinalizeMsLastFrame },
					{ "finalizeMsWindowMax", stats.FinalizeMsWindowMax },
					{ "finalizeBudgetMsPerFrame", stats.Budgets.FinalizeMsPerFrame },
					{ "windowFrames", AssetManagerBase::c_StatsWindowFrames } } },
				{ "evictions", stats.Evictions },
				{ "cancellations", stats.Cancellations },
				{ "stagingReleases", stats.StagingReleases } };
		}

	}

	void RegisterStreamingCommands(EditorCommandRegistry& registry)
	{
		registry.Register({ "asset.stats",
			"Asset memory and streaming: resident bytes and budget per pool (cpu, gpuTextures, gpuBuffers; null budget = unlimited), "
			"load counts, the streaming queue (queued loads per priority, loads and bytes in flight), finalization (GPU bytes uploaded and "
			"main-thread milliseconds, last frame and maximum of recent frames), evictions, cancellations and staging releases. With "
			"assets: true it also lists the assets that are resident, loading, failed or pinned, with their memory, state, last use and pins.",
			ObjectSchema({
				{ "assets", BoolSchema("Also list the assets (default false)") },
				{ "limit", IntegerSchema("At most this many assets in the list (default 100)", 1, c_MaxAssetRows) },
				{ "sort", { { "type", "string" }, { "enum", { "bytes", "lastUsed" } },
					{ "description", "Order of the list: \"bytes\" (most memory first, the default) or \"lastUsed\" (most recently used first)" } } } }),
			[](EditorContext&, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const bool listAssets = arguments.GetBool("assets", false);
				const int64_t limit = arguments.GetInt("limit", c_DefaultAssetRows, 1, c_MaxAssetRows);
				const std::string sort = arguments.GetString("sort", "bytes");
				if (arguments.IsValid() && sort != "bytes" && sort != "lastUsed")
					arguments.SetError("Parameter 'sort' must be \"bytes\" or \"lastUsed\"");
				if (!arguments.IsValid())
					return arguments.Fail();

				const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
				if (!manager)
					return EditorCommandResult::Fail("No asset manager is active");
				nlohmann::json result = DescribeAssetStats(manager->GetStats());
				if (!listAssets)
					return EditorCommandResult::Ok(std::move(result));

				std::vector<AssetResidencyInfo> assets = manager->GetResidencyInfo();
				std::sort(assets.begin(), assets.end(), [&sort](const AssetResidencyInfo& a, const AssetResidencyInfo& b)
				{
					const uint64_t aKey = sort == "bytes" ? a.Usage.GetTotal() : a.LastRequestedFrame;
					const uint64_t bKey = sort == "bytes" ? b.Usage.GetTotal() : b.LastRequestedFrame;
					return aKey != bKey ? aKey > bKey : a.Handle < b.Handle;
				});
				nlohmann::json rows = nlohmann::json::array();
				for (size_t index = 0; index < assets.size() && index < static_cast<size_t>(limit); index++)
					rows.push_back(DescribeResidency(assets[index]));
				result["assetCount"] = assets.size();
				result["assets"] = std::move(rows);
				return EditorCommandResult::Ok(std::move(result));
			} });

		registry.Register({ "asset.setBudget",
			"Sets the asset streaming budgets of the open project's asset manager (until it closes; not undoable): resident memory per pool in "
			"megabytes (1 MB = 1048576 bytes; assets over budget are evicted, least recently used first), the megabytes of loads in flight, the "
			"GPU megabytes uploaded per frame and the main-thread milliseconds finalization may take per frame. Values must be greater than 0; "
			"reset: true starts from the defaults (derived from the GPU). Returns the budgets in effect (bytes, null = unlimited).",
			ObjectSchema({
				{ "gpuTexturesMB", { { "type", "number" }, { "exclusiveMinimum", 0 }, { "description", "Resident GPU texture memory, in MB" } } },
				{ "gpuBuffersMB", { { "type", "number" }, { "exclusiveMinimum", 0 }, { "description", "Resident GPU buffer (mesh) memory, in MB" } } },
				{ "cpuMB", { { "type", "number" }, { "exclusiveMinimum", 0 }, { "description", "Resident CPU memory of asset data, in MB" } } },
				{ "inFlightMB", { { "type", "number" }, { "exclusiveMinimum", 0 },
					{ "description", "Stored bytes of loads being read, decoded or waiting to be finalized, in MB" } } },
				{ "uploadMBPerFrame", { { "type", "number" }, { "exclusiveMinimum", 0 }, { "description", "GPU bytes finalization uploads per frame, in MB" } } },
				{ "finalizeMsPerFrame", { { "type", "number" }, { "exclusiveMinimum", 0 },
					{ "description", "Main-thread milliseconds finalization may take per frame" } } },
				{ "reset", BoolSchema("Start from the default budgets before applying the given values (default false)") } }),
			[](EditorContext&, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				// Megabytes whose byte count still fits (with room to spare) in 64 bits, and up to a minute per frame.
				constexpr double c_MaxMB = 1.0e12;
				constexpr double c_MaxMs = 60000.0;
				const bool reset = arguments.GetBool("reset", false);
				const std::optional<double> gpuTextures = ReadPositive(parameters, "gpuTexturesMB", c_MaxMB, arguments);
				const std::optional<double> gpuBuffers = ReadPositive(parameters, "gpuBuffersMB", c_MaxMB, arguments);
				const std::optional<double> cpu = ReadPositive(parameters, "cpuMB", c_MaxMB, arguments);
				const std::optional<double> inFlight = ReadPositive(parameters, "inFlightMB", c_MaxMB, arguments);
				const std::optional<double> upload = ReadPositive(parameters, "uploadMBPerFrame", c_MaxMB, arguments);
				const std::optional<double> finalizeMs = ReadPositive(parameters, "finalizeMsPerFrame", c_MaxMs, arguments);
				if (!arguments.IsValid())
					return arguments.Fail();

				const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
				if (!manager)
					return EditorCommandResult::Fail("No asset manager is active");

				const auto toBytes = [](double megabytes) { return static_cast<uint64_t>(std::llround(megabytes * c_BytesPerMB)); };
				AssetResidencyBudgets budgets = reset ? AssetManagerBase::GetDefaultResidencyBudgets() : manager->GetResidencyBudgets();
				if (gpuTextures)
					budgets.GpuTextures = toBytes(*gpuTextures);
				if (gpuBuffers)
					budgets.GpuBuffers = toBytes(*gpuBuffers);
				if (cpu)
					budgets.Cpu = toBytes(*cpu);
				if (inFlight)
					budgets.InFlightBytes = toBytes(*inFlight);
				if (upload)
					budgets.UploadBytesPerFrame = toBytes(*upload);
				if (finalizeMs)
					budgets.FinalizeMsPerFrame = static_cast<float>(*finalizeMs);
				manager->SetResidencyBudgets(budgets);
				return EditorCommandResult::Ok({ { "budgets", DescribeBudgets(manager->GetResidencyBudgets()) } });
			} });
	}

}
