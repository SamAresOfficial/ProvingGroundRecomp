#pragma once

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/perf/counter.h>
#include <rex/rex_app.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/debug_overlay.h>

#include "pg_frame_pacing.h"
#include "pg_input_script.h"
#include "pg_menu_input.h"
#include "pg_sampler.h"
#include "pg_ui.h"

REXCVAR_DEFINE_BOOL(tracy, false, "Perf",
                    "Start the Tracy profiler so the Tracy viewer can connect (non-Release builds)");
REXCVAR_DEFINE_STRING(input_record, "", "Input",
                      "Record user 0 controller input to this file (F9 marks a measurement window)");
REXCVAR_DEFINE_STRING(input_replay, "", "Input", "Replay controller input from this file");
REXCVAR_DEFINE_BOOL(input_replay_quit_on_stop, false, "Input",
                    "Close the game when the replay reaches measure_stop");
REXCVAR_DEFINE_STRING(sample_profile, "", "Perf",
                      "Write a sampling CPU profile of all threads to this file; with an input "
                      "script, only between measure_start and measure_stop");
REXCVAR_DEFINE_UINT32(sample_period_ms, 1, "Perf", "Sampling profiler period in milliseconds");
REXCVAR_DEFINE_UINT32(sample_min_draws, 0, "Perf",
                      "Sample only while the last frame had at least this many draws");
REXCVAR_DEFINE_STRING(timeline_trace, "", "Perf",
                      "Write a CSV of game-thread and GPU-thread wait events to this file; with an "
                      "input script, only between measure_start and measure_stop");
REXCVAR_DECLARE(bool, show_launcher);
REXCVAR_DEFINE_BOOL(launcher, false, "General", "Show the launcher even if it is turned off");
REXCVAR_DEFINE_BOOL(developer, false, "General",
                    "Keep the SDK's diagnostic overlays: F3 debug, F4 settings, ` console");

class PgApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx, const rex::PPCImageInfo& image) {
    return std::unique_ptr<PgApp>(new PgApp(ctx, "pg", image));
  }

 protected:
  void OnPostInitLogging() override {
    if (!REXCVAR_GET(tracy)) return;
#ifdef REXGLUE_ENABLE_PROFILING
    // The SDK builds Tracy with TRACY_MANUAL_LIFETIME but never starts it.
    // Start early so the threads created afterwards get their names.
    rex::perf::Profiler::Startup();
    REXLOG_INFO("Tracy: profiler started; connect the Tracy 0.13.1 viewer to localhost");
#else
    REXLOG_WARN("Tracy requires a RelWithDebInfo build; Release compiles it out");
#endif
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (config.gpu_plugin.empty()) config.gpu_plugin = "xenos";

#if REX_PLATFORM_WIN32
    // The launcher has a window of its own, shown before the renderer is chosen and the game's
    // window opens.
    if (LauncherWanted(pg::CheckGameFolder(StartupFolder({})))) {
      if (!pg::RunLauncherWindow(config_path_, StartupFolder({}))) {
        REXLOG_INFO("Launcher closed");
        std::exit(0);
      }
      // Again, for the GPU plugin's settings the launcher changed; the plugin reads them below.
      rex::cvar::LoadConfig(config_path_);
      launcher_shown_ = true;
    }
#endif

    // The GPU cvars are registered when the plugin DLL loads, which ReXApp does only after this
    // hook; load it here so the defaults below can be set before the GPU reads them in Setup.
    // FSR 4 needs the Direct3D 12 renderer, everything else runs on Vulkan.
    if (!config.graphics) {
      renderer_ = pg::RequiredRenderer();
      config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, renderer_);
      if (!config.graphics && renderer_ != "vulkan") {
        REXLOG_WARN("No {} renderer in this build; using Vulkan", renderer_);
        renderer_ = "vulkan";
        config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, renderer_);
      }
    }
    // The main 3D pass keeps the world matrix in c216-c219 and world-view-projection in
    // c224-c227, which FSR 3 frame generation needs for the camera motion vectors.
    if (config.graphics && !rex::cvar::HasNonDefaultValue("fsr3_camera_world_constant")) {
      rex::cvar::SetFlagByName("fsr3_camera_world_constant", "216");
      rex::cvar::SetFlagByName("fsr3_camera_world_view_projection_constant", "224");
    }

    // Without the vertex shader on the CPU, draws without a viewport are assumed to cover the
    // whole EDRAM, and every render target transfer copies all of it. At 1440p internal this was
    // about 2.7 ms of GPU time per frame; the route went from about 105 to 125 FPS.
    if (config.graphics && !rex::cvar::HasNonDefaultValue("execute_unclipped_draw_vs_on_cpu")) {
      rex::cvar::SetFlagByName("execute_unclipped_draw_vs_on_cpu", "true");
    }

    // Without vsync the game waits for a vblank after each swap, and the 1 ms vblank tick came
    // about 0.75 ms after the swap on average. At 1440p the route went from about 125 to 133 FPS.
    if (config.graphics && !rex::cvar::HasNonDefaultValue("vblank_after_swap")) {
      rex::cvar::SetFlagByName("vblank_after_swap", "true");
    }

    const std::string record = REXCVAR_GET(input_record);
    const std::string replay = REXCVAR_GET(input_replay);
    if (record.empty() && replay.empty()) return;
    if (!record.empty() && !replay.empty()) {
      REXLOG_ERROR("Input script: use either --input_record or --input_replay, not both");
      return;
    }
    // The default system plus one synthetic device that records or replays.
    config.input_factory = [this, record, replay](bool tool_mode)
        -> std::unique_ptr<rex::system::IInputSystem> {
      auto mode = record.empty() ? PgInputScript::Mode::kReplay : PgInputScript::Mode::kRecord;
      // Unattended replays (measurements) take no physical input: a touched controller would be
      // merged with the route and desynchronize it.
      bool replay_only = !tool_mode && mode == PgInputScript::Mode::kReplay &&
                         REXCVAR_GET(input_replay_quit_on_stop);
      std::unique_ptr<rex::input::InputSystem> input;
      if (replay_only) {
        input = std::make_unique<rex::input::InputSystem>(nullptr);
        input->SetDeviceAssignment(std::make_unique<rex::input::SlotAssignment>());
      } else {
        input = rex::input::CreateDefaultInputSystem(tool_mode);
      }
      if (tool_mode) return input;
      auto script = PgInputScript::Create(
          input.get(), mode, record.empty() ? replay : record,
          [this] {
            app_context().CallInUIThread([this] {
              if (window()) window()->RequestClose();
            });
          },
          REXCVAR_GET(input_replay_quit_on_stop));
      if (script) {
        input_script_ = script.get();
        input->AddDriver(std::move(script));
      }
      return input;
    };
  }

  void OnPostSetup() override {
    // View + Menu on a controller opens the options menu (no keyboard on handhelds).
    pg::InstallMenuInput(
        static_cast<rex::input::InputSystem*>(runtime()->input_system()), [this](bool open) {
          app_context().CallInUIThread([this, open] { SetMenuOpen(open); });
        });
    const std::string sample_path = REXCVAR_GET(sample_profile);
    if (!sample_path.empty()) {
      sampler_ = std::make_unique<PgSampler>(sample_path, REXCVAR_GET(sample_period_ms));
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
      if (const uint32_t min_draws = REXCVAR_GET(sample_min_draws)) {
        sampler_->SetGate([min_draws] {
          return rex::perf::GetSnapshotCounter(rex::perf::CounterId::kDrawCalls) >= min_draws;
        });
      }
#endif
    }
    const std::string timeline_path = REXCVAR_GET(timeline_trace);
    if (!timeline_path.empty()) rex::perf::TimelineSetPath(timeline_path);
    const bool pacing_trace = !REXCVAR_QUERY(std::string, pacing_trace_csv).empty();
    if (sampler_ || !timeline_path.empty() || pacing_trace) {
      auto set_active = [this](bool active) {
        pg::SetPacingTraceActive(active);
        if (sampler_) sampler_->SetActive(active);
        rex::perf::TimelineSetActive(active);
        // The title hard-exits the process, so OnShutdown may never run.
        if (!active) {
          if (sampler_) sampler_->WriteReport();
          rex::perf::TimelineWrite();
        }
      };
      if (input_script_) {
        input_script_->SetMeasureCallback(set_active);
      } else {
        set_active(true);
      }
    }
    const auto csv = REXCVAR_QUERY(std::string, perf_log_csv);
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    // As in Project8Recomp, the app must arm the SDK writer explicitly.
    // The standard Release runtime compiles the frame counters out.
    if (!csv.empty()) rex::perf::SetCsvLogPath(csv);
#endif
    if (input_script_ && !REXCVAR_GET(input_record).empty()) {
      rex::ui::RegisterBind("bind_input_measure", "F9",
                            "Toggle the measurement window in the input recording",
                            [this] { input_script_->ToggleMeasure(); });
    }
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    SetGuestFrameStats([] {
      const auto us = rex::perf::GetSnapshotCounter(rex::perf::CounterId::kFrameTimeUs);
      rex::ui::FrameStats stats;
      if (us > 0) {
        stats.frame_time_ms = double(us) / 1000.0;
        stats.fps = 1000000.0 / double(us);
        // The SDK overlay uses this field only as a "sample available" guard.
        stats.frame_count = 1;
      }
      return stats;
    });
#else
    if (!csv.empty()) {
      REXLOG_WARN("FPS counters require the RelWithDebInfo build; Release has them disabled");
    }
#endif
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    config_path_ = paths.config_path;
    // Saves and caches go to Documents/ProvingGroundRecomp (Linux: ~/.local/share/...), or next
    // to the executable when a portable.txt file is there, unless set on the command line.
    if (REXCVAR_GET(user_data_root).empty()) {
      const auto exe_dir = rex::filesystem::GetExecutableFolder();
      std::error_code ec;
      paths.user_data_root = std::filesystem::exists(exe_dir / "portable.txt", ec)
                                 ? exe_dir / "user"
                                 : rex::filesystem::GetUserFolder() / "ProvingGroundRecomp";
      if (REXCVAR_GET(cache_root).empty()) paths.cache_root = paths.user_data_root / "cache";
    }
  }

  void OnConfigureFonts(ImFontAtlas* atlas) override {
    if (window()) {
      pg::ui::SetScale(float(window()->GetActualLogicalHeight()));
      pg::SetWindowPhysicalHeight(window()->GetActualPhysicalHeight());
    }
    pg::ui::ConfigureFonts(atlas);
  }

  void OnConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style) override {
    rex::ReXApp::OnConfigureStyle(imgui_style, ui_style);
    pg::ui::ConfigureStyle(imgui_style);
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    // Only the game's own menu is reachable unless --developer is set.
    if (!REXCVAR_GET(developer)) {
      for (const char* bind : {"bind_debug_overlay", "bind_console", "bind_settings",
                               "bind_achievements"}) {
        rex::ui::UnregisterBind(bind);
      }
    }
    drawer_ = drawer;
    rex::ui::RegisterBind("bind_pg_menu", "Insert", "Toggle the options menu",
                          [this] { SetMenuOpen(!menu_); });
    rex::ui::RegisterBind("bind_pg_fullscreen", "F11", "Toggle fullscreen", [this] {
      const bool fullscreen = rex::cvar::Query<bool>("fullscreen");
      rex::cvar::SetFlagByName("fullscreen", fullscreen ? "false" : "true");
      pg::SaveSettings(config_path_);
    });
    fps_overlay_ = std::make_unique<pg::FpsOverlay>(drawer);
    // show_fps changes from the Insert menu, the launcher or the console (any thread, under the
    // cvar registry's lock): follow it on the UI thread.
    rex::cvar::RegisterChangeCallback("show_fps", [this](std::string_view, std::string_view) {
      app_context().CallInUIThreadDeferred([this] {
        if (fps_overlay_) fps_overlay_->Refresh();
      });
    });
    pg::SetPresenterSource([drawer] { return drawer->presenter(); });
    pg::ApplyUpscaler();
  }

  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) override {
    if (window()) window()->SetTitle("Tony Hawk's Proving Ground");
    const std::filesystem::path folder = StartupFolder(defaults.game_data_root);
    const auto check = pg::CheckGameFolder(folder);
    const bool playable = check.status == pg::GameFolderCheck::Status::kOk ||
                          check.status == pg::GameFolderCheck::Status::kUnknownVersion;
    // After the launcher window, or when no launcher is wanted, the game starts. Without a
    // launcher window (other systems, or it could not open) the launcher screen shows here.
    if (playable && (launcher_shown_ || !LauncherWanted(check))) {
      pg::ApplyStartupSettings();
      rex::PathConfig paths = defaults;
      paths.game_data_root = check.folder;
      return paths;
    }
    launcher_ = std::make_unique<pg::Launcher>(
        imgui_drawer(), config_path_, folder,
        [this, defaults, resume](const std::filesystem::path& game_folder) {
          rex::PathConfig paths = defaults;
          paths.game_data_root = game_folder;
          // The renderer is set up before the launcher shows; switching it takes a new process.
          if (pg::RequiredRenderer() != renderer_ && pg::Relaunch(game_folder)) {
            app_context().CallInUIThreadDeferred([this] { app_context().QuitFromUIThread(); });
            return;
          }
          // Leave the dialog's draw before tearing it down and starting the game.
          app_context().CallInUIThreadDeferred([this, paths, resume] {
            launcher_.reset();
            pg::ApplyStartupSettings();
            resume(paths);
          });
        },
        [this] { app_context().CallInUIThreadDeferred([this] { app_context().QuitFromUIThread(); }); });
    return std::nullopt;
  }

  void OnShutdown() override {
    pg::StopMenuRequests();
    launcher_.reset();
    CloseMenu();
    drawer_ = nullptr;
    rex::cvar::UnregisterChangeCallbacks("show_fps");
    fps_overlay_.reset();
    rex::ui::UnregisterBind("bind_pg_menu");
    rex::ui::UnregisterBind("bind_pg_fullscreen");
    if (sampler_) sampler_->WriteReport();
    rex::perf::TimelineWrite();
    if (input_script_) rex::ui::UnregisterBind("bind_input_measure");
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    rex::perf::FlushCsv();
#endif
  }

 private:
  // Owned by the input system, which outlives OnShutdown.
  PgInputScript* input_script_ = nullptr;
  std::unique_ptr<PgSampler> sampler_;
  std::filesystem::path config_path_;
  // The renderer the GPU plugin was started with ("vulkan" or "d3d12").
  std::string renderer_ = "vulkan";
  std::unique_ptr<pg::Launcher> launcher_;
  // The launcher window was shown and Play pressed.
  bool launcher_shown_ = false;
  std::unique_ptr<pg::GameMenu> menu_;
  std::unique_ptr<pg::FpsOverlay> fps_overlay_;
  rex::ui::ImGuiDrawer* drawer_ = nullptr;

  // Opens or closes the options menu (UI thread): Insert, the controller combo, Resume.
  void SetMenuOpen(bool open) {
    if (open && !menu_ && drawer_) {
      menu_ = std::make_unique<pg::GameMenu>(drawer_, config_path_, [this] {
        app_context().CallInUIThreadDeferred([this] { CloseMenu(); });
      });
    } else if (!open) {
      menu_.reset();
    }
    // Also when nothing changed: the controller filter may have asked for a menu that could not
    // open, and must not keep the controllers from the game.
    pg::SetMenuInputOpen(menu_ != nullptr);
  }
  void CloseMenu() { SetMenuOpen(false); }

  // The game folder to start with: the saved or given one, or a "game" folder next to the
  // executable, which is found without asking.
  static std::filesystem::path StartupFolder(const std::filesystem::path& fallback) {
    // Strings are UTF-8, a path from a std::string would be in the ANSI code page.
    std::filesystem::path folder = rex::to_path(REXCVAR_GET(game_data_root));
    if (folder.empty()) folder = fallback;
    if (folder.empty()) {
      const auto bundled = rex::filesystem::GetExecutableFolder() / "game";
      if (pg::CheckGameFolder(bundled).status != pg::GameFolderCheck::Status::kMissing) {
        folder = bundled;
      }
    }
    return folder;
  }

  // --launcher always shows the launcher; otherwise a game folder given on the command line, or
  // the setting, skips it when the folder holds the game.
  static bool LauncherWanted(const pg::GameFolderCheck& check) {
    const bool playable = check.status == pg::GameFolderCheck::Status::kOk ||
                          check.status == pg::GameFolderCheck::Status::kUnknownVersion;
    const auto* root_info = rex::cvar::GetFlagInfo("game_data_root");
    const bool from_command_line =
        root_info && root_info->source == rex::cvar::Source::kCommandLine;
    return !playable || REXCVAR_GET(launcher) ||
           (!from_command_line && REXCVAR_GET(show_launcher));
  }
};
