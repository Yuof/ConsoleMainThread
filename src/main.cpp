// Runs the lines of a console `bat` file as a main-thread task, outside the UI's message processing (SkyrimSE 1.6.x,
// Address Library). `bat <file>` does not run its lines at once: it queues a message for the Console menu, and the
// lines run while UI::ProcessMessages handles that message - holding the UI's message lock. A line that changes cells
// (coc, cow, movetoqt, ...) then performs the whole load inside it (2026-10-28, crash dumps + Ghidra):
//  - while the game runs, that is the UI job thread: the load start / end processes UI messages again on the same
//    thread and its UIMessageQueue::ProcessCommands destroys the bat message's data, so the loop reads the next line
//    from a freed file stream and crashes; the menus are also advanced and drawn by two threads at once afterwards;
//  - with the console open it is the main thread: the load's end waits for the loading screen thread, which waits for
//    the message lock the main thread holds - the game freezes.
// The plugin wraps the call in Console::ProcessMessage that starts the line loop: it reads every line at once (the
// stream is still alive there) and queues one main-thread task that runs them in order the way the loop does -
// echoed to the console, compiled with the console's compiler and run on the reference selected when the bat started.

namespace
{
	constexpr REL::ID        kProcessMessage{ 442669 };  // Console::ProcessMessage
	constexpr std::uintptr_t kCallOffset = 0x69F;        // its call of the bat line loop
	constexpr REL::ID        kLineLoop{ 21891 };         // runs every line of a bat stream
	constexpr REL::ID        kReadLine{ 69036 };         // stream -> next line, 0 at the end

	using LineLoop = void(void* a_stream, void* a_compiler, std::uint32_t* a_selectedRef, void* a_printer);
	using ReadLine = std::int32_t(void* a_stream, char* a_buffer, std::uint32_t a_size);

	DWORD                      mainThread = 0;
	std::atomic<std::uint64_t> deferred{ 0 };

	void LineLoopThunk(void* a_stream, void*, std::uint32_t* a_selectedRef, void* a_printer)
	{
		static REL::Relocation<ReadLine> readLine{ kReadLine };
		std::vector<std::string> lines;
		std::array<char, 0x104>  buffer{};
		while (readLine(a_stream, buffer.data(), static_cast<std::uint32_t>(buffer.size())) != 0) {
			std::string line(buffer.data());
			std::replace(line.begin(), line.end(), '\r', ' ');
			if (line.size() > 1) {  // the original skips empty lines the same way
				lines.push_back(std::move(line));
			}
			buffer.fill(0);
		}
		const auto n = ++deferred;
		const RE::RefHandle selected = a_selectedRef ? *a_selectedRef : 0;
		logger::info("bat #{}: {} line(s) moved to the main thread (read on the {} thread)"sv, n, lines.size(),
			GetCurrentThreadId() == mainThread ? "main" : "UI job");
		SKSE::GetTaskInterface()->AddTask([lines = std::move(lines), selected, echo = a_printer != nullptr]() {
			const auto log = RE::ConsoleLog::GetSingleton();
			const auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
			for (const auto& line : lines) {
				if (echo && log) {
					log->Print(line.c_str());
				}
				const auto script = factory ? factory->Create() : nullptr;
				if (!script) {
					logger::error("no Script form: '{}' not run"sv, line);
					continue;
				}
				RE::NiPointer<RE::TESObjectREFR> ref;  // looked up per line, as the loop does
				RE::LookupReferenceByHandle(selected, ref);
				RE::ScriptCompiler compiler;
				script->SetCommand(line);
				script->CompileAndRun(&compiler, ref.get(), RE::COMPILER_NAME::kSystemWindowCompiler);
				delete script;
			}
		});
	}

	std::optional<std::string_view> Hook()
	{
		const auto site = kProcessMessage.address() + kCallOffset;
		const auto* code = reinterpret_cast<const std::uint8_t*>(site);
		if (code[0] != 0xE8) {
			return "no call at Console::ProcessMessage+0x69F"sv;
		}
		const auto target = site + 5 + *reinterpret_cast<const std::int32_t*>(code + 1);
		if (target != kLineLoop.address()) {
			return "the call there is not the bat line loop"sv;
		}
		SKSE::AllocTrampoline(14);
		SKSE::GetTrampoline().write_call<5>(site, LineLoopThunk);
		return std::nullopt;
	}
}

SKSEPluginInfo(
	.Version = Plugin::VERSION,
	.Name = Plugin::NAME,
	.Author = "Yuof"sv,
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse, { .log = true });
	mainThread = GetCurrentThreadId();
	logger::info("{} v{} on {}"sv, Plugin::NAME, Plugin::VERSION.string(), REL::Module::get().version().string());
	if (const auto why = Hook()) {
		logger::error("self-check failed ({}): inactive"sv, *why);
	} else {
		logger::info("self-check passed, bat lines run as a main-thread task"sv);
	}
	return true;
}
