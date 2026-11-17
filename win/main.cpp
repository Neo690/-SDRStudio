// SDRStudio - Windows front-end: native Win32 UI, GDI DIB rendering, WASAPI audio.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../core/dsp.h"
#include "../core/recorder.h"
#include "../core/selftest.h"
#include "../core/source.h"
#include "audio_wasapi.h"

namespace {

constexpr wchar_t kWindowClass[] = L"SDRStudioWindow";
constexpr wchar_t kWindowTitle[] = L"SDR Studio - recepteur RTL-SDR";
constexpr UINT_PTR kTimerId = 1;
constexpr const wchar_t* kVersion = L"1.0";

constexpr int kHeaderH = 76;
constexpr int kStatusH = 30;
constexpr int kMargin = 8;
constexpr float kMinDb = -110.0f;
constexpr float kMaxDb = -15.0f;

// ------------------------------------------------------------------ graphics

struct Bitmap {
    HBITMAP bmp = nullptr;
    HDC dc = nullptr;
    uint32_t* px = nullptr;
    int w = 0;
    int h = 0;

    void destroy() {
        if (dc) DeleteDC(dc);
        if (bmp) DeleteObject(bmp);
        bmp = nullptr;
        dc = nullptr;
        px = nullptr;
        w = h = 0;
    }

    bool create(int width, int height) {
        destroy();
        if (width <= 0 || height <= 0) return false;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -height; // top-down rows
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits) return false;
        dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;
        SelectObject(dc, bmp);
        px = static_cast<uint32_t*>(bits);
        w = width;
        h = height;
        return true;
    }
};

inline uint32_t rgb(int r, int g, int b) {
    return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

inline uint32_t scaleColor(uint32_t c, float k) {
    const int r = std::clamp(static_cast<int>(((c >> 16) & 0xFF) * k), 0, 255);
    const int g = std::clamp(static_cast<int>(((c >> 8) & 0xFF) * k), 0, 255);
    const int b = std::clamp(static_cast<int>((c & 0xFF) * k), 0, 255);
    return rgb(r, g, b);
}

uint32_t colorForDb(float db) {
    // Thermal style ramp: deep blue -> cyan -> green -> yellow -> white.
    struct Stop { float db; uint32_t color; };
    static const Stop stops[] = {
        {-110.0f, rgb(6, 10, 26)},
        {-90.0f, rgb(12, 32, 96)},
        {-72.0f, rgb(10, 92, 160)},
        {-56.0f, rgb(12, 168, 168)},
        {-42.0f, rgb(60, 200, 110)},
        {-30.0f, rgb(230, 214, 76)},
        {-20.0f, rgb(255, 150, 50)},
        {-13.0f, rgb(255, 250, 235)},
    };
    const int count = static_cast<int>(sizeof(stops) / sizeof(stops[0]));
    if (db <= stops[0].db) return stops[0].color;
    if (db >= stops[count - 1].db) return stops[count - 1].color;
    for (int i = 1; i < count; ++i) {
        if (db <= stops[i].db) {
            const float t = (db - stops[i - 1].db) / (stops[i].db - stops[i - 1].db);
            const uint32_t a = stops[i - 1].color;
            const uint32_t b = stops[i].color;
            auto mix = [t](int ca, int cb) {
                return std::clamp(static_cast<int>(ca + (cb - ca) * t), 0, 255);
            };
            return rgb(mix((a >> 16) & 0xFF, (b >> 16) & 0xFF),
                       mix((a >> 8) & 0xFF, (b >> 8) & 0xFF),
                       mix(a & 0xFF, b & 0xFF));
        }
    }
    return stops[count - 1].color;
}

std::string formatHz(double hz) {
    char buf[64];
    const long long v = static_cast<long long>(std::llround(hz));
    if (v >= 1000000)
        std::snprintf(buf, sizeof(buf), "%lld.%06lld MHz", v / 1000000, v % 1000000);
    else
        std::snprintf(buf, sizeof(buf), "%.3f kHz", hz / 1000.0);
    return buf;
}

std::string formatHzLong(double hz) {
    char buf[64];
    const long long v = static_cast<long long>(std::llround(hz));
    std::snprintf(buf, sizeof(buf), "%lld.%03lld.%03lld Hz", v / 1000000, (v / 1000) % 1000, v % 1000);
    return buf;
}

// Decalage dans la bande d'un fichier rejoue : "+125.0 kHz".
std::string formatOffset(double hz) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%+.1f kHz", hz / 1000.0);
    return buf;
}

// ---------------------------------------------------------------------- app

