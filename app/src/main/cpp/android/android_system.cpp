#include "android/android_system.h"

#include <android/log.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

#include "android/android_graphics.h"
#include "android/audio_engine.h"
#include "android/font_engine.h"
#include "android/game_file_system.h"
#include "utf8cpp/utf8.h"
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

/** 取 UTF-8 串的第一个码点。 */
bool FirstCodepoint(const std::string& text, uint32_t& codepoint) {
  if (text.empty()) return false;
  std::string::const_iterator it = text.begin();
  const std::string::const_iterator end = text.end();
  try {
    codepoint = utf8::next(it, end);
  } catch (...) {
    return false;
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// AndroidEventSystem
// ---------------------------------------------------------------------------

AndroidEventSystem::AndroidEventSystem(Gameexe& gexe)
    : EventSystem(gexe),
      mouse_pos_(Point(0, 0)),
      last_mouse_move_ticks_(0) {}

void AndroidEventSystem::PostTouchEvent(int action, const Point& position) {
  PostTouchEvent(action, position, 1);
}

void AndroidEventSystem::PostTouchEvent(int action,
                                        const Point& position,
                                        int buttons) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  // 引擎没在跑（或已经停止）时事件会堆积；留一个上限，丢掉最旧的。
  constexpr size_t kMaxPending = 64;
  if (pending_.size() >= kMaxPending) pending_.erase(pending_.begin());
  pending_.push_back(PendingTouch{action, position, buttons});
}

/** 按位掩码设置某个鼠标键的状态，并派发事件（语义与上游 SDL 后端一致）。 */
void AndroidEventSystem::ApplyButtonState(RLMachine& machine,
                                          int button,
                                          int state,
                                          int button_mask) {
  if ((button_mask & (button == 1 ? 1 : 2)) == 0) return;
  if (button == 1) {
    button1_state_ = state;
  } else {
    button2_state_ = state;
  }
  DispatchEvent(machine,
                std::bind(&EventListener::MouseButtonStateChanged,
                          std::placeholders::_1,
                          button == 1 ? MOUSE_LEFT : MOUSE_RIGHT, 1));
}

void AndroidEventSystem::ExecuteEventSystem(RLMachine& machine) {
  // 把 UI 线程投递的触摸事件注入引擎。
  //
  // 语义与上游 SDL 后端一致（sdl_event_system.cc）：
  //   InjectMouseDown  -> button1 = 1（按住）
  //   InjectMouseUp    -> button1 = 2（按下并抬起，脚本据此判定「点击完成」）
  //   FlushMouseClicks -> 清零（脚本用 FlushClick 清除已消费的点击）
  // 本作标题菜单就是靠 GetCursorPos 的第 3 个返回值 == 2 来判断点击的。
  std::vector<PendingTouch> events;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    events.swap(pending_);
  }

  for (size_t i = 0; i < events.size(); ++i) {
    const PendingTouch& event = events[i];

    // 同一批里「按下」紧接着「抬起」时，把抬起推迟到下一帧：
    // RealLive 脚本经常先看 button == 1（按住）再看 == 2（已松开），
    // 一帧内合并掉会让它永远看不到「按住」这个状态。
    if (event.action == 2 && i > 0 && events[i - 1].action == 0) {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      pending_.insert(pending_.begin(), events.begin() + i, events.end());
      break;
    }

    // 位置先行：按钮按下/抬起都发生在某个坐标上，事件回调需要一致的鼠标位置。
    InjectMouseMovement(machine, event.position);
    if (event.action == 0) {
      ApplyButtonState(machine, 1, 1, event.buttons);
      ApplyButtonState(machine, 2, 1, event.buttons);
    } else if (event.action == 2) {
      ApplyButtonState(machine, 1, 2, event.buttons);
      ApplyButtonState(machine, 2, 2, event.buttons);
    }
    __android_log_print(ANDROID_LOG_INFO, "rlvm-input",
                        "touch action=%d at %d,%d buttons=%d (b1=%d b2=%d)",
                        event.action, event.position.x(), event.position.y(),
                        event.buttons, button1_state_, button2_state_);
  }
}

unsigned int AndroidEventSystem::GetTicks() const { return NowMillis(); }

