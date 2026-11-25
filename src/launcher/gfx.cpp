#include "launcher/gfx.h"

#include <cmath>
#include <cstring>

namespace gfx {
namespace {

ID2D1Factory* g_d2d = nullptr;
IDWriteFactory* g_dw = nullptr;
IWICImagingFactory* g_wic = nullptr;
ID2D1StrokeStyle* g_round = nullptr;
std::map<const char*, ID2D1PathGeometry*> g_paths;

// The font of Windows that stands for a family: the first of its list that this PC has.
struct Face {
  const wchar_t* name = L"Arial";
  DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;
  int weight = 0;  // not 0: the family has this one weight (Impact)
};
Face g_face[kFamilies];

struct FormatKey {
  int family, size100, weight;
  bool operator<(const FormatKey& o) const {
    if (family != o.family) return family < o.family;
    if (size100 != o.size100) return size100 < o.size100;
    return weight < o.weight;
  }
};
std::map<FormatKey, IDWriteTextFormat*> g_formats;
struct Metrics {
  float ascent = 0.95f, descent = 0.3f, gap = 0;  // em
};
std::map<std::pair<int, int>, Metrics> g_metrics;

template <class T>
void release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

bool has_family(IDWriteFontCollection* coll, const wchar_t* name) {
  UINT32 index = 0;
  BOOL found = FALSE;
  return coll && SUCCEEDED(coll->FindFamilyName(name, &index, &found)) && found;
}

void pick_faces() {
  IDWriteFontCollection* coll = nullptr;
  g_dw->GetSystemFontCollection(&coll);
  const DWRITE_FONT_STRETCH N = DWRITE_FONT_STRETCH_NORMAL;
  const struct {
    Family family;
    Face face;
  } wanted[] = {
      {Display, {L"Bahnschrift", DWRITE_FONT_STRETCH_CONDENSED, 0}}, {Display, {L"Impact", N, 400}},
      {Display, {L"Arial Narrow", N, 0}},                            {Display, {L"Arial", N, 0}},
      {Label, {L"Bahnschrift", DWRITE_FONT_STRETCH_SEMI_CONDENSED, 0}}, {Label, {L"Arial Narrow", N, 0}},
      {Label, {L"Segoe UI", N, 0}},                                  {Label, {L"Arial", N, 0}},
      {Text, {L"Bahnschrift", DWRITE_FONT_STRETCH_SEMI_CONDENSED, 0}}, {Text, {L"Segoe UI", N, 0}},
      {Text, {L"Tahoma", N, 0}},                                     {Text, {L"Arial", N, 0}},
      {Mono, {L"Consolas", N, 0}},                                   {Mono, {L"Lucida Console", N, 0}},
      {Mono, {L"Courier New", N, 0}},
      {Typed, {L"Courier New", N, 0}},                               {Typed, {L"Consolas", N, 0}},
      {Typed, {L"Lucida Console", N, 0}},
  };
  bool chosen[kFamilies] = {};
  for (const auto& w : wanted) {
    if (chosen[w.family] || !has_family(coll, w.face.name)) continue;
    g_face[w.family] = w.face;
    chosen[w.family] = true;
  }
  release(coll);
}

DWRITE_FONT_WEIGHT weight_of(const Font& f) {
  const Face& face = g_face[f.family];
  return static_cast<DWRITE_FONT_WEIGHT>(face.weight ? face.weight : f.weight);
}

Metrics metrics(const Font& f) {
  auto key = std::make_pair(static_cast<int>(f.family), f.weight);
  auto it = g_metrics.find(key);
  if (it != g_metrics.end()) return it->second;
  Metrics m;
  IDWriteFontCollection* coll = nullptr;
  g_dw->GetSystemFontCollection(&coll);
  UINT32 index = 0;
  BOOL found = FALSE;
  if (coll && SUCCEEDED(coll->FindFamilyName(g_face[f.family].name, &index, &found)) && found) {
    IDWriteFontFamily* fam = nullptr;
    IDWriteFont* font = nullptr;
    if (SUCCEEDED(coll->GetFontFamily(index, &fam)) &&
        SUCCEEDED(fam->GetFirstMatchingFont(weight_of(f), g_face[f.family].stretch, DWRITE_FONT_STYLE_NORMAL, &font))) {
      DWRITE_FONT_METRICS fm{};
      font->GetMetrics(&fm);
      if (fm.designUnitsPerEm) {
        m.ascent = static_cast<float>(fm.ascent) / fm.designUnitsPerEm;
        m.descent = static_cast<float>(fm.descent) / fm.designUnitsPerEm;
        m.gap = static_cast<float>(fm.lineGap) / fm.designUnitsPerEm;
      }
    }
    release(font);
    release(fam);
  }
  release(coll);
  g_metrics[key] = m;
  return m;
}

IDWriteTextFormat* format(const Font& f) {
  FormatKey key{f.family, static_cast<int>(f.size * 100 + 0.5f), f.weight};
  auto it = g_formats.find(key);
  if (it != g_formats.end()) return it->second;
  IDWriteTextFormat* fmt = nullptr;
  g_dw->CreateTextFormat(g_face[f.family].name, nullptr, weight_of(f), DWRITE_FONT_STYLE_NORMAL, g_face[f.family].stretch, f.size,
                         L"ru-ru", &fmt);
  g_formats[key] = fmt;
  return fmt;
}

// --- SVG path data ---------------------------------------------------------------------------

struct PathReader {
  const char* p;
  void skip() {
    while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
  }
  bool more() {  // another number follows (an implicit repeat of the command)
    skip();
    return (*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.';
  }
  float num() {
    skip();
    char buf[32];
    int n = 0;
    if (*p == '-' || *p == '+') buf[n++] = *p++;
    bool dot = false;
    while (n < 30 && ((*p >= '0' && *p <= '9') || (*p == '.' && !dot))) {
      if (*p == '.') dot = true;
      buf[n++] = *p++;
    }
    buf[n] = 0;
    return static_cast<float>(atof(buf));
  }
  bool flag() {
    skip();
    return *p++ == '1';
  }
};

ID2D1PathGeometry* path(const char* svg) {
  auto it = g_paths.find(svg);
  if (it != g_paths.end()) return it->second;
  ID2D1PathGeometry* geo = nullptr;
  ID2D1GeometrySink* sink = nullptr;
  if (FAILED(g_d2d->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) {
    release(geo);
    g_paths[svg] = nullptr;
    return nullptr;
  }
  sink->SetFillMode(D2D1_FILL_MODE_WINDING);
  PathReader r{svg};
  float x = 0, y = 0, sx = 0, sy = 0;
  bool open = false;
  char cmd = 0;
  auto begin = [&]() {
    if (!open) {
      sink->BeginFigure(D2D1::Point2F(x, y), D2D1_FIGURE_BEGIN_FILLED);
      open = true;
    }
  };
  for (;;) {
    r.skip();
    if (!*r.p) break;
    if ((*r.p >= 'A' && *r.p <= 'Z') || (*r.p >= 'a' && *r.p <= 'z')) cmd = *r.p++;
    else if (!cmd) break;
    bool rel = cmd >= 'a';
    switch (cmd | 0x20) {
      case 'm': {
        float nx = r.num(), ny = r.num();
        if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
        open = false;
        x = rel ? x + nx : nx;
        y = rel ? y + ny : ny;
        sx = x;
        sy = y;
        cmd = rel ? 'l' : 'L';  // further pairs are lines
        break;
      }
      case 'l': {
        float nx = r.num(), ny = r.num();
        begin();
        x = rel ? x + nx : nx;
        y = rel ? y + ny : ny;
        sink->AddLine(D2D1::Point2F(x, y));
        break;
      }
      case 'h': {
        float nx = r.num();
        begin();
        x = rel ? x + nx : nx;
        sink->AddLine(D2D1::Point2F(x, y));
        break;
      }
      case 'v': {
        float ny = r.num();
        begin();
        y = rel ? y + ny : ny;
        sink->AddLine(D2D1::Point2F(x, y));
        break;
      }
      case 'q': {
        float cx = r.num(), cy = r.num(), nx = r.num(), ny = r.num();
        begin();
        if (rel) cx += x, cy += y, nx += x, ny += y;
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(cx, cy), D2D1::Point2F(nx, ny)));
        x = nx;
        y = ny;
        break;
      }
      case 'c': {
        float ax = r.num(), ay = r.num(), bx = r.num(), by = r.num(), nx = r.num(), ny = r.num();
        begin();
        if (rel) ax += x, ay += y, bx += x, by += y, nx += x, ny += y;
        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(ax, ay), D2D1::Point2F(bx, by), D2D1::Point2F(nx, ny)));
        x = nx;
        y = ny;
        break;
      }
      case 'a': {
        float rx = r.num(), ry = r.num(), rot = r.num();
        bool large = r.flag(), sweep = r.flag();
        float nx = r.num(), ny = r.num();
        begin();
        if (rel) nx += x, ny += y;
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(nx, ny), D2D1::SizeF(rx, ry), rot,
                                      sweep ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                      large ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
        x = nx;
        y = ny;
        break;
      }
      case 'z':
        if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        open = false;
        x = sx;
        y = sy;
        cmd = 0;
        break;
      default:
        cmd = 0;
        r.p += std::strlen(r.p);  // an unknown command: stop
        break;
    }
    if (cmd && (cmd | 0x20) != 'z' && !r.more()) cmd = 0;
  }
  if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
  sink->Close();
  sink->Release();
  g_paths[svg] = geo;
  return geo;
}

