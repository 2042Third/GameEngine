#include "Architecture/LayeringAnalyzer.h"

#include "Strata/Core/StringUtils.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>

namespace Strata::Tests::Layering
{

	namespace
	{

		bool IsIdentifierCharacter(char character)
		{
			return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') || character == '_';
		}

		// The identifier characters directly before `position` (a literal's encoding or raw prefix, or the digits of a
		// number when the quote is a digit separator).
		std::string_view IdentifierBefore(std::string_view source, size_t position)
		{
			size_t start = position;
			while (start > 0 && IsIdentifierCharacter(source[start - 1]))
				start--;
			return source.substr(start, position - start);
		}

		// The source with its comments and the contents of its raw string literals changed into spaces (line breaks stay, so
		// line numbers do not move). String and character literals are skipped over so that comment markers inside them stay
		// text; a raw string's contents are blanked because they may span lines that look like directives.
		std::string BlankCommentsAndRawStrings(std::string_view source)
		{
			std::string result(source);
			auto blank = [&result](size_t begin, size_t end)
			{
				for (size_t index = begin; index < end && index < result.size(); index++)
				{
					if (result[index] != '\n')
						result[index] = ' ';
				}
			};

			size_t index = 0;
			while (index < source.size())
			{
				const char character = source[index];
				const char next = index + 1 < source.size() ? source[index + 1] : '\0';
				if (character == '/' && next == '/')
				{
					const size_t end = source.find('\n', index);
					blank(index, end == std::string_view::npos ? source.size() : end);
					index = end == std::string_view::npos ? source.size() : end;
				}
				else if (character == '/' && next == '*')
				{
					const size_t end = source.find("*/", index + 2);
					const size_t stop = end == std::string_view::npos ? source.size() : end + 2;
					blank(index, stop);
					index = stop;
				}
				else if (character == '"')
				{
					const std::string_view prefix = IdentifierBefore(source, index);
					const bool raw = prefix == "R" || prefix == "u8R" || prefix == "uR" || prefix == "UR" || prefix == "LR";
					if (raw)
					{
						// R"delimiter( ... )delimiter"
						const size_t open = source.find('(', index + 1);
						if (open == std::string_view::npos)
						{
							index = source.size();
							continue;
						}
						const std::string terminator = ")" + std::string(source.substr(index + 1, open - index - 1)) + "\"";
						const size_t end = source.find(terminator, open + 1);
						const size_t stop = end == std::string_view::npos ? source.size() : end + terminator.size();
						blank(open + 1, end == std::string_view::npos ? source.size() : end);
						index = stop;
					}
					else
					{
						index++;
						while (index < source.size() && source[index] != '"' && source[index] != '\n')
							index += source[index] == '\\' ? 2 : 1;
						index++;
					}
				}
				else if (character == '\'')
				{
					// A quote after a number's digits is a digit separator (1'000), unless they are an encoding prefix.
					const std::string_view prefix = IdentifierBefore(source, index);
					const bool literal = prefix.empty() || prefix == "u8" || prefix == "u" || prefix == "U" || prefix == "L";
					index++;
					if (literal)
					{
						while (index < source.size() && source[index] != '\'' && source[index] != '\n')
							index += source[index] == '\\' ? 2 : 1;
						index++;
					}
				}
				else
				{
					index++;
				}
			}
			return result;
		}

		std::string_view Trim(std::string_view text)
		{
			while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
				text.remove_prefix(1);
			while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
				text.remove_suffix(1);
			return text;
		}

		// "a/b/../c/./d" -> "a/c/d"; nullopt when ".." leaves the root.
		std::optional<std::string> NormalizePath(std::string_view path)
		{
			std::vector<std::string_view> parts;
			while (!path.empty())
			{
				const size_t slash = path.find('/');
				const std::string_view part = path.substr(0, slash);
				path = slash == std::string_view::npos ? std::string_view() : path.substr(slash + 1);
				if (part.empty() || part == ".")
					continue;
				if (part == "..")
				{
					if (parts.empty())
						return std::nullopt;
					parts.pop_back();
					continue;
				}
				parts.push_back(part);
			}
			std::string result;
			for (const std::string_view part : parts)
			{
				if (!result.empty())
					result += '/';
				result += part;
			}
			return result;
		}

