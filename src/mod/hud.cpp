#include "mod/hud.h"

#include <windows.h>

#include <cmath>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "common/log.h"
#include "mod/game.h"
#include "mod/patch.h"

namespace hud {
namespace {

// exe+29AE70: a method the game calls every frame to draw its debug texts (the frame counter of the "stats"
// command and the like, exe+29AF20). Its object holds three fonts, at +8, +0x10 and +0x18. A font's calls, as
// that code makes them (the 4A engine keeps X-Ray's CGameFont here):
//   vtbl+0x08  void set_colour(font*, u32 argb)
//   vtbl+0x18  void set_height(font*, float part_of_the_screen, int 0)
//   vtbl+0x48  void set_alignment(font*, int)                    0 = from the left
//   vtbl+0xC8  void out(font*, float x, float y, const char* format, ...)   x, y in -1..1, (-1, -1) = top left
//   vtbl+0xE8  void flush(font*, 0, 0)                           draws what was put out
//
// The text a font takes is not UTF-8 but the font's own codes: the game passes every string through its
// localisation first (exe+707770). For plain text that is exe+674C30, which takes the text byte by byte (ASCII
// only: any other byte becomes the "no such letter" code F8FF) and hands the wide string to
//   exe+66CFA0  void to_font(table*, str** out, const wchar_t* text)   out: a counted string, chars at +0x14,
//               the count at +8 (the caller lets it go)
// The table (exe+D01E90) is the language the game runs in: +8 = the letters its fonts have (u16 each), +0x12 =
// how many. A letter that is not among them is drawn as "?" here; with a game language that has no Cyrillic the
// mod's own lines are shown in English.
//
// The camera, in the exe's own data (found by the lab's "!scan": the place of the eyes and a projection with the
// picture's aspect and the game's 50.6 degrees): exe+D076F0 the camera's place (then its direction, top and right,
// 16 bytes each), exe+D07730 the view matrix, exe+D07770 the projection - both 4x4, rows, a point is a row vector
// (as Direct3D has it).
constexpr uintptr_t kViewMatrix = 0xD07730;
constexpr uintptr_t kProjectionMatrix = 0xD07770;
constexpr uintptr_t kDrawDebugTexts = 0x29AE70;
constexpr uintptr_t kToFont = 0x66CFA0;
constexpr uintptr_t kFontTable = 0xD01E90;
constexpr size_t kFonts[3] = {0x08, 0x10, 0x18};

using DrawFn = void (*)(uint8_t*);
DrawFn g_draw = nullptr;

struct Line {
  std::string text, english;
  uint64_t until = 0;
  std::string drawn;  // the text in the font's codes (made at the first draw)
};
std::mutex g_lock;
std::vector<Line> g_notices;

struct DrawnTag {
  Tag tag;
  std::string drawn;
};
std::vector<DrawnTag> g_tags;

struct LabLine {
  bool on = false;
  int font = 1;
  float x = -0.95f, y = -0.80f, height = 0.03f;
  uint32_t colour = 0xFFFFFFFF;
  bool raw = false;  // the text goes to the font as it is
  std::string text;
  std::string drawn;
} g_lab;
std::atomic<uint64_t> g_frames{0};
uint32_t g_tag_colour = 0xFFE8E0C8;
int g_tag_alignment = 2;  // the lab tries 0, 1, 2 ("!text 9 <alignment> 0 0 <colour> x")

template <typename Fn>
Fn method(void* object, size_t offset) {
  return *reinterpret_cast<Fn*>(*reinterpret_cast<uint8_t**>(object) + offset);
}

void put(void* font, float x, float y, float height, uint32_t colour, const char* text, int alignment = 0) {
  method<void (*)(void*, uint32_t)>(font, 0x08)(font, colour);
  method<void (*)(void*, float, int)>(font, 0x18)(font, height, 0);
  method<void (*)(void*, int)>(font, 0x48)(font, alignment);
  method<void (*)(void*, float, float, const char*, ...)>(font, 0xC8)(font, x, y, "%s", text);
}

bool has_letter(wchar_t c) {
  uint8_t* table = game::ref<uint8_t*>(kFontTable);
  if (!table) return false;
  const uint16_t* letters = *reinterpret_cast<const uint16_t**>(table + 8);
  unsigned count = *reinterpret_cast<const uint16_t*>(table + 0x12);
  if (!letters) return false;
  for (unsigned i = 0; i < count; ++i)
    if (letters[i] == c) return true;
  return false;
}

uint8_t* make_counted(const std::string& text) {
  void* table = game::ref<void*>(kFontTable);
  if (!table || text.empty()) return nullptr;
  wchar_t wide[512] = {};
  if (!MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide, 511)) return nullptr;
  for (wchar_t* c = wide; *c; ++c)
    if (!has_letter(*c)) *c = has_letter(L'?') ? L'?' : L' ';
  uint8_t* counted = nullptr;
  using Fn = void (*)(void*, uint8_t**, const wchar_t*);
  reinterpret_cast<Fn>(game::at(kToFont))(table, &counted, wide);
  return counted;
}

// "" when the game has no table (yet).
std::string to_font(const std::string& text) {
  uint8_t* counted = make_counted(text);
  if (!counted) return "";
  std::string out = reinterpret_cast<const char*>(counted + 0x14);
  InterlockedDecrement(reinterpret_cast<volatile LONG*>(counted + 8));
  return out;
}

// Where a place of the world is on the screen (-1..1, as the fonts count) and how far it is; false when it is
// behind the camera or off the picture.
bool on_screen(const Tag& t, float* x, float* y, float* distance) {
  const float* v = reinterpret_cast<const float*>(game::at(kViewMatrix));
  const float* p = reinterpret_cast<const float*>(game::at(kProjectionMatrix));
  float eye[4], clip[4];
  for (int c = 0; c < 4; ++c) eye[c] = t.x * v[c] + t.y * v[4 + c] + t.z * v[8 + c] + v[12 + c];
  for (int c = 0; c < 4; ++c) clip[c] = eye[0] * p[c] + eye[1] * p[4 + c] + eye[2] * p[8 + c] + eye[3] * p[12 + c];
  if (!(clip[3] > 0.3f)) return false;
  *x = clip[0] / clip[3];
  *y = -clip[1] / clip[3];
  *distance = clip[3];
  return *x > -1.1f && *x < 1.1f && *y > -1.1f && *y < 1.1f;
}

void flush(void* font) { method<void (*)(void*, int, int)>(font, 0xE8)(font, 0, 0); }

void on_draw(uint8_t* self) {
  g_draw(self);
  ++g_frames;
  if (!game::ref<void*>(game::kLevel) || !game::ref<void*>(kFontTable)) return;
  LabLine lab;
  std::vector<std::string> lines;
  std::vector<DrawnTag> tag_list;
  {
    std::lock_guard<std::mutex> hold(g_lock);
    for (DrawnTag& t : g_tags)
      if (t.drawn.empty()) t.drawn = to_font(t.tag.text);
    tag_list = g_tags;
    if (g_lab.on && g_lab.drawn.empty()) g_lab.drawn = g_lab.raw ? g_lab.text : to_font(g_lab.text);
    lab = g_lab;
    uint64_t now = GetTickCount64();
    for (size_t i = 0; i < g_notices.size();) {
      Line& n = g_notices[i];
      if (n.until <= now) {
        g_notices.erase(g_notices.begin() + i);
        continue;
      }
      if (n.drawn.empty()) n.drawn = to_font(has_letter(L'\u044F') ? n.text : n.english);
      lines.push_back(n.drawn);
      ++i;
    }
  }
  if (!lab.on && lines.empty() && tag_list.empty()) return;
  void* font = *reinterpret_cast<void**>(self + kFonts[1]);
  if (font && !tag_list.empty()) {
    bool any = false;
    for (const DrawnTag& t : tag_list) {
      float x, y, distance;
      if (t.drawn.empty() || !on_screen(t.tag, &x, &y, &distance) || distance > 80.f) continue;
      // nearer than 6 m the letters keep their size; further away they shrink a little, never below two thirds
      float height = distance < 6.f ? 0.034f : distance > 30.f ? 0.023f : 0.034f - 0.011f * (distance - 6.f) / 24.f;
      put(font, x, y - height, height, g_tag_colour, t.drawn.c_str(), g_tag_alignment);
      any = true;
    }
    if (any) flush(font);
  }
  if (lab.on) {
    void* f = *reinterpret_cast<void**>(self + kFonts[lab.font - 1]);
    if (f) {
      put(f, lab.x, lab.y, lab.height, lab.colour, lab.drawn.c_str());
      flush(f);
    }
  }
  if (font && !lines.empty()) {
    float y = -0.90f;
    for (const std::string& line : lines) {
      put(font, -0.95f, y, 0.04f, 0xFFE8E0C8, line.c_str());
      y += 0.085f;
    }
    flush(font);
  }
}

}  // namespace