D2D1_COLOR_F rgba(uint32_t rgb, float a) {
  return D2D1::ColorF(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, a);
}

// --- grain: a square of noise that repeats without a seam -------------------------------------------

constexpr int kGrain = 256;

uint32_t hash2(int x, int y, uint32_t seed) {
  uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + seed * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

// Smooth noise over the square, cx x cy cells of it.
float value_noise(float u, float v, int cx, int cy, uint32_t seed) {
  const float fx = u * cx, fy = v * cy;
  const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
  float tx = fx - x0, ty = fy - y0;
  tx = tx * tx * (3 - 2 * tx);
  ty = ty * ty * (3 - 2 * ty);
  auto at = [&](int x, int y) { return (hash2(((x % cx) + cx) % cx, ((y % cy) + cy) % cy, seed) & 0xFFFF) / 65535.0f; };
  const float a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
  return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty;
}

float smooth(float lo, float hi, float v) {
  float t = (v - lo) / (hi - lo);
  t = t < 0 ? 0 : t > 1 ? 1 : t;
  return t * t * (3 - 2 * t);
}

float grain_at(Grain kind, int x, int y) {
  const float u = x / static_cast<float>(kGrain), v = y / static_cast<float>(kGrain);
  switch (kind) {
    case Speckle:
      return (hash2(x, y, 11) & 255) / 255.0f;
    case Blotch:
      return smooth(0.36f, 0.80f, 0.50f * value_noise(u, v, 4, 4, 21) + 0.30f * value_noise(u, v, 9, 9, 22) +
                                      0.20f * value_noise(u, v, 23, 23, 23));
    case Streak:
      return smooth(0.48f, 0.92f, 0.70f * value_noise(u, v, 30, 2, 31) + 0.30f * value_noise(u, v, 61, 5, 32));
    default:  // Fibre
      return smooth(0.40f, 0.95f, 0.55f * value_noise(u, v, 5, 70, 41) + 0.25f * value_noise(u, v, 40, 9, 42) +
                                      0.20f * ((hash2(x, y, 43) & 255) / 255.0f));
  }
}

}  // namespace