struct App {
    HWND hwnd = nullptr;
    Bitmap back;
    Bitmap waterfall;
    HFONT fontTiny = nullptr;
    HFONT fontSmall = nullptr;
    HFONT fontMed = nullptr;
    HFONT fontBig = nullptr;

    std::unique_ptr<sdr::IqSource> source;
    std::unique_ptr<sdr::Receiver> rx;
    std::unique_ptr<sdr::SpectrumAnalyzer> spec;
    sdr::SpscRing<sdr::cfloat> iqRing{1 << 19};
    AudioOutput audio;
    sdr::WavWriter wav;
    sdr::IqWriter iqWriter;

    std::thread captureThread;
    std::thread dspThread;
    std::atomic<bool> stopRequested{false};
    std::atomic<double> wantCenter{101.5e6};
    std::atomic<double> wantGain{-1.0};
    std::atomic<int> wantPpm{0};

    std::mutex rxMutex;
    std::mutex recordMutex;

    std::atomic<uint64_t> samplesIn{0};
    double measuredMsps = 0.0;
    uint64_t lastSamples = 0;
    DWORD lastTick = 0;
    uint64_t lastFftFrame = 0;

    bool isFile = false;      // rejeu : les fleches deplacent le canal dans la bande
    double bandOffset = 0.0;
    double sourceRate = 2.4e6;
    double centerHz = 101.5e6;
    double appliedCenter = -1.0;
    sdr::Mode mode = sdr::Mode::Wbfm;
    float volume = 0.7f;
    bool squelchOn = false;
    double squelchDb = -75.0;
    int ppm = 0;
    double gainDb = -1.0;
    double channelDb = -200.0;
    double audioPeak = 0.0;

    std::string sourceName = "aucune";
    std::string note = "pret";
    std::string errorText;

    bool dragging = false;
    POINT dragStart{0, 0};
    double dragStartHz = 0.0;

    std::vector<float> columns;

    int spectrumTop() const { return kHeaderH; }
    int spectrumHeight(int h) const { return std::max(80, static_cast<int>((h - kHeaderH - kStatusH) * 0.36)); }
    int waterfallTop(int h) const { return kHeaderH + spectrumHeight(h); }
    int waterfallHeight(int h) const { return std::max(0, h - kStatusH - waterfallTop(h)); }
};

App g_app;

void createFonts(App& app) {
    auto make = [](int height, int weight) {
        return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    };
    app.fontTiny = make(12, FW_NORMAL);
    app.fontSmall = make(14, FW_NORMAL);
    app.fontMed = make(19, FW_SEMIBOLD);
    app.fontBig = make(38, FW_BOLD);
}

void destroyFonts(App& app) {
    for (HFONT* f : {&app.fontTiny, &app.fontSmall, &app.fontMed, &app.fontBig}) {
        if (*f) DeleteObject(*f);
        *f = nullptr;
    }
}

void drawTextA(HDC dc, int x, int y, const std::string& text, COLORREF color, HFONT font, UINT align = TA_LEFT) {
    if (!dc || !font || text.empty()) return;
    wchar_t wide[512];
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide, 512);
    if (n <= 1) return;
    SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    SetTextAlign(dc, align | TA_TOP);
    TextOutW(dc, x, y, wide, n - 1);
}

// Column power (max dB of the bins falling in a screen column).
void computeColumns(App& app, int width, std::vector<float>& out) {
    if (!app.spec) return;
    const int x0 = kMargin;
    const int x1 = std::max(x0 + 1, width - kMargin);
    const int columns = x1 - x0;
    if (columns <= 0) return;
    out.resize(static_cast<size_t>(columns));
    const auto& bins = app.spec->binsDb();
    const int n = app.spec->size();
    for (int c = 0; c < columns; ++c) {
        const double t = (c + 0.5) / columns;
        const int center = static_cast<int>(std::llround((t - 0.5) * n));
        const int halfWindow = std::max(1, n / (2 * columns));
        float best = -200.0f;
        for (int k = -halfWindow; k <= halfWindow; ++k) {
            const int idx = ((center + k) % n + n) % n;
            if (bins[static_cast<size_t>(idx)] > best) best = bins[static_cast<size_t>(idx)];
        }
        out[static_cast<size_t>(c)] = best;
    }
}

