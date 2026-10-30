// Console Main Thread (SkyrimSE 1.6.x, Address Library): runs the console's cell changes and `bat` files on the main
// thread, at the start of its next loop (the main loop's call of Main::OnIdle). On 1.6.1170 a console command from a
// script, a bat file or a tool does not run on the main thread: SKSE's task queue and the console's UI message
// processing are drained by BSJobs worker threads (and an SKSE task is no way back to the main thread either).
//
// Cell changes (coc, coe, cow): the handlers call PlayerCharacter::CenterOnCell_Impl right there, so the whole switch -
// the old cell's teardown, ShadowSceneNode::SetPortalGraph, the light queues - runs on that worker while the main
// thread renders the frame (2026-10-24 / 2026-10-30, crash dumps + Ghidra): SkyrimSE+14EAFFA (an effect shape drawn
// with a light the switch had just freed), SkyrimSE+14A6BBA (ShadowSceneNode::AccumulateLight walking a room's light
// list the switch emptied mid-walk). Door travel runs the same switch on the main thread and shows neither. Off the
// main thread the handlers' call of CenterOnCell_Impl is queued; on the main thread (console open, doors) it runs as is.
//
// Bat files: `bat <file>` queues a message for the Console menu, and the lines run while UI::ProcessMessages handles
// it - holding the UI's message lock. A line that changes cells performs the whole load inside it (2026-10-28):
//  - while the game runs, that is the UI job thread: the load start / end processes UI messages again on the same
//    thread and its UIMessageQueue::ProcessCommands destroys the bat message's data, so the loop reads the next line
//    from a freed file stream and crashes; the menus are also advanced and drawn by two threads at once afterwards;
//  - with the console open it is the main thread: the load's end waits for the loading screen thread, which waits for
//    the message lock the main thread holds - the game freezes.
// The call in Console::ProcessMessage that starts the line loop is wrapped: every line is read at once (the stream is
// still alive there) and the lines are queued as one batch that runs them in order the way the loop does - echoed to
// the console, compiled with the console's compiler and run on the reference selected when the bat started. A cell
// change among them runs inline (main thread), so the lines keep their order.

namespace
{
	constexpr REL::ID        kMainLoop{ 36544 };
	constexpr std::uintptr_t kOnIdleCall = 0x160;  // MainLoop's call of Main::OnIdle (36564)

	constexpr REL::ID        kCenterOnCellImpl{ 40437 };  // PlayerCharacter::CenterOnCell_Impl(name, cell)
	struct Site
	{
		REL::ID          handler;
		std::uintptr_t   offset;  // the handler's call of CenterOnCell_Impl
		std::string_view command;
	};
	constexpr std::array kSites{
		Site{ REL::ID(22873), 0x5B, "coc"sv },   // ConsoleFunc CenterOnCell: (player, cell name, nullptr)
		Site{ REL::ID(22875), 0x109, "coe"sv },  // CenterOnExterior: (player, nullptr, cell)
		Site{ REL::ID(22876), 0x104, "cow"sv },  // CenterOnWorld: (player, nullptr, cell)
	};

	constexpr REL::ID        kProcessMessage{ 442669 };  // Console::ProcessMessage
	constexpr std::uintptr_t kLineLoopCall = 0x69F;      // its call of the bat line loop
	constexpr REL::ID        kLineLoop{ 21891 };         // runs every line of a bat stream
	constexpr REL::ID        kReadLine{ 69036 };         // stream -> next line, 0 at the end

	using OnIdle = void(void*);
	using CenterOnCell = bool(RE::PlayerCharacter*, const char*, RE::TESObjectCELL*);
	using ReadLine = std::int32_t(void* a_stream, char* a_buffer, std::uint32_t a_size);

	DWORD                              mainThread = 0;
	std::uintptr_t                     onIdle = 0;  // what the main loop called before (another plugin may hook it too)
	std::mutex                         queueLock;
	std::vector<std::function<void()>> queue;
	std::atomic<std::uint64_t>         moved{ 0 }, bats{ 0 };

	void Post(std::function<void()> a_task)
	{
		std::scoped_lock l{ queueLock };
		queue.push_back(std::move(a_task));
	}

