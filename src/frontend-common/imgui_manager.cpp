#include "imgui_manager.h"
#include "IconsFontAwesome5.h"
#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common/timer.h"
#include "common_host.h"
#include "core/gpu.h"
#include "core/host.h"
#include "core/host_display.h"
#include "core/system.h"
#include "fmt/format.h"
#include "fullscreen_ui.h"
#include "imgui.h"
#include "imgui_fullscreen.h"
#include "imgui_internal.h"
#include "input_manager.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <unordered_map>

Log_SetChannel(ImGuiManager);

namespace ImGuiManager {
static void SetStyle();
static void SetKeyMap();
static bool LoadFontData();
static bool AddImGuiFonts(bool fullscreen_fonts);
static ImFont* AddTextFont(float size);
static ImFont* AddFixedFont(float size);
static bool AddIconFonts(float size);
static void AcquirePendingOSDMessages();
static void DrawOSDMessages();
static void FormatProcessorStat(String& text, double usage, double time);
static void DrawPerformanceOverlay();
} // namespace ImGuiManager

static float s_global_scale = 1.0f;

static std::string s_font_path;
static const ImWchar* s_font_range = nullptr;

static ImFont* s_standard_font;
static ImFont* s_fixed_font;
static ImFont* s_medium_font;
static ImFont* s_large_font;

static std::vector<u8> s_standard_font_data;
static std::vector<u8> s_fixed_font_data;
static std::vector<u8> s_icon_font_data;

static Common::Timer s_last_render_time;

// cached copies of WantCaptureKeyboard/Mouse, used to know when to dispatch events
static std::atomic_bool s_imgui_wants_keyboard{false};
static std::atomic_bool s_imgui_wants_mouse{false};

// mapping of host key -> imgui key
static std::unordered_map<u32, ImGuiKey> s_imgui_key_map;

void ImGuiManager::SetFontPath(std::string path)
{
  s_font_path = std::move(path);
  s_standard_font_data = {};
}

void ImGuiManager::SetFontRange(const u16* range)
{
  s_font_range = range;
  s_standard_font_data = {};
}

bool ImGuiManager::Initialize()
{
  if (!LoadFontData())
  {
    Panic("Failed to load font data");
    return false;
  }

  s_global_scale =
    std::max(1.0f, g_host_display->GetWindowScale() * static_cast<float>(g_settings.display_osd_scale / 100.0f));

  ImGui::CreateContext();

  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  io.BackendUsingLegacyKeyArrays = 0;
  io.BackendUsingLegacyNavInputArray = 0;
#ifndef __ANDROID__
  // Android has no keyboard, nor are we using ImGui for any actual user-interactable windows.
  io.ConfigFlags |=
    ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NoMouseCursorChange;
#else
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
#endif

  io.DisplayFramebufferScale = ImVec2(1, 1); // We already scale things ourselves, this would double-apply scaling
  io.DisplaySize.x = static_cast<float>(g_host_display->GetWindowWidth());
  io.DisplaySize.y = static_cast<float>(g_host_display->GetWindowHeight());

  SetKeyMap();
  SetStyle();

  AssertMsg(!FullscreenUI::IsInitialized(), "Fullscreen UI is not initialized on ImGui init");

  if (!g_host_display->CreateImGuiContext())
  {
    Panic("Failed to create ImGui device context");
    g_host_display->DestroyImGuiContext();
    ImGui::DestroyContext();
    return false;
  }

  if (!AddImGuiFonts(false) || !g_host_display->UpdateImGuiFontTexture())
  {
    Panic("Failed to create ImGui font text");
    g_host_display->DestroyImGuiContext();
    ImGui::DestroyContext();
    return false;
  }

  // don't need the font data anymore, save some memory
  ImGui::GetIO().Fonts->ClearTexData();

  NewFrame();
  return true;
}

void ImGuiManager::Shutdown()
{
  FullscreenUI::Shutdown();

  if (g_host_display)
    g_host_display->DestroyImGuiContext();
  if (ImGui::GetCurrentContext())
    ImGui::DestroyContext();

  s_standard_font = nullptr;
  s_fixed_font = nullptr;
  s_medium_font = nullptr;
  s_large_font = nullptr;
  ImGuiFullscreen::SetFonts(nullptr, nullptr, nullptr);
}

void ImGuiManager::WindowResized()
{
  const u32 new_width = g_host_display ? g_host_display->GetWindowWidth() : 0;
  const u32 new_height = g_host_display ? g_host_display->GetWindowHeight() : 0;

  ImGui::GetIO().DisplaySize = ImVec2(static_cast<float>(new_width), static_cast<float>(new_height));

  UpdateScale();

  // restart imgui frame on the new window size to pick it up, otherwise we draw to the old size
  ImGui::EndFrame();
  NewFrame();
}

void ImGuiManager::UpdateScale()
{
  const float window_scale = g_host_display ? g_host_display->GetWindowScale() : 1.0f;
  const float scale = std::max(window_scale * static_cast<float>(/*EmuConfig.GS.OsdScale*/ 100.0 / 100.0), 1.0f);

  if (scale == s_global_scale && (!HasFullscreenFonts() || !ImGuiFullscreen::UpdateLayoutScale()))
    return;

  // This is assumed to be called mid-frame.
  ImGui::EndFrame();

  s_global_scale = scale;

  ImGui::GetStyle() = ImGuiStyle();
  ImGui::GetStyle().WindowMinSize = ImVec2(1.0f, 1.0f);
  SetStyle();
  ImGui::GetStyle().ScaleAllSizes(scale);

  if (!AddImGuiFonts(HasFullscreenFonts()))
    Panic("Failed to create ImGui font text");

  if (!g_host_display->UpdateImGuiFontTexture())
    Panic("Failed to recreate font texture after scale+resize");

  NewFrame();
}