void appendWaterfallRow(App& app, int width) {
    if (!app.waterfall.px || app.waterfall.w != width) return;
    computeColumns(app, width, app.columns);
    const int h = app.waterfall.h;
    if (h <= 0) return;
    const size_t rowBytes = static_cast<size_t>(width) * sizeof(uint32_t);
    if (h > 1) std::memmove(app.waterfall.px, app.waterfall.px + width, rowBytes * static_cast<size_t>(h - 1));
    uint32_t* row = app.waterfall.px + static_cast<size_t>(h - 1) * static_cast<size_t>(width);
    std::fill(row, row + width, rgb(6, 8, 14));
    const int x0 = kMargin;
    const int columns = static_cast<int>(app.columns.size());
    for (int c = 0; c < columns; ++c) row[x0 + c] = colorForDb(app.columns[static_cast<size_t>(c)]);
}

void paintApp(App& app, int width, int height) {
    if (!app.back.px || app.back.w != width || app.back.h != height) {
        if (!app.back.create(width, height)) return;
    }
    HDC dc = app.back.dc;
    uint32_t* px = app.back.px;
    const size_t total = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::fill(px, px + total, rgb(12, 16, 22));

    const int specTop = app.spectrumTop();
    const int specH = app.spectrumHeight(height);
    const int specBottom = specTop + specH;
    const int wfTop = app.waterfallTop(height);
    const int wfH = app.waterfallHeight(height);

    // ------------------------------------------------------------ header
    const std::string freqLine = app.isFile ? formatOffset(app.bandOffset) : formatHzLong(app.centerHz);
    drawTextA(dc, kMargin + 2, 6, freqLine, RGB(232, 244, 255), app.fontBig);

    char buf[256];
    if (app.isFile) {
        // En rejeu il n'y a pas de frequence absolue : c'est le decalage dans
        // la bande enregistree qui compte.
        std::snprintf(buf, sizeof(buf), "%s   %s", sdr::modeAbbrev(app.mode), app.sourceName.c_str());
    } else {
        std::snprintf(buf, sizeof(buf), "%s   gain %s   ppm %d   %s", sdr::modeAbbrev(app.mode),
                      app.gainDb < 0 ? "auto" : (std::to_string(static_cast<int>(app.gainDb)) + " dB").c_str(),
                      app.ppm, app.sourceName.c_str());
    }
    drawTextA(dc, kMargin + 4, 52, buf, RGB(150, 190, 220), app.fontSmall);

    std::snprintf(buf, sizeof(buf), "%.0f kHz de large", app.sourceRate / 1000.0);
    drawTextA(dc, width - kMargin, 8, buf, RGB(130, 160, 190), app.fontSmall, TA_RIGHT);
    const char* sourceKind = app.isFile ? "FICHIER" : (app.source->info().hardware ? "MATERIEL" : "SIMULATION");
    std::snprintf(buf, sizeof(buf), "%s v%s", sourceKind, "1.0");
    drawTextA(dc, width - kMargin, 28, buf, RGB(110, 140, 170), app.fontTiny, TA_RIGHT);

    // ---------------------------------------------------------- spectrum
    for (int y = specTop; y < specBottom; ++y)
        std::fill(px + static_cast<size_t>(y) * width, px + static_cast<size_t>(y) * width + width, rgb(10, 14, 20));

    computeColumns(app, width, app.columns);
    const int columns = static_cast<int>(app.columns.size());
    const int plotTop = specTop + 4;
    const int plotBottom = specBottom - 2;
    for (int c = 0; c < columns; ++c) {
        const float db = app.columns[static_cast<size_t>(c)];
        const float norm = std::clamp((db - kMinDb) / (kMaxDb - kMinDb), 0.0f, 1.0f);
        const int y = plotBottom - static_cast<int>(norm * static_cast<float>(plotBottom - plotTop));
        const uint32_t color = colorForDb(db);
        const uint32_t fill = scaleColor(color, 0.42f);
        for (int yy = y; yy <= plotBottom; ++yy)
            px[static_cast<size_t>(yy) * width + kMargin + c] = fill;
        px[static_cast<size_t>(std::clamp(y, plotTop, plotBottom)) * width + kMargin + c] = color;
    }

    // Grid: 200 kHz steps with labels.
    const double stepHz = 200000.0;
    for (double off = -static_cast<double>(width) * 0.0 - stepHz * 6.0; off <= stepHz * 6.0; off += stepHz) {
        const double t = (off / app.sourceRate) + 0.5;
        if (t < 0.0 || t > 1.0) continue;
        const int x = kMargin + static_cast<int>(t * (width - 2 * kMargin));
        for (int y = specTop; y < specBottom; y += 4)
            px[static_cast<size_t>(y) * width + x] = rgb(30, 42, 56);
        std::snprintf(buf, sizeof(buf), "%+.0f", off / 1000.0);
        drawTextA(dc, x + 3, specBottom - 16, buf, RGB(110, 140, 170), app.fontTiny);
    }

    // Centre marker.
    const int cx = width / 2;
    for (int y = specTop; y < specBottom; ++y) px[static_cast<size_t>(y) * width + cx] = rgb(220, 90, 60);

    // En rejeu : position du canal dans la bande enregistree (pointilles).
    if (app.isFile) {
        const double t = std::clamp(app.bandOffset / app.sourceRate + 0.5, 0.0, 1.0);
        const int x = kMargin + static_cast<int>(t * (width - 2 * kMargin));
        for (int y = specTop; y < specBottom; y += 4) px[static_cast<size_t>(y) * width + x] = rgb(90, 220, 255);
        std::snprintf(buf, sizeof(buf), "%+.1f kHz", app.bandOffset / 1000.0);
        drawTextA(dc, std::clamp(x + 4, kMargin, width - 90), specTop + 4, buf, RGB(90, 220, 255), app.fontTiny);
    }

    // ---------------------------------------------------------- waterfall
    if (wfH > 0 && width > 2 * kMargin) {
        if (!app.waterfall.px || app.waterfall.w != width || app.waterfall.h != wfH) {
            app.waterfall.create(width, wfH);
            if (app.waterfall.px) std::fill(app.waterfall.px, app.waterfall.px + static_cast<size_t>(width) * wfH, rgb(6, 8, 14));
        }
        if (app.waterfall.px) {
            std::memcpy(px + static_cast<size_t>(wfTop) * width, app.waterfall.px,
                        static_cast<size_t>(width) * static_cast<size_t>(wfH) * sizeof(uint32_t));
        }
    }

    // -------------------------------------------------------------- footer
    const int statusY = height - kStatusH;
    for (int y = statusY; y < height; ++y)
        std::fill(px + static_cast<size_t>(y) * width, px + static_cast<size_t>(y) * width + width, rgb(20, 26, 34));

    std::snprintf(buf, sizeof(buf),
                  "%.1f MS/s  |  FFT %llu  |  audio 48 kHz%s  |  canal %.1f dBFS  |  sq %s %.0f dB  |  R/I: %s/%s",
                  app.measuredMsps,
                  static_cast<unsigned long long>(app.spec ? app.spec->frames() : 0),
                  app.audio.running() ? "" : " (indisponible)",
                  app.channelDb,
                  app.squelchOn ? "on" : "off", app.squelchDb,
                  app.wav.isOpen() ? "rec" : "-", app.iqWriter.isOpen() ? "rec" : "-");
    drawTextA(dc, kMargin, statusY + 6, buf, RGB(150, 175, 200), app.fontTiny);

    std::snprintf(buf, sizeof(buf), "%s", app.note.c_str());
    drawTextA(dc, width - kMargin, statusY + 6, buf, RGB(120, 200, 150), app.fontTiny, TA_RIGHT);

    // Volume meter.
    const int meterW = 90;
    const int meterX = width - kMargin - meterW;
    const int meterY = statusY - 8;
    if (meterY > specBottom) {
        const int filled = static_cast<int>(std::clamp(app.audioPeak, 0.0, 1.0) * meterW);
        for (int y = meterY; y < meterY + 4; ++y) {
            for (int x = 0; x < meterW; ++x) {
                if (meterX + x >= width) break;
                px[static_cast<size_t>(y) * width + meterX + x] = (x < filled) ? rgb(70, 200, 120) : rgb(35, 45, 55);
            }
        }
    }
}

