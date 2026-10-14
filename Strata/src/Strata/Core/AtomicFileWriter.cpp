#include "stpch.h"
#include "Strata/Core/AtomicFileWriter.h"

#include "Strata/Core/FileSystem.h"

#include <atomic>
#include <system_error>

namespace Strata
{

	namespace
	{

		// Numbers the temporary files of this process, so that two writers of one file never share theirs.
		std::atomic<uint64_t> s_NextTemporary = 0;

	}

	AtomicFileWriter::AtomicFileWriter(std::filesystem::path path)
		: m_Path(std::move(path))
	{
		m_TemporaryPath = m_Path;
		m_TemporaryPath += ".tmp" + std::to_string(s_NextTemporary.fetch_add(1, std::memory_order_relaxed));
		if (m_Path.has_parent_path() && !FileSystem::CreateDirectories(m_Path.parent_path()))
		{
			m_Error = fmt::format("Could not create the folder '{}'", FileSystem::ToUTF8(m_Path.parent_path()));
			return;
		}
		m_Stream.open(m_TemporaryPath, std::ios::binary | std::ios::trunc);
		if (!m_Stream)
		{
			m_Error = fmt::format("Could not create '{}'", FileSystem::ToUTF8(m_TemporaryPath));
			return;
		}
		m_Buffer.reserve(c_BufferSize);
		m_State = State::Open;
	}

	AtomicFileWriter::~AtomicFileWriter()
	{
		Abort();
	}

	bool AtomicFileWriter::Write(std::string_view bytes)
	{
		if (m_State != State::Open)
			return false;
		m_BytesWritten += bytes.size();
		// Large pieces go straight to the stream once the buffer is out of the way; small ones gather in the buffer.
		if (m_Buffer.size() + bytes.size() > c_BufferSize)
		{
			if (!Flush())
				return false;
			if (bytes.size() >= c_BufferSize)
			{
				m_Stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
				if (!m_Stream)
				{
					Fail(fmt::format("Could not write '{}'", FileSystem::ToUTF8(m_TemporaryPath)));
					return false;
				}
				return true;
			}
		}
		m_Buffer.append(bytes);
		return true;
	}

	bool AtomicFileWriter::Flush()
	{
		if (m_Buffer.empty())
			return true;
		m_Stream.write(m_Buffer.data(), static_cast<std::streamsize>(m_Buffer.size()));
		m_Buffer.clear();
		if (!m_Stream)
		{
			Fail(fmt::format("Could not write '{}'", FileSystem::ToUTF8(m_TemporaryPath)));
			return false;
		}
		return true;
	}

	bool AtomicFileWriter::Finish(std::string* outError)
	{
		if (m_State == State::Open)
		{
			if (Flush())
			{
				m_Stream.flush();
				m_Stream.close();
				if (m_Stream.fail())
					Fail(fmt::format("Could not write '{}'", FileSystem::ToUTF8(m_TemporaryPath)));
				else
					m_State = State::Finished;
			}
		}
		if (m_State == State::Finished || m_State == State::Committed)
			return true;
		if (outError)
			*outError = m_Error.empty() ? "The file was not written" : m_Error;
		return false;
	}

	bool AtomicFileWriter::Commit(std::string* outError)
	{
		if (m_State == State::Committed)
			return true;
		if (!Finish(outError))
			return false;
		std::error_code error;
		std::filesystem::rename(m_TemporaryPath, m_Path, error);
		if (error)
		{
			Fail(fmt::format("Could not replace '{}': {}", FileSystem::ToUTF8(m_Path), error.message()));
			if (outError)
				*outError = m_Error;
			return false;
		}
		m_State = State::Committed;
		return true;
	}

	void AtomicFileWriter::Abort()
	{
		if (m_State == State::Committed || m_State == State::Failed)
			return;
		Fail(m_Error.empty() ? std::string("Aborted") : m_Error);
	}

	void AtomicFileWriter::Fail(std::string error)
	{
		if (m_Stream.is_open())
			m_Stream.close();
		m_Buffer.clear();
		m_Buffer.shrink_to_fit();
		if (m_State != State::Failed)
		{
			std::error_code removeError;
			std::filesystem::remove(m_TemporaryPath, removeError);
		}
		m_State = State::Failed;
		if (m_Error.empty())
			m_Error = std::move(error);
	}

}
