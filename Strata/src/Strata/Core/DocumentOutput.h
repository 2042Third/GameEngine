#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace Strata
{

	// Where a document writer (SceneDocumentWriter) puts the bytes it produces, in order: a file (AtomicFileWriter) or a
	// string (StringDocumentOutput). Used by one thread at a time.
	class DocumentOutput
	{
	public:
		virtual ~DocumentOutput() = default;

		// Appends bytes. False once a write failed: the output stays failed and takes nothing more.
		virtual bool Write(std::string_view bytes) = 0;
		// The bytes taken so far.
		virtual uint64_t GetBytesWritten() const = 0;
	};

	// A document written into a string: small documents and tests.
	class StringDocumentOutput final : public DocumentOutput
	{
	public:
		bool Write(std::string_view bytes) override
		{
			m_Text.append(bytes);
			return true;
		}

		uint64_t GetBytesWritten() const override { return m_Text.size(); }

		const std::string& GetText() const { return m_Text; }
		std::string TakeText() { return std::exchange(m_Text, std::string()); }
	private:
		std::string m_Text;
	};

}