void AndroidEventSystem::Wait(unsigned int milliseconds) const {
  std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

bool AndroidEventSystem::ShiftPressed() const { return false; }

bool AndroidEventSystem::CtrlPressed() const { return false; }

Point AndroidEventSystem::GetCursorPos() { return mouse_pos_; }

void AndroidEventSystem::GetCursorPos(Point& position, int& button1, int& button2) {
  position = mouse_pos_;
  button1 = button1_state_;
  button2 = button2_state_;
}

void AndroidEventSystem::FlushMouseClicks() {
  button1_state_ = 0;
  button2_state_ = 0;
}

unsigned int AndroidEventSystem::TimeOfLastMouseMove() {
  return last_mouse_move_ticks_;
}

void AndroidEventSystem::InjectMouseMovement(RLMachine& machine, const Point& loc) {
  mouse_pos_ = loc;
  last_mouse_move_ticks_ = NowMillis();
  BroadcastEvent(machine,
                 std::bind(&EventListener::MouseMotion, std::placeholders::_1, loc));
}

void AndroidEventSystem::InjectMouseDown(RLMachine& machine) {
  button1_state_ = 1;
  button2_state_ = 0;
  DispatchEvent(machine,
                std::bind(&EventListener::MouseButtonStateChanged,
                          std::placeholders::_1, MOUSE_LEFT, 1));
}

void AndroidEventSystem::InjectMouseUp(RLMachine& machine) {
  button1_state_ = 2;
  button2_state_ = 0;
  DispatchEvent(machine,
                std::bind(&EventListener::MouseButtonStateChanged,
                          std::placeholders::_1, MOUSE_LEFT, 1));
}

// ---------------------------------------------------------------------------
// AndroidTextSystem
// ---------------------------------------------------------------------------

AndroidTextSystem::AndroidTextSystem(System& system, Gameexe& gexe)
    : TextSystem(system, gexe) {}

std::shared_ptr<TextWindow> AndroidTextSystem::GetTextWindow(
    int text_window_number) {
  static bool logged = false;
  if (!logged) {
    logged = true;
    __android_log_print(ANDROID_LOG_INFO, "rlvm-font",
                        "GetTextWindow called (first window=%d)",
                        text_window_number);
  }
  WindowMap::iterator it = text_window_.find(text_window_number);
  if (it != text_window_.end()) return it->second;

  std::shared_ptr<TextWindow> window(
      new AndroidTextWindow(system_, text_window_number));
  text_window_[text_window_number] = window;
  return window;
}

Size AndroidTextSystem::RenderGlyphOnto(const std::string& current,
                                        int font_size,
                                        bool italic,
                                        const RGBColour& font_colour,
                                        const RGBColour* shadow_colour,
                                        int insertion_point_x,
                                        int insertion_point_y,
                                        const std::shared_ptr<Surface>& destination) {
  // current 是 UTF-8 字符串（通常一个字符）。取第一个码点交给字体引擎。
  uint32_t codepoint = 0;
  if (!FirstCodepoint(current, codepoint)) return Size(0, 0);

  static int rendered = 0;
  if (++rendered <= 5) {
    __android_log_print(ANDROID_LOG_INFO, "rlvm-font",
                        "RenderGlyphOnto #%d codepoint=U+%04X size=%d at (%d,%d)",
                        rendered, codepoint, font_size, insertion_point_x,
                        insertion_point_y);
  }

  rlvm_android::FontEngine& fonts = rlvm_android::FontEngine::Instance();
  // 上游只用了斜体（TTF_STYLE_ITALIC），不合成加粗，这里保持一致。
  const rlvm_android::GlyphBitmap* glyph =
      fonts.Rasterize(codepoint, font_size, italic, false);
  if (glyph == nullptr) return Size(0, 0);

  AndroidSurface* target = dynamic_cast<AndroidSurface*>(destination.get());
  if (target == nullptr) return Size(0, 0);

  // 与上游 SDLTextSystem 相同的做法：阴影先画在 (+2, +2)，再把字形画在原点。
  if (shadow_colour != nullptr && font_shadow() != 0) {
    target->BlendCoverage(glyph->coverage.data(), glyph->width, glyph->height,
                          insertion_point_x + 2, insertion_point_y + 2,
                          *shadow_colour);
  }
  target->BlendCoverage(glyph->coverage.data(), glyph->width, glyph->height,
                        insertion_point_x, insertion_point_y, font_colour);

  return Size(glyph->width, glyph->height);
}

int AndroidTextSystem::GetCharWidth(int size, uint16_t codepoint) {
  static int measured = 0;
  if (++measured <= 5) {
    __android_log_print(ANDROID_LOG_INFO, "rlvm-font",
                        "GetCharWidth #%d codepoint=U+%04X size=%d", measured,
                        codepoint, size);
  }
  return rlvm_android::FontEngine::Instance().Advance(codepoint, size);
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
// AndroidSoundSystem（AAudio，见 docs/DECISIONS.md D-004）
// ---------------------------------------------------------------------------

namespace {

constexpr char kAudioTag[] = "rlvm-audio";

// BGM 单独占用一个引擎通道：对应上游用 Mix_HookMusic 维护的独立音乐流。
// RLVM 自己的通道 0..23（WAV/SE）与 24（KOE）直接一对一映射。
constexpr int kBgmEngineChannel = 30;

/** 从文件标识里取小写扩展名，用于选择解码器。 */
std::string ExtensionOf(const std::string& file_id) {
  const size_t dot = file_id.find_last_of('.');
  if (dot == std::string::npos || dot + 1 >= file_id.size()) return std::string();
  std::string extension = file_id.substr(dot + 1);
  for (char& c : extension) c = static_cast<char>(tolower(c));
  return extension;
}

}  // namespace

AndroidSoundSystem::AndroidSoundSystem(System& system) : SoundSystem(system) {}

int AndroidSoundSystem::CurrentBgmVolume() {
  return compute_channel_volume(bgm_volume_script(), bgm_volume_mod());
}

void AndroidSoundSystem::ApplyChannelVolume(int channel) {
  const int system_volume = (channel == kBgmEngineChannel) ? bgm_volume_mod()
                                                           : pcm_volume_mod();
  rlvm_android::AudioEngine::Instance().SetVolume(
      channel, compute_channel_volume(GetChannelVolume(channel), system_volume));
}

void AndroidSoundSystem::PlayOnChannel(int engine_channel,
                                       const std::string& file_name,
                                       bool loop,
                                       int volume) {
  // 与图像加载同样的路径：先经查找层定位（SAF 下是不透明标识），再用 fd 读取。
  const boost::filesystem::path file_id =
      system().FindFile(file_name, SOUND_FILETYPES);
  if (file_id.empty()) {
    __android_log_print(ANDROID_LOG_WARN, kAudioTag, "audio file not found: %s",
                        file_name.c_str());
    return;
  }

  std::shared_ptr<rlvm_android::GameFileSystem> files =
      rlvm_android::GetGameFileSystem();
  if (!files) return;

  rlvm_android::AudioEngine& audio = rlvm_android::AudioEngine::Instance();
  if (!audio.Start()) {
    __android_log_print(ANDROID_LOG_ERROR, kAudioTag, "audio start failed: %s",
                        audio.LastError().c_str());
    return;
  }

  const int fd = files->OpenFd(file_id.string());
  if (fd < 0) return;

  // OpenSource 接管 fd 的所有权（fdopen），失败时由它负责关闭。
  std::unique_ptr<rlvm_android::AudioSource> source =
      audio.OpenSource(fd, ExtensionOf(file_id.string()));
  if (!source) {
    __android_log_print(ANDROID_LOG_WARN, kAudioTag, "audio decode failed: %s",
                        file_id.string().c_str());
    return;
  }

  __android_log_print(ANDROID_LOG_INFO, kAudioTag,
                      "play channel=%d file=%s loop=%d volume=%d", engine_channel,
                      file_id.string().c_str(), loop ? 1 : 0, volume);
  audio.Play(engine_channel, std::move(source), loop,
             std::max(0, std::min(255, volume)));
}

// -- BGM --------------------------------------------------------------------

int AndroidSoundSystem::BgmStatus() const {
  return rlvm_android::AudioEngine::Instance().IsPlaying(kBgmEngineChannel) ? 1 : 0;
}

void AndroidSoundSystem::BgmPlay(const std::string& bgm_name, bool loop) {
  BgmPlay(bgm_name, loop, 0, 0);
}

void AndroidSoundSystem::BgmPlay(const std::string& bgm_name,
                                 bool loop,
                                 int fade_in_ms) {
  BgmPlay(bgm_name, loop, fade_in_ms, 0);
}

void AndroidSoundSystem::BgmPlay(const std::string& bgm_name,
                                 bool loop,
                                 int fade_in_ms,
                                 int /*fade_out_ms*/) {
  rlvm_android::AudioEngine& audio = rlvm_android::AudioEngine::Instance();
  audio.Stop(kBgmEngineChannel);

  bgm_name_ = bgm_name;
  bgm_looping_ = loop;
  bgm_paused_ = false;

  // 淡入：先以 0 音量开始，再渐变到目标音量。
  PlayOnChannel(kBgmEngineChannel, bgm_name, loop,
                fade_in_ms > 0 ? 0 : CurrentBgmVolume());
  if (fade_in_ms > 0) {
    audio.FadeVolume(kBgmEngineChannel, CurrentBgmVolume(), fade_in_ms);
  }
}

void AndroidSoundSystem::BgmStop() {
  rlvm_android::AudioEngine::Instance().Stop(kBgmEngineChannel);
  bgm_name_.clear();
  bgm_looping_ = false;
  bgm_paused_ = false;
}

void AndroidSoundSystem::BgmPause() {
  // 引擎没有真正的暂停：把音量降到 0。流继续解码，所以恢复是无缝的。
  rlvm_android::AudioEngine::Instance().SetVolume(kBgmEngineChannel, 0);
  bgm_paused_ = true;
}

void AndroidSoundSystem::BgmUnPause() {
  if (!bgm_paused_) return;
  rlvm_android::AudioEngine::Instance().SetVolume(kBgmEngineChannel,
                                                  CurrentBgmVolume());
  bgm_paused_ = false;
}

void AndroidSoundSystem::BgmFadeOut(int fade_out_ms) {
  rlvm_android::AudioEngine::Instance().FadeVolume(kBgmEngineChannel, 0,
                                                   fade_out_ms);
}

std::string AndroidSoundSystem::GetBgmName() const { return bgm_name_; }

bool AndroidSoundSystem::BgmLooping() const { return bgm_looping_; }

void AndroidSoundSystem::SetBgmVolumeScript(const int level, const int fade_in_ms) {
  SoundSystem::SetBgmVolumeScript(level, fade_in_ms);
  rlvm_android::AudioEngine::Instance().FadeVolume(kBgmEngineChannel,
                                                   CurrentBgmVolume(), fade_in_ms);
}

void AndroidSoundSystem::SetBgmVolumeMod(const int in) {
  SoundSystem::SetBgmVolumeMod(in);
  ApplyChannelVolume(kBgmEngineChannel);
}

// -- WAV / SE ---------------------------------------------------------------

void AndroidSoundSystem::WavPlay(const std::string& wav_file, bool loop) {
  WavPlay(wav_file, loop, 0, 0);
}

void AndroidSoundSystem::WavPlay(const std::string& wav_file,
                                 bool loop,
                                 const int channel) {
  WavPlay(wav_file, loop, channel, 0);
}

void AndroidSoundSystem::WavPlay(const std::string& wav_file,
                                 bool loop,
                                 const int channel,
                                 const int fadein_ms) {
  if (channel < 0 || channel >= KOE_CHANNEL) return;
  const int volume = compute_channel_volume(GetChannelVolume(channel),
                                            pcm_volume_mod());
  PlayOnChannel(channel, wav_file, loop, fadein_ms > 0 ? 0 : volume);
  if (fadein_ms > 0) {
    rlvm_android::AudioEngine::Instance().FadeVolume(channel, volume, fadein_ms);
  }
}

bool AndroidSoundSystem::WavPlaying(const int channel) {
  return rlvm_android::AudioEngine::Instance().IsPlaying(channel);
}

void AndroidSoundSystem::WavStop(const int channel) {
  rlvm_android::AudioEngine::Instance().Stop(channel);
}

void AndroidSoundSystem::WavStopAll() {
  for (int channel = 0; channel < KOE_CHANNEL; ++channel) {
    rlvm_android::AudioEngine::Instance().Stop(channel);
  }
}

void AndroidSoundSystem::WavFadeOut(const int channel, const int fadetime) {
  rlvm_android::AudioEngine::Instance().FadeVolume(channel, 0, fadetime);
}

void AndroidSoundSystem::PlaySe(const int se_num) {
  SeTable::const_iterator entry = se_table().find(se_num);
  if (entry == se_table().end()) return;
  const int channel = entry->second.second;
  if (channel < 0 || channel >= KOE_CHANNEL) return;
  PlayOnChannel(channel, entry->second.first, false,
                compute_channel_volume(GetChannelVolume(channel), se_volume_mod()));
}

bool AndroidSoundSystem::HasSe(const int se_num) {
  return se_table().find(se_num) != se_table().end();
}

void AndroidSoundSystem::SetChannelVolume(const int channel, const int level) {
  SoundSystem::SetChannelVolume(channel, level);
  ApplyChannelVolume(channel);
}

void AndroidSoundSystem::SetPcmVolumeMod(const int in) {
  SoundSystem::SetPcmVolumeMod(in);
  for (int channel = 0; channel < KOE_CHANNEL; ++channel) ApplyChannelVolume(channel);
}

void AndroidSoundSystem::SetSeVolumeMod(const int in) {
  SoundSystem::SetSeVolumeMod(in);
}

// -- 语音（尚未实现）--------------------------------------------------------

bool AndroidSoundSystem::KoePlaying() const { return false; }

void AndroidSoundSystem::KoeStop() {
  rlvm_android::AudioEngine::Instance().Stop(KOE_CHANNEL);
}

void AndroidSoundSystem::KoePlayImpl(int /*id*/) {
  // 语音解码要先把 KOE / NWK / OVK 语音包链路接上（voice_cache 目前仍走
  // 上游基于路径的读取）。这里先记录，避免静默。
  __android_log_print(ANDROID_LOG_INFO, kAudioTag, "koe playback not implemented");
}

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