bool init() {
  if (g_d2d) return true;
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d))) return false;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g_dw))))
    return false;
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
  pick_faces();
  D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                              D2D1_LINE_JOIN_ROUND);
  g_d2d->CreateStrokeStyle(sp, nullptr, 0, &g_round);
  return true;
}

void shutdown() {
  for (auto& f : g_formats) release(f.second);
  g_formats.clear();
  for (auto& p : g_paths) release(p.second);
  g_paths.clear();
  release(g_round);
  release(g_wic);
  release(g_dw);
  release(g_d2d);
}

const wchar_t* face_name(Family f) { return g_face[f].name; }
IWICImagingFactory* wic() { return g_wic; }
ID2D1Factory* d2d() { return g_d2d; }

// --- Canvas -------------------------------------------------------------------------------------

Canvas::Canvas(ID2D1RenderTarget* target) : rt(target) {
  rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush_);
  rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
}

Canvas::~Canvas() {
  for (auto& c : cache_) release(c.second);
  for (auto& g : grain_) release(g.second);
  release(brush_);
}

ID2D1SolidColorBrush* Canvas::color(uint32_t rgb, float a) {
  brush_->SetColor(rgba(rgb, a));
  return brush_;
}

void Canvas::fill(float x, float y, float w, float h, uint32_t rgb, float a) {
  if (w <= 0 || h <= 0) return;
  rt->FillRectangle(D2D1::RectF(x, y, x + w, y + h), color(rgb, a));
}