void ImGuiManager::NewFrame()
{
  ImGuiIO& io = ImGui::GetIO();
  io.DeltaTime = static_cast<float>(s_last_render_time.GetTimeSecondsAndReset());

  ImGui::NewFrame();

  // Disable nav input on the implicit (Debug##Default) window. Otherwise we end up requesting keyboard
  // focus when there's nothing there. We use GetCurrentWindowRead() because otherwise it'll make it visible.
  ImGui::GetCurrentWindowRead()->Flags |= ImGuiWindowFlags_NoNavInputs;
  s_imgui_wants_keyboard.store(io.WantCaptureKeyboard, std::memory_order_relaxed);
  s_imgui_wants_mouse.store(io.WantCaptureMouse, std::memory_order_release);
}

void ImGuiManager::SetStyle()
{
  ImGuiStyle& style = ImGui::GetStyle();
  style = ImGuiStyle();
  style.WindowMinSize = ImVec2(1.0f, 1.0f);

  ImVec4* colors = style.Colors;
  colors[ImGuiCol_Text] = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
  colors[ImGuiCol_TextDisabled] = ImVec4(0.36f, 0.42f, 0.47f, 1.00f);
  colors[ImGuiCol_WindowBg] = ImVec4(0.11f, 0.15f, 0.17f, 1.00f);
  colors[ImGuiCol_ChildBg] = ImVec4(0.15f, 0.18f, 0.22f, 1.00f);
  colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.08f, 0.08f, 0.94f);
  colors[ImGuiCol_Border] = ImVec4(0.08f, 0.10f, 0.12f, 1.00f);
  colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
  colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.25f, 0.29f, 1.00f);
  colors[ImGuiCol_FrameBgHovered] = ImVec4(0.12f, 0.20f, 0.28f, 1.00f);
  colors[ImGuiCol_FrameBgActive] = ImVec4(0.09f, 0.12f, 0.14f, 1.00f);
  colors[ImGuiCol_TitleBg] = ImVec4(0.09f, 0.12f, 0.14f, 0.65f);
  colors[ImGuiCol_TitleBgActive] = ImVec4(0.08f, 0.10f, 0.12f, 1.00f);
  colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);
  colors[ImGuiCol_MenuBarBg] = ImVec4(0.15f, 0.18f, 0.22f, 1.00f);
  colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.02f, 0.02f, 0.39f);
  colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.20f, 0.25f, 0.29f, 1.00f);
  colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.18f, 0.22f, 0.25f, 1.00f);
  colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.09f, 0.21f, 0.31f, 1.00f);
  colors[ImGuiCol_CheckMark] = ImVec4(0.28f, 0.56f, 1.00f, 1.00f);
  colors[ImGuiCol_SliderGrab] = ImVec4(0.28f, 0.56f, 1.00f, 1.00f);
  colors[ImGuiCol_SliderGrabActive] = ImVec4(0.37f, 0.61f, 1.00f, 1.00f);
  colors[ImGuiCol_Button] = ImVec4(0.20f, 0.25f, 0.29f, 1.00f);
  colors[ImGuiCol_ButtonHovered] = ImVec4(0.33f, 0.38f, 0.46f, 1.00f);
  colors[ImGuiCol_ButtonActive] = ImVec4(0.27f, 0.32f, 0.38f, 1.00f);
  colors[ImGuiCol_Header] = ImVec4(0.20f, 0.25f, 0.29f, 0.55f);
  colors[ImGuiCol_HeaderHovered] = ImVec4(0.33f, 0.38f, 0.46f, 1.00f);
  colors[ImGuiCol_HeaderActive] = ImVec4(0.27f, 0.32f, 0.38f, 1.00f);
  colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.25f, 0.29f, 1.00f);
  colors[ImGuiCol_SeparatorHovered] = ImVec4(0.33f, 0.38f, 0.46f, 1.00f);
  colors[ImGuiCol_SeparatorActive] = ImVec4(0.27f, 0.32f, 0.38f, 1.00f);
  colors[ImGuiCol_ResizeGrip] = ImVec4(0.26f, 0.59f, 0.98f, 0.25f);
  colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.33f, 0.38f, 0.46f, 1.00f);
  colors[ImGuiCol_ResizeGripActive] = ImVec4(0.27f, 0.32f, 0.38f, 1.00f);
  colors[ImGuiCol_Tab] = ImVec4(0.11f, 0.15f, 0.17f, 1.00f);
  colors[ImGuiCol_TabHovered] = ImVec4(0.33f, 0.38f, 0.46f, 1.00f);
  colors[ImGuiCol_TabActive] = ImVec4(0.27f, 0.32f, 0.38f, 1.00f);
  colors[ImGuiCol_TabUnfocused] = ImVec4(0.11f, 0.15f, 0.17f, 1.00f);
  colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.11f, 0.15f, 0.17f, 1.00f);
  colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
  colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
  colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
  colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
  colors[ImGuiCol_TextSelectedBg] = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);
  colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
  colors[ImGuiCol_NavHighlight] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
  colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
  colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
  colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);

  style.ScaleAllSizes(s_global_scale);
}