uint8_t* counted_text(const std::string& text) { return make_counted(text); }
bool cyrillic() { return has_letter(L'\u044F'); }

void tags(std::vector<Tag> list) {
  std::lock_guard<std::mutex> hold(g_lock);
  std::vector<DrawnTag> next;
  next.reserve(list.size());
  for (Tag& t : list) {
    DrawnTag d;
    for (const DrawnTag& old : g_tags)  // the same name as a frame ago: its converted text is kept
      if (old.tag.text == t.text) d.drawn = old.drawn;
    d.tag = std::move(t);
    next.push_back(std::move(d));
  }
  g_tags.swap(next);
}

void notice(const std::string& text, const std::string& english, unsigned seconds) {
  LOGI("hud: %s", text.c_str());
  std::lock_guard<std::mutex> hold(g_lock);
  if (g_notices.size() >= 6) g_notices.erase(g_notices.begin());
  g_notices.push_back({text, english, GetTickCount64() + seconds * 1000ull, ""});
}

void lab_text(const char* args) {
  LabLine l;
  char text[256] = {};
  unsigned colour = 0xFFFFFFFF;
  int got = sscanf(args, "%d %f %f %f %x %255[^\n]", &l.font, &l.x, &l.y, &l.height, &colour, text);
  l.colour = colour;
  if (got == 6 && l.font == 9) {  // "!text 9 <alignment> 0 0 <colour> x": how the names over the heads are put
    g_tag_alignment = static_cast<int>(l.x);
    g_tag_colour = colour;
    LOGI("hud [lab]: names with alignment %d, colour %08X", g_tag_alignment, g_tag_colour);
    return;
  }
  if (got == 6 && l.font < 0) {  // a font number below zero: the text is not converted
    l.font = -l.font;
    l.raw = true;
  }
  if (got == 6 && l.font >= 1 && l.font <= 3) {
    for (const char* p = text; *p;) {  // "\xNN" = a byte
      if (p[0] == '\\' && p[1] == 'x' && isxdigit(static_cast<unsigned char>(p[2])) && isxdigit(static_cast<unsigned char>(p[3]))) {
        char hex[3] = {p[2], p[3], 0};
        l.text += static_cast<char>(strtoul(hex, nullptr, 16));
        p += 4;
      } else {
        l.text += *p++;
      }
    }
    l.on = true;
  }
  LOGI("hud [lab]: %s (the draw method ran %llu times so far)", l.on ? "a line is on" : "no line",
       static_cast<unsigned long long>(g_frames.load()));
  std::lock_guard<std::mutex> hold(g_lock);
  g_lab = l;
}