		bool ReadStringArray(const nlohmann::json& object, const char* key, std::vector<std::string>& outValues, const std::string& context, std::vector<std::string>& outErrors)
		{
			const auto it = object.find(key);
			if (it == object.end())
				return true;
			if (!it->is_array())
			{
				outErrors.push_back(fmt::format("{}: '{}' must be an array of strings", context, key));
				return false;
			}
			for (const nlohmann::json& value : *it)
			{
				if (!value.is_string() || value.get_ref<const std::string&>().empty())
				{
					outErrors.push_back(fmt::format("{}: '{}' must hold non-empty strings", context, key));
					return false;
				}
				outValues.push_back(value.get<std::string>());
			}
			return true;
		}

		bool MatchesAny(const std::vector<std::string>& patterns, std::string_view path)
		{
			return std::any_of(patterns.begin(), patterns.end(), [&](const std::string& pattern) { return MatchesGlob(pattern, path); });
		}

		// The layers whose paths hold `path`.
		std::vector<const Layer*> FindLayers(const LayerTable& table, std::string_view path)
		{
			std::vector<const Layer*> layers;
			for (const Layer& layer : table.Layers)
			{
				if (MatchesAny(layer.Paths, path) && !MatchesAny(layer.Exclude, path))
					layers.push_back(&layer);
			}
			return layers;
		}

	}

	std::vector<IncludeDirective> ParseIncludes(std::string_view source)
	{
		const std::string code = BlankCommentsAndRawStrings(source);

		// One entry per open conditional: whether its current branch is skipped (the first branch of an #if 0).
		std::vector<bool> conditionals;
		auto skipping = [&conditionals]()
		{
			return std::find(conditionals.begin(), conditionals.end(), true) != conditionals.end();
		};

		std::vector<IncludeDirective> includes;
		uint32_t lineNumber = 0;
		size_t lineStart = 0;
		while (lineStart <= code.size())
		{
			const size_t lineEnd = std::min(code.find('\n', lineStart), code.size());
			lineNumber++;
			std::string_view line = Trim(std::string_view(code).substr(lineStart, lineEnd - lineStart));
			lineStart = lineEnd + 1;
			if (line.empty() || line.front() != '#')
				continue;

			line = Trim(line.substr(1));
			size_t nameLength = 0;
			while (nameLength < line.size() && IsIdentifierCharacter(line[nameLength]))
				nameLength++;
			const std::string_view directive = line.substr(0, nameLength);
			const std::string_view argument = Trim(line.substr(nameLength));

			if (directive == "if" || directive == "ifdef" || directive == "ifndef")
			{
				conditionals.push_back(directive == "if" && argument == "0");
			}
			else if (directive == "elif" || directive == "elifdef" || directive == "elifndef" || directive == "else")
			{
				// The branch after an #if 0 may be taken; no other condition is evaluated.
				if (!conditionals.empty())
					conditionals.back() = false;
			}
			else if (directive == "endif")
			{
				if (!conditionals.empty())
					conditionals.pop_back();
			}
			else if (directive == "include" && !skipping() && argument.size() >= 2 && (argument.front() == '"' || argument.front() == '<'))
			{
				const bool angled = argument.front() == '<';
				const size_t close = argument.find(angled ? '>' : '"', 1);
				if (close != std::string_view::npos && close > 1)
					includes.push_back({ std::string(argument.substr(1, close - 1)), lineNumber, angled });
			}
		}
		return includes;
	}

	bool IsSourceFile(std::string_view path)
	{
		const size_t slash = path.rfind('/');
		const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
		const size_t dot = name.rfind('.');
		if (dot == std::string_view::npos || dot == 0)
			return false;

		const std::string extension = StringUtils::ToLower(name.substr(dot));
		return extension == ".h" || extension == ".hpp" || extension == ".inl" || extension == ".c" || extension == ".cpp"
			|| extension == ".m" || extension == ".mm";
	}