void ImGuiManager::SetKeyMap()
{
  struct KeyMapping
  {
    int index;
    const char* name;
    const char* alt_name;
  };

  static constexpr KeyMapping mapping[] = {{ImGuiKey_LeftArrow, "Left"},
                                           {ImGuiKey_RightArrow, "Right"},
                                           {ImGuiKey_UpArrow, "Up"},
                                           {ImGuiKey_DownArrow, "Down"},
                                           {ImGuiKey_PageUp, "PageUp"},
                                           {ImGuiKey_PageDown, "PageDown"},
                                           {ImGuiKey_Home, "Home"},
                                           {ImGuiKey_End, "End"},
                                           {ImGuiKey_Insert, "Insert"},
                                           {ImGuiKey_Delete, "Delete"},
                                           {ImGuiKey_Backspace, "Backspace"},
                                           {ImGuiKey_Space, "Space"},
                                           {ImGuiKey_Enter, "Return"},
                                           {ImGuiKey_Escape, "Escape"},
                                           {ImGuiKey_LeftCtrl, "LeftCtrl", "Ctrl"},
                                           {ImGuiKey_LeftShift, "LeftShift", "Shift"},
                                           {ImGuiKey_LeftAlt, "LeftAlt", "Alt"},
                                           {ImGuiKey_LeftSuper, "LeftSuper", "Super"},
                                           {ImGuiKey_RightCtrl, "RightCtrl"},
                                           {ImGuiKey_RightShift, "RightShift"},
                                           {ImGuiKey_RightAlt, "RightAlt"},
                                           {ImGuiKey_RightSuper, "RightSuper"},
                                           {ImGuiKey_Menu, "Menu"},
                                           {ImGuiKey_0, "0"},
                                           {ImGuiKey_1, "1"},
                                           {ImGuiKey_2, "2"},
                                           {ImGuiKey_3, "3"},
                                           {ImGuiKey_4, "4"},
                                           {ImGuiKey_5, "5"},
                                           {ImGuiKey_6, "6"},
                                           {ImGuiKey_7, "7"},
                                           {ImGuiKey_8, "8"},
                                           {ImGuiKey_9, "9"},
                                           {ImGuiKey_A, "A"},
                                           {ImGuiKey_B, "B"},
                                           {ImGuiKey_C, "C"},
                                           {ImGuiKey_D, "D"},
                                           {ImGuiKey_E, "E"},
                                           {ImGuiKey_F, "F"},
                                           {ImGuiKey_G, "G"},
                                           {ImGuiKey_H, "H"},
                                           {ImGuiKey_I, "I"},
                                           {ImGuiKey_J, "J"},
                                           {ImGuiKey_K, "K"},
                                           {ImGuiKey_L, "L"},
                                           {ImGuiKey_M, "M"},
                                           {ImGuiKey_N, "N"},
                                           {ImGuiKey_O, "O"},
                                           {ImGuiKey_P, "P"},
                                           {ImGuiKey_Q, "Q"},
                                           {ImGuiKey_R, "R"},
                                           {ImGuiKey_S, "S"},
                                           {ImGuiKey_T, "T"},
                                           {ImGuiKey_U, "U"},
                                           {ImGuiKey_V, "V"},
                                           {ImGuiKey_W, "W"},
                                           {ImGuiKey_X, "X"},
                                           {ImGuiKey_Y, "Y"},
                                           {ImGuiKey_Z, "Z"},
                                           {ImGuiKey_F1, "F1"},
                                           {ImGuiKey_F2, "F2"},
                                           {ImGuiKey_F3, "F3"},
                                           {ImGuiKey_F4, "F4"},
                                           {ImGuiKey_F5, "F5"},
                                           {ImGuiKey_F6, "F6"},
                                           {ImGuiKey_F7, "F7"},
                                           {ImGuiKey_F8, "F8"},
                                           {ImGuiKey_F9, "F9"},
                                           {ImGuiKey_F10, "F10"},
                                           {ImGuiKey_F11, "F11"},
                                           {ImGuiKey_F12, "F12"},
                                           {ImGuiKey_Apostrophe, "Apostrophe"},
                                           {ImGuiKey_Comma, "Comma"},
                                           {ImGuiKey_Minus, "Minus"},
                                           {ImGuiKey_Period, "Period"},
                                           {ImGuiKey_Slash, "Slash"},
                                           {ImGuiKey_Semicolon, "Semicolon"},
                                           {ImGuiKey_Equal, "Equal"},
                                           {ImGuiKey_LeftBracket, "BracketLeft"},
                                           {ImGuiKey_Backslash, "Backslash"},
                                           {ImGuiKey_RightBracket, "BracketRight"},
                                           {ImGuiKey_GraveAccent, "QuoteLeft"},
                                           {ImGuiKey_CapsLock, "CapsLock"},
                                           {ImGuiKey_ScrollLock, "ScrollLock"},
                                           {ImGuiKey_NumLock, "NumLock"},
                                           {ImGuiKey_PrintScreen, "PrintScreen"},
                                           {ImGuiKey_Pause, "Pause"},
                                           {ImGuiKey_Keypad0, "Keypad0"},
                                           {ImGuiKey_Keypad1, "Keypad1"},
                                           {ImGuiKey_Keypad2, "Keypad2"},
                                           {ImGuiKey_Keypad3, "Keypad3"},
                                           {ImGuiKey_Keypad4, "Keypad4"},
                                           {ImGuiKey_Keypad5, "Keypad5"},
                                           {ImGuiKey_Keypad6, "Keypad6"},
                                           {ImGuiKey_Keypad7, "Keypad7"},
                                           {ImGuiKey_Keypad8, "Keypad8"},
                                           {ImGuiKey_Keypad9, "Keypad9"},
                                           {ImGuiKey_KeypadDecimal, "KeypadPeriod"},
                                           {ImGuiKey_KeypadDivide, "KeypadDivide"},
                                           {ImGuiKey_KeypadMultiply, "KeypadMultiply"},
                                           {ImGuiKey_KeypadSubtract, "KeypadMinus"},
                                           {ImGuiKey_KeypadAdd, "KeypadPlus"},
                                           {ImGuiKey_KeypadEnter, "KeypadReturn"},
                                           {ImGuiKey_KeypadEqual, "KeypadEqual"}};

  s_imgui_key_map.clear();
  for (const KeyMapping& km : mapping)
  {
    std::optional<u32> map(InputManager::ConvertHostKeyboardStringToCode(km.name));
    if (!map.has_value() && km.alt_name)
      map = InputManager::ConvertHostKeyboardStringToCode(km.alt_name);
    if (map.has_value())
      s_imgui_key_map[map.value()] = km.index;
  }
}

