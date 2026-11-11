// Controller access to the options menu (see pg_menu_input.h).
//
// The filter sits in the SDK's InputSystem::GetState, after the devices of a user are merged and
// before the game gets the state, so it works with any controller the SDK reads and runs every
// time the game polls. It runs on the game's threads; the menu itself opens on the UI thread.
#include "pg_menu_input.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>

#include <imgui.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>

namespace pg {

namespace {

// XInput button bits.
constexpr uint16_t kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004,
                   kDpadRight = 0x0008, kStart = 0x0010, kBack = 0x0020, kLeftThumb = 0x0040,
                   kRightThumb = 0x0080, kLeftShoulder = 0x0100, kRightShoulder = 0x0200,
                   kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;
constexpr uint16_t kCombo = kBack | kStart;
constexpr uint32_t kUsers = 4;
// A user the game has not polled for this long has no controller any more (the SDK only runs
// the filter for users with a device), so their last state is stale.
constexpr auto kStale = std::chrono::milliseconds(500);

using Clock = std::chrono::steady_clock;

struct User {
  // Buttons at the previous poll, for presses.
  uint16_t previous = 0;
  // Hidden after the menu opened or closed until every button is up, so the buttons that
  // opened or closed it never reach the game.
  bool wait_release = false;
  // The packet number the game sees, bumped whenever the state it sees changes. Games skip
  // states whose packet number did not change. The merged packet number of several devices
  // (the larger one) can stay the same while one of them changes, and hiding the controller
  // must count as a change too, or a stick held through it would be missed.
  uint32_t packet = 0;
  rex::input::X_INPUT_GAMEPAD reported = {};
  // The last real state, for the menu.
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  int16_t thumb_lx = 0, thumb_ly = 0;
  bool seen = false;
  Clock::time_point last_poll;

