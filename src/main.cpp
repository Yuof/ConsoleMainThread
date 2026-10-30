// Console Main Thread (Skyrim SE 1.5.97 and AE, Address Library): runs the console's cell changes and `bat` files on the main
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
	// SE 1.5.97 / AE ids; read from 1.5.97, 1.6.1170 and 1.7.104. The main loop's call of Main::OnIdle sits at the same
	// offset in every build (other plugins hook it too, so its target is not checked); the other call sites are found
	// by their target inside the function, as their offsets differ between builds (Console::ProcessMessage's call of
	// the bat line loop: +0x69F on 1.6.1170, +0x6BF on 1.7.104, +0x3E6 on 1.5.97).
	constexpr REL::RelocationID kMainLoop{ 35551, 36544 };
	constexpr REL::VariantOffset kOnIdleCall{ 0x11F, 0x160, 0 };

	constexpr REL::RelocationID kCenterOnCellImpl{ 39365, 40437 };  // PlayerCharacter::CenterOnCell_Impl(name, cell)
	struct Site
	{
		REL::RelocationID handler;
		std::string_view  command;
	};
	constexpr std::array kSites{
		Site{ REL::RelocationID(22398, 22873), "coc"sv },  // ConsoleFunc CenterOnCell: (player, cell name, nullptr)
		Site{ REL::RelocationID(22400, 22875), "coe"sv },  // CenterOnExterior: (player, nullptr, cell)
		Site{ REL::RelocationID(22401, 22876), "cow"sv },  // CenterOnWorld: (player, nullptr, cell)
	};

	// Console::ProcessMessage = slot 4 of Console's vtable; its own id (442669) is missing from the AE 1.6.317-1.6.659
	// Address Library databases, the vtable's is in all of them.
	constexpr REL::RelocationID kConsoleVtbl{ 268119, 215002 };
	constexpr REL::RelocationID kLineLoop{ 21417, 21891 };         // runs every line of a bat stream
	constexpr REL::RelocationID kReadLine{ 67711, 69036 };         // stream -> next line, 0 at the end

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

	// The first call of a_target inside the function at a_function, up to its int3 padding (or a_max bytes).
	std::optional<std::uintptr_t> FindCall(std::uintptr_t a_function, std::uintptr_t a_target, std::size_t a_max = 0x1000)
	{
		const auto* code = reinterpret_cast<const std::uint8_t*>(a_function);
		for (std::size_t i = 0; i + 5 <= a_max; ++i) {
			if (code[i] == 0xCC && code[i + 1] == 0xCC && code[i + 2] == 0xCC) {
				break;
			}
			if (CallTarget(a_function + i) == a_target) {
				return a_function + i;
			}
		}
		return std::nullopt;
	}

	struct Parts
	{
		bool        travel = false;
		bool        bat = false;
		std::string why;  // what failed, for the log
	};

	template <std::size_t... I>
	Parts Hook(std::index_sequence<I...>)
	{
		Parts      p;
		const auto loop = kMainLoop.address() + kOnIdleCall.offset();
		if (!CallTarget(loop)) {
			p.why = "no call at the main loop's Main::OnIdle site";
			return p;  // no way to the main thread: nothing else is patched
		}
		std::array<std::optional<std::uintptr_t>, kSites.size()> sites{};
		for (std::size_t k = 0; k < kSites.size(); ++k) {
			sites[k] = FindCall(kSites[k].handler.address(), kCenterOnCellImpl.address());
			if (!sites[k]) {
				p.why += fmt::format("the {} handler has no call of CenterOnCell_Impl; ", kSites[k].command);
			}
		}
		p.travel = std::ranges::all_of(sites, [](const auto& s) { return s.has_value(); });
		const auto processMessage = reinterpret_cast<const std::uintptr_t*>(kConsoleVtbl.address())[4];
		const auto bat = FindCall(processMessage, kLineLoop.address());
		if (!bat) {
			p.why += "Console::ProcessMessage has no call of the bat line loop; ";
		}
		p.bat = bat.has_value();
		if (!p.travel && !p.bat) {
			return p;
		}
		SKSE::AllocTrampoline(14 * (2 + kSites.size()));
		auto& t = SKSE::GetTrampoline();
		onIdle = t.write_call<5>(loop, OnIdleThunk);
		if (p.travel) {
			(t.write_call<5>(*sites[I], CenterOnCellThunk<I>), ...);
		}
		if (p.bat) {
			t.write_call<5>(*bat, LineLoopThunk);
		}
		return p;
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
	const auto p = Hook(std::make_index_sequence<kSites.size()>{});
	if (!p.travel && !p.bat) {
		logger::error("self-check failed ({}): inactive"sv, p.why);
	} else {
		if (!p.why.empty()) {
			logger::warn("self-check: {}that part stays off"sv, p.why);
		}
		logger::info("self-check passed: {}{}{} run on the main thread"sv, p.travel ? "coc / coe / cow off the main thread"sv : ""sv,
			p.travel && p.bat ? " and "sv : ""sv, p.bat ? "bat lines"sv : ""sv);
	}
	return true;
}
