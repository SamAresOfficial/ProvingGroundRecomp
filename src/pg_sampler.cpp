#include "pg_sampler.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/filesystem.h>
#include <rex/logging.h>

#ifdef _WIN32

#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <timeapi.h>
#include <tlhelp32.h>

namespace {

constexpr int kMaxFrames = 24;

struct ThreadStats {
  std::string name;
  uint64_t samples = 0;
  std::unordered_map<uint64_t, uint64_t> self;       // leaf address -> samples
  std::unordered_map<uint64_t, uint64_t> inclusive;  // any frame address -> samples
  // For samples inside system DLLs: first frame outside them -> samples.
  std::unordered_map<uint64_t, uint64_t> system_callers;
  // For samples inside system DLLs: (leaf address, first frame outside them) -> samples.
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> system_leaf_callers;
};

struct ModuleRange {
  uint64_t begin = 0, end = 0;
};

// System and graphics driver modules whose frames are skipped to find who
// called into them.
std::vector<ModuleRange> SystemModules() {
  std::vector<ModuleRange> ranges;
  for (const wchar_t* name :
       {L"ntdll.dll", L"KERNELBASE.dll", L"KERNEL32.dll", L"msvcp140.dll", L"VCRUNTIME140.dll",
        L"ucrtbase.dll", L"vulkan-1.dll", L"amdvlk64.dll", L"nvoglv64.dll", L"igvk64.dll"}) {
    HMODULE module = GetModuleHandleW(name);
    MODULEINFO info = {};
    if (module && GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) {
      uint64_t begin = reinterpret_cast<uint64_t>(info.lpBaseOfDll);
      ranges.push_back({begin, begin + info.SizeOfImage});
    }
  }
  return ranges;
}

std::string ThreadName(HANDLE thread) {
  using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static auto get_description = reinterpret_cast<GetThreadDescriptionFn>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  if (!get_description) return {};
  PWSTR wide = nullptr;
  if (FAILED(get_description(thread, &wide)) || !wide) return {};
  int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  std::string name(size > 0 ? size - 1 : 0, '\0');
  if (size > 1) WideCharToMultiByte(CP_UTF8, 0, wide, -1, name.data(), size, nullptr, nullptr);
  LocalFree(wide);
  return name;
}

// The unwind tables of the loaded modules, read from their PE headers while no thread is
// suspended. RtlLookupFunctionEntry can't be used on a suspended thread: it takes ntdll's
// function table locks, and a thread suspended while holding them (loading a DLL, or a driver
// registering JIT code) would never be resumed, freezing the game. Code without a table here
// (dynamic function tables) is unwound as a leaf.
class UnwindTables {
 public:
  ~UnwindTables() { Release(); }