void Canvas::border(float x, float y, float w, float h, float bw, uint32_t rgb, float a) {
  fill(x, y, w, bw, rgb, a);
  fill(x, y + h - bw, w, bw, rgb, a);
  fill(x, y + bw, bw, h - 2 * bw, rgb, a);
  fill(x + w - bw, y + bw, bw, h - 2 * bw, rgb, a);
}

void Canvas::round_fill(float x, float y, float w, float h, float radius, uint32_t rgb, float a) {
  if (w <= 0 || h <= 0) return;
  rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), radius, radius), color(rgb, a));
}

void Canvas::round_line(float x, float y, float w, float h, float radius, float stroke, uint32_t rgb, float a) {
  const float half = stroke / 2;  // (the line lies inside the box)
  rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + half, y + half, x + w - half, y + h - half), radius, radius),
                           color(rgb, a), stroke);
}

void Canvas::ellipse(float cx, float cy, float rx, float ry, uint32_t rgb, float a) {
  rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), color(rgb, a));
}

void Canvas::ring(float cx, float cy, float r, float w, uint32_t rgb, float a) {
  rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), color(rgb, a), w);
}

void Canvas::poly(const D2D1_POINT_2F* pts, int n, uint32_t rgb, float a) {
  ID2D1PathGeometry* geo = nullptr;
  ID2D1GeometrySink* sink = nullptr;
  if (FAILED(g_d2d->CreatePathGeometry(&geo))) return;
  if (SUCCEEDED(geo->Open(&sink))) {
    sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLines(pts + 1, n - 1);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    sink->Release();
    rt->FillGeometry(geo, color(rgb, a));
  }
  geo->Release();
}

void Canvas::line(float x0, float y0, float x1, float y1, float w, uint32_t rgb, float a) {
  rt->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), color(rgb, a), w);
}

void Canvas::dashed_h(float x, float y, float w, float thick, float dash, float gap, uint32_t rgb, float a) {
  for (float p = 0; p < w; p += dash + gap) fill(x + p, y, p + dash > w ? w - p : dash, thick, rgb, a);
}

void Canvas::gradient(float x, float y, float w, float h, bool vertical, const Stop* stops, int n) {
  if (w <= 0 || h <= 0 || n < 2) return;
  std::vector<D2D1_GRADIENT_STOP> gs(n);
  for (int i = 0; i < n; ++i) gs[i] = {stops[i].pos, rgba(stops[i].rgb, stops[i].a)};
  ID2D1GradientStopCollection* coll = nullptr;
  ID2D1LinearGradientBrush* b = nullptr;
  if (SUCCEEDED(rt->CreateGradientStopCollection(gs.data(), n, &coll)) &&
      SUCCEEDED(rt->CreateLinearGradientBrush(
          D2D1::LinearGradientBrushProperties(D2D1::Point2F(x, y), vertical ? D2D1::Point2F(x, y + h) : D2D1::Point2F(x + w, y)), coll,
          &b)))
    rt->FillRectangle(D2D1::RectF(x, y, x + w, y + h), b);
  release(b);
  release(coll);
}

void Canvas::glow(float cx, float cy, float rx, float ry, uint32_t rgb, float a) {
  // (a soft fall: most of the light near the middle, a long faint skirt)
  const D2D1_GRADIENT_STOP gs[6] = {{0.00f, rgba(rgb, a)},         {0.18f, rgba(rgb, a * 0.80f)}, {0.40f, rgba(rgb, a * 0.44f)},
                                    {0.62f, rgba(rgb, a * 0.18f)}, {0.82f, rgba(rgb, a * 0.05f)}, {1.00f, rgba(rgb, 0)}};
  ID2D1GradientStopCollection* coll = nullptr;
  ID2D1RadialGradientBrush* b = nullptr;
  if (SUCCEEDED(rt->CreateGradientStopCollection(gs, 6, &coll)) &&
      SUCCEEDED(rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), rx, ry),
                                              coll, &b)))
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), b);
  release(b);
  release(coll);
}

