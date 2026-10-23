#include "Editor/SettingsFileWriter.h"

#include <Strata/Core/JobSystem.h>

#include <atomic>
#include <utility>

namespace Strata
{

	const char* SettingsFileStateToString(SettingsFileState state)
	{
		switch (state)
		{
			case SettingsFileState::Written: return "written";
			case SettingsFileState::Writing: return "writing";
			case SettingsFileState::Unwritten: return "unwritten";
		}
		return "unknown";
	}

	struct SettingsFileWriter::Job
	{
		WriteFunction Write;
		std::function<void()> TestHook;
		std::string Error;            // Written by the job before Done
		bool Succeeded = false;       // Likewise
		std::atomic<bool> Done = false;
		JobHandle Handle;

		void Run()
		{
			if (TestHook)
				TestHook();
			Succeeded = Write(&Error);
			if (Succeeded)
				Error.clear();
			else if (Error.empty())
				Error = "the file could not be written";
			Done.store(true, std::memory_order_release);
		}
	};

	SettingsFileWriter::SettingsFileWriter(Completion onWritten)
		: m_OnWritten(std::move(onWritten))
	{
	}

	SettingsFileWriter::~SettingsFileWriter()
	{
		Finish();
	}

	void SettingsFileWriter::Submit(WriteFunction write)
	{
		if (m_Running)
			m_Waiting = std::move(write);
		else
			Start(std::move(write));
	}

	void SettingsFileWriter::Update()
	{
		if (Collect() && m_Waiting)
			Start(std::exchange(m_Waiting, nullptr));
	}

	void SettingsFileWriter::Finish()
	{
		while (IsBusy())
		{
			if (m_Running)
			{
				JobSystem::Wait(m_Running->Handle);
				Collect();
				continue;
			}
			Start(std::exchange(m_Waiting, nullptr));
		}
	}

	SettingsFileState SettingsFileWriter::GetState() const
	{
		if (IsBusy())
			return SettingsFileState::Writing;
		return m_Failed ? SettingsFileState::Unwritten : SettingsFileState::Written;
	}

	void SettingsFileWriter::Start(WriteFunction write)
	{
		Ref<Job> job = CreateRef<Job>();
		job->Write = std::move(write);
		job->TestHook = m_TestHook;
		job->Handle = JobSystem::SubmitIO([job]() { job->Run(); }, JobPriority::Background);
		m_Running = std::move(job);
	}

	bool SettingsFileWriter::Collect()
	{
		if (!m_Running)
			return true;
		if (!m_Running->Done.load(std::memory_order_acquire))
			return false;
		JobSystem::Wait(m_Running->Handle);
		const Ref<Job> ended = std::exchange(m_Running, nullptr);
		m_WriteCount++;
		m_Failed = !ended->Succeeded;
		m_LastError = ended->Error;
		if (m_OnWritten)
			m_OnWritten(ended->Error);
		return true;
	}

}