  void Refresh() {
    Release();
    HANDLE process = GetCurrentProcess();
    std::vector<HMODULE> modules(256);
    DWORD needed = 0;
    while (EnumProcessModules(process, modules.data(), DWORD(modules.size() * sizeof(HMODULE)),
                              &needed) &&
           needed > modules.size() * sizeof(HMODULE)) {
      modules.resize(needed / sizeof(HMODULE));
    }
    modules.resize(std::min<size_t>(modules.size(), needed / sizeof(HMODULE)));
    for (HMODULE module : modules) {
      // Hold a reference, so the module can't be unloaded while its table is in use.
      HMODULE held = nullptr;
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                              reinterpret_cast<LPCWSTR>(module), &held)) {
        continue;
      }
      const auto* base = reinterpret_cast<const uint8_t*>(held);
      const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
      const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
      if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE ||
          nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        FreeLibrary(held);
        continue;
      }
      const IMAGE_DATA_DIRECTORY& dir =
          nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
      Image image;
      image.begin = reinterpret_cast<uint64_t>(base);
      image.end = image.begin + nt->OptionalHeader.SizeOfImage;
      image.held = held;
      if (dir.VirtualAddress && dir.Size >= sizeof(RUNTIME_FUNCTION)) {
        image.table = reinterpret_cast<const RUNTIME_FUNCTION*>(base + dir.VirtualAddress);
        image.count = dir.Size / sizeof(RUNTIME_FUNCTION);
      }
      images_.push_back(image);
    }
    std::sort(images_.begin(), images_.end(),
              [](const Image& a, const Image& b) { return a.begin < b.begin; });
  }

  // Lock-free and allocation-free: safe while another thread is suspended.
  PRUNTIME_FUNCTION Lookup(uint64_t address, DWORD64* image_base) const {
    auto it = std::upper_bound(images_.begin(), images_.end(), address,
                               [](uint64_t a, const Image& image) { return a < image.begin; });
    if (it == images_.begin()) return nullptr;
    const Image& image = *--it;
    if (address >= image.end || !image.table) return nullptr;
    const DWORD rva = DWORD(address - image.begin);
    size_t low = 0, high = image.count;
    while (low < high) {
      const size_t mid = (low + high) / 2;
      const RUNTIME_FUNCTION& entry = image.table[mid];
      if (rva < entry.BeginAddress) {
        high = mid;
      } else if (rva >= entry.EndAddress) {
        low = mid + 1;
      } else {
        *image_base = image.begin;
        return const_cast<PRUNTIME_FUNCTION>(&entry);
      }
    }
    return nullptr;
  }

 private:
  struct Image {
    uint64_t begin = 0, end = 0;
    const RUNTIME_FUNCTION* table = nullptr;
    DWORD count = 0;
    HMODULE held = nullptr;
  };

  void Release() {
    for (const Image& image : images_) FreeLibrary(image.held);
    images_.clear();
  }

  std::vector<Image> images_;
};

// Unwinds a suspended thread's stack. Must not allocate or take locks: the thread may hold the
// heap lock or the loader's.
int CaptureStack(HANDLE thread, const UnwindTables& tables, uint64_t* frames) {
  CONTEXT context = {};
  context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
  if (!GetThreadContext(thread, &context)) return 0;
  int count = 0;
  while (count < kMaxFrames && context.Rip) {
    frames[count++] = context.Rip;
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION function = tables.Lookup(context.Rip, &image_base);
    if (!function) {
      // Leaf function without unwind data: the return address is on top of the stack.
      DWORD64 return_address = 0;
      SIZE_T read = 0;
      if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(context.Rsp),
                             &return_address, sizeof(return_address), &read) ||
          read != sizeof(return_address)) {
        break;
      }
      context.Rip = return_address;
      context.Rsp += 8;
      continue;
    }
    void* handler_data = nullptr;
    DWORD64 establisher_frame = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                     &handler_data, &establisher_frame, nullptr);
  }
  return count;
}

}  // namespace

struct PgSampler::State {
  std::string report_path;
  uint32_t period_ms;
  std::atomic<bool> running{true};
  std::thread worker;
  std::mutex mutex;  // guards stats
  std::map<DWORD, ThreadStats> stats;
  uint64_t passes = 0;
  bool written = false;
};

