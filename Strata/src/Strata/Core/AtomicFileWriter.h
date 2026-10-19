#pragma once

#include "Strata/Core/DocumentOutput.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace Strata
{

	// Writes a file's new contents as a stream without ever leaving it half written: the bytes go, through a buffer of
	// c_BufferSize, into a temporary file next to it ("<path>.tmp<n>"), which Commit renames over the file once everything is
	// written. A writer that is aborted, or destroyed before it committed, removes its temporary file and leaves the file as
	// it was. Large documents (a scene of a million entities is hundreds of megabytes) are written this way on background
	// threads, without holding them in memory. Used by one thread at a time.
	class AtomicFileWriter final : public DocumentOutput
	{
	public:
		static constexpr size_t c_BufferSize = size_t(1) << 20;

		// Opens the temporary file (creating the file's folder when it is missing). IsOpen tells whether that worked, and
		// GetError why not.
		explicit AtomicFileWriter(std::filesystem::path path);
		// Aborts unless it committed.
		~AtomicFileWriter() override;

		AtomicFileWriter(const AtomicFileWriter&) = delete;
		AtomicFileWriter& operator=(const AtomicFileWriter&) = delete;

		bool IsOpen() const { return m_State == State::Open; }
		// Buffers the bytes and writes them out whenever the buffer fills. False (and the writer failed) when the temporary
		// file is not open or a write failed.
		bool Write(std::string_view bytes) override;
		uint64_t GetBytesWritten() const override { return m_BytesWritten; }

		// Writes what is buffered and closes the temporary file, which then holds the complete new contents; the file itself
		// is unchanged. False, with the reason (and the writer failed, its temporary file removed), when that fails.
		bool Finish(std::string* outError = nullptr);
		// Finish (when not finished yet), then replaces the file with the temporary file. False, with the reason, when either
		// fails: the file is then as it was and the temporary file is removed.
		bool Commit(std::string* outError = nullptr);
		// Removes the temporary file; the file stays as it was. Nothing happens once committed.
		void Abort();

		const std::filesystem::path& GetPath() const { return m_Path; }
		const std::filesystem::path& GetTemporaryPath() const { return m_TemporaryPath; }
		bool IsCommitted() const { return m_State == State::Committed; }
		// Why the writer failed (empty while it has not).
		const std::string& GetError() const { return m_Error; }
	private:
		enum class State : uint8_t
		{
			Open = 0,
			Finished,  // Complete and closed, not renamed yet
			Committed,
			Failed     // Its temporary file is gone
		};

		bool Flush();
		void Fail(std::string error);
	private:
		std::filesystem::path m_Path;
		std::filesystem::path m_TemporaryPath;
		std::ofstream m_Stream;
		std::string m_Buffer;
		uint64_t m_BytesWritten = 0;
		State m_State = State::Failed;
		std::string m_Error;
	};

}