	void OnIdleThunk(void* a_main)
	{
		std::vector<std::function<void()>> run;
		{
			std::scoped_lock l{ queueLock };
			run.swap(queue);
		}
		for (auto& f : run) {
			f();
		}
		reinterpret_cast<OnIdle*>(onIdle)(a_main);
	}

	bool Original(RE::PlayerCharacter* a_player, const char* a_name, RE::TESObjectCELL* a_cell)
	{
		static REL::Relocation<CenterOnCell*> func{ kCenterOnCellImpl };
		return func(a_player, a_name, a_cell);
	}

	template <std::size_t I>
	bool CenterOnCellThunk(RE::PlayerCharacter* a_player, const char* a_name, RE::TESObjectCELL* a_cell)
	{
		if (GetCurrentThreadId() == mainThread) {
			return Original(a_player, a_name, a_cell);
		}
		// The name lives in the handler's stack buffer; the handler ignores the result (it returns true either way).
		const auto        n = ++moved;
		const bool        hasName = a_name != nullptr;
		std::string       name = hasName ? a_name : "";
		const auto        queued = std::chrono::steady_clock::now();
		const std::string what = hasName ? fmt::format("'{}'", name) : fmt::format("cell {:08X}", a_cell ? a_cell->GetFormID() : 0);
		logger::info("{} #{} {} called on thread {}: moved to the main thread"sv, kSites[I].command, n, what, GetCurrentThreadId());
		Post([n, hasName, name = std::move(name), a_cell, queued, what]() {
			const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - queued).count();
			logger::info("{} #{} {} runs on the main thread ({:.1f} ms later)"sv, kSites[I].command, n, what, ms);
			Original(RE::PlayerCharacter::GetSingleton(), hasName ? name.c_str() : nullptr, a_cell);
		});
		return true;
	}

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
		const auto n = ++bats;
		const RE::RefHandle selected = a_selectedRef ? *a_selectedRef : 0;
		logger::info("bat #{}: {} line(s) moved to the main thread (read on the {} thread)"sv, n, lines.size(),
			GetCurrentThreadId() == mainThread ? "main" : "UI job");
		Post([lines = std::move(lines), selected, echo = a_printer != nullptr]() {
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

	std::optional<std::uintptr_t> CallTarget(std::uintptr_t a_site)
	{
		const auto* code = reinterpret_cast<const std::uint8_t*>(a_site);
		if (code[0] != 0xE8) {
			return std::nullopt;
		}
		return a_site + 5 + *reinterpret_cast<const std::int32_t*>(code + 1);
	}

	template <std::size_t... I>
	std::optional<std::string> Hook(std::index_sequence<I...>)
	{
		const auto loop = kMainLoop.address() + kOnIdleCall;
		if (!CallTarget(loop)) {
			return "no call at MainLoop+0x160"s;
		}
		for (const auto& s : kSites) {
			if (CallTarget(s.handler.address() + s.offset) != kCenterOnCellImpl.address()) {
				return fmt::format("the {} handler does not call CenterOnCell_Impl at +0x{:X}", s.command, s.offset);
			}
		}
		const auto bat = kProcessMessage.address() + kLineLoopCall;
		if (CallTarget(bat) != kLineLoop.address()) {
			return "Console::ProcessMessage+0x69F is not a call of the bat line loop"s;
		}
		SKSE::AllocTrampoline(14 * (2 + kSites.size()));
		auto& t = SKSE::GetTrampoline();
		onIdle = t.write_call<5>(loop, OnIdleThunk);
		(t.write_call<5>(kSites[I].handler.address() + kSites[I].offset, CenterOnCellThunk<I>), ...);
		t.write_call<5>(bat, LineLoopThunk);
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
	mainThread = GetCurrentThreadId();  // plugins load on the main thread
	logger::info("{} v{} on {}"sv, Plugin::NAME, Plugin::VERSION.string(), REL::Module::get().version().string());
	if (const auto why = Hook(std::make_index_sequence<kSites.size()>{})) {
		logger::error("self-check failed ({}): inactive"sv, *why);
	} else {
		logger::info("self-check passed: coc / coe / cow off the main thread and bat lines run on the main thread"sv);
	}
	return true;
}