  bool Fresh(Clock::time_point now) const { return seen && now - last_poll < kStale; }
};

std::mutex g_mutex;
std::array<User, kUsers> g_users;
// Whether the menu is open or was asked to open (the request reaches the UI thread later).
bool g_menu_wanted = false;
std::function<void(bool)> g_request_menu;

// Only users with a controller: the flag is cleared when the game polls them with every button
// up, and users without a device are never polled.
void WaitForReleaseLocked() {
  const auto now = Clock::now();
  for (auto& user : g_users) user.wait_release = user.Fresh(now);
}

// Forgets the controllers that went away (UI or input thread, under the lock).
void ForgetStaleLocked(Clock::time_point now) {
  for (auto& user : g_users) {
    if (user.seen && !user.Fresh(now)) {
      user.seen = false;
      user.wait_release = false;
      user.previous = 0;
      user.buttons = 0;
      user.left_trigger = user.right_trigger = 0;
      user.thumb_lx = user.thumb_ly = 0;
    }
  }
}

void FilterState(uint32_t user_index, rex::input::X_INPUT_STATE& state) {
  if (user_index >= kUsers) return;
  const uint16_t buttons = uint16_t(state.gamepad.buttons);
  bool request = false;
  bool request_open = false;
  std::function<void(bool)> request_menu;
  {
    std::lock_guard lock(g_mutex);
    User& user = g_users[user_index];
    user.last_poll = Clock::now();
    user.seen = true;
    const uint16_t pressed = buttons & ~user.previous;
    user.previous = buttons;
    if ((buttons & kCombo) == kCombo && (pressed & kCombo)) {
      request = true;
      request_open = !g_menu_wanted;
    } else if (g_menu_wanted && (pressed & kStart) && !(buttons & kBack)) {
      request = true;
      request_open = false;
    }
    if (request) {
      g_menu_wanted = request_open;
      WaitForReleaseLocked();
      request_menu = g_request_menu;
    }
    if (user.wait_release && buttons == 0) user.wait_release = false;

    user.buttons = buttons;
    user.left_trigger = state.gamepad.left_trigger;
    user.right_trigger = state.gamepad.right_trigger;
    user.thumb_lx = int16_t(state.gamepad.thumb_lx);
    user.thumb_ly = int16_t(state.gamepad.thumb_ly);

    const bool hide = g_menu_wanted || user.wait_release;
    if (hide) state.gamepad = {};
    if (std::memcmp(&state.gamepad, &user.reported, sizeof(user.reported)) != 0) {
      user.reported = state.gamepad;
      ++user.packet;
    }
    state.packet_number = user.packet;
  }
  if (request_menu) request_menu(request_open);
}

bool FilterKeystroke(uint32_t user_index) {
  std::lock_guard lock(g_mutex);
  if (g_menu_wanted) return false;
  ForgetStaleLocked(Clock::now());
  if (user_index < kUsers) return !g_users[user_index].wait_release;
  // Any user (XUSER_INDEX_ANY).
  return std::none_of(g_users.begin(), g_users.end(),
                      [](const User& user) { return user.wait_release; });
}

// The button keys Dear ImGui's gamepad navigation reads.
constexpr std::pair<uint16_t, ImGuiKey> kButtonKeys[] = {
    {kDpadUp, ImGuiKey_GamepadDpadUp},       {kDpadDown, ImGuiKey_GamepadDpadDown},
    {kDpadLeft, ImGuiKey_GamepadDpadLeft},   {kDpadRight, ImGuiKey_GamepadDpadRight},
    {kStart, ImGuiKey_GamepadStart},         {kBack, ImGuiKey_GamepadBack},
    {kLeftThumb, ImGuiKey_GamepadL3},        {kRightThumb, ImGuiKey_GamepadR3},
    {kLeftShoulder, ImGuiKey_GamepadL1},     {kRightShoulder, ImGuiKey_GamepadR1},
    {kA, ImGuiKey_GamepadFaceDown},          {kB, ImGuiKey_GamepadFaceRight},
    {kX, ImGuiKey_GamepadFaceLeft},          {kY, ImGuiKey_GamepadFaceUp},
};

// A stick axis past the dead zone, 0 to 1.
float StickAmount(int value, bool negative) {
  constexpr float kDeadZone = 7849.0f;  // XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE
  const float v = negative ? -float(value) : float(value);
  return std::clamp((v - kDeadZone) / (32767.0f - kDeadZone), 0.0f, 1.0f);
}

void AddAnalog(ImGuiIO& io, ImGuiKey key, float value) {
  io.AddKeyAnalogEvent(key, value > 0.1f, value);
}

}  // namespace

void InstallMenuInput(rex::input::InputSystem* input, std::function<void(bool open)> request_menu) {
  if (!input) return;
  {
    std::lock_guard lock(g_mutex);
    g_request_menu = std::move(request_menu);
  }
  input->SetStateFilter(FilterState);
  input->SetKeystrokeFilter(FilterKeystroke);
  REXLOG_INFO("Options menu: View + Menu (Back + Start) on a controller opens it");
}

void StopMenuRequests() {
  std::lock_guard lock(g_mutex);
  g_request_menu = nullptr;
}

void SetMenuInputOpen(bool open) {
  std::lock_guard lock(g_mutex);
  if (g_menu_wanted == open) return;
  g_menu_wanted = open;
  // Opened or closed from the keyboard: the controllers' held buttons stay away from the game
  // until released, as with the controller.
  WaitForReleaseLocked();
}

void FeedMenuGamepad(ImGuiIO& io) {
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  // All controllers together: whichever one is in the player's hands moves through the menu.
  uint16_t buttons = 0;
  int lx = 0, ly = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  {
    std::lock_guard lock(g_mutex);
    ForgetStaleLocked(Clock::now());
    for (const auto& user : g_users) {
      if (!user.seen) continue;
      buttons |= user.buttons;
      if (std::abs(int(user.thumb_lx)) > std::abs(lx)) lx = user.thumb_lx;
      if (std::abs(int(user.thumb_ly)) > std::abs(ly)) ly = user.thumb_ly;
      left_trigger = std::max(left_trigger, user.left_trigger);
      right_trigger = std::max(right_trigger, user.right_trigger);
    }
  }
  for (const auto& [bit, key] : kButtonKeys) io.AddKeyEvent(key, (buttons & bit) != 0);
  AddAnalog(io, ImGuiKey_GamepadLStickLeft, StickAmount(lx, true));
  AddAnalog(io, ImGuiKey_GamepadLStickRight, StickAmount(lx, false));
  // XInput's Y axis points up.
  AddAnalog(io, ImGuiKey_GamepadLStickUp, StickAmount(ly, false));
  AddAnalog(io, ImGuiKey_GamepadLStickDown, StickAmount(ly, true));
  AddAnalog(io, ImGuiKey_GamepadL2, left_trigger / 255.0f);
  AddAnalog(io, ImGuiKey_GamepadR2, right_trigger / 255.0f);
}

void ReleaseMenuGamepad(ImGuiIO& io) {
  for (const auto& [bit, key] : kButtonKeys) io.AddKeyEvent(key, false);
  for (ImGuiKey key : {ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight,
                       ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadL2,
                       ImGuiKey_GamepadR2}) {
    io.AddKeyAnalogEvent(key, false, 0.0f);
  }
  io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
}

}  // namespace pg
