// Drawing for the launcher: Direct2D + DirectWrite with the fonts Windows has (nothing is carried in the
// exe), SVG path strings as geometry, soft lights and procedural grain for the walls and the paper. A
// Canvas draws to a window's target or to a bitmap (a PNG of a screen: launcher --shot).
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite_1.h>
#include <wincodec.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace gfx {

// Display: bold condensed capitals, as a metro map names its stations (Bahnschrift Condensed; Impact or Arial
// stand in). Label: the same hand a little wider, for small capitals. Text: what is read, a slightly
// condensed sans (Bahnschrift SemiCondensed; Segoe UI stands in). Mono: paths and the game's own names
// (Consolas). Typed: typewriter capitals for headings (Courier New).
enum Family { Display = 0, Label = 1, Text = 2, Mono = 3, Typed = 4, kFamilies = 5 };
struct Font {
  Family family;
  float size;      // px
  int weight;      // 300 .. 700
  float spacing;   // letter spacing, em
};
struct TextOpt {
  float max_w = 0;         // the box's width: wrapping, the ellipsis, centring
  float line_h = 0;        // the line box, px (0: the font's own)
  bool wrap = false;
  bool ellipsis = false;
  bool underline = false;
  bool center = false;
};
struct Stop {
  float pos;
  uint32_t rgb;
  float a;
};
enum Grain { Speckle = 0, Blotch = 1, Streak = 2, Fibre = 3 };

bool init();      // factories and fonts, once
void shutdown();
const wchar_t* face_name(Family f);  // the font that stands for a family on this PC
IWICImagingFactory* wic();
ID2D1Factory* d2d();

using Matrix = D2D1::Matrix3x2F;

class Canvas {
 public:
  explicit Canvas(ID2D1RenderTarget* target);
  ~Canvas();
  Canvas(const Canvas&) = delete;
  Canvas& operator=(const Canvas&) = delete;

  ID2D1RenderTarget* rt;

  // shapes (colours are 0xRRGGBB)
  void fill(float x, float y, float w, float h, uint32_t rgb, float a = 1);
  void border(float x, float y, float w, float h, float bw, uint32_t rgb, float a = 1);  // inside the box
  void round_fill(float x, float y, float w, float h, float radius, uint32_t rgb, float a = 1);
  void round_line(float x, float y, float w, float h, float radius, float stroke, uint32_t rgb, float a = 1);
  void ellipse(float cx, float cy, float rx, float ry, uint32_t rgb, float a = 1);
  void ring(float cx, float cy, float r, float w, uint32_t rgb, float a = 1);
  void poly(const D2D1_POINT_2F* pts, int n, uint32_t rgb, float a = 1);
  void line(float x0, float y0, float x1, float y1, float w, uint32_t rgb, float a = 1);
  void dashed_h(float x, float y, float w, float thick, float dash, float gap, uint32_t rgb, float a);
  // a gradient across a box, top to bottom or left to right; every stop has its own opacity
  void gradient(float x, float y, float w, float h, bool vertical, const Stop* stops, int n);
  // a light: `a` in the middle, nothing at the rim of the ellipse
  void glow(float cx, float cy, float rx, float ry, uint32_t rgb, float a);
  // grain of one colour over a box: specks, blotches of soot, streaks running down, paper's fibres
  void noise(float x, float y, float w, float h, Grain kind, uint32_t rgb, float a, float scale = 1);
  // SVG path data (a string literal: the geometry is kept by its address)
  void fill_path(const char* svg, uint32_t rgb, float a = 1);
  void stroke_path(const char* svg, float w, uint32_t rgb, bool round = true, float a = 1);
  // an icon drawn in a 24 x 24 box (or `box`), stroked, at x, y with the given size
  void icon(const char* svg, float x, float y, float size, uint32_t rgb, float stroke_w, float box = 24, bool filled = false);

  // transforms: m is in the local space (applied before what is set already)
  void push(const Matrix& m);
  void pop();
  Matrix current() const;
  void clip(float x, float y, float w, float h);  // until unclip()
  void unclip();

  // text
  float line_h(const Font& f);  // the font's own line box
  D2D1_SIZE_F measure(const std::wstring& s, const Font& f, const TextOpt& o = {});
  // Draws at x, y (the top of the first line box); returns the height taken.
  float text(const std::wstring& s, const Font& f, float x, float y, uint32_t rgb, const TextOpt& o = {}, float a = 1);
  // One line, its box centred on cy.
  float text_mid(const std::wstring& s, const Font& f, float x, float cy, uint32_t rgb, const TextOpt& o = {}, float a = 1);

  // Something that never changes, drawn once into a bitmap of w x h and shown at 0, 0.
  void cached(int key, float w, float h, const std::function<void(Canvas&)>& paint);

 private:
  IDWriteTextLayout* layout(const std::wstring& s, const Font& f, const TextOpt& o);
  ID2D1SolidColorBrush* brush_ = nullptr;
  std::vector<Matrix> stack_;
  std::map<int, ID2D1BitmapRenderTarget*> cache_;
  std::map<uint64_t, ID2D1Bitmap*> grain_;
  ID2D1SolidColorBrush* color(uint32_t rgb, float a);
};

// A screen of w x h (in the launcher's units) drawn at `scale` into a PNG file (no path: drawn and dropped).
bool save_png(const wchar_t* path, int w, int h, float scale, const std::function<void(Canvas&)>& paint);
extern long g_last_hr;   // of the step of save_png that failed
extern int g_last_step;

}  // namespace gfx