bool ImGuiManager::LoadFontData()
{
  if (s_standard_font_data.empty())
  {
    std::optional<std::vector<u8>> font_data = s_font_path.empty() ?
                                                 Host::ReadResourceFile("fonts/Roboto-Regular.ttf") :
                                                 FileSystem::ReadBinaryFile(s_font_path.c_str());
    if (!font_data.has_value())
      return false;

    s_standard_font_data = std::move(font_data.value());
  }

  if (s_fixed_font_data.empty())
  {
    std::optional<std::vector<u8>> font_data = Host::ReadResourceFile("fonts/RobotoMono-Medium.ttf");
    if (!font_data.has_value())
      return false;

    s_fixed_font_data = std::move(font_data.value());
  }

  if (s_icon_font_data.empty())
  {
    std::optional<std::vector<u8>> font_data = Host::ReadResourceFile("fonts/fa-solid-900.ttf");
    if (!font_data.has_value())
      return false;

    s_icon_font_data = std::move(font_data.value());
  }

  return true;
}

ImFont* ImGuiManager::AddTextFont(float size)
{
  static const ImWchar default_ranges[] = {
    // Basic Latin + Latin Supplement + Central European diacritics
    0x0020,
    0x017F,

    // Cyrillic + Cyrillic Supplement
    0x0400,
    0x052F,

    // Cyrillic Extended-A
    0x2DE0,
    0x2DFF,

    // Cyrillic Extended-B
    0xA640,
    0xA69F,

    0,
  };

  ImFontConfig cfg;
  cfg.FontDataOwnedByAtlas = false;
  return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(s_standard_font_data.data(),
                                                    static_cast<int>(s_standard_font_data.size()), size, &cfg,
                                                    s_font_range ? s_font_range : default_ranges);
}

ImFont* ImGuiManager::AddFixedFont(float size)
{
  ImFontConfig cfg;
  cfg.FontDataOwnedByAtlas = false;
  return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(s_fixed_font_data.data(),
                                                    static_cast<int>(s_fixed_font_data.size()), size, &cfg, nullptr);
}

bool ImGuiManager::AddIconFonts(float size)
{
  static const ImWchar range_fa[] = {ICON_MIN_FA, ICON_MAX_FA, 0};

  ImFontConfig cfg;
  cfg.MergeMode = true;
  cfg.PixelSnapH = true;
  cfg.GlyphMinAdvanceX = size * 0.75f;
  cfg.GlyphMaxAdvanceX = size * 0.75f;
  cfg.FontDataOwnedByAtlas = false;

  return (ImGui::GetIO().Fonts->AddFontFromMemoryTTF(s_icon_font_data.data(), static_cast<int>(s_icon_font_data.size()),
                                                     size * 0.75f, &cfg, range_fa) != nullptr);
}

bool ImGuiManager::AddImGuiFonts(bool fullscreen_fonts)
{
  const float standard_font_size = std::ceil(15.0f * s_global_scale);

  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->Clear();

  s_standard_font = AddTextFont(standard_font_size);
  if (!s_standard_font || !AddIconFonts(standard_font_size))
    return false;

  s_fixed_font = AddFixedFont(standard_font_size);
  if (!s_fixed_font)
    return false;

  if (fullscreen_fonts)
  {
    const float medium_font_size = std::ceil(ImGuiFullscreen::LayoutScale(ImGuiFullscreen::LAYOUT_MEDIUM_FONT_SIZE));
    s_medium_font = AddTextFont(medium_font_size);
    if (!s_medium_font || !AddIconFonts(medium_font_size))
      return false;

    const float large_font_size = std::ceil(ImGuiFullscreen::LayoutScale(ImGuiFullscreen::LAYOUT_LARGE_FONT_SIZE));
    s_large_font = AddTextFont(large_font_size);
    if (!s_large_font || !AddIconFonts(large_font_size))
      return false;
  }
  else
  {
    s_medium_font = nullptr;
    s_large_font = nullptr;
  }

  ImGuiFullscreen::SetFonts(s_standard_font, s_medium_font, s_large_font);

  return io.Fonts->Build();
}