	bool MatchesGlob(std::string_view pattern, std::string_view path)
	{
		if (pattern.empty())
			return path.empty();

		if (pattern.starts_with("**"))
		{
			const std::string_view rest = pattern.substr(2);
			// "**/" may also stand for no directory at all.
			if (rest.starts_with('/') && MatchesGlob(rest.substr(1), path))
				return true;
			for (size_t skipped = 0; skipped <= path.size(); skipped++)
			{
				if (MatchesGlob(rest, path.substr(skipped)))
					return true;
			}
			return false;
		}
		if (pattern.front() == '*')
		{
			const std::string_view rest = pattern.substr(1);
			for (size_t skipped = 0; skipped <= path.size(); skipped++)
			{
				if (MatchesGlob(rest, path.substr(skipped)))
					return true;
				if (skipped < path.size() && path[skipped] == '/')
					return false;
			}
			return false;
		}
		if (path.empty())
			return false;
		if (pattern.front() == '?')
			return path.front() != '/' && MatchesGlob(pattern.substr(1), path.substr(1));
		return pattern.front() == path.front() && MatchesGlob(pattern.substr(1), path.substr(1));
	}

	bool ParseLayerTable(const nlohmann::json& json, LayerTable& outTable, std::vector<std::string>& outErrors)
	{
		const size_t errorCount = outErrors.size();
		outTable = LayerTable();
		if (!json.is_object() || !json.contains("Layers") || !json["Layers"].is_array())
		{
			outErrors.push_back("The layer table must be an object with a 'Layers' array");
			return false;
		}
		const auto maxEntries = json.find("MaxAllowlistEntries");
		if (maxEntries == json.end() || !maxEntries->is_number_integer() || maxEntries->get<int64_t>() < 0)
			outErrors.push_back("The layer table needs 'MaxAllowlistEntries', the number of allowlist entries");
		else
			outTable.MaxAllowlistEntries = static_cast<size_t>(maxEntries->get<int64_t>());

		std::unordered_map<std::string, size_t> indices;
		for (const nlohmann::json& layerJson : json["Layers"])
		{
			const std::string context = fmt::format("Layer {}", outTable.Layers.size());
			if (!layerJson.is_object() || !layerJson.contains("Name") || !layerJson["Name"].is_string() || layerJson["Name"].get_ref<const std::string&>().empty())
			{
				outErrors.push_back(context + ": every layer needs a 'Name'");
				continue;
			}
			Layer layer;
			layer.Name = layerJson["Name"].get<std::string>();
			const std::string named = fmt::format("Layer '{}'", layer.Name);
			ReadStringArray(layerJson, "Paths", layer.Paths, named, outErrors);
			ReadStringArray(layerJson, "Exclude", layer.Exclude, named, outErrors);
			ReadStringArray(layerJson, "MayInclude", layer.MayInclude, named, outErrors);
			if (layer.Paths.empty())
				outErrors.push_back(named + " has no 'Paths'");
			for (const std::string& allowed : layer.MayInclude)
			{
				// Only lower layers: the table cannot describe a cycle.
				if (!indices.contains(allowed))
					outErrors.push_back(fmt::format("{} may include '{}', which is not a layer listed below it", named, allowed));
			}
			if (!indices.emplace(layer.Name, outTable.Layers.size()).second)
				outErrors.push_back(named + " is listed twice");
			outTable.Layers.push_back(std::move(layer));
		}
		return outErrors.size() == errorCount;
	}

	bool ParseAllowlist(std::string_view text, std::vector<AllowlistEntry>& outEntries, std::vector<std::string>& outErrors)
	{
		const size_t errorCount = outErrors.size();
		std::set<std::pair<std::string, std::string>> seen;
		uint32_t lineNumber = 0;
		size_t lineStart = 0;
		while (lineStart <= text.size())
		{
			const size_t lineEnd = std::min(text.find('\n', lineStart), text.size());
			lineNumber++;
			const std::string_view line = Trim(text.substr(lineStart, lineEnd - lineStart));
			lineStart = lineEnd + 1;
			if (line.empty() || line.front() == '#')
				continue;

			const size_t firstBar = line.find('|');
			const size_t secondBar = firstBar == std::string_view::npos ? std::string_view::npos : line.find('|', firstBar + 1);
			const std::string_view edge = Trim(line.substr(0, firstBar));
			const size_t colon = edge.find(':');
			AllowlistEntry entry;
			entry.Line = lineNumber;
			if (secondBar != std::string_view::npos && colon != std::string_view::npos)
			{
				entry.File = std::string(Trim(edge.substr(0, colon)));
				entry.Header = std::string(Trim(edge.substr(colon + 1)));
				entry.Owner = std::string(Trim(line.substr(firstBar + 1, secondBar - firstBar - 1)));
				entry.Reason = std::string(Trim(line.substr(secondBar + 1)));
			}
			if (entry.File.empty() || entry.Header.empty() || entry.Owner.empty() || entry.Reason.empty())
			{
				outErrors.push_back(fmt::format("Allowlist line {}: expected '<file>:<included header> | <workstream that removes it> | <reason>'", lineNumber));
				continue;
			}
			if (!seen.emplace(entry.File, entry.Header).second)
			{
				outErrors.push_back(fmt::format("Allowlist line {}: {}:{} is listed twice", lineNumber, entry.File, entry.Header));
				continue;
			}
			outEntries.push_back(std::move(entry));
		}
		return outErrors.size() == errorCount;
	}