// ------------------------------------------------------------------ actions

void setNote(App& app, const std::string& text) { app.note = text; }

void tuneTo(App& app, double hz) {
    const auto& info = app.source->info();
    hz = std::clamp(hz, info.minHz, info.maxHz);
    app.centerHz = hz;
    app.wantCenter.store(hz);
}

// Rejeu d'un fichier : la frequence d'ecoute est figee, on deplace le canal a
// l'interieur de la bande capturee (Receiver::setOffsetHz).
void setBandOffset(App& app, double hz) {
    if (!app.rx) return;
    const double limit = app.rx->maxOffsetHz();
    const double clamped = std::clamp(hz, -limit, limit);
    app.bandOffset = clamped;
    {
        std::lock_guard<std::mutex> lock(app.rxMutex);
        app.rx->setOffsetHz(clamped);
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "offset %+.1f kHz", clamped / 1000.0);
    setNote(app, buf);
}

// Position absolue demandee (clic ou glisser sur le spectre).
void moveTo(App& app, double hz) {
    if (app.isFile) setBandOffset(app, hz - app.centerHz);
    else tuneTo(app, hz);
}

// Pas de syntonisation (fleches).
void moveBy(App& app, double hz) {
    if (app.isFile) setBandOffset(app, app.bandOffset + hz);
    else tuneTo(app, app.centerHz + hz);
}

