#include "android/android_system.h"

#include <chrono>
#include <thread>

#include "android/android_graphics.h"
#include "libreallive/gameexe.h"
#include "machine/rlmachine.h"
#include "systems/base/colour.h"
#include "systems/base/platform.h"

namespace {

// 引擎只要求时间单调递增，与真实时钟起点无关。
unsigned int NowMillis() {
  using namespace std::chrono;
  return static_cast<unsigned int>(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

}  // namespace

// ---------------------------------------------------------------------------
// AndroidEventSystem
// ---------------------------------------------------------------------------

AndroidEventSystem::AndroidEventSystem(Gameexe& gexe)
    : EventSystem(gexe), last_mouse_move_ticks_(0) {}

void AndroidEventSystem::ExecuteEventSystem(RLMachine& /*machine*/) {
  // 触摸/按键的注入由 T2.3 实现；当前主循环不需要事件源即可推进。
}

unsigned int AndroidEventSystem::GetTicks() const { return NowMillis(); }

void AndroidEventSystem::Wait(unsigned int milliseconds) const {
  std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

bool AndroidEventSystem::ShiftPressed() const { return false; }

bool AndroidEventSystem::CtrlPressed() const { return false; }

Point AndroidEventSystem::GetCursorPos() { return Point(0, 0); }

void AndroidEventSystem::GetCursorPos(Point& position, int& button1, int& button2) {
  position = Point(0, 0);
  button1 = 0;
  button2 = 0;
}

void AndroidEventSystem::FlushMouseClicks() {}

unsigned int AndroidEventSystem::TimeOfLastMouseMove() {
  return last_mouse_move_ticks_;
}

void AndroidEventSystem::InjectMouseMovement(RLMachine& /*machine*/,
                                             const Point& /*loc*/) {
  last_mouse_move_ticks_ = NowMillis();
}

void AndroidEventSystem::InjectMouseDown(RLMachine& /*machine*/) {}

void AndroidEventSystem::InjectMouseUp(RLMachine& /*machine*/) {}

// ---------------------------------------------------------------------------
// AndroidTextSystem
// ---------------------------------------------------------------------------

AndroidTextSystem::AndroidTextSystem(System& system, Gameexe& gexe)
    : TextSystem(system, gexe) {}

std::shared_ptr<TextWindow> AndroidTextSystem::GetTextWindow(
    int text_window_number) {
  WindowMap::iterator it = text_window_.find(text_window_number);
  if (it != text_window_.end()) return it->second;

  std::shared_ptr<TextWindow> window(
      new AndroidTextWindow(system_, text_window_number));
  text_window_[text_window_number] = window;
  return window;
}

Size AndroidTextSystem::RenderGlyphOnto(const std::string& /*current*/,
                                        int /*font_size*/,
                                        bool /*italic*/,
                                        const RGBColour& /*font_colour*/,
                                        const RGBColour* /*shadow_colour*/,
                                        int /*insertion_point_x*/,
                                        int /*insertion_point_y*/,
                                        const std::shared_ptr<Surface>& /*destination*/) {
  return Size(0, 0);
}

int AndroidTextSystem::GetCharWidth(int size, uint16_t codepoint) {
  // 占位宽度：等宽近似。真正的度量在接入 FreeType 后替换。
  return (codepoint < 0x80) ? size / 2 : size;
}

// ---------------------------------------------------------------------------
// AndroidTextWindow
// ---------------------------------------------------------------------------

AndroidTextWindow::AndroidTextWindow(System& system, int window_num)
    : TextWindow(system, window_num) {}

std::shared_ptr<Surface> AndroidTextWindow::GetTextSurface() {
  if (!text_surface_) {
    text_surface_ = system().graphics().BuildSurface(system().graphics().screen_size());
    if (text_surface_) text_surface_->Fill(RGBAColour(0, 0, 0, 0));
  }
  return text_surface_;
}

std::shared_ptr<Surface> AndroidTextWindow::GetNameSurface() {
  if (!name_surface_) {
    name_surface_ = system().graphics().BuildSurface(system().graphics().screen_size());
    if (name_surface_) name_surface_->Fill(RGBAColour(0, 0, 0, 0));
  }
  return name_surface_;
}

// 名字框与 ruby（注音）的绘制同样依赖字形光栅化，阶段 4 再实现。
void AndroidTextWindow::RenderNameInBox(const std::string& /*utf8str*/) {}

void AndroidTextWindow::DisplayRubyText(const std::string& /*utf8str*/) {}

// 选择肢的显示与点击命中属于触屏交互（T2.3 / T3.x）。
void AndroidTextWindow::AddSelectionItem(const std::string& /*utf8str*/,
                                         int /*selection_id*/) {}

void AndroidTextWindow::ClearWin() {
  if (text_surface_) text_surface_->Fill(RGBAColour(0, 0, 0, 0));
  if (name_surface_) name_surface_->Fill(RGBAColour(0, 0, 0, 0));
}

// ---------------------------------------------------------------------------
// AndroidSoundSystem（阶段 4 用 AAudio 实现，见 docs/DECISIONS.md D-004）
// ---------------------------------------------------------------------------

AndroidSoundSystem::AndroidSoundSystem(System& system) : SoundSystem(system) {}

int AndroidSoundSystem::BgmStatus() const { return 0; }
void AndroidSoundSystem::BgmPlay(const std::string& /*bgm_name*/, bool /*loop*/) {}
void AndroidSoundSystem::BgmPlay(const std::string& /*bgm_name*/, bool /*loop*/,
                                 int /*fade_in_ms*/) {}
void AndroidSoundSystem::BgmPlay(const std::string& /*bgm_name*/, bool /*loop*/,
                                 int /*fade_in_ms*/, int /*fade_out_ms*/) {}
void AndroidSoundSystem::BgmStop() {}
void AndroidSoundSystem::BgmPause() {}
void AndroidSoundSystem::BgmUnPause() {}
void AndroidSoundSystem::BgmFadeOut(int /*fade_out_ms*/) {}
std::string AndroidSoundSystem::GetBgmName() const { return std::string(); }
bool AndroidSoundSystem::BgmLooping() const { return false; }

void AndroidSoundSystem::WavPlay(const std::string& /*wav_file*/, bool /*loop*/) {}
void AndroidSoundSystem::WavPlay(const std::string& /*wav_file*/, bool /*loop*/,
                                 const int /*channel*/) {}
void AndroidSoundSystem::WavPlay(const std::string& /*wav_file*/, bool /*loop*/,
                                 const int /*channel*/, const int /*fadein_ms*/) {}
bool AndroidSoundSystem::WavPlaying(const int /*channel*/) { return false; }
void AndroidSoundSystem::WavStop(const int /*channel*/) {}
void AndroidSoundSystem::WavStopAll() {}
void AndroidSoundSystem::WavFadeOut(const int /*channel*/, const int /*fadetime*/) {}

void AndroidSoundSystem::PlaySe(const int /*se_num*/) {}
bool AndroidSoundSystem::HasSe(const int /*se_num*/) { return false; }

bool AndroidSoundSystem::KoePlaying() const { return false; }
void AndroidSoundSystem::KoeStop() {}
void AndroidSoundSystem::KoePlayImpl(int /*id*/) {}

// ---------------------------------------------------------------------------
// AndroidSystem
// ---------------------------------------------------------------------------

AndroidSystem::AndroidSystem(Gameexe& gameexe)
    : gameexe_(gameexe),
      graphics_(new AndroidGraphicsSystem(*this, gameexe)),
      event_system_(new AndroidEventSystem(gameexe)),
      text_system_(new AndroidTextSystem(*this, gameexe)),
      sound_system_(new AndroidSoundSystem(*this)) {}

AndroidSystem::~AndroidSystem() = default;

void AndroidSystem::Run(RLMachine& machine) {
  // 与上游 SDLSystem::Run 相同的顺序：事件 -> 文本 -> 声音 -> 图形 -> 平台。
  event_system_->ExecuteEventSystem(machine);
  text_system_->ExecuteTextSystem();
  sound_system_->ExecuteSoundSystem();
  graphics_->ExecuteGraphicsSystem(machine);
  if (platform())
    platform()->Run(machine);

  // 上游由 SDL 的视频事件驱动重绘；Android 侧没有这种事件源，
  // 因此每轮主循环主动合成一帧。GL 线程按自己的节奏取走最新的一帧。
  graphics_->Refresh(nullptr);
}

GraphicsSystem& AndroidSystem::graphics() { return *graphics_; }
EventSystem& AndroidSystem::event() { return *event_system_; }
Gameexe& AndroidSystem::gameexe() { return gameexe_; }
TextSystem& AndroidSystem::text() { return *text_system_; }
SoundSystem& AndroidSystem::sound() { return *sound_system_; }