bool ImGuiManager::AddFullscreenFontsIfMissing()
{
  if (HasFullscreenFonts())
    return true;

  // can't do this in the middle of a frame
  ImGui::EndFrame();

  if (!AddImGuiFonts(true))
  {
    Log_ErrorPrint("Failed to lazily allocate fullscreen fonts.");
    AddImGuiFonts(false);
  }

  g_host_display->UpdateImGuiFontTexture();
  NewFrame();

  return HasFullscreenFonts();
}

bool ImGuiManager::HasFullscreenFonts()
{
  return (s_medium_font && s_large_font);
}

struct OSDMessage
{
  std::string key;
  std::string text;
  std::chrono::steady_clock::time_point time;
  float duration;
};

static std::deque<OSDMessage> s_osd_active_messages;
static std::deque<OSDMessage> s_osd_posted_messages;
static std::mutex s_osd_messages_lock;

void Host::AddOSDMessage(std::string message, float duration /*= 2.0f*/)
{
  AddKeyedOSDMessage(std::string(), std::move(message), duration);
}

void Host::AddKeyedOSDMessage(std::string key, std::string message, float duration /* = 2.0f */)
{
  OSDMessage msg;
  msg.key = std::move(key);
  msg.text = std::move(message);
  msg.duration = duration;
  msg.time = std::chrono::steady_clock::now();

  std::unique_lock<std::mutex> lock(s_osd_messages_lock);
  s_osd_posted_messages.push_back(std::move(msg));
}

void Host::AddFormattedOSDMessage(float duration, const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);
  std::string ret = StringUtil::StdStringFromFormatV(format, ap);
  va_end(ap);
  return AddKeyedOSDMessage(std::string(), std::move(ret), duration);
}

void Host::AddKeyedFormattedOSDMessage(std::string key, float duration, const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);
  std::string ret = StringUtil::StdStringFromFormatV(format, ap);
  va_end(ap);
  return AddKeyedOSDMessage(std::move(key), std::move(ret), duration);
}

void Host::RemoveKeyedOSDMessage(std::string key)
{
  OSDMessage msg;
  msg.key = std::move(key);
  msg.duration = 0.0f;
  msg.time = std::chrono::steady_clock::now();

  std::unique_lock<std::mutex> lock(s_osd_messages_lock);
  s_osd_posted_messages.push_back(std::move(msg));
}

void Host::ClearOSDMessages()
{
  {
    std::unique_lock<std::mutex> lock(s_osd_messages_lock);
    s_osd_posted_messages.clear();
  }

  s_osd_active_messages.clear();
}

void ImGuiManager::AcquirePendingOSDMessages()
{
  std::atomic_thread_fence(std::memory_order_consume);
  if (s_osd_posted_messages.empty())
    return;

  std::unique_lock lock(s_osd_messages_lock);
  for (;;)
  {
    if (s_osd_posted_messages.empty())
      break;

    if (g_settings.display_show_osd_messages)
    {
      OSDMessage& new_msg = s_osd_posted_messages.front();
      std::deque<OSDMessage>::iterator iter;
      if (!new_msg.key.empty() && (iter = std::find_if(s_osd_active_messages.begin(), s_osd_active_messages.end(),
                                                       [&new_msg](const OSDMessage& other) {
                                                         return new_msg.key == other.key;
                                                       })) != s_osd_active_messages.end())
      {
        iter->text = std::move(new_msg.text);
        iter->duration = new_msg.duration;
        iter->time = new_msg.time;
      }
      else
      {
        s_osd_active_messages.push_back(std::move(new_msg));
      }
    }

    s_osd_posted_messages.pop_front();

    static constexpr size_t MAX_ACTIVE_OSD_MESSAGES = 512;
    if (s_osd_active_messages.size() > MAX_ACTIVE_OSD_MESSAGES)
      s_osd_active_messages.pop_front();
  }
}

void ImGuiManager::DrawOSDMessages()
{
  ImFont* const font = ImGui::GetFont();
  const float scale = s_global_scale;
  const float spacing = std::ceil(5.0f * scale);
  const float margin = std::ceil(10.0f * scale);
  const float padding = std::ceil(8.0f * scale);
  const float rounding = std::ceil(5.0f * scale);
  const float max_width = ImGui::GetIO().DisplaySize.x - (margin + padding) * 2.0f;
  float position_x = margin;
  float position_y = margin;

  const auto now = std::chrono::steady_clock::now();

  auto iter = s_osd_active_messages.begin();
  while (iter != s_osd_active_messages.end())
  {
    const OSDMessage& msg = *iter;
    const double time = std::chrono::duration<double>(now - msg.time).count();
    const float time_remaining = static_cast<float>(msg.duration - time);
    if (time_remaining <= 0.0f)
    {
      iter = s_osd_active_messages.erase(iter);
      continue;
    }

    ++iter;

    const float opacity = std::min(time_remaining, 1.0f);
    const u32 alpha = static_cast<u32>(opacity * 255.0f);

    if (position_y >= ImGui::GetIO().DisplaySize.y)
      break;

    const ImVec2 pos(position_x, position_y);
    const ImVec2 text_size(font->CalcTextSizeA(font->FontSize, max_width, max_width, msg.text.c_str(),
                                               msg.text.c_str() + msg.text.length()));
    const ImVec2 size(text_size.x + padding * 2.0f, text_size.y + padding * 2.0f);
    const ImVec4 text_rect(pos.x + padding, pos.y + padding, pos.x + size.x - padding, pos.y + size.y - padding);

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(0x21, 0x21, 0x21, alpha), rounding);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(0x48, 0x48, 0x48, alpha), rounding);
    dl->AddText(font, font->FontSize, ImVec2(text_rect.x, text_rect.y), IM_COL32(0xff, 0xff, 0xff, alpha),
                msg.text.c_str(), msg.text.c_str() + msg.text.length(), max_width, &text_rect);
    position_y += size.y + spacing;
  }
}