void Canvas::noise(float x, float y, float w, float h, Grain kind, uint32_t rgb, float a, float scale) {
  if (w <= 0 || h <= 0 || a <= 0) return;
  const uint64_t key = (static_cast<uint64_t>(kind) << 32) | rgb;
  auto it = grain_.find(key);
  if (it == grain_.end()) {
    std::vector<uint32_t> px(kGrain * kGrain);
    const uint32_t r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    for (int j = 0; j < kGrain; ++j)
      for (int i = 0; i < kGrain; ++i) {
        const uint32_t al = static_cast<uint32_t>(grain_at(kind, i, j) * 255 + 0.5f);
        px[j * kGrain + i] = (al << 24) | ((r * al / 255) << 16) | ((g * al / 255) << 8) | (b * al / 255);  // premultiplied
      }
    ID2D1Bitmap* bmp = nullptr;
    rt->CreateBitmap(D2D1::SizeU(kGrain, kGrain), px.data(), kGrain * 4,
                     D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &bmp);
    it = grain_.emplace(key, bmp).first;
  }
  if (!it->second) return;
  ID2D1BitmapBrush* brush = nullptr;
  if (SUCCEEDED(rt->CreateBitmapBrush(
          it->second, D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
          D2D1::BrushProperties(a, Matrix::Scale(scale, scale)), &brush)))
    rt->FillRectangle(D2D1::RectF(x, y, x + w, y + h), brush);
  release(brush);
}

void Canvas::fill_path(const char* svg, uint32_t rgb, float a) {
  if (ID2D1PathGeometry* g = path(svg)) rt->FillGeometry(g, color(rgb, a));
}

void Canvas::stroke_path(const char* svg, float w, uint32_t rgb, bool round, float a) {
  if (ID2D1PathGeometry* g = path(svg)) rt->DrawGeometry(g, color(rgb, a), w, round ? g_round : nullptr);
}

void Canvas::icon(const char* svg, float x, float y, float size, uint32_t rgb, float stroke_w, float box, bool filled) {
  push(Matrix::Scale(size / box, size / box) * Matrix::Translation(x, y));
  if (filled) fill_path(svg, rgb);
  if (stroke_w > 0) stroke_path(svg, stroke_w, rgb);
  pop();
}

void Canvas::push(const Matrix& m) {
  D2D1_MATRIX_3X2_F old;
  rt->GetTransform(&old);
  stack_.push_back(Matrix(old._11, old._12, old._21, old._22, old._31, old._32));
  rt->SetTransform(m * stack_.back());
}

void Canvas::pop() {
  if (stack_.empty()) return;
  rt->SetTransform(stack_.back());
  stack_.pop_back();
}

Matrix Canvas::current() const {
  D2D1_MATRIX_3X2_F m;
  rt->GetTransform(&m);
  return Matrix(m._11, m._12, m._21, m._22, m._31, m._32);
}

void Canvas::clip(float x, float y, float w, float h) {
  rt->PushAxisAlignedClip(D2D1::RectF(x, y, x + w, y + h), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
}

void Canvas::unclip() { rt->PopAxisAlignedClip(); }

float Canvas::line_h(const Font& f) {
  Metrics m = metrics(f);
  return (m.ascent + m.descent + m.gap) * f.size;
}

IDWriteTextLayout* Canvas::layout(const std::wstring& s, const Font& f, const TextOpt& o) {
  IDWriteTextFormat* fmt = format(f);
  if (!fmt) return nullptr;
  IDWriteTextLayout* l = nullptr;
  float max_w = o.max_w > 0 ? o.max_w : 100000.0f;
  if (FAILED(g_dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt, max_w, 100000.0f, &l))) return nullptr;
  DWRITE_TEXT_RANGE all{0, static_cast<UINT32>(s.size())};
  l->SetWordWrapping(o.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
  Metrics m = metrics(f);
  float lh = o.line_h > 0 ? o.line_h : (m.ascent + m.descent + m.gap) * f.size;
  float baseline = (lh - (m.ascent + m.descent) * f.size) / 2 + m.ascent * f.size;
  l->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lh, baseline);
  if (f.spacing != 0) {
    IDWriteTextLayout1* l1 = nullptr;
    if (SUCCEEDED(l->QueryInterface(__uuidof(IDWriteTextLayout1), reinterpret_cast<void**>(&l1)))) {
      l1->SetCharacterSpacing(0, f.spacing * f.size, 0, all);
      l1->Release();
    }
  }
  if (o.ellipsis && o.max_w > 0) {
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    IDWriteInlineObject* sign = nullptr;
    if (SUCCEEDED(g_dw->CreateEllipsisTrimmingSign(fmt, &sign))) {
      l->SetTrimming(&trim, sign);
      sign->Release();
    }
  }
  if (o.underline) l->SetUnderline(TRUE, all);
  if (o.center && o.max_w > 0) l->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  return l;
}

