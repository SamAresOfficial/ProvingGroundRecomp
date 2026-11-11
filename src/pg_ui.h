#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <rex/ui/imgui_dialog.h>

struct ImFontAtlas;
struct ImGuiStyle;

namespace rex::ui {
class Presenter;
}

namespace pg {

// Settings shown in the launcher and the in-game menu, saved next to the executable.
void SaveSettings(const std::filesystem::path& config_path);

// Applies the settings that the GPU reads when the runtime starts (internal resolution).
void ApplyStartupSettings();

// Where the upscaler settings are applied while running; returns null before there is one.
void SetPresenterSource(std::function<rex::ui::Presenter*()> source);
// Height in pixels of the screen the game is shown on: the monitor in fullscreen, else the window.
void SetWindowPhysicalHeight(uint32_t height);
// Applies the upscaler and sharpness settings to the presenter immediately.
void ApplyUpscaler();

// The renderer the settings need: "d3d12" for FSR 4, else "vulkan". It is chosen at startup.
const char* RequiredRenderer();
// Starts the game again with the same command line, straight into this game folder.
bool Relaunch(const std::filesystem::path& game_folder);

// What the launcher found in a candidate game folder.
struct GameFolderCheck {
  // kNoData: default.xex is there, but not the rest of the disc (only the executable was
  // extracted, or the data is in another folder).
  enum class Status { kEmpty, kMissing, kNoData, kUnknownVersion, kOk };
  Status status = Status::kEmpty;
  // The folder that actually holds default.xex (it may be a subfolder of the chosen one).
  std::filesystem::path folder;
};
GameFolderCheck CheckGameFolder(const std::filesystem::path& chosen);

// Native folder picker; nullopt if cancelled or unavailable on this system.
std::optional<std::filesystem::path> PickFolder(const std::string& title);

namespace ui {

// Sets the UI scale from the window height in logical pixels; call before the fonts are built.
void SetScale(float window_height);
void ConfigureFonts(ImFontAtlas* atlas);
void ConfigureStyle(ImGuiStyle& style);

}  // namespace ui

// The start-up launcher's own window (Windows), in the style of a GNOME settings window: a
// header bar with the pages and Play, and the settings in cards below.
struct LauncherState {
  char path[1024] = {};
  GameFolderCheck check;
  int page = 0;
  // The graphics card the launcher window runs on, for the Graphics card row.
  std::string gpu;
  // The monitors by name, in the order the monitor setting counts them.
  std::vector<std::string> monitors;
  // The keyboard setting waiting for a key press, if any.
  std::string capturing;
};
void SetLauncherFolder(LauncherState& state, const std::filesystem::path& folder);
// Reads the GPU plugin's settings from the settings file so the launcher can show and change
// them before the plugin is loaded; SaveSettings writes them back.
void StageSettings(const std::filesystem::path& config_path);
namespace ui {
void ConfigureLauncherStyle(ImGuiStyle& style);
// Draws the launcher over the whole window; true when Play was pressed.
bool DrawLauncher(LauncherState& state);
}  // namespace ui
// Shows the launcher window until Play (returns the game folder) or until it is closed
// (nullopt), and saves the settings. If the window cannot open, returns the given folder. Windows
// only.
std::optional<std::filesystem::path> RunLauncherWindow(const std::filesystem::path& config_path,
                                                       const std::filesystem::path& folder);

// Start screen: game folder and the main options, then Play.
class Launcher final : public rex::ui::ImGuiDialog {
 public:
  using PlayCallback = std::function<void(const std::filesystem::path& game_folder)>;
  Launcher(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path,
           std::filesystem::path initial_folder, PlayCallback on_play, std::function<void()> on_quit);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void SetFolder(const std::filesystem::path& folder);

  std::filesystem::path config_path_;
  PlayCallback on_play_;
  std::function<void()> on_quit_;
  char path_buffer_[1024] = {};
  GameFolderCheck check_;
  bool started_ = false;
};

// In-game options, toggled with Insert or View + Menu on a controller.
class GameMenu final : public rex::ui::ImGuiDialog {
 public:
  GameMenu(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path,
           std::function<void()> on_close);
  ~GameMenu() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  std::filesystem::path config_path_;
  std::function<void()> on_close_;
  bool dirty_ = false;
  bool focused_ = false;
};

// Small frame rate readout in the corner, shown with the show_fps setting. It is attached to the
// drawer only while the setting is on: any attached dialog makes the presenter paint on the UI
// thread, which adds latency and keeps presentation from pacing the game on the vblanks.
class FpsOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit FpsOverlay(rex::ui::ImGuiDrawer* drawer);
  // Attaches or detaches the overlay to follow show_fps (UI thread).
  void Refresh();
  // Updated with the game's frames; repainting the window continuously for it presented the
  // same frame (with FSR) a few hundred times a second.
  bool WantsContinuousRepaint() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override;
};

}  // namespace pg