void ImGuiManager::FormatProcessorStat(String& text, double usage, double time)
{
  // Some values, such as GPU (and even CPU to some extent) can be out of phase with the wall clock,
  // which the processor time is divided by to get a utilization percentage. Let's clamp it at 100%,
  // so that people don't get confused, and remove the decimal places when it's there while we're at it.
  if (usage >= 99.95)
    text.AppendFmtString("100% ({:.2f}ms)", time);
  else
    text.AppendFmtString("{:.1f}% ({:.2f}ms)", usage, time);
}

void ImGuiManager::DrawPerformanceOverlay()
{
  if (!(g_settings.display_show_fps || g_settings.display_show_speed || g_settings.display_show_resolution ||
        g_settings.display_show_cpu ||
        (g_settings.display_show_status_indicators &&
         (System::IsPaused() || System::IsFastForwardEnabled() || System::IsTurboEnabled()))))
  {
    return;
  }

  const float scale = s_global_scale;
  const float shadow_offset = std::ceil(1.0f * scale);
  const float margin = std::ceil(10.0f * scale);
  const float spacing = std::ceil(5.0f * scale);
  float position_y = margin;

  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  SmallString text;
  ImVec2 text_size;
  bool first = true;

#define DRAW_LINE(font, text, color)                                                                                   \
  do                                                                                                                   \
  {                                                                                                                    \
    text_size =                                                                                                        \
      font->CalcTextSizeA(font->FontSize, std::numeric_limits<float>::max(), -1.0f, (text), nullptr, nullptr);         \
    dl->AddText(                                                                                                       \
      font, font->FontSize,                                                                                            \
      ImVec2(ImGui::GetIO().DisplaySize.x - margin - text_size.x + shadow_offset, position_y + shadow_offset),         \
      IM_COL32(0, 0, 0, 100), text, text.GetCharArray() + text.GetLength());                                           \
    dl->AddText(font, font->FontSize, ImVec2(ImGui::GetIO().DisplaySize.x - margin - text_size.x, position_y), color,  \
                (text));                                                                                               \
    position_y += text_size.y + spacing;                                                                               \
  } while (0)

  const System::State state = System::GetState();
  if (state == System::State::Running)
  {
    const float speed = System::GetEmulationSpeed();
    if (g_settings.display_show_fps)
    {
      text.AppendFmtString("G: {:.2f} | V: {:.2f}", System::GetFPS(), System::GetVPS());
      first = false;
    }
    if (g_settings.display_show_speed)
    {
      text.AppendFmtString("{}{}%", first ? "" : " | ", static_cast<u32>(std::round(speed)));

      const float target_speed = System::GetTargetSpeed();
      if (target_speed <= 0.0f)
        text.AppendString(" (Max)");
      else
        text.AppendFmtString(" ({:.0f}%)", target_speed * 100.0f);

      first = false;
    }
    if (!text.IsEmpty())
    {
      ImU32 color;
      if (speed < 95.0f)
        color = IM_COL32(255, 100, 100, 255);
      else if (speed > 105.0f)
        color = IM_COL32(100, 255, 100, 255);
      else
        color = IM_COL32(255, 255, 255, 255);

      DRAW_LINE(s_fixed_font, text, color);
    }

    if (g_settings.display_show_resolution)
    {
      const auto [effective_width, effective_height] = g_gpu->GetEffectiveDisplayResolution();
      const bool interlaced = g_gpu->IsInterlacedDisplayEnabled();
      text.Fmt("{}x{} ({})", effective_width, effective_height, interlaced ? "interlaced" : "progressive");
      DRAW_LINE(s_fixed_font, text, IM_COL32(255, 255, 255, 255));
    }

    if (g_settings.display_show_cpu)
    {
      text.Clear();
      text.AppendFmtString("{:.2f}ms ({:.2f}ms worst)", System::GetAverageFrameTime(), System::GetWorstFrameTime());
      DRAW_LINE(s_fixed_font, text, IM_COL32(255, 255, 255, 255));

      text.Clear();
      if (g_settings.cpu_overclock_active)
        text.Fmt("CPU[{}]: ", g_settings.GetCPUOverclockPercent());
      else
        text.Assign("CPU: ");
      FormatProcessorStat(text, System::GetCPUThreadUsage(), System::GetCPUThreadAverageTime());
      DRAW_LINE(s_fixed_font, text, IM_COL32(255, 255, 255, 255));

      if (!g_gpu->IsHardwareRenderer() && g_settings.gpu_use_thread)
      {
        text.Assign("SW: ");
        FormatProcessorStat(text, System::GetSWThreadUsage(), System::GetSWThreadAverageTime());
        DRAW_LINE(s_fixed_font, text, IM_COL32(255, 255, 255, 255));
      }
    }

    if (g_settings.display_show_status_indicators)
    {
      const bool rewinding = System::IsRewinding();
      if (rewinding || System::IsFastForwardEnabled() || System::IsTurboEnabled())
      {
        text.Assign(rewinding ? ICON_FA_FAST_BACKWARD : ICON_FA_FAST_FORWARD);
        DRAW_LINE(s_standard_font, text, IM_COL32(255, 255, 255, 255));
      }
    }
  }
  else if (g_settings.display_show_status_indicators && state == System::State::Paused)
  {
    text.Assign(ICON_FA_PAUSE);
    DRAW_LINE(s_standard_font, text, IM_COL32(255, 255, 255, 255));
  }

#undef DRAW_LINE
}