// Touches 1-4 : bandes en reception, retour au centre du fichier en rejeu.
void selectBand(App& app, double hz, const char* label) {
    if (app.isFile) {
        setBandOffset(app, 0.0);
        setNote(app, "centre du fichier");
        return;
    }
    tuneTo(app, hz);
    setNote(app, label);
}

void cycleMode(App& app, int delta) {
    int m = (static_cast<int>(app.mode) + delta + sdr::kModeCount) % sdr::kModeCount;
    app.mode = static_cast<sdr::Mode>(m);
    {
        std::lock_guard<std::mutex> lock(app.rxMutex);
        app.rx->setMode(app.mode);
    }
    setNote(app, std::string("mode ") + sdr::modeAbbrev(app.mode));
}

void cycleGain(App& app) {
    std::vector<double> steps = app.source->gainsDb();
    double next = -1.0;
    if (app.gainDb < 0.0) {
        if (!steps.empty()) next = steps.front();
    } else {
        auto it = std::find_if(steps.begin(), steps.end(), [&](double g) { return g > app.gainDb + 0.01; });
        next = (it == steps.end()) ? -1.0 : *it;
    }
    app.gainDb = next;
    app.wantGain.store(next);
    char buf[64];
    std::snprintf(buf, sizeof(buf), next < 0 ? "gain automatique" : "gain %.1f dB", next);
    setNote(app, buf);
}

