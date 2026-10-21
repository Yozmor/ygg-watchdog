// ui.h - мини-библиотека отрисовки окон ygg-watchdog на Direct2D/DirectWrite.
// Стиль: тёмный фон, тонкие неоновые линии, уголки-скобки у панелей,
// кнопки со срезанными углами, моноширинный шрифт, лёгкое свечение.
// Всё рисуется в "дизайн-координатах" (1040x640 у окна сторожа) и
// масштабируется под реальный размер окна одной матрицей.
#pragma once
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>
#include <string>
#include <vector>
#include <map>
#include <cmath>

namespace ui {

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

inline D2D1_COLOR_F Rgb(unsigned rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, a);
}

// Палитры: зелёная (норма), янтарная (обходной путь), красная (беда),
// голубая (регион, панель Иггдрасиля).
struct Theme {
    unsigned accent;   // основной неон
    unsigned text;     // светлый текст
    unsigned muted;    // приглушённый текст
    unsigned bg;       // фон окна
    unsigned panel;    // заливка панелей
};
inline Theme ThemeGreen() { return { 0x3DFF9A, 0xD9F7E8, 0x6F8F80, 0x040A08, 0x07130F }; }
inline Theme ThemeAmber() { return { 0xFFB23D, 0xF7EBD9, 0x8F806B, 0x0A0805, 0x141009 }; }
inline Theme ThemeRed()   { return { 0xFF4D6A, 0xF7DCE0, 0x8F6B72, 0x0B0506, 0x150A0C }; }
inline Theme ThemeBlue()  { return { 0x6EC8FF, 0xDCEFFA, 0x6B8494, 0x040810, 0x081220 }; }
inline Theme ThemeGray()  { return { 0x9AA7B0, 0xE0E6EA, 0x6B757C, 0x07090A, 0x0E1113 }; }

const unsigned COL_OK    = 0x3DFF9A;
const unsigned COL_WARN  = 0xFFB23D;
const unsigned COL_ALERT = 0xFF4D6A;
const unsigned COL_INFO  = 0x6EC8FF;
const unsigned COL_PINK  = 0xFF5FD2;

enum Align { Left, Center, Right };

// Серая картинка (8 бит на пиксель), загруженная из ресурса exe через WIC
struct GrayImage {
    UINT w = 0, h = 0;
    std::vector<BYTE> px;
    bool ok() const { return w > 0 && h > 0 && px.size() == (size_t)w * h; }
};

inline GrayImage LoadGrayPngResource(int resId) {
    GrayImage img;
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(resId), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
    if (!res) return img;
    HGLOBAL hg = LoadResource(nullptr, res);
    DWORD size = SizeofResource(nullptr, res);
    void* data = hg ? LockResource(hg) : nullptr;
    if (!data || !size) return img;

    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWICImagingFactory, (void**)&wic)) || !wic) return img;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory((BYTE*)data, size)) &&
        SUCCEEDED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) &&
        SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat8bppGray, WICBitmapDitherTypeNone,
                                   nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        UINT w = 0, h = 0;
        conv->GetSize(&w, &h);
        img.px.resize((size_t)w * h);
        if (SUCCEEDED(conv->CopyPixels(nullptr, w, (UINT)img.px.size(), img.px.data()))) { img.w = w; img.h = h; }
        else img.px.clear();
    }
    SafeRelease(conv); SafeRelease(frame); SafeRelease(dec); SafeRelease(stream); SafeRelease(wic);
    return img;
}

// Простая зона нажатия
struct HitBox {
    float x = 0, y = 0, w = 0, h = 0;
    int id = 0;
    bool contains(D2D1_POINT_2F p) const { return p.x >= x && p.x <= x + w && p.y >= y && p.y <= y + h; }
};

// ------------------------------------------------------------------
// Поле ввода (одна строка). Ввод обрабатывает окно: OnChar / OnKey.
// ------------------------------------------------------------------
struct TextBox {
    int id = 0;
    std::wstring text, placeholder;
    size_t caret = 0;
    bool focus = false;
    bool password = false;   // показывать точки вместо текста (токены)
    float x = 0, y = 0, w = 0, h = 30;

