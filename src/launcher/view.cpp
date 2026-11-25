// The launcher's picture: every page, drawn from the model (a 960 x 600 window). It is made to read as
// a part of Metro 2033 Redux's own interface: black, a fragment of a metro map faded into it (thin pale
// lines, ring stations, names in bold condensed capitals with a small second line), flat charcoal panels
// with rows "name - value", plain off-white text, typewriter capitals for headings, and one accent, the
// game's orange: the station that is open, the main action, a value that was changed. The launcher's pages
// are the stations of one line of that map. Along the very bottom runs a ragged orange glow, as on the
// game's loading screen. Nothing else: no frames, no ornaments, a state is said in words.
// Everything is drawn here, with Windows' own fonts; no picture comes from anywhere.
#include <algorithm>
#include <cmath>

#include "launcher/gamecfg.h"
#include "launcher/launcher.h"
#include "launcher/modset.h"

namespace lz {
namespace view {
namespace {

using gfx::Canvas;
using gfx::Font;
using gfx::Matrix;
using gfx::Stop;
using gfx::TextOpt;

const uint32_t BG = 0x0A0A0A, PANEL = 0x141413, BAR = 0x1F1E1C, TEXT = 0xE6E2D8, GREY = 0x8C8880, DIM = 0x56534D, ORANGE = 0xF0641E,
               ORANGE_HOT = 0xFF7E3A, ORANGE_LOW = 0xC94F14, BLACK = 0x000000;

constexpr float kNavX = 72;                  // the line the stations are on
constexpr float kNavTop = 138, kNavStep = 62;
constexpr float kPageX = 300, kPageW = 620;  // a page's column
constexpr float kPageTop = 88;
constexpr float kRow = 34;                   // a row of a panel

Font Ty(float size, float spacing = 0.24f) { return {gfx::Typed, size, 700, spacing}; }                         // typewriter capitals
Font D(float size, float spacing = 0.02f, int weight = 700) { return {gfx::Display, size, weight, spacing}; }   // a station's name
Font Sm(float size, float spacing = 0.12f, int weight = 700) { return {gfx::Label, size, weight, spacing}; }    // small capitals
Font T(float size, int weight = 400) { return {gfx::Text, size, weight, 0}; }                                   // what is read
Font Mo(float size) { return {gfx::Mono, size, 400, 0}; }

// glyphs, in a 24 x 24 box
const char kMin[] = "M5 17h14";
const char kClose[] = "M6 6l12 12M18 6L6 18";
const char kPrev[] = "M14.5 6l-6 6 6 6";
const char kNext[] = "M9.5 6l6 6-6 6";
const char kBackArrow[] = "M5 9a7.5 7.5 0 1 1 2.2 9.8M5 4.5V9h4.5";  // "as it was": an arrow that turns back

struct Ctx {
  Canvas& c;
  const Model& m;
  std::vector<Hit>* hits;
};

void hit(Ctx& x, int id, float l, float t, float w, float h) {
  if (!x.hits) return;
  Matrix inv = x.c.current();
  if (!inv.Invert()) return;
  x.hits->push_back({id, D2D1::RectF(l, t, l + w, t + h), inv});
}

float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

std::wstring megabytes(uint64_t bytes) {
  wchar_t buf[32];
  swprintf(buf, 32, L"%.1f", bytes / 1000000.0);
  for (wchar_t* p = buf; *p; ++p)
    if (*p == L'.') *p = L',';
  return buf;
}

// --- what the ИГРА page says ---------------------------------------------------------------------------

struct Vals {
  std::wstring state, hint;      // one line of where things stand; under it, quietly, what to do
  std::wstring primary, secondary;
  bool primary_on = true;
};

// What there is to install: GitHub's version when it is newer than the one inside this launcher and its
// file can be had (it is downloaded then), else the one inside.
bool from_github(const Model& m) {
  return m.rel.known && !m.assets_blocked && (!m.pack.known || core::compare_versions(m.rel.version, m.pack.version) > 0);
}
const Release& offer(const Model& m) { return from_github(m) ? m.rel : m.pack; }

Vals vals(const Model& m) {
  Vals v;
  const Release& t = offer(m);
  const std::wstring have = m.inst.version;
  const mset::Values& net = mset::get();
  const bool ini = m.inst.present && mset::has_ini();
  // the mod, then how the game together goes
  const std::wstring mod = have.empty() ? std::wstring(L"Мод установлен") : L"Мод " + have + L" установлен";
  std::wstring way, way_hint;
  if (ini && net.mode == mset::Mode::Off) {
    way = L"совместная игра выключена";
    way_hint = L"Включается на станции СЕТЬ.";
  } else if (ini && net.mode == mset::Mode::Host) {
    way = L"игра по IP-адресу: вы хост";
  } else if (ini && net.mode == mset::Mode::Join && !net.address.empty() && net.address != L"steam") {
    way = L"вход по IP-адресу " + net.address;
  } else if (m.steam) {
    way = L"Steam запущен";
  } else {
    way = L"Steam не запущен";
    way_hint = L"Совместная игра идёт через Steam: запустите его перед игрой.";
  }
  switch (m.screen) {
    case Screen::Checking:
      v.state = mod + L" · " + way;
      v.hint = L"Проверяю обновления. Играть можно сразу.";
      v.primary = L"ИГРАТЬ";
      v.primary_on = m.inst.present;
      break;
    case Screen::Update:
      v.state = (have.empty() ? std::wstring(L"Мод установлен, версия неизвестна") : L"Установлен мод " + have) + L" · есть версия " + t.version;
      v.hint = from_github(m) ? L"У всех игроков должна стоять одна версия мода: обновитесь вместе с другом."
                              : L"Версия " + t.version + L" уже внутри лаунчера: скачивать ничего не нужно.";
      v.primary = L"ОБНОВИТЬ ДО " + t.version;
      v.secondary = have.empty() ? std::wstring(L"ИГРАТЬ") : L"ИГРАТЬ НА " + have;
      break;
    case Screen::Install:
      v.state = L"Мод не установлен · " + std::wstring(from_github(m) ? L"на GitHub версия " : L"в лаунчере версия ") + t.version;
      v.hint = L"Файлы игры не меняются: мод кладётся рядом с metro.exe и убирается на станции ЛАУНЧЕР.";
      v.primary = L"УСТАНОВИТЬ МОД";
      break;
    case Screen::Downloading:
      break;
    case Screen::Ready:
      v.state = mod + L" · " + way;
      v.hint = way_hint;
      v.primary = m.launching ? L"ИГРА ЗАПУСКАЕТСЯ" : L"ИГРАТЬ";
      break;
    case Screen::Offline:
      v.state = mod + L" · " + way;
      v.hint = !way_hint.empty() ? way_hint
               : m.assets_blocked ? L"GitHub отвечает, но файлы не отдаёт: обновления не проверить. Играть можно."
                                  : L"GitHub не отвечает: обновления не проверить. Играть можно.";
      v.primary = L"ИГРАТЬ";
      break;
    case Screen::NoGame:
      v.state = L"Игра не найдена";
      v.hint = L"Укажите папку Metro 2033 Redux из GOG — ту, где лежит metro.exe.";
      v.primary = L"ВЫБРАТЬ ПАПКУ ИГРЫ";
      break;
    case Screen::WrongBuild:
      v.state = L"В этой папке другая сборка игры";
      v.hint = L"Мод сделан для GOG-версии Metro 2033 Redux (metro.exe от сентября 2014). С другим metro.exe он не заработает, и "
               L"лаунчер его сюда не ставит.";
      v.primary = L"ВЫБРАТЬ ДРУГУЮ ПАПКУ";
      break;
    case Screen::Running:
      v.state = L"Игра сейчас открыта";
      v.hint = m.pending == Pending::Uninstall ? L"Пока она открыта, мод не убрать. Закройте игру — удаление начнётся само."
                                               : L"Версия " + t.version + L" готова. Закройте игру — установка начнётся сама.";
      v.primary = m.pending == Pending::Uninstall ? L"ЗАКРЫТЬ ИГРУ И УДАЛИТЬ МОД" : L"ЗАКРЫТЬ ИГРУ И ОБНОВИТЬ";
      v.secondary = L"ПОЗЖЕ";
      break;
    case Screen::Problem:
    default:
      v.state = m.problem_title;
      v.hint = m.problem_text;
      v.primary = m.problem_primary;
      v.secondary = m.problem_secondary;
      break;
  }
  return v;
}

// --- the map behind everything: what never changes ----------------------------------------------------

unsigned mix_bits(unsigned h) {
  h = (h ^ (h >> 13)) * 0x5BD1E995u;
  return h ^ (h >> 15);
}

// A station of the faded map: a ring, its name, its second line.
void map_station(Canvas& c, float x, float y, const wchar_t* name, const wchar_t* latin, float a) {
  c.ellipse(x, y, 9.5f, 9.5f, TEXT, a * 1.25f);
  c.ellipse(x, y, 5.2f, 5.2f, BG);
  if (!name) return;
  TextOpt box;
  box.line_h = 22;
  c.text(name, D(21, 0.02f), x + 20, y - 15, TEXT, box, a);
  c.text(latin, Sm(8.5f, 0.10f), x + 20.5f, y + 7, TEXT, {}, a * 0.8f);
}

// The glow along the very bottom, as on the game's loading screen: a low uneven light that the dark breaks
// into, brightest at the very edge.
void embers(Canvas& c) {
  const float top = kH - 34.0f;
  const Stop haze[4] = {{0, 0x8A2E08, 0}, {0.40f, 0x8A2E08, 0.08f}, {0.80f, 0xB8420C, 0.26f}, {1, ORANGE, 0.50f}};
  c.gradient(0, top, kW, 34, true, haze, 4);
  c.noise(0, top, kW, 34, gfx::Blotch, BLACK, 0.95f, 1.9f);  // the dark breaks into it from above
  // the edge itself: a thin ragged line of fire
  const int n = kW / 4 + 2;
  std::vector<D2D1_POINT_2F> edge;
  auto height = [](int i) {  // long swells, a little roughness on them
    auto at = [](int k) { return (mix_bits(static_cast<unsigned>(k) * 2654435761u + 99) & 1023) / 1023.0f; };
    const int cell = i / 14;
    float t = (i % 14) / 14.0f;
    t = t * t * (3 - 2 * t);
    const float slow = at(cell) * (1 - t) + at(cell + 1) * t, rough = at(i * 7 + 3);
    return 0.8f + slow * slow * 5.2f + rough * 1.1f;
  };
  edge.push_back(D2D1::Point2F(0, kH + 1.0f));
  for (int i = 0; i < n; ++i) edge.push_back(D2D1::Point2F(i * 4.0f, kH - height(i)));
  edge.push_back(D2D1::Point2F(kW + 4.0f, kH + 1.0f));
  c.poly(edge.data(), static_cast<int>(edge.size()), ORANGE, 0.95f);
  c.fill(0, kH - 1.2f, kW, 1.2f, 0xFFB060, 0.50f);
}

// full: with the other line of the map and its stations at the right (the ИГРА page has room for it; a page
// of settings covers that place with its panels).
void background(Canvas& c, bool full) {
  c.fill(0, 0, kW, kH, BG);
  // the line the launcher's stations are on, out of the dark above and into it below
  const Stop line[4] = {{0, TEXT, 0}, {0.12f, TEXT, 0.20f}, {0.80f, TEXT, 0.20f}, {1, TEXT, 0.05f}};
  c.gradient(kNavX - 1.5f, 52, 3, kH - 52.0f, true, line, 4);
  if (full) {
    // a line that leaves it below the last station and runs off to the right
    c.stroke_path("M72 528a34 34 0 0 0 34 34H960", 2.2f, TEXT, false, 0.10f);
    for (float sx : {430.0f, 668.0f}) {
      c.ellipse(sx, 562, 7, 7, TEXT, 0.13f);
      c.ellipse(sx, 562, 3.8f, 3.8f, BG);
    }
    // another line of the map at the right, with its stations
    const Stop far_line[4] = {{0, TEXT, 0}, {0.14f, TEXT, 0.13f}, {0.84f, TEXT, 0.13f}, {1, TEXT, 0.03f}};
    c.gradient(770 - 1.5f, 40, 3, kH - 40.0f, true, far_line, 4);
    const struct {
      float y;
      const wchar_t *name, *latin;
    } stations[5] = {{132, L"КОМСОМОЛЬСКАЯ", L"KOMSOMOLSKAYA"}, {222, L"КРАСНЫЕ ВОРОТА", L"KRASNYE VOROTA"},
                     {312, L"ЧИСТЫЕ ПРУДЫ", L"CHISTYE PRUDY"},   {402, L"ЛУБЯНКА", L"LUBYANKA"},
                     {492, L"ОХОТНЫЙ РЯД", L"OKHOTNY RYAD"}};
    for (const auto& st : stations) map_station(c, 770, st.y, st.name, st.latin, 0.13f);
  }
  // the dark gathers at the edges; a film's grain over all
  const Stop from_left[2] = {{0, BLACK, 0.55f}, {1, BLACK, 0}}, from_right[2] = {{0, BLACK, 0}, {1, BLACK, 0.65f}};
  c.gradient(0, 0, 60, kH, false, from_left, 2);
  c.gradient(kW - 150.0f, 0, 150, kH, false, from_right, 2);
  const Stop from_top[2] = {{0, BLACK, 0.6f}, {1, BLACK, 0}};
  c.gradient(0, 0, kW, 70, true, from_top, 2);
  embers(c);
  c.noise(0, 0, kW, kH, gfx::Speckle, 0xFFFFFF, 0.022f);
}

// --- the window's own: its name, its two glyphs, the stations -------------------------------------------

void chrome(Ctx& x) {
  Canvas& c = x.c;
  c.text_mid(L"METRO 2033 REDUX — СОВМЕСТНАЯ ИГРА", Ty(10.5f, 0.30f), 28, 27, TEXT, {}, 0.62f);
  const struct {
    int id;
    float bx;
    const char* glyph;
  } glyphs[2] = {{H_Min, 884, kMin}, {H_Close, 920, kClose}};
  for (const auto& g : glyphs) {
    const bool over = x.m.hover == g.id;
    c.icon(g.glyph, g.bx, 18, 18, over ? (g.id == H_Close ? ORANGE : TEXT) : GREY, 1.7f);
    hit(x, g.id, g.bx - 9, 6, 36, 42);
  }
}

void stations(Ctx& x) {
  Canvas& c = x.c;
  const Model& m = x.m;
  const struct {
    const wchar_t *name, *second;
  } tabs[kTabs] = {{L"ИГРА", L"ЗАПУСК"}, {L"СЕТЬ", L"СОВМЕСТНАЯ ИГРА"}, {L"ГРАФИКА", L"КАРТИНКА ИГРЫ"},
                   {L"УПРАВЛЕНИЕ", L"КЛАВИШИ И МЫШЬ"}, {L"ЛАУНЧЕР", L"ПАПКА И ОБНОВЛЕНИЯ"}};
  for (int i = 0; i < kTabs; ++i) {
    const float y = kNavTop + i * kNavStep;
    const bool on = static_cast<int>(m.tab) == i, over = m.hover == H_Tab0 + i;
    if (on) {
      c.ellipse(kNavX, y, 11, 11, ORANGE);
      c.ellipse(kNavX, y, 4.6f, 4.6f, BG);
    } else {
      c.ellipse(kNavX, y, 9.5f, 9.5f, over ? TEXT : GREY, over ? 0.95f : 0.75f);
      c.ellipse(kNavX, y, 5.2f, 5.2f, BG);
    }
    TextOpt box;
    box.line_h = 24;
    c.text(tabs[i].name, D(23, 0.02f), kNavX + 24, y - 17, on ? TEXT : over ? TEXT : GREY, box, on || over ? 1.0f : 0.9f);
    c.text(tabs[i].second, Sm(9, 0.14f), kNavX + 24.5f, y + 8, on ? ORANGE : DIM, {}, 1);
    hit(x, H_Tab0 + i, kNavX - 22, y - kNavStep / 2, 232, kNavStep);
  }
}

// --- what the pages are made of --------------------------------------------------------------------------

// A heading: typewriter capitals, widely spaced.
float heading(Ctx& x, const std::wstring& text, float y) {
  x.c.text_mid(text, Ty(10.5f, 0.26f), kPageX, y + 8, GREY);
  return 26;
}

struct Panel {
  float x, y, w, h;
  float right() const { return x + w - 20; }
};

// A flat dark panel; a few faint scratches are all it has.
Panel panel(Canvas& c, float y, float h) {
  Panel p{kPageX, y, kPageW, h};
  c.fill(p.x, p.y, p.w, p.h, PANEL, 0.95f);
  c.clip(p.x, p.y, p.w, p.h);
  unsigned s = static_cast<unsigned>(y * 131 + h * 17 + 5);
  for (int i = 0; i < 6; ++i) {
    s = mix_bits(s + 0x9E3779B9u);
    const float fx = p.x + (s & 1023) / 1023.0f * p.w, fy = p.y + ((s >> 10) & 1023) / 1023.0f * p.h;
    const float len = 40 + ((s >> 20) & 255) / 255.0f * 150, ang = -0.75f + ((s >> 28) & 15) / 15.0f * 0.35f;
    c.line(fx, fy, fx + std::cos(ang) * len, fy + std::sin(ang) * len, 0.8f, TEXT, 0.030f);
  }
  c.unclip();
  return p;
}

// A row: the lighter bar under the pointer, and its name at the left.
void row(Ctx& x, const Panel& p, float ry, float rh, const std::wstring& name, bool enabled, bool hot) {
  if (hot) x.c.fill(p.x + 6, ry, p.w - 12, rh, BAR);
  x.c.text_mid(name, T(15.5f), p.x + 20, ry + rh / 2, enabled ? TEXT : DIM);
}

bool hover_any(const Ctx& x, std::initializer_list<int> ids) {
  for (int id : ids)
    if (x.m.hover == id) return true;
  return false;
}

// A value of a row, in words at its right end. Orange: it was changed.
void word_at(Ctx& x, const std::wstring& text, float right, float cy, uint32_t rgb) {
  Font f = Sm(12.5f, 0.12f);
  x.c.text_mid(text, f, right - x.c.measure(text, f).width, cy, rgb);
}

// On / off, said in a word: the whole row is pressed.
void switch_row(Ctx& x, const Panel& p, float ry, float rh, int id, const std::wstring& name, bool on, bool enabled, bool changed) {
  row(x, p, ry, rh, name, enabled, enabled && x.m.hover == id);
  word_at(x, on ? L"ВКЛ" : L"ВЫКЛ", p.right(), ry + rh / 2, !enabled ? DIM : changed ? ORANGE : on ? TEXT : GREY);
  if (enabled) hit(x, id, p.x + 6, ry, p.w - 12, rh);
}

// "< value >": the value between two arrows.
void stepper_at(Ctx& x, int id_prev, int id_next, const std::wstring& value, float right, float ry, float rh, float width, bool enabled,
                bool changed) {
  Canvas& c = x.c;
  const float cy = ry + rh / 2, left = right - width;
  Font f = T(15);
  TextOpt one;
  one.ellipsis = true;
  one.max_w = width - 44;
  const float tw = std::min(c.measure(value, f).width, one.max_w);
  c.text_mid(value, f, left + (width - tw) / 2, cy, !enabled ? DIM : changed ? ORANGE : TEXT, one);
  c.icon(kPrev, left, cy - 8, 16, !enabled ? DIM : x.m.hover == id_prev ? TEXT : GREY, 1.8f);
  c.icon(kNext, right - 16, cy - 8, 16, !enabled ? DIM : x.m.hover == id_next ? TEXT : GREY, 1.8f);
  if (enabled) {
    hit(x, id_prev, left - 8, ry, width / 2 + 8, rh);
    hit(x, id_next, left + width / 2, ry, width / 2 + 8, rh);
  }
}

void stepper_row(Ctx& x, const Panel& p, float ry, float rh, int id_prev, int id_next, const std::wstring& name, const std::wstring& value,
                 bool enabled, bool changed) {
  row(x, p, ry, rh, name, enabled, enabled && hover_any(x, {id_prev, id_next}));
  stepper_at(x, id_prev, id_next, value, p.right(), ry, rh, 210, enabled, changed);
}

// A thin line with a small orange knob; a tick where the game's own value is; the value in digits.
void slider_row(Ctx& x, const Panel& p, float ry, float rh, int id, const std::wstring& name, float at, float def, const std::wstring& value,
                bool enabled, bool changed) {
  Canvas& c = x.c;
  row(x, p, ry, rh, name, enabled, enabled && (x.m.hover == id || x.m.pressed == id));
  const float cy = ry + rh / 2, len = 190, x0 = p.right() - 62 - len;
  c.fill(x0, cy - 0.75f, len, 1.5f, enabled ? DIM : 0x2A2927);
  if (enabled) {
    c.fill(x0, cy - 0.75f, len * at, 1.5f, TEXT, 0.85f);
    if (def >= 0) c.fill(x0 + len * def - 0.75f, cy - 8, 1.5f, 5, GREY);
    c.ellipse(x0 + len * at, cy, 4.8f, 4.8f, ORANGE);
    hit(x, id, x0 - 8, ry, len + 16, rh);  // (its ends are 8 inside the box: main.cpp counts with that)
  }
  Font f = T(15);
  c.text_mid(value, f, p.right() - c.measure(value, f).width, cy, !enabled ? DIM : changed ? ORANGE : TEXT);
}

// A place to type into: the text over a thin rule. Its right end is at `right`.
void field_at(Ctx& x, int id, int which, const std::wstring& text, const wchar_t* placeholder, float right, float ry, float rh, float width,
              bool enabled, bool changed) {
  Canvas& c = x.c;
  const Model& m = x.m;
  const bool focus = enabled && m.focus == which, over = enabled && m.hover == id;
  const float fx = right - width, cy = ry + rh / 2;
  c.fill(fx, cy + 11, width, 1, focus ? ORANGE : over ? GREY : DIM, enabled ? 1.0f : 0.5f);
  Font f = T(15);
  if (text.empty() && !focus) {
    c.text_mid(placeholder, f, fx + 2, cy, DIM);
  } else {
    TextOpt clip;
    clip.max_w = width - 10;
    clip.ellipsis = true;
    c.text_mid(text, f, fx + 2, cy, !enabled ? DIM : changed && !focus ? ORANGE : TEXT, clip);
    if (focus && std::fmod(m.time, 1.0) < 0.55) c.fill(fx + 3 + std::min(c.measure(text, f).width, clip.max_w), cy - 9, 1.5f, 18, ORANGE);
  }
  if (enabled) hit(x, id, fx - 6, ry, width + 12, rh);
}

// Something to do: capitals over a thin rule, the game's orange under the pointer (orange is kept for the
// one main action of a page). Its right end is at `right`; returns its left end.
float action_at(Ctx& x, int id, const std::wstring& name, float right, float cy, bool enabled = true) {
  Canvas& c = x.c;
  Font f = Sm(12.5f, 0.12f);
  const float w = c.measure(name, f).width - f.spacing * f.size, left = right - w;
  const bool over = enabled && x.m.hover == id;
  c.text_mid(name, f, left, cy, !enabled ? DIM : over ? ORANGE : TEXT);
  if (enabled) c.fill(left, cy + 10, w, 1, over ? ORANGE : DIM);
  if (enabled) hit(x, id, left - 8, cy - 16, w + 16, 32);
  return left;
}

// The main action: a flat orange block with black capitals. Returns its width.
float primary_button(Ctx& x, const Vals& v, float bx, float by) {
  Canvas& c = x.c;
  Font f = D(23, 0.11f);
  const float tw = c.measure(v.primary, f).width - f.spacing * f.size, w = std::max(tw + 76, 224.0f), h = 52;
  const bool on = v.primary_on, over = on && x.m.hover == H_Primary, down = on && x.m.pressed == H_Primary;
  if (on) {
    c.fill(bx, by, w, h, down ? ORANGE_LOW : over ? ORANGE_HOT : ORANGE);
    c.text_mid(v.primary, f, bx + (w - tw) / 2, by + h / 2 + 1, 0x0A0704);
    hit(x, H_Primary, bx, by, w, h);
  } else {
    c.border(bx, by, w, h, 1, DIM);
    c.text_mid(v.primary, f, bx + (w - tw) / 2, by + h / 2 + 1, DIM);
  }
  return w;
}

// A quiet line under a page: what is to be known about its settings. Orange: something is in the way.
void page_note(Ctx& x, float y, const std::wstring& text, bool warns = false) {
  TextOpt wrap;
  wrap.wrap = true;
  wrap.max_w = kPageW - 20;
  wrap.line_h = 19;
  x.c.text(text, T(13.5f), kPageX, y, warns ? ORANGE : GREY, wrap);
}

// --- ИГРА ------------------------------------------------------------------------------------------------

void downloading(Ctx& x, float y) {
  Canvas& c = x.c;
  const Model& m = x.m;
  const float w = 420;
  const float share = m.stage > 0 ? 1.0f : m.total ? clamp01(static_cast<float>(static_cast<double>(m.got) / m.total)) : 0.0f;
  std::wstring what;
  if (m.stage == 0)
    what = m.total ? L"Загрузка: " + megabytes(m.got) + L" из " + megabytes(m.total) + L" МБ" : std::wstring(L"Мод уже в лаунчере");
  else if (m.stage == 1)
    what = m.total ? L"Проверка: сверяю SHA-256" : L"Проверка";
  else
    what = m.pending == Pending::Uninstall ? L"Убираю файлы мода из папки игры"
           : m.pending == Pending::Launcher ? L"Меняю файл лаунчера: он запустится заново" : L"Кладу файлы в папку игры";
  c.text(what, T(18), kPageX, y, TEXT);
  y += 44;
  // the way: three steps, the line lit as far as the work has gone
  const float at = m.stage == 0 ? 0.5f * share : m.stage == 1 ? 0.75f : 1.0f;
  c.fill(kPageX, y - 0.75f, w, 1.5f, DIM);
  c.fill(kPageX, y - 0.75f, w * at, 1.5f, TEXT, 0.9f);
  c.ellipse(kPageX + w * at, y, 5.5f, 5.5f, ORANGE);
  const wchar_t* steps[3] = {L"ЗАГРУЗКА", L"ПРОВЕРКА", m.pending == Pending::Uninstall ? L"УДАЛЕНИЕ"
                                                       : m.pending == Pending::Launcher ? L"ПЕРЕЗАПУСК" : L"УСТАНОВКА"};
  Font f = Ty(9.5f, 0.20f);
  for (int i = 0; i < 3; ++i) {
    const float tw = c.measure(steps[i], f).width - f.spacing * f.size;
    const float tx = i == 0 ? kPageX : i == 1 ? kPageX + (w - tw) / 2 : kPageX + w - tw;
    c.text_mid(steps[i], f, tx, y + 20, i == m.stage ? TEXT : i < m.stage ? GREY : DIM);
  }
  y += 56;
  Font af = Sm(12.5f, 0.12f);
  action_at(x, H_Cancel, L"ОТМЕНА", kPageX + c.measure(L"ОТМЕНА", af).width - af.spacing * af.size, y, true);
}

void game_page(Ctx& x, const Vals& v) {
  Canvas& c = x.c;
  const Model& m = x.m;
  float y = 118;
  // the name, as the map names a station: big capitals, a small orange line under them
  TextOpt big;
  big.line_h = 62;
  c.text(L"METRO 2033 REDUX", D(60, 0.012f), kPageX - 3, y, TEXT, big);
  y += 66;
  c.text(L"СОВМЕСТНАЯ ИГРА", Sm(13.5f, 0.36f), kPageX + 1, y, ORANGE);
  y += 58;
  if (m.screen == Screen::Downloading) {
    downloading(x, y);
  } else {
    TextOpt wrap;
    wrap.wrap = true;
    wrap.max_w = 430;
    wrap.line_h = 27;
    y += c.text(v.state, T(19), kPageX, y, TEXT, wrap);
    if (!v.hint.empty()) {
      wrap.line_h = 20;
      y += 6 + c.text(v.hint, T(14), kPageX, y + 6, GREY, wrap);
    }
    y += 30;
    const float pw = primary_button(x, v, kPageX, y);
    if (!v.secondary.empty()) {
      Font f = Sm(12.5f, 0.12f);
      const float sw = c.measure(v.secondary, f).width;
      action_at(x, H_Secondary, v.secondary, kPageX + pw + 30 + sw, y + 26);
    }
  }
  // how a game together begins: three quiet lines
  if (m.screen == Screen::Problem || m.screen == Screen::Running || m.screen == Screen::WrongBuild) return;
  y = 450;
  c.text_mid(L"КАК ИГРАТЬ ВМЕСТЕ", Ty(10, 0.26f), kPageX, y, DIM);
  y += 20;
  const wchar_t* steps[3] = {L"У обоих запущен Steam, и вы друзья в Steam.", L"Один начинает или продолжает сюжет, как обычно.",
                             L"Второй в главном меню игры отвечает «OK» на её вопрос «…Присоединиться к этой игре?»"};
  for (int i = 0; i < 3; ++i) {
    const wchar_t n[2] = {static_cast<wchar_t>(L'1' + i), 0};
    c.text_mid(n, Ty(11, 0), kPageX, y + 10, DIM);
    c.text_mid(steps[i], T(13.5f), kPageX + 20, y + 10, GREY);
    y += 21;
  }
}

// --- СЕТЬ: [net] of metronet.ini --------------------------------------------------------------------------

const wchar_t kNoMod[] = L"Мод не установлен: эти настройки появятся вместе с ним.";

void net_page(Ctx& x) {
  const Model& m = x.m;
  const mset::Values& s = mset::get();
  const mset::Values& was = mset::first();
  const bool on = mset::has_ini(), coop = s.mode != mset::Mode::Off;
  float y = kPageTop;
  y += heading(x, L"СОВМЕСТНАЯ ИГРА · METRONET.INI", y);
  Panel p = panel(x.c, y, 4 * kRow + 16);
  float ry = y + 8;
  switch_row(x, p, ry, kRow, H_Coop, L"Совместная игра через Steam", coop, on, coop != (was.mode != mset::Mode::Off));
  ry += kRow;
  switch_row(x, p, ry, kRow, H_Names, L"Имена над головами игроков", s.names, on && coop, s.names != was.names);
  ry += kRow;
  switch_row(x, p, ry, kRow, H_FriendlyFire, L"Огонь по своим", s.friendly_fire, on && coop, s.friendly_fire != was.friendly_fire);
  ry += kRow;
  row(x, p, ry, kRow, L"Ваше имя, когда Steam не запущен", on && coop, on && coop && m.hover == H_NameField);
  field_at(x, H_NameField, F_Name, m.focus == F_Name ? m.edit : s.name, L"не задано", p.right(), ry, kRow, 210, on && coop, s.name != was.name);
  y = p.y + p.h + 22;

  y += heading(x, L"ИГРА ПО IP-АДРЕСУ · ЗАПАСНОЙ СПОСОБ", y);
  p = panel(x.c, y, 2 * kRow + 16);
  ry = y + 8;
  const int way = s.mode == mset::Mode::Host ? 1 : s.mode == mset::Mode::Join ? 2 : 0;
  const wchar_t* ways[3] = {L"Не нужна", L"Я хост", L"Я вхожу к хосту"};
  stepper_row(x, p, ry, kRow, H_IpPrev, H_IpNext, L"Игра по IP-адресу", ways[way], on && coop,
              way != (was.mode == mset::Mode::Host ? 1 : was.mode == mset::Mode::Join ? 2 : 0));
  ry += kRow;
  const bool join = on && coop && way == 2;
  row(x, p, ry, kRow, L"IP-адрес хоста", join, join && m.hover == H_AddressField);
  field_at(x, H_AddressField, F_Address, m.focus == F_Address ? m.edit : s.address, L"например, 192.168.1.5", p.right(), ry, kRow, 210, join,
           s.address != was.address);
  y = p.y + p.h + 16;
  page_note(x, y,
            !on ? std::wstring(kNoMod)
            : !coop ? L"Совместная игра выключена: в таком режиме мод ничего не делает."
            : way == 1 ? L"Вы ведёте игру: начните сюжет, друг входит по вашему адресу (порт UDP 1000). Оба компьютера — в одной сети или VPN."
            : way == 2 ? L"Вы входите к хосту: игра подключится к его адресу из главного меню. Оба компьютера — в одной сети или VPN."
                       : L"Через Steam настраивать нечего: кто играет сюжет — к тому входят друзья. Изменения действуют со следующего запуска игры.",
            !on);
}

// --- ГРАФИКА and УПРАВЛЕНИЕ: the game's user.cfg ---------------------------------------------------------

const wchar_t kNoFile[] = L"Сначала один раз запустите игру: файла её настроек (user.cfg) ещё нет.";
const wchar_t kAtStart[] = L"Это настройки самой игры: лаунчер меняет в её user.cfg только нужные строки. Действуют со следующего запуска.";

void game_row(Ctx& x, const Panel& p, float ry, float rh, gcfg::Row r) {
  const bool on = gcfg::usable(r), changed = gcfg::changed(r);
  const int base = H_Row + r * kRowParts;
  switch (gcfg::kind(r)) {
    case gcfg::Kind::Switch:
      switch_row(x, p, ry, rh, base + P_Next, gcfg::label(r), gcfg::value(r) != 0, on, changed);
      break;
    case gcfg::Kind::Slider:
      slider_row(x, p, ry, rh, base + P_Slide, gcfg::label(r), gcfg::fraction(r), gcfg::default_fraction(r), gcfg::text(r), on, changed);
      break;
    default:
      stepper_row(x, p, ry, rh, base + P_Prev, base + P_Next, gcfg::label(r), on ? gcfg::name(r, gcfg::value(r)) : std::wstring(L"—"), on,
                  changed);
      break;
  }
}

void graphics_page(Ctx& x) {
  float y = kPageTop;
  y += heading(x, L"КАРТИНКА ИГРЫ · USER.CFG", y);
  Panel p = panel(x.c, y, gcfg::kFirstControl * kRow + 16);
  for (int r = 0; r < gcfg::kFirstControl; ++r) game_row(x, p, y + 8 + r * kRow, kRow, static_cast<gcfg::Row>(r));
  page_note(x, p.y + p.h + 16, gcfg::has_file() ? kAtStart : kNoFile, !gcfg::has_file());
}

void controls_page(Ctx& x) {
  Canvas& c = x.c;
  const Model& m = x.m;
  const bool on = gcfg::has_file();
  const float rh = 29;
  float y = kPageTop;
  y += heading(x, L"МЫШЬ И ГЕЙМПАД · USER.CFG", y);
  Panel p = panel(c, y, (gcfg::kRows - gcfg::kFirstControl) * rh + 12);
  for (int r = gcfg::kFirstControl; r < gcfg::kRows; ++r) game_row(x, p, y + 6 + (r - gcfg::kFirstControl) * rh, rh, static_cast<gcfg::Row>(r));
  y = p.y + p.h + 20;

  // the keys: a page of them at a time, in two columns
  const int pages = gcfg::pages(), page = pages ? std::min(m.keys_page, pages - 1) : 0;
  c.text_mid(L"КЛАВИШИ", Ty(10.5f, 0.26f), kPageX, y + 8, GREY);
  if (pages) stepper_at(x, H_KeysPrev, H_KeysNext, gcfg::page_name(page), kPageX + kPageW - 20, y - 8, 32, 200, on, false);
  y += 26;
  const int per = gcfg::kPageRows / 2;
  p = panel(c, y, per * rh + 12);
  const std::vector<int>& rows = gcfg::page(page);
  const float col_w = (p.w - 12) / 2;
  for (size_t k = 0; k < rows.size(); ++k) {
    const int i = rows[k];
    const float cx = p.x + 6 + (k / per) * col_w, ry = y + 6 + (k % per) * rh, cy = ry + rh / 2;
    const bool waits = m.capture == i, over = m.hover == H_Bind + i || m.hover == H_BindReset + i;
    if (waits || over) c.fill(cx, ry, col_w, rh, BAR);
    bool raw = false;
    const std::wstring name = gcfg::action_label(i, &raw);
    c.text_mid(name, raw ? Mo(13.5f) : T(15), cx + 14, cy, raw ? GREY : TEXT);
    float right = cx + col_w - 14;
    if (!gcfg::bind_is_default(i) && !waits) {  // back to the game's own key
      c.icon(kBackArrow, right - 14, cy - 7, 14, m.hover == H_BindReset + i ? TEXT : GREY, 1.9f);
      hit(x, H_BindReset + i, right - 22, ry, 30, rh);
      right -= 26;
    }
    Font kf = Sm(12.5f, 0.06f);
    const std::wstring key = waits ? std::wstring(L"НАЖМИТЕ КЛАВИШУ") : gcfg::key_label(gcfg::bound(i));
    c.text_mid(key, kf, right - c.measure(key, kf).width + kf.spacing * kf.size, cy, waits || gcfg::bind_changed(i) ? ORANGE : TEXT);
    hit(x, H_Bind + i, cx, ry, right - cx + 4, rh);
  }
  page_note(x, p.y + p.h + 14,
            !on ? std::wstring(kNoFile)
            : m.capture >= 0 ? L"Нажмите клавишу или кнопку мыши (левую — по этой же строке). Esc — отмена."
                             : L"Нажмите на строку, затем на клавишу. Занятая клавиша поменяется местами с прежней; стрелка справа возвращает клавишу самой игры.",
            !on);
}

// --- ЛАУНЧЕР: the game folder, the mod, updates, the way to GitHub, the files -------------------------------

void launcher_page(Ctx& x) {
  Canvas& c = x.c;
  const Model& m = x.m;
  const float rh = 30;
  float y = kPageTop;
  y += heading(x, L"ИГРА И МОД", y);
  Panel p = panel(c, y, 9 * rh + 14);
  float ry = y + 7;
  auto cy = [&]() { return ry + rh / 2; };

  // the game folder
  row(x, p, ry, rh, L"Папка игры", true, m.hover == H_Folder);
  float left = action_at(x, H_Folder, L"ИЗМЕНИТЬ", p.right(), cy());
  TextOpt path;
  path.ellipsis = true;
  path.max_w = left - 24 - (p.x + 150);
  const std::wstring dir = m.has_game ? m.set.game_dir : std::wstring(L"не выбрана");
  c.text_mid(dir, Mo(13.5f), std::max(p.x + 150, left - 24 - c.measure(dir, Mo(13.5f)).width), cy(), m.has_game ? TEXT : GREY, path);
  ry += rh;

  // the mod in it
  row(x, p, ry, rh, L"Мод", true, hover_any(x, {H_Reinstall, H_Uninstall}));
  left = action_at(x, H_Uninstall, m.confirm_uninstall ? L"ТОЧНО УДАЛИТЬ?" : L"УДАЛИТЬ", p.right(), cy(), m.inst.present);
  left = action_at(x, H_Reinstall, m.inst.present ? L"ПЕРЕУСТАНОВИТЬ" : L"УСТАНОВИТЬ", left - 22, cy(), m.has_game && m.exe_ok);
  const std::wstring mod = !m.has_game ? std::wstring(L"—")
                           : !m.exe_ok ? std::wstring(L"другая сборка игры")
                           : !m.inst.present ? std::wstring(L"не установлен")
                           : m.inst.version.empty() ? std::wstring(L"версия неизвестна") : L"версия " + m.inst.version;
  c.text_mid(mod, T(15), left - 24 - c.measure(mod, T(15)).width, cy(), m.inst.present && m.exe_ok ? TEXT : GREY);
  ry += rh;

  // updates: what GitHub said
  row(x, p, ry, rh, L"Обновления", true, hover_any(x, {H_Recheck, H_NewLauncher}));
  const bool newer_launcher = m.rel.full && !from_github(m) && !m.rel.launcher_version.empty() && !m.swapped &&
                              core::compare_versions(m.rel.launcher_version, kLauncherVersion) > 0;
  left = newer_launcher ? action_at(x, H_NewLauncher, L"ОБНОВИТЬ ЛАУНЧЕР", p.right(), cy())
                        : action_at(x, H_Recheck, L"ПРОВЕРИТЬ", p.right(), cy(), m.repo && !m.rechecking && m.screen != Screen::Checking);
  const bool asking = m.rechecking || m.screen == Screen::Checking;
  const std::wstring news = m.swapped ? std::wstring(L"новый лаунчер откроется при следующем запуске")
                            : asking ? std::wstring(L"проверяю…")
                            : !m.repo ? std::wstring(L"мод внутри этого лаунчера")
                            : newer_launcher ? L"вышел лаунчер " + m.rel.launcher_version
                            : m.rel.known && m.assets_blocked ? std::wstring(L"файлы не скачиваются: включите прокси")
                            : m.rel.known && from_github(m) ? L"вышла версия " + m.rel.version
                            : m.rel.known ? (m.checked_at.empty() ? std::wstring(L"новее нет") : L"проверено в " + m.checked_at + L", новее нет")
                            : m.no_release ? std::wstring(L"релизов пока нет")
                            : m.unchecked ? std::wstring(L"не проверялось") : std::wstring(L"GitHub не отвечает");
  c.text_mid(news, T(15), left - 24 - c.measure(news, T(15)).width, cy(), GREY);
  ry += rh;

  switch_row(x, p, ry, rh, H_AutoCheck, L"Проверять обновления при запуске", m.set.auto_check, true, false);
  ry += rh;
  switch_row(x, p, ry, rh, H_AutoSelf, L"Обновлять лаунчер при запуске", m.set.auto_self, true, false);
  ry += rh;
  switch_row(x, p, ry, rh, H_AutoInstall, L"Ставить обновления мода без вопроса", m.set.auto_install, true, false);
  ry += rh;
  switch_row(x, p, ry, rh, H_ClosePlay, L"Закрывать лаунчер, когда игра запущена", m.set.close_on_play, true, false);
  ry += rh;

  // the way to GitHub
  row(x, p, ry, rh, L"Прокси для GitHub", true, hover_any(x, {H_ProxyPrev, H_ProxyNext, H_ProxyField}));
  const wchar_t* modes[3] = {L"Авто", L"Нет", L"Свой"};
  stepper_at(x, H_ProxyPrev, H_ProxyNext, modes[m.set.proxy % 3], p.right(), ry, rh, 120, true, false);
  if (m.set.proxy == 2) field_at(x, H_ProxyField, F_Proxy, m.set.proxy_addr, L"адрес:порт", p.right() - 120 - 26, ry, rh, 190, true, false);
  ry += rh;

  // what can be opened
  row(x, p, ry, rh, L"Открыть", true, hover_any(x, {H_OpenLog, H_OpenReadme, H_Repo}));
  left = action_at(x, H_Repo, L"СТРАНИЦА МОДА", p.right(), cy(), m.repo);
  left = action_at(x, H_OpenReadme, L"ОПИСАНИЕ", left - 22, cy(), m.inst.present);
  action_at(x, H_OpenLog, L"ЖУРНАЛ ИГРЫ", left - 22, cy(), m.inst.present);
  y = p.y + p.h + 20;

  // what the version brought, in plain lines
  const Release* r = nullptr;
  if (m.inst.present && m.rel.known && m.rel.version == m.inst.version && !m.rel.notes.empty()) r = &m.rel;
  else if (m.pack.known && !m.pack.notes.empty() && (!m.inst.present || m.inst.version.empty() || m.pack.version == m.inst.version)) r = &m.pack;
  std::wstring head, title;
  std::vector<std::wstring> items;
  if (from_github(m) && !m.rel.notes.empty()) r = &m.rel;  // (a newer one is out: what it brings)
  if (r) {
    head = L"ВЕРСИЯ " + r->version + (r->date.empty() ? L"" : L" · " + r->date);
    title = r->title;
    items = r->notes;
  } else if (m.inst.present && !m.set.notes.empty() && m.set.notes_version == m.inst.version) {
    head = L"ВЕРСИЯ " + m.inst.version + (m.set.notes_date.empty() ? L"" : L" · " + m.set.notes_date);
    title = m.set.notes_title;
    items = m.set.notes;
  }
  if (head.empty()) return;
  CharUpperBuffW(head.data(), static_cast<DWORD>(head.size()));
  y += heading(x, head, y);
  y += c.text(title, T(15.5f), kPageX, y, TEXT) + 4;
  TextOpt one;
  one.ellipsis = true;
  one.max_w = kPageW - 24;
  for (const std::wstring& item : items) {
    if (y + 19 > kH - 46.0f) break;
    c.text(L"—", T(13.5f), kPageX, y, DIM);
    c.text(item, T(13.5f), kPageX + 20, y, GREY, one);
    y += 19;
  }
}

// What happened a moment ago, said once at the foot of the page (no bar: a line that passes).
void passing_note(Ctx& x) {
  if (x.m.flash.empty()) return;
  TextOpt one;
  one.ellipsis = true;
  one.max_w = kPageW;
  x.c.fill(kPageX, 562, 10, 1.5f, ORANGE);
  x.c.text_mid(x.m.flash, T(13.5f), kPageX + 18, 562, TEXT, one);
}

}  // namespace

bool primary_enabled(const Model& m) { return vals(m).primary_on; }

void draw(gfx::Canvas& c, const Model& m, std::vector<Hit>* hits) {
  if (hits) hits->clear();
  Ctx x{c, m, hits};
  const bool full = m.tab == Tab::Game;
  c.cached(full ? 1 : 2, kW, kH, [full](Canvas& bg) { background(bg, full); });
  chrome(x);
  stations(x);
  switch (m.tab) {
    case Tab::Net: net_page(x); break;
    case Tab::Graphics: graphics_page(x); break;
    case Tab::Controls: controls_page(x); break;
    case Tab::Launcher: launcher_page(x); break;
    default: game_page(x, vals(m)); break;
  }
  passing_note(x);
}

// The pages as the design shows them.
bool demo(const std::wstring& screen, Model& m) {
  m = Model{};
  m.has_game = true;
  m.steam = true;
  m.set.game_dir = L"C:\\GOG Games\\Metro 2033 Redux";
  m.inst.present = true;
  m.repo = true;
  m.inst.version = L"0.3.0";
  m.inst.protocol = 2;
  m.rel.known = m.rel.full = true;
  m.rel.version = L"0.4.0";
  m.rel.protocol = 2;
  m.rel.date = L"12 декабря 2026";
  m.rel.title = L"Двери, лифты и дрезины";
  m.rel.notes = {L"Двери и подъёмники у всех игроков движутся одинаково.", L"На дрезине второй игрок сидит, а не стоит на краю.",
                 L"Вход в игру друга стал быстрее."};
  m.rel.pkg_name = L"metronet-launcher.exe";
  m.rel.pkg_size = 2400000;
  m.rel.pkg_sha256 = "x";
  m.pack.known = true;
  m.pack.version = L"0.3.0";
  m.pack.protocol = 2;
  m.pack.date = L"25 ноября 2026";
  m.pack.title = L"Вместе без настроек";
  m.pack.notes = {L"Ничего не нужно настраивать: кто играет сюжет — того видят друзья в Steam.",
                  L"Игра друга в главном меню сама спрашивает, присоединиться ли.",
                  L"Сохранение одно — ваше обычное; перед первой общей игрой остаётся его копия.",
                  L"Выйти в главное меню и войти снова можно в любой момент."};
  m.total = 2400000;
  m.got = static_cast<uint64_t>(2400000 * 0.62);
  mset::load_canned(mset::Mode::Auto, L"", L"");
  gcfg::load_canned();
  auto latest = [&]() {  // GitHub has what is installed
    m.rel = m.pack;
    m.rel.full = true;
    m.rel.pkg_name = L"metronet-launcher.exe";
    m.rel.pkg_size = 2400000;
    m.rel.pkg_sha256 = "x";
    m.checked_at = L"21:40";
  };
  m.screen = Screen::Ready;
  if (screen == L"main" || screen == L"ready") {
    latest();
  } else if (screen == L"update") {
    m.screen = Screen::Update;
  } else if (screen == L"downloading") {
    m.screen = Screen::Downloading;
    m.pending = Pending::Install;
  } else if (screen == L"checking") {
    m.screen = Screen::Checking;
    m.rel = Release{};
  } else if (screen == L"install") {
    m.screen = Screen::Install;
    m.inst = Installed{};
    latest();
  } else if (screen == L"nogame") {
    m.screen = Screen::NoGame;
    m.has_game = false;
    m.inst = Installed{};
    latest();
  } else if (screen == L"wrongbuild") {
    m.screen = Screen::WrongBuild;
    m.exe_ok = false;
    m.inst = Installed{};
    latest();
  } else if (screen == L"offline") {
    m.screen = Screen::Offline;
    m.rel = Release{};
  } else if (screen == L"norelease") {  // nothing is published yet: a quiet state
    m.rel = Release{};
    m.no_release = true;
  } else if (screen == L"running") {
    m.screen = Screen::Running;
    m.pending = Pending::Install;
  } else if (screen == L"problem") {
    m.screen = Screen::Problem;
    m.problem_title = L"Файл повреждён";
    m.problem_text = L"Контрольная сумма скачанного файла не совпала с той, что указана в релизе. Файл удалён — скачайте его ещё раз.";
    m.problem_primary = L"СКАЧАТЬ ЗАНОВО";
    m.problem_secondary = L"НАЗАД";
  } else if (screen == L"selfupdate") {  // at its start the launcher found a newer one and takes it by itself
    m.screen = Screen::Downloading;
    m.pending = Pending::Launcher;
    latest();
    m.rel.launcher_version = L"1.0.1";
  } else if (screen == L"off") {  // the co-op is turned off in the mod's settings
    latest();
    mset::load_canned(mset::Mode::Off, L"", L"");
  } else if (screen == L"net" || screen == L"net-ip" || screen == L"net-nomod") {
    m.tab = Tab::Net;
    latest();
    if (screen == L"net-ip") {
      mset::load_canned(mset::Mode::Auto, L"", L"");
      mset::set_mode(mset::Mode::Join);  // (changed on this page: shown so)
      mset::set_address(L"192.168.1.5");
    }
    if (screen == L"net-nomod") {
      m.inst = Installed{};
      m.screen = Screen::Install;
      mset::load(L"");
    }
  } else if (screen == L"graphics" || screen == L"graphics-nofile") {
    m.tab = Tab::Graphics;
    latest();
    if (screen == L"graphics") gcfg::slide(gcfg::kFov, (60.0f - 45.0f) / 45.0f);  // (changed on this page: shown so)
    else gcfg::load(L"");
  } else if (screen == L"controls" || screen == L"controls-wait" || screen == L"controls-weapons") {
    m.tab = Tab::Controls;
    latest();
    if (screen == L"controls-weapons") m.keys_page = 1;
    for (int i = 0; i < gcfg::binds(); ++i) {
      if (screen == L"controls" && gcfg::action(i) == "sprint") gcfg::bind(i, "kLMENU");  // (changed on this page: shown so)
      if (screen == L"controls-wait" && gcfg::action(i) == "jump") m.capture = i;
    }
  } else if (screen == L"launcher" || screen == L"launcher-update") {
    m.tab = Tab::Launcher;
    if (screen == L"launcher") latest();
    else m.screen = Screen::Update;
  } else {
    return false;
  }
  return true;
}

}  // namespace view
}  // namespace lz