void ImGuiManager::RenderOSD()
{
  // acquire for IO.MousePos.
  std::atomic_thread_fence(std::memory_order_acquire);

  // Don't draw OSD when we're just running big picture.
  if (System::IsValid())
    DrawPerformanceOverlay();

  AcquirePendingOSDMessages();
  DrawOSDMessages();
}

float ImGuiManager::GetGlobalScale()
{
  return s_global_scale;
}

ImFont* ImGuiManager::GetStandardFont()
{
  return s_standard_font;
}

ImFont* ImGuiManager::GetFixedFont()
{
  return s_fixed_font;
}

ImFont* ImGuiManager::GetMediumFont()
{
  AddFullscreenFontsIfMissing();
  return s_medium_font;
}

ImFont* ImGuiManager::GetLargeFont()
{
  AddFullscreenFontsIfMissing();
  return s_large_font;
}

void ImGuiManager::UpdateMousePosition(float x, float y)
{
  if (!ImGui::GetCurrentContext())
    return;

  ImGui::GetIO().MousePos = ImVec2(x, y);
  std::atomic_thread_fence(std::memory_order_release);
}

bool ImGuiManager::ProcessPointerButtonEvent(InputBindingKey key, float value)
{
  if (!ImGui::GetCurrentContext() || (key.data - 1u) >= std::size(ImGui::GetIO().MouseDown))
    return false;

  // still update state anyway
  ImGui::GetIO().AddMouseButtonEvent(key.data - 1, value != 0.0f);

  return s_imgui_wants_mouse.load(std::memory_order_acquire);
}

bool ImGuiManager::ProcessPointerAxisEvent(InputBindingKey key, float value)
{
  if (!ImGui::GetCurrentContext() || value == 0.0f || key.data < static_cast<u32>(InputPointerAxis::WheelX))
    return false;

  // still update state anyway
  const bool horizontal = (key.data == static_cast<u32>(InputPointerAxis::WheelX));
  ImGui::GetIO().AddMouseWheelEvent(horizontal ? value : 0.0f, horizontal ? 0.0f : value);

  return s_imgui_wants_mouse.load(std::memory_order_acquire);
}

bool ImGuiManager::ProcessHostKeyEvent(InputBindingKey key, float value)
{
  decltype(s_imgui_key_map)::iterator iter;
  if (!ImGui::GetCurrentContext() || (iter = s_imgui_key_map.find(key.data)) == s_imgui_key_map.end())
    return false;

  // still update state anyway
  ImGui::GetIO().AddKeyEvent(iter->second, value != 0.0);

  return s_imgui_wants_keyboard.load(std::memory_order_acquire);
}

bool ImGuiManager::ProcessGenericInputEvent(GenericInputBinding key, float value)
{
  static constexpr ImGuiKey key_map[] = {
    ImGuiKey_None,             // Unknown,
    ImGuiKey_GamepadDpadUp,    // DPadUp
    ImGuiKey_GamepadDpadRight, // DPadRight
    ImGuiKey_GamepadDpadLeft,  // DPadLeft
    ImGuiKey_GamepadDpadDown,  // DPadDown
    ImGuiKey_None,             // LeftStickUp
    ImGuiKey_None,             // LeftStickRight
    ImGuiKey_None,             // LeftStickDown
    ImGuiKey_None,             // LeftStickLeft
    ImGuiKey_GamepadL3,        // L3
    ImGuiKey_None,             // RightStickUp
    ImGuiKey_None,             // RightStickRight
    ImGuiKey_None,             // RightStickDown
    ImGuiKey_None,             // RightStickLeft
    ImGuiKey_GamepadR3,        // R3
    ImGuiKey_GamepadFaceUp,    // Triangle
    ImGuiKey_GamepadFaceRight, // Circle
    ImGuiKey_GamepadFaceDown,  // Cross
    ImGuiKey_GamepadFaceLeft,  // Square
    ImGuiKey_GamepadBack,      // Select
    ImGuiKey_GamepadStart,     // Start
    ImGuiKey_None,             // System
    ImGuiKey_GamepadL1,        // L1
    ImGuiKey_GamepadL2,        // L2
    ImGuiKey_GamepadR1,        // R1
    ImGuiKey_GamepadL2,        // R2
  };

  if (!ImGui::GetCurrentContext() || !s_imgui_wants_keyboard.load(std::memory_order_acquire))
    return false;

  if (static_cast<u32>(key) >= std::size(key_map) || key_map[static_cast<u32>(key)] == ImGuiKey_None)
    return false;

  ImGui::GetIO().AddKeyAnalogEvent(key_map[static_cast<u32>(key)], (value > 0.0f), value);
  return true;
}