namespace {
void scan_floats(const char* what, uintptr_t where, const uint8_t* bytes, size_t size, const float* body, int* said) {
  const float* f = reinterpret_cast<const float*>(bytes);
  size_t n = size / 4;
  auto zero = [](float v) { return v == 0.f; };
  for (size_t i = 0; i + 16 <= n && *said < 120; ++i) {
    const float* m = f + i;
    if (m[0] > 0.3f && m[0] < 5.f && m[5] > 0.3f && m[5] < 5.f && zero(m[1]) && zero(m[2]) && zero(m[3]) && zero(m[4]) && zero(m[6]) &&
        zero(m[7]) && zero(m[8]) && zero(m[9]) && fabsf(fabsf(m[11]) - 1.f) < 1e-4f && zero(m[12]) && zero(m[13]) && zero(m[15])) {
      LOGI("hud [lab]: %s %llX + %zX: a projection? w %.4f h %.4f q %.5f near-term %.5f", what, static_cast<unsigned long long>(where), i * 4,
           m[0], m[5], m[10], m[14]);
      ++*said;
    }
    if (fabsf(m[0] - body[0]) < 0.4f && fabsf(m[2] - body[2]) < 0.4f && m[1] - body[1] > 0.8f && m[1] - body[1] < 2.2f) {
      LOGI("hud [lab]: %s %llX + %zX: the camera's place? (%.3f, %.3f, %.3f)", what, static_cast<unsigned long long>(where), i * 4, m[0],
           m[1], m[2]);
      ++*said;
    }
  }
}
}  // namespace

void lab_scan() {
  uint8_t* level = game::ref<uint8_t*>(game::kLevel);
  uint8_t* me = level ? *reinterpret_cast<uint8_t**>(level + game::kLevelControlled) : nullptr;
  if (!me) {
    LOGI("hud [lab]: scan: no body");
    return;
  }
  float body[3];
  memcpy(body, me + game::kEntityPosition, sizeof(body));
  LOGI("hud [lab]: scan; the body stands at (%.3f, %.3f, %.3f)", body[0], body[1], body[2]);
  int said = 0;
  // the exe's own data
  scan_floats("exe", 0xCF0000, reinterpret_cast<const uint8_t*>(game::at(0xCF0000)), 0x30000, body, &said);
  // the objects its global pointers lead to
  std::vector<uint8_t> buffer(0x4000);
  HANDLE self = GetCurrentProcess();
  for (uintptr_t g = 0xD01C00; g < 0xD02400; g += 8) {
    uint8_t* object = game::ref<uint8_t*>(g);
    if (!object || reinterpret_cast<uintptr_t>(object) < 0x10000) continue;
    SIZE_T got = 0;
    if (!ReadProcessMemory(self, object, buffer.data(), buffer.size(), &got) &&
        !ReadProcessMemory(self, object, buffer.data(), 0x1000 - (reinterpret_cast<uintptr_t>(object) & 0xFFF), &got))
      continue;
    scan_floats("object of global", g, buffer.data(), got, body, &said);
  }
  LOGI("hud [lab]: scan done, %d finds", said);
}

void install(const Config& cfg) {
  if (cfg.net_mode == Config::NetMode::Off && !cfg.lab) return;
  patch::hook("debug texts", game::at(kDrawDebugTexts), reinterpret_cast<void*>(&on_draw), reinterpret_cast<void**>(&g_draw));
}

}  // namespace hud
