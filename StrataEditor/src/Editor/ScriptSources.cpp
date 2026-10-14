#include "Editor/ScriptSources.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/StringUtils.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <iterator>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Strata
{

	namespace
	{

		constexpr std::string_view c_CodeExtensions[] = { ".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hh", ".hxx", ".inl", ".ipp" };
		// The longest delimiter a raw string literal may have.
		constexpr size_t c_MaxRawStringDelimiter = 16;

		bool IsIdentifierCharacter(char character)
		{
			return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
		}

		bool IsBlank(char character)
		{
			return std::isspace(static_cast<unsigned char>(character)) != 0;
		}

		size_t SkipBlanks(std::string_view text, size_t position)
		{
			while (position < text.size() && IsBlank(text[position]))
				position++;
			return position;
		}

		// The end of the line starting at or before `position`, past backslash continuations (a preprocessor line).
		size_t FindLogicalLineEnd(std::string_view text, size_t position)
		{
			while (position < text.size())
			{
				const size_t end = text.find('\n', position);
				if (end == std::string_view::npos)
					return text.size();
				size_t last = end;
				if (last > position && text[last - 1] == '\r')
					last--;
				if (last == position || text[last - 1] != '\\')
					return end;
				position = end + 1;
			}
			return text.size();
		}

		// The end (one past the closing quote) of the raw string literal whose opening quote is at `quote`, or nothing when
		// what follows the quote is no raw string's opening (a delimiter of at most 16 characters without blanks, '\\' or
		// parentheses, then '('). An unterminated literal ends with the text.
		std::optional<size_t> FindRawStringEnd(std::string_view text, size_t quote)
		{
			const size_t open = text.find('(', quote + 1);
			if (open == std::string_view::npos || open - quote - 1 > c_MaxRawStringDelimiter)
				return std::nullopt;
			const std::string_view delimiter = text.substr(quote + 1, open - quote - 1);
			for (const char character : delimiter)
			{
				if (IsBlank(character) || character == '\\' || character == ')' || character == '"')
					return std::nullopt;
			}
			const std::string closing = ")" + std::string(delimiter) + "\"";
			const size_t close = text.find(closing, open + 1);
			return close == std::string_view::npos ? text.size() : close + closing.size();
		}

		// Whether an identifier that ends right before a quote is the prefix of a raw string literal (R, LR, uR, UR, u8R).
		bool IsRawStringPrefix(std::string_view word)
		{
			return word == "R" || word == "LR" || word == "uR" || word == "UR" || word == "u8R";
		}

	}

	bool IsScriptCodeFileName(std::string_view fileName)
	{
		const size_t dot = fileName.rfind('.');
		if (dot == std::string_view::npos)
			return false;
		const std::string extension = StringUtils::ToLower(fileName.substr(dot));
		return std::find(std::begin(c_CodeExtensions), std::end(c_CodeExtensions), extension) != std::end(c_CodeExtensions);
	}

	bool IsScriptSourceFileName(std::string_view fileName)
	{
		if (fileName == "CMakeLists.txt")
			return true;
		if (IsScriptCodeFileName(fileName))
			return true;
		const size_t dot = fileName.rfind('.');
		return dot != std::string_view::npos && StringUtils::ToLower(fileName.substr(dot)) == ".cmake";
	}

	std::vector<std::string> FindScriptClassRegistrations(std::string_view code)
	{
		constexpr std::string_view c_Macro = "ST_SCRIPT_CLASS";
		std::vector<std::string> classes;
		bool lineStart = true; // Only blanks (and comments) since the line began
		size_t position = 0;
		while (position < code.size())
		{
			const char character = code[position];
			const char next = position + 1 < code.size() ? code[position + 1] : '\0';
			if (character == '\n')
			{
				lineStart = true;
				position++;
			}
			else if (IsBlank(character))
			{
				position++;
			}
			else if (character == '/' && next == '/')
			{
				const size_t end = code.find('\n', position);
				position = end == std::string_view::npos ? code.size() : end;
			}
			else if (character == '/' && next == '*')
			{
				const size_t end = code.find("*/", position + 2);
				position = end == std::string_view::npos ? code.size() : end + 2;
			}
			else if (character == '#' && lineStart)
			{
				// A preprocessor line (the SDK's #define of the macro names a parameter, not a class).
				position = FindLogicalLineEnd(code, position);
			}
			else if (character == '"' || character == '\'')
			{
				// A string or character literal: what it holds is no code.
				size_t end = position + 1;
				while (end < code.size() && code[end] != character && code[end] != '\n')
					end += code[end] == '\\' ? 2 : 1;
				// An unterminated literal ends with its line, whose end the next character starts again.
				position = end < code.size() && code[end] == '\n' ? end : std::min(end + 1, code.size());
				lineStart = false;
			}
			else if (IsIdentifierCharacter(character))
			{
				size_t end = position;
				while (end < code.size() && IsIdentifierCharacter(code[end]))
					end++;
				const std::string_view word = code.substr(position, end - position);
				position = end;
				lineStart = false;
				// A raw string literal may span lines and hold quotes: nothing in it is code.
				if (end < code.size() && code[end] == '"' && IsRawStringPrefix(word))
				{
					if (const std::optional<size_t> rawEnd = FindRawStringEnd(code, end))
					{
						position = *rawEnd;
						continue;
					}
				}
				if (word != c_Macro)
					continue;
				size_t cursor = SkipBlanks(code, end);
				if (cursor >= code.size() || code[cursor] != '(')
					continue;
				cursor = SkipBlanks(code, cursor + 1);
				size_t nameEnd = cursor;
				while (nameEnd < code.size() && IsIdentifierCharacter(code[nameEnd]))
					nameEnd++;
				const std::string_view name = code.substr(cursor, nameEnd - cursor);
				cursor = SkipBlanks(code, nameEnd);
				if (!name.empty() && !std::isdigit(static_cast<unsigned char>(name.front())) && cursor < code.size() && code[cursor] == ')')
				{
					classes.emplace_back(name);
					position = cursor + 1;
				}
			}
			else
			{
				position++;
				lineStart = false;
			}
		}
		return classes;
	}

	namespace
	{

		// The walk of ListScriptSources and StampScriptSources: code files are read for their classes only when `readClasses`.
		ScriptSourceListing CollectScriptSources(const std::filesystem::path& directory, const ScriptSourceListing* previous, size_t maxFiles,
			size_t maxEntries, bool readClasses)
		{
			ScriptSourceListing listing;
			// A missing directory holds no files. Anything else the walk cannot finish (a file in its place, an error while it
			// walks) leaves the files unknown: Truncated.
			if (!FileSystem::Exists(directory))
				return listing;

			// The classes of the previous listing, lent to files whose size and write time are unchanged.
			std::unordered_map<std::string_view, const ScriptSourceFile*> known;
			if (previous)
			{
				for (const ScriptSourceFile& file : previous->Files)
					known.emplace(file.Path, &file);
			}

			size_t entries = 0;
			std::error_code error;
			std::filesystem::recursive_directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, error);
			for (; !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
			{
				if (++entries > maxEntries)
				{
					listing.Truncated = true;
					break;
				}
				const std::filesystem::path& path = it->path();
				const std::string name = FileSystem::ToUTF8(path.filename());
				std::error_code typeError;
				if (it->is_directory(typeError))
				{
					// An IDE's folder, version control, or a build tree: none of it is the scripts' source.
					if ((!name.empty() && name.front() == '.') || FileSystem::IsRegularFile(path / "CMakeCache.txt"))
						it.disable_recursion_pending();
					continue;
				}
				if (!IsScriptSourceFileName(name) || !it->is_regular_file(typeError))
					continue;
				if (listing.Files.size() >= maxFiles)
				{
					listing.Truncated = true;
					break;
				}

				ScriptSourceFile file;
				file.Path = FileSystem::ToUTF8(path.lexically_relative(directory));
				file.Size = FileSystem::GetFileSize(path).value_or(0);
				file.LastWriteTime = FileSystem::GetLastWriteTime(path).value_or(0);
				if (readClasses && IsScriptCodeFileName(name))
				{
					const auto found = known.find(file.Path);
					if (found != known.end() && found->second->Size == file.Size && found->second->LastWriteTime == file.LastWriteTime)
					{
						file.Classes = found->second->Classes;
					}
					else if (file.Size <= c_MaxClassScanBytes)
					{
						if (const std::optional<std::string> text = FileSystem::ReadText(path))
							file.Classes = FindScriptClassRegistrations(*text);
					}
				}
				listing.Files.push_back(std::move(file));
			}
			// The walk ended on an error (the path is no directory, a folder was removed or replaced while it walked): the files
			// after it are unknown. Folders that may not be read are skipped without one (skip_permission_denied).
			if (error)
				listing.Truncated = true;

			// By path ignoring case (each lowered once), and by the exact path where only case differs.
			std::vector<std::pair<std::string, size_t>> order;
			order.reserve(listing.Files.size());
			for (size_t index = 0; index < listing.Files.size(); index++)
				order.emplace_back(StringUtils::ToLower(listing.Files[index].Path), index);
			std::sort(order.begin(), order.end(), [&listing](const std::pair<std::string, size_t>& a, const std::pair<std::string, size_t>& b)
			{
				return a.first != b.first ? a.first < b.first : listing.Files[a.second].Path < listing.Files[b.second].Path;
			});
			std::vector<ScriptSourceFile> sorted;
			sorted.reserve(order.size());
			for (const std::pair<std::string, size_t>& entry : order)
				sorted.push_back(std::move(listing.Files[entry.second]));
			listing.Files = std::move(sorted);
			return listing;
		}

	}

	ScriptSourceListing ListScriptSources(const std::filesystem::path& directory, const ScriptSourceListing* previous, size_t maxFiles, size_t maxEntries)
	{
		return CollectScriptSources(directory, previous, maxFiles, maxEntries, true);
	}

	ScriptSourceListing StampScriptSources(const std::filesystem::path& directory)
	{
		return CollectScriptSources(directory, nullptr, c_MaxListedScriptSources, c_MaxScannedScriptEntries, false);
	}

	std::optional<ScriptSourceChange> FindScriptSourceChange(const ScriptSourceListing& before, const ScriptSourceListing& after)
	{
		std::unordered_map<std::string_view, const ScriptSourceFile*> earlier;
		earlier.reserve(before.Files.size());
		for (const ScriptSourceFile& file : before.Files)
			earlier.emplace(file.Path, &file);
		size_t kept = 0;
		for (const ScriptSourceFile& file : after.Files)
		{
			const auto found = earlier.find(file.Path);
			if (found == earlier.end())
				return ScriptSourceChange { file.Path, ScriptSourceChangeKind::Added };
			if (found->second->Size != file.Size || found->second->LastWriteTime != file.LastWriteTime)
				return ScriptSourceChange { file.Path, ScriptSourceChangeKind::Changed };
			kept++;
		}
		if (kept == before.Files.size())
			return std::nullopt;
		std::unordered_set<std::string_view> later;
		later.reserve(after.Files.size());
		for (const ScriptSourceFile& file : after.Files)
			later.insert(file.Path);
		for (const ScriptSourceFile& file : before.Files)
		{
			if (!later.contains(file.Path))
				return ScriptSourceChange { file.Path, ScriptSourceChangeKind::Removed };
		}
		return std::nullopt;
	}

	const char* ScriptSourceChangeKindToString(ScriptSourceChangeKind kind)
	{
		switch (kind)
		{
			case ScriptSourceChangeKind::Changed: return "changed";
			case ScriptSourceChangeKind::Added:   return "was added";
			case ScriptSourceChangeKind::Removed: return "was removed";
		}
		return "changed";
	}

	struct ScriptSourceList::Job
	{
		std::filesystem::path Directory;
		Ref<const ScriptSourceListing> Previous;
		bool HasPrevious = false; // Previous is a listing of Directory (not the empty one a new directory starts with)
		// Written by the job before Done; Result only when Changed.
		Ref<const ScriptSourceListing> Result;
		bool Changed = false;
		std::atomic<bool> Done = false;
	};

	ScriptSourceList::ScriptSourceList()
		: m_Listing(CreateRef<ScriptSourceListing>())
	{
	}

	void ScriptSourceList::Refresh(const std::filesystem::path& directory)
	{
		if (directory != m_Directory)
		{
			Forget();
			m_Directory = directory;
		}
		if (m_Job)
		{
			m_RefreshPending = true;
			return;
		}
		Start();
	}

	void ScriptSourceList::Start()
	{
		Ref<Job> job = CreateRef<Job>();
		job->Directory = m_Directory;
		job->Previous = m_Listing;
		job->HasPrevious = m_HasListed;
		m_Job = job;
		JobSystem::SubmitIO([job]()
		{
			ScriptSourceListing listing = ListScriptSources(job->Directory, job->Previous.get());
			job->Changed = !job->HasPrevious || listing != *job->Previous;
			// An unchanged listing is freed here, not on the main thread.
			if (job->Changed)
				job->Result = CreateRef<ScriptSourceListing>(std::move(listing));
			job->Done.store(true, std::memory_order_release);
		});
	}

	bool ScriptSourceList::Update()
	{
		if (!m_Job || !m_Job->Done.load(std::memory_order_acquire))
			return false;
		const bool changed = m_Job->Changed;
		if (changed)
		{
			m_Listing = m_Job->Result;
			m_Version++;
		}
		m_HasListed = true;
		m_Job.reset();
		if (m_RefreshPending)
		{
			m_RefreshPending = false;
			Start();
		}
		return changed;
	}

	void ScriptSourceList::Clear()
	{
		Forget();
		m_Directory.clear();
	}

	void ScriptSourceList::Forget()
	{
		// A job still listing finishes on its own; its result is never taken.
		m_Listing = CreateRef<ScriptSourceListing>();
		m_HasListed = false;
		m_Job.reset();
		m_RefreshPending = false;
		m_Version++;
	}

}