void toggleWav(App& app) {
    std::lock_guard<std::mutex> lock(app.recordMutex);
    if (app.wav.isOpen()) {
        app.wav.close();
        setNote(app, "enregistrement audio arrete");
        return;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    char path[128];
    std::snprintf(path, sizeof(path), "rec_audio_%02d%02d%02d.wav", st.wHour, st.wMinute, st.wSecond);
    std::string err;
    if (app.wav.open(path, 48000, 1, err))
        setNote(app, std::string("audio -> ") + path);
    else
        setNote(app, err);
}

void toggleIq(App& app) {
    std::lock_guard<std::mutex> lock(app.recordMutex);
    if (app.iqWriter.isOpen()) {
        app.iqWriter.close();
        setNote(app, "enregistrement IQ arrete");
        return;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    char path[128];
    std::snprintf(path, sizeof(path), "rec_iq_%02d%02d%02d.cu8", st.wHour, st.wMinute, st.wSecond);
    std::string err;
    if (app.iqWriter.open(path, err))
        setNote(app, std::string("IQ -> ") + path);
    else
        setNote(app, err);
}

// ------------------------------------------------------------------ threads

void captureLoop(App* app) {
    std::vector<sdr::cfloat> buffer(32768);
    while (!app->stopRequested.load()) {
        const double want = app->wantCenter.load();
        if (want != app->appliedCenter) {
            std::string err;
            if (app->source->setCenterHz(want, err)) {
                app->appliedCenter = want;
            } else {
                app->errorText = err;
            }
        }
        const double wantGain = app->wantGain.load();
        if (wantGain != app->gainDb) {
            std::string err;
            app->source->setGainDb(wantGain, err);
        }
        const int wantPpm = app->wantPpm.load();
        if (wantPpm != app->ppm) {
            std::string err;
            if (app->source->setPpm(wantPpm, err)) app->ppm = wantPpm;
        }

        std::string err;
        const size_t n = app->source->read(buffer.data(), buffer.size(), err);
        if (n == 0) {
            if (!err.empty()) {
                app->errorText = err;
                break;
            }
            if (app->source->atEnd()) {
                app->stopRequested = true;
                break;
            }
            Sleep(2);
            continue;
        }
        if (app->iqWriter.isOpen()) {
            std::lock_guard<std::mutex> lock(app->recordMutex);
            app->iqWriter.write(buffer.data(), n);
        }
        app->samplesIn.fetch_add(n);
        size_t offset = 0;
        while (offset < n && !app->stopRequested.load()) {
            const size_t written = app->iqRing.write(buffer.data() + offset, n - offset);
            if (written == 0) Sleep(1);
            else offset += written;
        }
    }
}

void dspLoop(App* app) {
    std::vector<sdr::cfloat> block(8192);
    std::vector<float> audio;
    while (!app->stopRequested.load()) {
        const size_t n = app->iqRing.read(block.data(), block.size());
        if (n == 0) {
            Sleep(2);
            continue;
        }
        if (app->spec) app->spec->process(block.data(), n);
        audio.clear();
        {
            std::lock_guard<std::mutex> lock(app->rxMutex);
            app->rx->process(block.data(), n, audio);
            app->channelDb = app->rx->channelPowerDb();
        }
        if (!audio.empty()) {
            if (app->wav.isOpen()) {
                std::lock_guard<std::mutex> lock(app->recordMutex);
                app->wav.write(audio.data(), audio.size());
            }
            size_t offset = 0;
            while (offset < audio.size()) {
                const size_t written = app->audio.ring().write(audio.data() + offset, audio.size() - offset);
                if (written == 0) break;
                offset += written;
            }
        }
        app->audioPeak = app->rx->outputPeak();
    }
}

// ------------------------------------------------------------------- setup

bool initApp(App& app, const sdr::SourceRequest& request, std::string& err) {
    app.isFile = (request.kind == "file");
    app.bandOffset = 0.0;
    app.source = sdr::createSource(request, err);
    if (!app.source) return false;

    std::string openErr;
    const std::string name = app.source->open(openErr);
    if (name.empty()) {
        if (request.kind == "auto") {
            app.source = sdr::createSyntheticSource(request);
            app.source->open(openErr);
            app.note = "aucune cle detectee - mode simulation";
        } else {
            err = openErr.empty() ? "source indisponible" : openErr;
            return false;
        }
    }
    app.sourceName = app.source->info().name;
    if (app.isFile) {
        // Seul le nom du fichier tient dans l'en-tete.
        const size_t slash = app.sourceName.find_last_of("\\/");
        if (slash != std::string::npos) app.sourceName = app.sourceName.substr(slash + 1);
    }

    std::string ignored;
    app.source->setSampleRate(2.4e6, ignored);
    app.source->setCenterHz(app.centerHz, ignored);
    app.source->setGainDb(-1.0, ignored);
    app.source->setPpm(0, ignored);
    app.appliedCenter = app.source->info().centerHz;
    app.centerHz = app.appliedCenter;
    app.wantCenter.store(app.centerHz);

    app.sourceRate = app.source->info().sampleRate;
    if (app.sourceRate < 1e5) app.sourceRate = 2.4e6;

    app.rx = std::make_unique<sdr::Receiver>(app.sourceRate);
    app.rx->setMode(app.mode);
    app.rx->setDeemphasisMicros(75);
    app.rx->setAgcEnabled(true);
    app.rx->setVolume(app.volume);
    app.rx->setSquelchEnabled(app.squelchOn);
    app.rx->setSquelchDb(app.squelchDb);
    app.rx->setOffsetHz(0.0);
    app.spec = std::make_unique<sdr::SpectrumAnalyzer>(4096, app.sourceRate);

    if (!app.audio.start(err)) {
        app.note = "audio indisponible: " + err;
        err.clear();
    }

    app.lastTick = GetTickCount();
    app.stopRequested = false;
    app.captureThread = std::thread(captureLoop, &app);
    app.dspThread = std::thread(dspLoop, &app);
    return true;
}

void shutdownApp(App& app) {
    app.stopRequested = true;
    if (app.captureThread.joinable()) app.captureThread.join();
    if (app.dspThread.joinable()) app.dspThread.join();
    app.audio.stop();
    {
        std::lock_guard<std::mutex> lock(app.recordMutex);
        if (app.wav.isOpen()) app.wav.close();
        if (app.iqWriter.isOpen()) app.iqWriter.close();
    }
    if (app.source) app.source->close();
    app.back.destroy();
    app.waterfall.destroy();
    destroyFonts(app);
}

// ------------------------------------------------------------------ messages

void onTimer(App& app) {
    const DWORD now = GetTickCount();
    if (now - app.lastTick >= 500) {
        const uint64_t total = app.samplesIn.load();
        const double seconds = static_cast<double>(now - app.lastTick) / 1000.0;
        app.measuredMsps = static_cast<double>(total - app.lastSamples) / 1e6 / std::max(0.05, seconds);
        app.lastSamples = total;
        app.lastTick = now;
        if (!app.errorText.empty()) {
            app.note = app.errorText;
            app.errorText.clear();
        }
    }
    if (app.spec && app.spec->frames() != app.lastFftFrame) {
        app.lastFftFrame = app.spec->frames();
        RECT rc;
        GetClientRect(app.hwnd, &rc);
        appendWaterfallRow(app, rc.right - rc.left);
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    App& app = g_app;
    switch (msg) {
        case WM_CREATE:
            app.hwnd = hwnd;
            SetTimer(hwnd, kTimerId, 30, nullptr);
            return 0;

        case WM_TIMER:
            if (wParam == kTimerId) onTimer(app);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            const int w = rc.right - rc.left;
            const int h = rc.bottom - rc.top;
            if (w > 0 && h > 0) {
                paintApp(app, w, h);
                if (app.back.dc) BitBlt(dc, 0, 0, w, h, app.back.dc, 0, 0, SRCCOPY);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SIZE: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            const int w = rc.right - rc.left;
            const int h = rc.bottom - rc.top;
            if (w > 0 && h > 0) {
                app.back.create(w, h);
                app.waterfall.create(w, app.waterfallHeight(h));
                if (app.waterfall.px)
                    std::fill(app.waterfall.px,
                              app.waterfall.px + static_cast<size_t>(app.waterfall.w) * app.waterfall.h,
                              rgb(6, 8, 14));
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            const int w = rc.right - rc.left;
            const int x = GET_X_LPARAM(lParam);
            const double t = static_cast<double>(x - kMargin) / std::max(1, w - 2 * kMargin);
            const double hz = app.centerHz + (t - 0.5) * app.sourceRate;
            moveTo(app, hz);
            app.dragging = true;
            app.dragStart = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            app.dragStartHz = app.isFile ? app.bandOffset + app.centerHz : app.centerHz;
            SetCapture(hwnd);
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!app.dragging) return 0;
            RECT rc;
            GetClientRect(hwnd, &rc);
            const int w = std::max(1, static_cast<int>(rc.right - rc.left));
            const int dx = GET_X_LPARAM(lParam) - app.dragStart.x;
            const double hz = app.dragStartHz - static_cast<double>(dx) * app.sourceRate / w;
            moveTo(app, hz);
            return 0;
        }

        case WM_LBUTTONUP:
            if (app.dragging) {
                app.dragging = false;
                ReleaseCapture();
                app.dragStartHz = app.centerHz;
            }
            return 0;

        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            app.volume = std::clamp(app.volume + (delta > 0 ? 0.05f : -0.05f), 0.0f, 1.0f);
            {
                std::lock_guard<std::mutex> lock(app.rxMutex);
                app.rx->setVolume(app.volume);
            }
            char buf[64];
            std::snprintf(buf, sizeof(buf), "volume %.0f%%", app.volume * 100.0f);
            setNote(app, buf);
            return 0;
        }

        case WM_KEYDOWN: {
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const double fine = shift ? 1000.0 : 12500.0;
            switch (wParam) {
                case VK_LEFT: moveBy(app, -fine); return 0;
                case VK_RIGHT: moveBy(app, fine); return 0;
                case VK_UP: moveBy(app, 500000.0); return 0;
                case VK_DOWN: moveBy(app, -500000.0); return 0;
                case VK_PRIOR: moveBy(app, 2500000.0); return 0;
                case VK_NEXT: moveBy(app, -2500000.0); return 0;
                case 'M': cycleMode(app, shift ? -1 : 1); return 0;
                case 'G': cycleGain(app); return 0;
                case 'S':
                    app.squelchOn = !app.squelchOn;
                    {
                        std::lock_guard<std::mutex> lock(app.rxMutex);
                        app.rx->setSquelchEnabled(app.squelchOn);
                    }
                    setNote(app, app.squelchOn ? "squelch actif" : "squelch coupe");
                    return 0;
                case 'R': toggleWav(app); return 0;
                case 'I': toggleIq(app); return 0;
                case '1': selectBand(app, 98.0e6, "bande FM broadcast"); return 0;
                case '2': selectBand(app, 127.0e6, "bande aeronautique"); return 0;
                case '3': selectBand(app, 156.8e6, "bande marine"); return 0;
                case '4': selectBand(app, 446.05e6, "PMR446"); return 0;
                case VK_OEM_PLUS:
                case VK_ADD:
                    app.volume = std::clamp(app.volume + 0.05f, 0.0f, 1.0f);
                    { std::lock_guard<std::mutex> lock(app.rxMutex); app.rx->setVolume(app.volume); }
                    return 0;
                case VK_OEM_MINUS:
                case VK_SUBTRACT:
                    app.volume = std::clamp(app.volume - 0.05f, 0.0f, 1.0f);
                    { std::lock_guard<std::mutex> lock(app.rxMutex); app.rx->setVolume(app.volume); }
                    return 0;
                default: return 0;
            }
        }

        case WM_CLOSE:
            KillTimer(hwnd, kTimerId);
            shutdownApp(app);
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// --------------------------------------------------------------------- probe

int runProbe() {
    sdr::SourceRequest req;
    req.kind = "rtl";
    std::string err;
    auto source = sdr::createSource(req, err);
    if (!source) {
        std::printf("ERREUR: %s\n", err.c_str());
        return 2;
    }
    const std::string name = source->open(err);
    if (name.empty()) {
        std::printf("ERREUR ouverture: %s\n", err.c_str());
        std::printf("(pilote WinUSB installe ? voir README - Zadig)\n");
        return 3;
    }
    std::printf("Cle      : %s\n", name.c_str());
    source->setSampleRate(2.4e6, err);
    source->setCenterHz(101.5e6, err);
    source->setGainDb(-1.0, err);
    const sdr::SourceInfo info = source->info();
    std::printf("Debit    : %.0f ech/s\n", info.sampleRate);
    std::printf("Centre   : %.3f MHz\n", info.centerHz / 1e6);

    sdr::SpectrumAnalyzer spec(4096, info.sampleRate);
    std::vector<sdr::cfloat> buffer(32768);
    double power = 0.0;
    float peak = 0.0f;
    size_t total = 0;
    const size_t wanted = static_cast<size_t>(info.sampleRate); // 1 seconde
    while (total < wanted) {
        const size_t n = source->read(buffer.data(), buffer.size(), err);
        if (n == 0) {
            if (!err.empty()) std::printf("ERREUR lecture: %s\n", err.c_str());
            break;
        }
        for (size_t i = 0; i < n; ++i) {
            power += std::norm(buffer[i]);
            peak = std::max(peak, std::abs(buffer[i]));
        }
        spec.process(buffer.data(), n);
        total += n;
    }
    source->close();
    if (total == 0) {
        std::printf("ERREUR: aucun echantillon lu\n");
        return 4;
    }
    std::printf("Echant.  : %zu\n", total);
    std::printf("Puissance: %.1f dBFS (pic %.2f)\n",
                10.0 * std::log10(power / static_cast<double>(total) + 1e-20), peak);
    std::printf("Pic spec.: %+.1f kHz du centre (%.2f dB)\n", spec.peakHz() / 1e3, spec.peakDb());
    return 0;
}

void printHelp() {
    std::printf(
        "SDR Studio - recepteur RTL-SDR\n"
        "  SDRStudio.exe                  reception (cle RTL-SDR, sinon simulation)\n"
        "  SDRStudio.exe --synthetic      mode simulation (sans cle)\n"
        "  SDRStudio.exe --file x.cu8     rejoue un fichier IQ (.cu8/.cs16/.cf32) [--rate 2400000]\n"
        "  SDRStudio.exe --probe          test materiel en console (1 s de signal)\n"
        "  SDRStudio.exe --selftest       auto-test DSP + benchmark\n"
        "\nTouches: fleches = accord | Maj+fleches = fin | M = mode | G = gain | S = squelch\n"
        "         R = enregistrer audio | I = enregistrer IQ | 1-4 = bandes | molette = volume\n"
        "\nEn rejeu (--file) : fleches / clic / glisser deplacent le canal dans la bande\n"
        "enregistree (bande = 2,4 MHz par defaut, voir --rate) ; le fichier boucle.\n");
}

int runGui(const sdr::SourceRequest& request) {
    SetProcessDPIAware();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                                80, 60, 1280, 820, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 1;

    createFonts(g_app);
    g_app.hwnd = hwnd;
    std::string err;
    if (!initApp(g_app, request, err)) {
        MessageBoxA(hwnd, err.c_str(), "SDR Studio - erreur", MB_ICONERROR | MB_OK);
        DestroyWindow(hwnd);
        destroyFonts(g_app);
        return 2;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    destroyFonts(g_app);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    sdr::SourceRequest request;
    bool probe = false;
    bool selftest = false;
    double rate = 2.4e6;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--selftest") selftest = true;
        else if (arg == "--probe") probe = true;
        else if (arg == "--synthetic") request.kind = "synthetic";
        else if (arg == "--rtl") request.kind = "rtl";
        else if (arg == "--file" && i + 1 < argc) {
            request.kind = "file";
            request.path = argv[++i];
        } else if (arg == "--rate" && i + 1 < argc) {
            rate = std::atof(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            printHelp();
            return 0;
        } else if (!arg.empty() && arg[0] != '-') {
            request.kind = "file";
            request.path = arg;
        }
    }
    request.sampleRate = rate;

    if (selftest) return sdr::runSelfTest() ? 0 : 1;
    if (probe) return runProbe();
    return runGui(request);
}