PgSampler::PgSampler(std::string report_path, uint32_t period_ms)
    : state_(std::make_unique<State>()) {
  state_->report_path = std::move(report_path);
  state_->period_ms = std::max<uint32_t>(period_ms, 1);
  State* state = state_.get();
  state->worker = std::thread([this, state] {
    timeBeginPeriod(1);
    const DWORD self_id = GetCurrentThreadId();
    const DWORD process_id = GetCurrentProcessId();
    struct Target {
      DWORD id;
      HANDLE handle;
    };
    std::vector<Target> targets;
    UnwindTables unwind_tables;
    ULONGLONG last_enumeration = 0;
    uint64_t frames[kMaxFrames];
    const std::vector<ModuleRange> system_modules = SystemModules();
    auto in_system = [&](uint64_t address) {
      for (const ModuleRange& range : system_modules) {
        if (address >= range.begin && address < range.end) return true;
      }
      return false;
    };
    while (state->running.load(std::memory_order_relaxed)) {
      Sleep(state->period_ms);
      if (!active_.load(std::memory_order_relaxed)) continue;
      if (gate_ && !gate_()) continue;
      ULONGLONG now = GetTickCount64();
      if (now - last_enumeration > 500) {
        last_enumeration = now;
        unwind_tables.Refresh();
        for (auto& target : targets) CloseHandle(target.handle);
        targets.clear();
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
          THREADENTRY32 entry = {sizeof(entry)};
          for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != process_id || entry.th32ThreadID == self_id) continue;
            HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                           THREAD_QUERY_LIMITED_INFORMATION,
                                       FALSE, entry.th32ThreadID);
            if (handle) targets.push_back({entry.th32ThreadID, handle});
          }
          CloseHandle(snapshot);
        }
      }
      for (const Target& target : targets) {
        if (SuspendThread(target.handle) == DWORD(-1)) continue;
        int count = CaptureStack(target.handle, unwind_tables, frames);
        ResumeThread(target.handle);
        if (!count) continue;
        std::lock_guard lock(state->mutex);
        ThreadStats& stats = state->stats[target.id];
        if (stats.name.empty()) {
          stats.name = ThreadName(target.handle);
          if (stats.name.empty()) stats.name = "thread " + std::to_string(target.id);
        }
        ++stats.samples;
        ++stats.self[frames[0]];
        if (in_system(frames[0])) {
          for (int i = 1; i < count; ++i) {
            if (!in_system(frames[i])) {
              ++stats.system_callers[frames[i]];
              ++stats.system_leaf_callers[{frames[0], frames[i]}];
              break;
            }
          }
        }
        // Count each address once per sample, so recursion does not inflate it.
        for (int i = 0; i < count; ++i) {
          bool seen = false;
          for (int j = 0; j < i && !seen; ++j) seen = frames[j] == frames[i];
          if (!seen) ++stats.inclusive[frames[i]];
        }
      }
      ++state->passes;
    }
    for (auto& target : targets) CloseHandle(target.handle);
    timeEndPeriod(1);
  });
}

PgSampler::~PgSampler() {
  WriteReport();
}

