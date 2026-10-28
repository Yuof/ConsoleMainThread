// Runs the lines of a console `bat` file on the main thread (SkyrimSE 1.6.x, Address Library).
// `bat <file>` does not run its lines at once: it queues a message for the Console menu, and the lines run when the
// UI processes that message - on the UI job thread while the game is running. A line that changes cells (coc, cow,
// movetoqt, ...) then performs the whole load on that thread. Three crashes follow from it (2026-10-28, crash dumps +
// Ghidra): the load start / end processes UI messages again on the same thread and its UIMessageQueue::ProcessCommands
// destroys the bat message's data, so the loop reads the next line from a freed file stream; the main thread keeps
// rendering the scene that is being unloaded; and menus are advanced and drawn by two threads at once.
// The plugin wraps the call in Console::ProcessMessage that starts the line loop: on the main thread it runs as
// before; elsewhere it reads every line at once (the stream is still alive there) and queues one main-thread task
// that runs them in order, the way the console runs a typed command.

namespace
{
	constexpr REL::ID        kProcessMessage{ 442669 };  // Console::ProcessMessage
	constexpr std::uintptr_t kCallOffset = 0x69F;        // its call of the bat line loop
	constexpr REL::ID        kLineLoop{ 21891 };         // runs every line of a bat stream
	constexpr REL::ID        kReadLine{ 69036 };         // stream -> next line, 0 at the end

	using LineLoop = void(void* a_stream, void* a_compiler, std::uint32_t* a_selectedRef, void* a_printer);
	using ReadLine = std::int32_t(void* a_stream, char* a_buffer, std::uint32_t a_size);

	REL::Relocation<LineLoop>  lineLoop;
	DWORD                      mainThread = 0;
	std::atomic<std::uint64_t> deferred{ 0 };

	void LineLoopThunk(void* a_stream, void* a_compiler, std::uint32_t* a_selectedRef, void* a_printer)
	{
		if (GetCurrentThreadId() == mainThread) {
			return lineLoop(a_stream, a_compiler, a_selectedRef, a_printer);
		}
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
		logger::info("bat #{}: {} line(s) moved to the main thread"sv, n, lines.size());
		SKSE::GetTaskInterface()->AddTask([lines = std::move(lines)]() {
			const auto log = RE::ConsoleLog::GetSingleton();
			for (const auto& line : lines) {
				if (log) {
					log->Print(line.c_str());
				}
				RE::Console::ExecuteCommand(line.c_str());
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
		lineLoop = SKSE::GetTrampoline().write_call<5>(site, LineLoopThunk);
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
		logger::info("self-check passed, bat lines off the main thread are moved to it"sv);
	}
	return true;
}
