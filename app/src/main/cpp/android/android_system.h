// Android 平台后端（T2.4）。
//
// 用 AndroidSystem 顶替上游硬编码在 RLVMInstance::Run() 里的 SDLSystem。
// 本阶段的目标是让引擎能在真机上完成**初始化并执行字节码**，因此四个子系统中：
//   - EventSystem：时间与休眠是真实实现（主循环需要它计时），鼠标/键盘暂时是桩；
//   - TextSystem：文字排版依赖 FreeType，属于阶段 4，当前是桩；
//   - SoundSystem：AAudio 后端属于阶段 4（T4.1），当前全部是桩。
// 这样内存类脚本可以完整执行，而渲染/音频/文字各自的实现留到对应阶段。

#ifndef RLVM_APP_SRC_MAIN_CPP_ANDROID_ANDROID_SYSTEM_H_
#define RLVM_APP_SRC_MAIN_CPP_ANDROID_ANDROID_SYSTEM_H_

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "systems/base/event_system.h"
#include "systems/base/sound_system.h"
#include "systems/base/system.h"
#include "systems/base/text_system.h"
#include "systems/base/text_window.h"

class AndroidGraphicsSystem;
class Gameexe;
class RLMachine;

class AndroidEventSystem : public EventSystem {
 public:
  explicit AndroidEventSystem(Gameexe& gexe);
  ~AndroidEventSystem() override = default;

  void ExecuteEventSystem(RLMachine& machine) override;
  unsigned int GetTicks() const override;
  void Wait(unsigned int milliseconds) const override;
  bool ShiftPressed() const override;
  bool CtrlPressed() const override;
  Point GetCursorPos() override;
  void GetCursorPos(Point& position, int& button1, int& button2) override;
  void FlushMouseClicks() override;
  unsigned int TimeOfLastMouseMove() override;
  void InjectMouseMovement(RLMachine& machine, const Point& loc) override;
  void InjectMouseDown(RLMachine& machine) override;
  void InjectMouseUp(RLMachine& machine) override;

  /**
   * 由 UI 线程投递一个触摸事件（T2.3）。
   *
   * action 取值与 native-bridge 的 kTouch* 常量一致：0=按下 1=移动 2=抬起。
   * 这里只入队，真正的注入发生在引擎线程的 ExecuteEventSystem 里：
   * 注入要广播给 EventListener（按钮对象、选择项等），必须在引擎线程上做；
   * 同时 UI 线程也绝不能被引擎的工作量阻塞。
   */
  void PostTouchEvent(int action, const Point& position);
  void PostTouchEvent(int action, const Point& position, int buttons);

 private:
  /** 按位掩码设置某个鼠标键状态并派发事件（1=左键 2=右键）。 */
  void ApplyButtonState(RLMachine& machine, int button, int state, int button_mask);

  struct PendingTouch {
    int action;
    Point position;
    int buttons;  // 位掩码：1=左键 2=右键
  };

  std::mutex queue_mutex_;
  std::vector<PendingTouch> pending_;
  Point mouse_pos_;
  int button1_state_ = 0;
  int button2_state_ = 0;
  unsigned int last_mouse_move_ticks_;
};

class AndroidTextSystem : public TextSystem {
 public:
  AndroidTextSystem(System& system, Gameexe& gexe);
  ~AndroidTextSystem() override = default;

  std::shared_ptr<TextWindow> GetTextWindow(int text_window_number) override;

  Size RenderGlyphOnto(const std::string& current,
                       int font_size,
                       bool italic,
                       const RGBColour& font_colour,
                       const RGBColour* shadow_colour,
                       int insertion_point_x,
                       int insertion_point_y,
                       const std::shared_ptr<Surface>& destination) override;

  int GetCharWidth(int size, uint16_t codepoint) override;
};

// 文字窗口。真正的字形光栅化要等 FreeType 接入（阶段 4），
// 但窗口对象本身必须存在——引擎的文本输出路径会解引用它，
// 早期返回 nullptr 曾导致真机上 TextPage::CharacterImpl 空指针崩溃。
class AndroidTextWindow : public TextWindow {
 public:
  AndroidTextWindow(System& system, int window_num);
  ~AndroidTextWindow() override = default;