void PgSampler::WriteReport() {
  State& state = *state_;
  if (state.written) return;
  state.written = true;
  state.running.store(false);
  if (state.worker.joinable()) state.worker.join();

  HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  bool symbols = SymInitialize(process, nullptr, TRUE);
  std::unordered_map<uint64_t, std::string> names;
  auto function_of = [&](uint64_t address) -> const std::string& {
    auto found = names.find(address);
    if (found != names.end()) return found->second;
    std::string name;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;
    DWORD64 displacement = 0;
    IMAGEHLP_MODULE64 module = {sizeof(module)};
    std::string module_name =
        symbols && SymGetModuleInfo64(process, address, &module) ? module.ModuleName : "?";
    if (symbols && SymFromAddr(process, address, &displacement, symbol)) {
      name = module_name + "!" + symbol->Name;
    } else {
      char text[64];
      std::snprintf(text, sizeof(text), "%s!0x%llx", module_name.c_str(),
                    static_cast<unsigned long long>(address));
      name = text;
    }
    return names.emplace(address, std::move(name)).first->second;
  };
  // Leaf functions in which a thread is blocked rather than running.
  auto is_wait = [](const std::string& name) {
    static const char* kWaits[] = {"!NtWaitFor", "!ZwWaitFor", "!NtDelayExecution",
                                   "!ZwDelayExecution", "!NtRemoveIoCompletion",
                                   "!NtWaitForAlertByThreadId", "!ZwRemoveIoCompletion",
                                   "!NtYieldExecution", "!ZwYieldExecution",
                                   "!NtSignalAndWaitForSingleObject", "!NtUserMsgWaitFor",
                                   "!NtUserGetMessage", "!NtGdiDdDDIWaitFor"};
    for (const char* wait : kWaits) {
      if (name.find(wait) != std::string::npos) return true;
    }
    return false;
  };

  std::FILE* file = rex::filesystem::OpenFile(rex::to_path(state.report_path), "w");
  if (!file) {
    REXLOG_ERROR("Sampler: cannot write {}", state.report_path);
    if (symbols) SymCleanup(process);
    return;
  }
  std::lock_guard lock(state.mutex);
  struct Summary {
    const ThreadStats* stats;
    uint64_t busy;
  };
  std::vector<Summary> threads;
  for (auto& [id, stats] : state.stats) {
    uint64_t waiting = 0;
    for (auto& [address, count] : stats.self) {
      if (is_wait(function_of(address))) waiting += count;
    }
    threads.push_back({&stats, stats.samples - waiting});
  }
  std::sort(threads.begin(), threads.end(),
            [](const Summary& a, const Summary& b) { return a.busy > b.busy; });
  std::fprintf(file, "# pg sampler: %llu passes, period %u ms\n",
               static_cast<unsigned long long>(state.passes), state.period_ms);
  std::fprintf(file, "# busy = samples not in a blocking system call\n\n");
  for (size_t t = 0; t < threads.size() && t < 10; ++t) {
    const ThreadStats& stats = *threads[t].stats;
    std::fprintf(file, "== %s: %llu samples, busy %.1f%%\n", stats.name.c_str(),
                 static_cast<unsigned long long>(stats.samples),
                 stats.samples ? 100.0 * threads[t].busy / stats.samples : 0.0);
    for (int pass = 0; pass < 2; ++pass) {
      std::unordered_map<std::string, uint64_t> by_function;
      for (auto& [address, count] : pass ? stats.inclusive : stats.self) {
        by_function[function_of(address)] += count;
      }
      std::vector<std::pair<std::string, uint64_t>> sorted(by_function.begin(), by_function.end());
      std::sort(sorted.begin(), sorted.end(),
                [](const auto& a, const auto& b) { return a.second > b.second; });
      std::fprintf(file, "  %s:\n", pass ? "inclusive" : "self");
      for (size_t i = 0; i < sorted.size() && i < (pass ? 80 : 40); ++i) {
        std::fprintf(file, "    %5.1f%%  %s\n", 100.0 * sorted[i].second / stats.samples,
                     sorted[i].first.c_str());
      }
    }
    std::unordered_map<std::string, uint64_t> callers;
    for (auto& [address, count] : stats.system_callers) callers[function_of(address)] += count;
    std::vector<std::pair<std::string, uint64_t>> sorted_callers(callers.begin(), callers.end());
    std::sort(sorted_callers.begin(), sorted_callers.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(file, "  callers of system and driver DLL code (waits, syscalls, CRT, Vulkan):\n");
    for (size_t i = 0; i < sorted_callers.size() && i < 15; ++i) {
      std::fprintf(file, "    %5.1f%%  %s\n", 100.0 * sorted_callers[i].second / stats.samples,
                   sorted_callers[i].first.c_str());
    }
    std::map<std::string, uint64_t> leaf_callers;
    for (auto& [addresses, count] : stats.system_leaf_callers) {
      leaf_callers[function_of(addresses.first) + "  <-  " + function_of(addresses.second)] +=
          count;
    }
    std::vector<std::pair<std::string, uint64_t>> sorted_leaf_callers(leaf_callers.begin(),
                                                                      leaf_callers.end());
    std::sort(sorted_leaf_callers.begin(), sorted_leaf_callers.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(file, "  system and driver code by caller:\n");
    for (size_t i = 0; i < sorted_leaf_callers.size() && i < 25; ++i) {
      std::fprintf(file, "    %5.1f%%  %s\n",
                   100.0 * sorted_leaf_callers[i].second / stats.samples,
                   sorted_leaf_callers[i].first.c_str());
    }
    std::fputc('\n', file);
  }
  std::fclose(file);
  if (symbols) SymCleanup(process);
  REXLOG_INFO("Sampler: report written to {}", state.report_path);
}

#else

struct PgSampler::State {};
PgSampler::PgSampler(std::string, uint32_t) {}
PgSampler::~PgSampler() = default;
void PgSampler::WriteReport() {}

#endif