	LayeringReport CheckLayering(const LayerTable& table, const std::map<std::string, std::string>& files, const std::vector<AllowlistEntry>& allowlist)
	{
		LayeringReport report;

		std::unordered_map<std::string, const Layer*> fileLayers;
		for (const auto& [path, contents] : files)
		{
			const std::vector<const Layer*> layers = FindLayers(table, path);
			if (layers.size() == 1)
			{
				fileLayers.emplace(path, layers.front());
				continue;
			}
			std::string names;
			for (const Layer* layer : layers)
				names += fmt::format("{}'{}'", names.empty() ? "" : ", ", layer->Name);
			report.Errors.push_back(layers.empty()
				? fmt::format("{} is in no layer: add it to the paths of its layer in Layers.json", path)
				: fmt::format("{} is in several layers ({}): exclude it from all but one in Layers.json", path, names));
		}

		std::map<std::pair<std::string, std::string>, bool> allowlistUse;
		for (const AllowlistEntry& entry : allowlist)
			allowlistUse.emplace(std::pair(entry.File, entry.Header), false);

		for (const auto& [path, contents] : files)
		{
			const auto fileLayer = fileLayers.find(path);
			if (fileLayer == fileLayers.end())
				continue;
			const size_t slash = path.rfind('/');
			const std::string directory = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);

			for (const IncludeDirective& include : ParseIncludes(contents))
			{
				// Like the preprocessor: a quoted include next to the including file first, then (like an angle-bracket
				// include) from the include root.
				const std::optional<std::string> nextToFile = include.Angled ? std::nullopt : NormalizePath(directory + include.Path);
				const std::optional<std::string> fromRoot = NormalizePath(include.Path);
				const Layer* headerLayer = nullptr;
				std::string header;
				if (nextToFile && files.contains(*nextToFile))
				{
					header = *nextToFile;
					headerLayer = fileLayers.contains(header) ? fileLayers.at(header) : nullptr;
				}
				else if (fromRoot && files.contains(*fromRoot))
				{
					header = *fromRoot;
					headerLayer = fileLayers.contains(header) ? fileLayers.at(header) : nullptr;
				}
				else if (fromRoot)
				{
					// Not a source file: a generated header of a layer is still checked; anything else is another
					// target's header (StrataScript/ScriptABI.h) or a third-party one.
					const std::vector<const Layer*> layers = FindLayers(table, *fromRoot);
					header = *fromRoot;
					headerLayer = layers.size() == 1 ? layers.front() : nullptr;
				}
				if (!headerLayer)
					continue;

				report.CheckedIncludes++;
				const Layer& from = *fileLayer->second;
				const bool allowed = headerLayer == &from
					|| std::find(from.MayInclude.begin(), from.MayInclude.end(), headerLayer->Name) != from.MayInclude.end();
				if (allowed)
					continue;

				Violation violation { path, include.Line, header, from.Name, headerLayer->Name };
				auto entry = allowlistUse.find(std::pair(path, header));
				if (entry != allowlistUse.end())
				{
					entry->second = true;
					report.Allowlisted.push_back(std::move(violation));
				}
				else
				{
					report.Violations.push_back(std::move(violation));
				}
			}
		}

		for (const AllowlistEntry& entry : allowlist)
		{
			if (!allowlistUse.at(std::pair(entry.File, entry.Header)))
				report.StaleEntries.push_back(entry);
		}
		return report;
	}

	std::string Describe(const Violation& violation)
	{
		return fmt::format("{}({}): includes {}, but layer {} may not include layer {}", violation.File, violation.Line, violation.Header,
			violation.FileLayer, violation.HeaderLayer);
	}

}