#if 0
void CommonHostInterface::DrawImGuiWindows()
{
  const bool system_valid = System::IsValid();
  if (system_valid)
  {
    if (m_save_state_selector_ui->IsOpen())
      m_save_state_selector_ui->Draw();

    if (s_input_overlay_ui)
      s_input_overlay_ui->Draw();

    if (g_settings.display_show_enhancements)
      DrawEnhancementsOverlay();
  }

  if (m_fullscreen_ui_enabled)
  {
    FullscreenUI::Render();
    return;
  }

  if (system_valid)
  {
    if (!IsCheevosChallengeModeActive())
      DrawDebugWindows();
    DrawStatsOverlay();
  }

  DrawOSDMessages();
}

void CommonHostInterface::DrawEnhancementsOverlay()
{
  LargeString text;
  text.AppendString(Settings::GetConsoleRegionName(System::GetRegion()));

  if (g_settings.rewind_enable)
    text.AppendFormattedString(" RW=%g/%u", g_settings.rewind_save_frequency, g_settings.rewind_save_slots);
  if (g_settings.IsRunaheadEnabled())
    text.AppendFormattedString(" RA=%u", g_settings.runahead_frames);

  if (g_settings.cpu_overclock_active)
    text.AppendFormattedString(" CPU=%u%%", g_settings.GetCPUOverclockPercent());
  if (g_settings.enable_8mb_ram)
    text.AppendString(" 8MB");
  if (g_settings.cdrom_read_speedup != 1)
    text.AppendFormattedString(" CDR=%ux", g_settings.cdrom_read_speedup);
  if (g_settings.cdrom_seek_speedup != 1)
    text.AppendFormattedString(" CDS=%ux", g_settings.cdrom_seek_speedup);
  if (g_settings.gpu_resolution_scale != 1)
    text.AppendFormattedString(" IR=%ux", g_settings.gpu_resolution_scale);
  if (g_settings.gpu_multisamples != 1)
  {
    text.AppendFormattedString(" %ux%s", g_settings.gpu_multisamples,
      g_settings.gpu_per_sample_shading ? "MSAA" : "SSAA");
  }
  if (g_settings.gpu_true_color)
    text.AppendString(" TrueCol");
  if (g_settings.gpu_disable_interlacing)
    text.AppendString(" ForceProg");
  if (g_settings.gpu_force_ntsc_timings && System::GetRegion() == ConsoleRegion::PAL)
    text.AppendString(" PAL60");
  if (g_settings.gpu_texture_filter != GPUTextureFilter::Nearest)
    text.AppendFormattedString(" %s", Settings::GetTextureFilterName(g_settings.gpu_texture_filter));
  if (g_settings.gpu_widescreen_hack && g_settings.display_aspect_ratio != DisplayAspectRatio::Auto &&
    g_settings.display_aspect_ratio != DisplayAspectRatio::R4_3)
  {
    text.AppendString(" WSHack");
  }
  if (g_settings.gpu_pgxp_enable)
  {
    text.AppendString(" PGXP");
    if (g_settings.gpu_pgxp_culling)
      text.AppendString("/Cull");
    if (g_settings.gpu_pgxp_texture_correction)
      text.AppendString("/Tex");
    if (g_settings.gpu_pgxp_vertex_cache)
      text.AppendString("/VC");
    if (g_settings.gpu_pgxp_cpu)
      text.AppendString("/CPU");
    if (g_settings.gpu_pgxp_depth_buffer)
      text.AppendString("/Depth");
  }

  float shadow_offset, margin, position_y;
  ImFont* font;

  if (m_fullscreen_ui_enabled)
  {
    margin = ImGuiFullscreen::LayoutScale(10.0f);
    shadow_offset = ImGuiFullscreen::DPIScale(1.0f);
    font = ImGuiFullscreen::g_medium_font;
    position_y = ImGui::GetIO().DisplaySize.y - margin - font->FontSize;
  }
  else
  {
    const float scale = ImGui::GetIO().DisplayFramebufferScale.x;
    shadow_offset = 1.0f * scale;
    margin = 10.0f * scale;
    font = ImGui::GetFont();
    position_y = ImGui::GetIO().DisplaySize.y - margin - font->FontSize;
  }

  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  ImVec2 text_size = font->CalcTextSizeA(font->FontSize, std::numeric_limits<float>::max(), -1.0f, text,
    text.GetCharArray() + text.GetLength(), nullptr);
  dl->AddText(font, font->FontSize,
    ImVec2(ImGui::GetIO().DisplaySize.x - margin - text_size.x + shadow_offset, position_y + shadow_offset),
    IM_COL32(0, 0, 0, 100), text, text.GetCharArray() + text.GetLength());
  dl->AddText(font, font->FontSize, ImVec2(ImGui::GetIO().DisplaySize.x - margin - text_size.x, position_y),
    IM_COL32(255, 255, 255, 255), text, text.GetCharArray() + text.GetLength());
}

#endif