D2D1_SIZE_F Canvas::measure(const std::wstring& s, const Font& f, const TextOpt& o) {
  D2D1_SIZE_F out{0, 0};
  if (IDWriteTextLayout* l = layout(s, f, o)) {
    DWRITE_TEXT_METRICS tm{};
    l->GetMetrics(&tm);
    out.width = tm.widthIncludingTrailingWhitespace;
    out.height = tm.height;
    l->Release();
  }
  return out;
}

float Canvas::text(const std::wstring& s, const Font& f, float x, float y, uint32_t rgb, const TextOpt& o, float a) {
  float h = 0;
  if (IDWriteTextLayout* l = layout(s, f, o)) {
    DWRITE_TEXT_METRICS tm{};
    l->GetMetrics(&tm);
    h = tm.height;
    rt->DrawTextLayout(D2D1::Point2F(x, y), l, color(rgb, a), D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
    l->Release();
  }
  return h;
}

float Canvas::text_mid(const std::wstring& s, const Font& f, float x, float cy, uint32_t rgb, const TextOpt& o, float a) {
  float lh = o.line_h > 0 ? o.line_h : line_h(f);
  text(s, f, x, cy - lh / 2, rgb, o, a);
  return lh;
}

void Canvas::cached(int key, float w, float h, const std::function<void(Canvas&)>& paint) {
  auto it = cache_.find(key);
  if (it == cache_.end()) {
    ID2D1BitmapRenderTarget* brt = nullptr;
    if (FAILED(rt->CreateCompatibleRenderTarget(D2D1::SizeF(w, h), &brt))) {
      paint(*this);
      return;
    }
    {
      Canvas sub(brt);
      brt->BeginDraw();
      brt->Clear(D2D1::ColorF(0, 0, 0, 0));
      paint(sub);
      brt->EndDraw();
    }
    it = cache_.emplace(key, brt).first;
  }
  ID2D1Bitmap* bmp = nullptr;
  if (SUCCEEDED(it->second->GetBitmap(&bmp))) {
    rt->DrawBitmap(bmp, D2D1::RectF(0, 0, w, h), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    bmp->Release();
  }
}

long g_last_hr = 0;
int g_last_step = 0;

bool save_png(const wchar_t* file, int w, int h, float scale, const std::function<void(Canvas&)>& paint) {
  g_last_step = 1;
  if (!g_wic || !g_d2d) return false;
  UINT pw = static_cast<UINT>(w * scale + 0.5f), ph = static_cast<UINT>(h * scale + 0.5f);
  IWICBitmap* bmp = nullptr;
  ID2D1RenderTarget* rt = nullptr;
  IWICStream* stream = nullptr;
  IWICBitmapEncoder* enc = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;
  bool ok = false;
  auto step = [](int n, HRESULT hr) {
    if (FAILED(hr)) g_last_step = n, g_last_hr = hr;
    return SUCCEEDED(hr);
  };
  if (step(2, g_wic->CreateBitmap(pw, ph, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)) &&
      step(3, g_d2d->CreateWicBitmapRenderTarget(
                  bmp,
                  D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                               D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                               96.0f * scale, 96.0f * scale),
                  &rt))) {
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 1));
    {
      Canvas c(rt);
      paint(c);
    }
    ok = step(4, rt->EndDraw());
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (file)
      ok = ok && step(5, g_wic->CreateStream(&stream)) && step(6, stream->InitializeFromFilename(file, GENERIC_WRITE)) &&
           step(7, g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
           step(8, enc->Initialize(stream, WICBitmapEncoderNoCache)) && step(9, enc->CreateNewFrame(&frame, nullptr)) &&
           step(10, frame->Initialize(nullptr)) && step(11, frame->SetSize(pw, ph)) && step(12, frame->SetPixelFormat(&fmt)) &&
           step(13, frame->WriteSource(bmp, nullptr)) && step(14, frame->Commit()) && step(15, enc->Commit());
  }
  release(frame);
  release(enc);
  release(stream);
  release(rt);
  release(bmp);
  return ok;
}

}  // namespace gfx