    void OnChar(wchar_t c) {
        if (c < 32 || c == 127) return;
        text.insert(text.begin() + caret, c);
        ++caret;
    }
    // true - клавиша обработана
    bool OnKey(WPARAM vk, bool ctrl) {
        if (vk == VK_BACK) { if (caret > 0) { text.erase(caret - 1, 1); --caret; } return true; }
        if (vk == VK_DELETE) { if (caret < text.size()) text.erase(caret, 1); return true; }
        if (vk == VK_LEFT) { if (caret > 0) --caret; return true; }
        if (vk == VK_RIGHT) { if (caret < text.size()) ++caret; return true; }
        if (vk == VK_HOME) { caret = 0; return true; }
        if (vk == VK_END) { caret = text.size(); return true; }
        if (ctrl && vk == 'V') { Paste(); return true; }
        if (ctrl && vk == 'A') { caret = text.size(); return true; }
        return false;
    }
    void Paste() {
        if (!OpenClipboard(nullptr)) return;
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            const wchar_t* s = (const wchar_t*)GlobalLock(h);
            if (s) {
                std::wstring add;
                for (const wchar_t* q = s; *q; ++q) if (*q >= 32) add += *q;
                text.insert(caret, add);
                caret += add.size();
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
    }
    void Set(const std::wstring& s) { text = s; caret = s.size(); }
    std::wstring Trimmed() const {
        size_t a = text.find_first_not_of(L" \t"), b = text.find_last_not_of(L" \t");
        return a == std::wstring::npos ? L"" : text.substr(a, b - a + 1);
    }
    HitBox Hit() const { return { x, y, w, h, id }; }
};

// ------------------------------------------------------------------
// Список с выбором и прокруткой колесом.
// ------------------------------------------------------------------
struct ListItem {
    std::wstring left, right;
    unsigned color = 0;       // 0 - цвет текста темы
    unsigned rightColor = 0;  // 0 - приглушённый
    std::wstring key;         // значение для действий (uri, slug, имя...)
};

struct ListBox {
    int id = 0;
    std::vector<ListItem> items;
    int sel = -1;
    int scroll = 0;
    float x = 0, y = 0, w = 0, h = 0, rowH = 24;
    std::wstring emptyText = L"пусто";
    // Режим переноса текста: у строк разная высота (её считает экран и кладёт в hts,
    // по одному значению на строку). Пока hts пуст - обычные строки высотой rowH.
    std::vector<float> hts;

    bool Wrap() const { return !hts.empty() && hts.size() == items.size(); }
    float H(int i) const { return Wrap() ? hts[i] : rowH; }
    // сколько строк помещается, начиная с первой видимой
    int Visible() const {
        if (!Wrap()) return (int)(h / rowH);
        float used = 0; int n = 0;
        for (int i = scroll; i < (int)items.size(); ++i) { if (used + hts[i] > h && n > 0) break; used += hts[i]; ++n; }
        return (std::max)(1, n);
    }
    void Clamp() {
        int maxScroll;
        if (Wrap()) {
            float used = 0; int i = (int)items.size();
            while (i > 0 && used + hts[i - 1] <= h) { used += hts[i - 1]; --i; }
            maxScroll = i;   // с этой строки и до конца всё помещается
        } else maxScroll = (std::max)(0, (int)items.size() - Visible());
        if (scroll > maxScroll) scroll = maxScroll;
        if (scroll < 0) scroll = 0;
        if (sel >= (int)items.size()) sel = (int)items.size() - 1;
    }
    void Wheel(int delta) { scroll -= delta / 40; Clamp(); }
    bool ClickAt(D2D1_POINT_2F p) {
        if (p.x < x || p.x > x + w || p.y < y || p.y > y + h) return false;
        float yy = y;
        for (int i = scroll; i < (int)items.size(); ++i) {
            if (p.y < yy + H(i)) { sel = i; break; }
            yy += H(i);
        }
        return true;
    }
    void Move(int d) {
        if (items.empty()) return;
        sel = sel < 0 ? 0 : sel + d;
        if (sel < 0) sel = 0;
        if (sel >= (int)items.size()) sel = (int)items.size() - 1;
        if (sel < scroll) scroll = sel;
        if (Wrap()) {
            for (;;) {   // двигаем начало, пока выбранная строка не влезет целиком
                float used = 0;
                for (int i = scroll; i <= sel; ++i) used += hts[i];
                if (used <= h || scroll >= sel) break;
                ++scroll;
            }
        } else if (sel >= scroll + Visible()) scroll = sel - Visible() + 1;
        Clamp();
    }
    const ListItem* Selected() const { return (sel >= 0 && sel < (int)items.size()) ? &items[sel] : nullptr; }
    // Заменить содержимое, сохранив выбор по ключу
    void SetItems(const std::vector<ListItem>& v) {
        std::wstring keep = Selected() ? Selected()->key : L"";
        items = v;
        sel = -1;
        if (!keep.empty()) for (size_t i = 0; i < items.size(); ++i) if (items[i].key == keep) { sel = (int)i; break; }
        Clamp();
    }
};


class Renderer {
public:
    ID2D1Factory* factory = nullptr;
    IDWriteFactory* dwrite = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    HWND hwnd = nullptr;
    float scale = 1.0f, offX = 0, offY = 0; // дизайн -> DIP
    float designW = 1040, designH = 640;
    std::wstring monoFamily = L"Consolas";

