#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Checks that the engine's sources include only what their layer may include (StrataTests/Architecture/Layers.json,
// AGENTS.md "Architecture rules"). Works on in-memory file sets, so its parts are tested on their own.
namespace Strata::Tests::Layering
{

	// A quoted #include and its line (1-based).
	struct IncludeDirective
	{
		std::string Path;
		uint32_t Line = 0;
	};

	// The quoted #include directives of a C++ source. Comments, string and character literals (raw strings included) and
	// the lines of #if 0 blocks (up to their #else or #elif) are skipped; includes under other conditions count, whatever
	// the condition. Angle-bracket includes (standard and third-party headers) are not engine includes and are skipped.
	std::vector<IncludeDirective> ParseIncludes(std::string_view source);

	// Glob over '/'-separated relative paths: '*' matches within one path component, '**' across components ("a/**"
	// matches everything below a, "a/**/b" also "a/b"), '?' one character other than '/'.
	bool MatchesGlob(std::string_view pattern, std::string_view path);

	struct Layer
	{
		std::string Name;
		std::vector<std::string> Paths;      // Globs of the files the layer holds, relative to the source root
		std::vector<std::string> Exclude;    // Globs of files the paths match that belong to another layer
		std::vector<std::string> MayInclude; // Lower layers this one may include (it may always include itself)
	};

	struct LayerTable
	{
		std::vector<Layer> Layers; // Bottom up
		size_t MaxAllowlistEntries = 0;
	};

	// Reads a Layers.json document. Returns false (with the problems) for a malformed table, a duplicate layer name, a
	// layer without paths, or an allowed layer that is not listed below the one that includes it.
	bool ParseLayerTable(const nlohmann::json& json, LayerTable& outTable, std::vector<std::string>& outErrors);

	struct AllowlistEntry
	{
		std::string File;   // The including file, relative to the source root
		std::string Header; // The included file, relative to the source root
		std::string Owner;  // The workstream that removes the include
		std::string Reason;
		uint32_t Line = 0;  // Line of the entry in the allowlist file
	};

	// Reads LayeringAllowlist.txt: one "<file>:<included header> | <workstream> | <reason>" per line; lines starting with
	// '#' and blank lines are skipped. Returns false (with the problems) for malformed and duplicate lines.
	bool ParseAllowlist(std::string_view text, std::vector<AllowlistEntry>& outEntries, std::vector<std::string>& outErrors);

	// An include of a file from a layer the including file's layer may not include.
	struct Violation
	{
		std::string File;
		uint32_t Line = 0;
		std::string Header;
		std::string FileLayer;
		std::string HeaderLayer;
	};

	struct LayeringReport
	{
		std::vector<std::string> Errors;           // Files that are in no layer or in several
		std::vector<Violation> Violations;         // Forbidden includes that are not allowlisted
		std::vector<Violation> Allowlisted;        // Forbidden includes covered by an allowlist entry
		std::vector<AllowlistEntry> StaleEntries;  // Allowlist entries that cover no forbidden include
		size_t CheckedIncludes = 0;                // Includes of engine files that were checked
	};

	// Checks every include of `files` (path relative to the source root -> contents). A quoted include resolves like the
	// preprocessor's: next to the including file first, then from the source root; includes of files outside the set are
	// still checked when a layer's paths match them (generated headers), and skipped otherwise (other targets' headers).
	LayeringReport CheckLayering(const LayerTable& table, const std::map<std::string, std::string>& files, const std::vector<AllowlistEntry>& allowlist);

	// "<file>(<line>): includes <header>, but layer <A> may not include layer <B>"
	std::string Describe(const Violation& violation);

}