  std::shared_ptr<Surface> GetTextSurface() override;
  std::shared_ptr<Surface> GetNameSurface() override;
  void SetFontColor(const std::vector<int>& colour_data) override;
  void RenderNameInBox(const std::string& utf8str) override;
  void DisplayRubyText(const std::string& utf8str) override;
  void AddSelectionItem(const std::string& utf8str, int selection_id) override;
  void ClearWin() override;

 private:
  std::shared_ptr<Surface> text_surface_;
  std::shared_ptr<Surface> name_surface_;
};

// AAudio 音频后端：把引擎的 BGM / WAV / SE 调用接到 AudioEngine 上。
//
// 通道映射：RLVM 的通道 0..23 用于 WAV/SE、24 是 KOE 语音，直接一对一映射到
// AudioEngine 的通道；BGM 单独占用一个引擎通道（对应上游用 Mix_HookMusic 的
// 独立音乐流）。
//
// 尚未实现：KOE 语音——需要先把 KOE/NWK/OVK 语音包的解码链路接上。
class AndroidSoundSystem : public SoundSystem {
 public:
  explicit AndroidSoundSystem(System& system);
  ~AndroidSoundSystem() override = default;

  int BgmStatus() const override;
  void BgmPlay(const std::string& bgm_name, bool loop) override;
  void BgmPlay(const std::string& bgm_name, bool loop, int fade_in_ms) override;
  void BgmPlay(const std::string& bgm_name,
               bool loop,
               int fade_in_ms,
               int fade_out_ms) override;
  void BgmStop() override;
  void BgmPause() override;
  void BgmUnPause() override;
  void BgmFadeOut(int fade_out_ms) override;
  std::string GetBgmName() const override;
  bool BgmLooping() const override;

  void WavPlay(const std::string& wav_file, bool loop) override;
  void WavPlay(const std::string& wav_file, bool loop, const int channel) override;
  void WavPlay(const std::string& wav_file,
               bool loop,
               const int channel,
               const int fadein_ms) override;
  bool WavPlaying(const int channel) override;
  void WavStop(const int channel) override;
  void WavStopAll() override;
  void WavFadeOut(const int channel, const int fadetime) override;

  void PlaySe(const int se_num) override;
  bool HasSe(const int se_num) override;

  void SetChannelVolume(const int channel, const int level) override;
  void SetBgmVolumeScript(const int level, const int fade_in_ms) override;
  void SetBgmVolumeMod(const int in) override;
  void SetPcmVolumeMod(const int in) override;
  void SetSeVolumeMod(const int in) override;

  bool KoePlaying() const override;
  void KoeStop() override;

 protected:
  void KoePlayImpl(int id) override;

 private:
  // 经游戏文件系统定位并播放；找不到文件或解码失败时安静返回。
  void PlayOnChannel(int engine_channel,
                     const std::string& file_name,
                     bool loop,
                     int volume);
  int CurrentBgmVolume();
  void ApplyChannelVolume(int channel);

  std::string bgm_name_;
  bool bgm_looping_ = false;
  bool bgm_paused_ = false;
};

class AndroidSystem : public System {
 public:
  explicit AndroidSystem(Gameexe& gameexe);
  ~AndroidSystem() override;

  void Run(RLMachine& machine) override;
  GraphicsSystem& graphics() override;
  EventSystem& event() override;
  Gameexe& gameexe() override;
  TextSystem& text() override;
  SoundSystem& sound() override;

 private:
  // 注意：必须声明在四个子系统之前。C++ 的成员初始化顺序由**声明顺序**决定，
  // 而子系统的构造函数会经由 System::gameexe() 取这个引用；
  // 若声明在后面，构造子系统时它尚未初始化（曾因此在真机上 SIGSEGV）。
  Gameexe& gameexe_;

  std::unique_ptr<AndroidGraphicsSystem> graphics_;
  std::unique_ptr<AndroidEventSystem> event_system_;
  std::unique_ptr<AndroidTextSystem> text_system_;
  std::unique_ptr<AndroidSoundSystem> sound_system_;
};

#endif  // RLVM_APP_SRC_MAIN_CPP_ANDROID_ANDROID_SYSTEM_H_