    bool Init(HWND h) {
        hwnd = h;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory))) return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                        reinterpret_cast<IUnknown**>(&dwrite)))) return false;
        // Cascadia Mono есть в Windows 11; если нет - Consolas
        IDWriteFontCollection* coll = nullptr;
        if (SUCCEEDED(dwrite->GetSystemFontCollection(&coll, FALSE)) && coll) {
            UINT32 idx; BOOL exists = FALSE;
            if (SUCCEEDED(coll->FindFamilyName(L"Cascadia Mono", &idx, &exists)) && exists) monoFamily = L"Cascadia Mono";
            coll->Release();
        }
        return true;
    }

    bool EnsureTarget() {
        if (rt) return true;
        RECT rc; GetClientRect(hwnd, &rc);
        D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
        HRESULT hr = factory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
                                                     D2D1::HwndRenderTargetProperties(hwnd, size), &rt);
        if (FAILED(hr)) return false;
        UINT dpi = GetDpiForWindow(hwnd);
        rt->SetDpi((float)dpi, (float)dpi);
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush);
        return true;
    }

    void DiscardTarget() { SafeRelease(bgBitmap); SafeRelease(brush); SafeRelease(rt); ClearFonts(); }

    // ---------------- фон-древо ----------------
    // Серая текстура перекрашивается в цвет темы на процессоре (один раз
    // при смене темы): пиксель = фон + яркость * акцент * сила.
    GrayImage bgGray;
    ID2D1Bitmap* bgBitmap = nullptr;
    unsigned bgForAccent = 0xFFFFFFFF, bgForBack = 0xFFFFFFFF;
    float bgStrength = 0.34f;

    void Background(const Theme& th) {
        if (!bgGray.ok()) return;
        if (!bgBitmap || bgForAccent != th.accent || bgForBack != th.bg) {
            SafeRelease(bgBitmap);
            std::vector<BYTE> bgra((size_t)bgGray.w * bgGray.h * 4);
            float ar = ((th.accent >> 16) & 255) * bgStrength, ag = ((th.accent >> 8) & 255) * bgStrength, ab = (th.accent & 255) * bgStrength;
            float br = (float)((th.bg >> 16) & 255), bgc = (float)((th.bg >> 8) & 255), bb = (float)(th.bg & 255);
            BYTE lut[256][3];
            for (int v = 0; v < 256; ++v) {
                float k = (v - 20) / 170.0f;   // 20 - фон текстуры, ~190 - самые светлые грани
                if (k < 0) k = 0; if (k > 1.2f) k = 1.2f;
                float r = br + ar * k, g = bgc + ag * k, b = bb + ab * k;
                lut[v][0] = (BYTE)(b > 255 ? 255 : b); lut[v][1] = (BYTE)(g > 255 ? 255 : g); lut[v][2] = (BYTE)(r > 255 ? 255 : r);
            }
            for (size_t i = 0; i < bgGray.px.size(); ++i) {
                const BYTE* c = lut[bgGray.px[i]];
                bgra[i * 4 + 0] = c[0]; bgra[i * 4 + 1] = c[1]; bgra[i * 4 + 2] = c[2]; bgra[i * 4 + 3] = 255;
            }
            D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
            rt->CreateBitmap(D2D1::SizeU(bgGray.w, bgGray.h), bgra.data(), bgGray.w * 4, props, &bgBitmap);
            bgForAccent = th.accent; bgForBack = th.bg;
        }
        if (bgBitmap) rt->DrawBitmap(bgBitmap, D2D1::RectF(0, 0, designW, designH), 1.0f,
                                     D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    void Resize() {
        if (!rt) return;
        RECT rc; GetClientRect(hwnd, &rc);
        rt->Resize(D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top));
        UINT dpi = GetDpiForWindow(hwnd);
        rt->SetDpi((float)dpi, (float)dpi);
    }

    // Вписать дизайн-холст в окно с сохранением пропорций
    void BeginFrame(unsigned bgColor) {
        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->Clear(Rgb(bgColor));
        D2D1_SIZE_F sz = rt->GetSize();
        scale = (std::min)(sz.width / designW, sz.height / designH);
        offX = (sz.width - designW * scale) / 2;
        offY = (sz.height - designH * scale) / 2;
        rt->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(offX, offY));
    }
    bool EndFrame() {
        HRESULT hr = rt->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) { DiscardTarget(); return false; }
        return true;
    }

    // Пиксели окна -> дизайн-координаты (для мыши)
    D2D1_POINT_2F ToDesign(int px, int py) {
        UINT dpi = GetDpiForWindow(hwnd);
        float x = px * 96.0f / dpi, y = py * 96.0f / dpi;
        return D2D1::Point2F((x - offX) / scale, (y - offY) / scale);
    }

    // ---------------- шрифты и текст ----------------
    IDWriteTextFormat* Font(float size, bool bold = false, bool mono = true) {
        long long key = (long long)(size * 10) * 4 + (bold ? 2 : 0) + (mono ? 1 : 0);
        auto it = fonts.find(key);
        if (it != fonts.end()) return it->second;
        IDWriteTextFormat* f = nullptr;
        dwrite->CreateTextFormat(mono ? monoFamily.c_str() : L"Segoe UI", nullptr,
                                 bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"ru-ru", &f);
        if (f) f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fonts[key] = f;
        return f;
    }

    void Text(const std::wstring& s, float x, float y, float w, float h, D2D1_COLOR_F color,
              float size, bool bold = false, Align align = Left, bool vcenter = true, bool mono = true) {
        IDWriteTextFormat* f = Font(size, bold, mono);
        if (!f) return;
        f->SetTextAlignment(align == Left ? DWRITE_TEXT_ALIGNMENT_LEADING :
                            align == Center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_TRAILING);
        f->SetParagraphAlignment(vcenter ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        brush->SetColor(color);
        rt->DrawText(s.c_str(), (UINT32)s.size(), f, D2D1::RectF(x, y, x + w, y + h), brush,
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    // Текст с переносом строк; возвращает занятую высоту
    float TextWrapped(const std::wstring& s, float x, float y, float w, D2D1_COLOR_F color,
                      float size, bool bold = false, bool draw = true) {
        IDWriteTextFormat* f = Font(size, bold, true);
        if (!f) return 0;
        IDWriteTextLayout* layout = nullptr;
        dwrite->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, w, 1000, &layout);
        if (!layout) return 0;
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        DWRITE_TEXT_METRICS m = {};
        layout->GetMetrics(&m);
        if (draw) {
            brush->SetColor(color);
            rt->DrawTextLayout(D2D1::Point2F(x, y), layout, brush);
        }
        layout->Release();
        return m.height;
    }

    float TextWidth(const std::wstring& s, float size, bool bold = false) {
        IDWriteTextFormat* f = Font(size, bold, true);
        IDWriteTextLayout* layout = nullptr;
        dwrite->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 5000, 100, &layout);
        if (!layout) return 0;
        DWRITE_TEXT_METRICS m = {};
        layout->GetMetrics(&m);
        layout->Release();
        return m.widthIncludingTrailingWhitespace;
    }

    // ---------------- примитивы ----------------
    void Line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float width = 1.0f) {
        brush->SetColor(c);
        rt->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush, width);
    }
    void Fill(float x, float y, float w, float h, D2D1_COLOR_F c) {
        brush->SetColor(c);
        rt->FillRectangle(D2D1::RectF(x, y, x + w, y + h), brush);
    }
    void Frame(float x, float y, float w, float h, D2D1_COLOR_F c, float width = 1.0f) {
        brush->SetColor(c);
        rt->DrawRectangle(D2D1::RectF(x, y, x + w, y + h), brush, width);
    }
    void Circle(float cx, float cy, float r, D2D1_COLOR_F c, float width = 1.0f) {
        brush->SetColor(c);
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush, width);
    }
    void Disc(float cx, float cy, float r, D2D1_COLOR_F c) {
        brush->SetColor(c);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush);
    }
    // Залитый сектор круга (луч радара), углы в градусах
    void Wedge(float cx, float cy, float rad, float a0, float a1, D2D1_COLOR_F c) {
        ID2D1PathGeometry* g = nullptr;
        factory->CreatePathGeometry(&g);
        if (!g) return;
        ID2D1GeometrySink* s = nullptr;
        g->Open(&s);
        const float k = 3.14159265f / 180.0f;
        s->BeginFigure(D2D1::Point2F(cx, cy), D2D1_FIGURE_BEGIN_FILLED);
        s->AddLine(D2D1::Point2F(cx + rad * cosf(a0 * k), cy + rad * sinf(a0 * k)));
        s->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + rad * cosf(a1 * k), cy + rad * sinf(a1 * k)),
                                   D2D1::SizeF(rad, rad), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
        s->Close();
        s->Release();
        brush->SetColor(c);
        rt->FillGeometry(g, brush);
        g->Release();
    }

    // Свечение: несколько расширяющихся полупрозрачных контуров
    void GlowCircle(float cx, float cy, float r, unsigned rgb, float alpha = 1.0f) {
        for (int i = 3; i >= 1; --i) Circle(cx, cy, r + i * 1.6f, Rgb(rgb, 0.07f * alpha), 2.0f);
    }

    // Многоугольник со срезанными углами (кнопки, плашки)
    ID2D1PathGeometry* CutRect(float x, float y, float w, float h, float cut) {
        ID2D1PathGeometry* g = nullptr;
        factory->CreatePathGeometry(&g);
        if (!g) return nullptr;
        ID2D1GeometrySink* s = nullptr;
        g->Open(&s);
        s->BeginFigure(D2D1::Point2F(x + cut, y), D2D1_FIGURE_BEGIN_FILLED);
        D2D1_POINT_2F pts[] = {
            D2D1::Point2F(x + w - cut, y), D2D1::Point2F(x + w, y + cut),
            D2D1::Point2F(x + w, y + h - cut), D2D1::Point2F(x + w - cut, y + h),
            D2D1::Point2F(x + cut, y + h), D2D1::Point2F(x, y + h - cut),
            D2D1::Point2F(x, y + cut) };
        s->AddLines(pts, 7);
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
        s->Close();
        s->Release();
        return g;
    }

    // Панель: еле заметная заливка + уголки-скобки + заголовок
    void Panel(float x, float y, float w, float h, const Theme& th, const std::wstring& title = L"",
               float alpha = 0.42f) {
        Fill(x, y, w, h, Rgb(th.panel, alpha));
        Brackets(x, y, w, h, Rgb(th.accent, 0.9f), 16, 2.0f);
        if (!title.empty()) Text(title, x + 16, y + 10, w - 32, 18, Rgb(th.accent, 0.85f), 11);
    }

    void Brackets(float x, float y, float w, float h, D2D1_COLOR_F c, float len, float width) {
        Line(x, y, x + len, y, c, width);           Line(x, y, x, y + len, c, width);
        Line(x + w, y, x + w - len, y, c, width);   Line(x + w, y, x + w, y + len, c, width);
        Line(x, y + h, x + len, y + h, c, width);   Line(x, y + h, x, y + h - len, c, width);
        Line(x + w, y + h, x + w - len, y + h, c, width); Line(x + w, y + h, x + w, y + h - len, c, width);
    }

    // Кнопка: filled - залитая неоном (главное действие), иначе контурная
    void Button(float x, float y, float w, float h, const std::wstring& label, unsigned rgb,
                bool filled, bool hover, bool pressed, unsigned darkText = 0x061009) {
        ID2D1PathGeometry* g = CutRect(x, y, w, h, 10);
        if (!g) return;
        if (filled) {
            for (int i = 3; i >= 1; --i) {  // ореол
                ID2D1PathGeometry* gg = CutRect(x - i * 2.0f, y - i * 2.0f, w + i * 4.0f, h + i * 4.0f, 10 + i);
                if (gg) { brush->SetColor(Rgb(rgb, hover ? 0.10f : 0.06f)); rt->FillGeometry(gg, brush); gg->Release(); }
            }
            brush->SetColor(Rgb(rgb, pressed ? 0.75f : (hover ? 1.0f : 0.9f)));
            rt->FillGeometry(g, brush);
            brush->SetColor(Rgb(0xFFFFFF, 0.35f));
            rt->DrawGeometry(g, brush, 1.0f);
            Text(label, x, y, w, h, Rgb(darkText), 13, true, Center);
        } else {
            brush->SetColor(Rgb(rgb, hover ? 0.14f : 0.05f));
            rt->FillGeometry(g, brush);
            brush->SetColor(Rgb(rgb, hover ? 1.0f : 0.75f));
            rt->DrawGeometry(g, brush, 1.5f);
            Text(label, x, y, w, h, Rgb(rgb), 13, false, Center);
        }
        g->Release();
    }

    // Полоса прогресса с "делениями"
    void Progress(float x, float y, float w, float h, float frac, unsigned rgb) {
        if (frac < 0) frac = 0; if (frac > 1) frac = 1;
        Fill(x, y, w, h, Rgb(rgb, 0.12f));
        Fill(x, y, w * frac, h, Rgb(rgb, 0.85f));
        for (float t = x + 6; t < x + w; t += 6) Line(t, y, t, y + h, Rgb(0x000000, 0.25f), 1.0f);
    }

    // Фон: сетка + сканлайны (очень слабые, чтобы не мешать тексту)
    void Backdrop(const Theme& th) {
        Background(th);
        for (float gx = 0; gx <= designW; gx += 40) Line(gx, 0, gx, designH, Rgb(th.accent, 0.018f));
        for (float gy = 0; gy <= designH; gy += 40) Line(0, gy, designW, gy, Rgb(th.accent, 0.018f));
        for (float sy = 0; sy <= designH; sy += 3) Line(0, sy, designW, sy, Rgb(0x000000, 0.18f), 1.0f);
    }

    // ---------------- виджеты ----------------
    void Clip(float x, float y, float w, float h) { rt->PushAxisAlignedClip(D2D1::RectF(x, y, x + w, y + h), D2D1_ANTIALIAS_MODE_ALIASED); }
    void Unclip() { rt->PopAxisAlignedClip(); }

    void DrawTextBox(const TextBox& tb, const Theme& th) {
        Fill(tb.x, tb.y, tb.w, tb.h, Rgb(th.panel, 0.85f));
        Frame(tb.x, tb.y, tb.w, tb.h, Rgb(th.accent, tb.focus ? 0.95f : 0.45f), tb.focus ? 1.5f : 1.0f);
        float pad = 8, avail = tb.w - 2 * pad;
        Clip(tb.x + 2, tb.y, tb.w - 4, tb.h);
        if (tb.text.empty() && !tb.focus) {
            Text(tb.placeholder, tb.x + pad, tb.y, avail, tb.h, Rgb(th.muted, 0.8f), 12);
        } else {
            std::wstring shown = tb.password ? std::wstring(tb.text.size(), L'\x2022') : tb.text;
            float caretX = TextWidth(shown.substr(0, tb.caret), 12);
            float shift = caretX > avail ? caretX - avail : 0;
            Text(shown, tb.x + pad - shift, tb.y, 4000, tb.h, Rgb(th.text), 12);
            if (tb.focus && (GetTickCount() / 530) % 2 == 0) {
                float cx = tb.x + pad - shift + caretX;
                Line(cx, tb.y + 7, cx, tb.y + tb.h - 7, Rgb(th.accent), 1.5f);
            }
        }
        Unclip();
    }

    void DrawList(const ListBox& lb, const Theme& th, bool focused) {
        Fill(lb.x, lb.y, lb.w, lb.h, Rgb(th.panel, 0.55f));
        Frame(lb.x, lb.y, lb.w, lb.h, Rgb(th.accent, focused ? 0.6f : 0.25f));
        if (lb.items.empty()) {
            Text(lb.emptyText, lb.x + 10, lb.y + 6, lb.w - 20, 20, Rgb(th.muted), 11, false, Left, false);
            return;
        }
        Clip(lb.x + 1, lb.y + 1, lb.w - 2, lb.h - 2);
        if (lb.Wrap()) {
            float ry = lb.y;
            for (int idx = lb.scroll; idx < (int)lb.items.size() && ry < lb.y + lb.h; ++idx) {
                const ListItem& it = lb.items[idx];
                float hh = lb.hts[idx];
                if (idx == lb.sel) {
                    Fill(lb.x + 1, ry, lb.w - 2, hh, Rgb(th.accent, 0.16f));
                    Fill(lb.x + 1, ry, 3, hh, Rgb(th.accent));
                }
                TextWrapped(it.left, lb.x + 10, ry + 5, lb.w - 28, Rgb(it.color ? it.color : th.text), 12);
                Line(lb.x + 8, ry + hh - 0.5f, lb.x + lb.w - 8, ry + hh - 0.5f, Rgb(th.accent, 0.08f));
                ry += hh;
            }
            Unclip();
            int total = (int)lb.items.size(), shown = lb.Visible();
            if (total > shown) {
                float trackH = lb.h - 4;
                float thumbH = (std::max)(16.0f, trackH * shown / total);
                int maxS = (std::max)(1, total - shown);
                float thumbY = lb.y + 2 + (trackH - thumbH) * (std::min)(lb.scroll, maxS) / (float)maxS;
                Fill(lb.x + lb.w - 4, thumbY, 2, thumbH, Rgb(th.accent, 0.6f));
            }
            return;
        }
        int vis = lb.Visible() + 1;
        for (int i = 0; i < vis && lb.scroll + i < (int)lb.items.size(); ++i) {
            int idx = lb.scroll + i;
            const ListItem& it = lb.items[idx];
            float ry = lb.y + i * lb.rowH;
            if (idx == lb.sel) {
                Fill(lb.x + 1, ry, lb.w - 2, lb.rowH, Rgb(th.accent, 0.16f));
                Fill(lb.x + 1, ry, 3, lb.rowH, Rgb(th.accent));
            }
            float rightW = it.right.empty() ? 0 : (std::min)(TextWidth(it.right, 11) + 12, lb.w * 0.5f);
            Text(it.left, lb.x + 10, ry, lb.w - 20 - rightW, lb.rowH, Rgb(it.color ? it.color : th.text), 12);
            if (!it.right.empty())
                Text(it.right, lb.x + lb.w - rightW - 12, ry, rightW, lb.rowH, Rgb(it.rightColor ? it.rightColor : th.muted), 11, false, Right);
        }
        Unclip();
        // полоса прокрутки
        int total = (int)lb.items.size(), shown = lb.Visible();
        if (total > shown) {
            float trackH = lb.h - 4;
            float thumbH = (std::max)(16.0f, trackH * shown / total);
            float thumbY = lb.y + 2 + (trackH - thumbH) * lb.scroll / (float)(total - shown);
            Fill(lb.x + lb.w - 4, thumbY, 2, thumbH, Rgb(th.accent, 0.6f));
        }
    }

private:
    std::map<long long, IDWriteTextFormat*> fonts;
    void ClearFonts() { for (auto& kv : fonts) SafeRelease(kv.second); fonts.clear(); }
};

} // namespace ui
