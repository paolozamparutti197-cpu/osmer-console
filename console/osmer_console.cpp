// Console OSMER: finestra a pieno schermo con menu laterale e grafici GDI+.
#include "osmer_console.h"

void closeYearEdit(bool apply);
void beginYearEdit(int target);

// ------------------------------------------------------------ DPI

using SetDpiContextFn = BOOL(WINAPI*)(HANDLE);
using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
using AdjustForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);

static FARPROC user32Proc(const char* name) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    return user32 ? GetProcAddress(user32, name) : nullptr;
}
static void enableDpiAwareness() {
    auto setContext = (SetDpiContextFn)user32Proc("SetProcessDpiAwarenessContext");
    if (setContext && setContext((HANDLE)(LONG_PTR)-4)) return;
    SetProcessDPIAware();
}
static UINT windowDpi(HWND hwnd) {
    auto getDpi = (GetDpiForWindowFn)user32Proc("GetDpiForWindow");
    if (getDpi && hwnd) { UINT dpi = getDpi(hwnd); if (dpi) return dpi; }
    HDC dc = GetDC(nullptr);
    UINT dpi = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi ? dpi : 96;
}
static int px(float logical) { return (int)std::lround(logical * app.scale); }
static POINT toLogical(POINT p) {
    return POINT{(LONG)std::lround(p.x / app.scale), (LONG)std::lround(p.y / app.scale)};
}
static void createYearFont() {
    if (app.yearFont) DeleteObject(app.yearFont);
    app.yearFont = CreateFontW(-px(18), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (app.yearEdit) SendMessageW(app.yearEdit, WM_SETFONT, (WPARAM)app.yearFont, TRUE);
}
static void applyDpi(UINT dpi) {
    app.scale = std::max(1.f, dpi / 96.f);
    app.sceneDirty = true;
    if (app.yearEdit && IsWindowVisible(app.yearEdit)) closeYearEdit(true);
    if (app.yearFont || app.yearEdit) createYearFont();
}
static POINT minimumWindowSize(HWND hwnd) {
    RECT rc{0, 0, px((float)MinClientWidth), px((float)MinClientHeight)};
    DWORD style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
    DWORD exStyle = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    auto adjust = (AdjustForDpiFn)user32Proc("AdjustWindowRectExForDpi");
    if (!adjust || !adjust(&rc, style, FALSE, exStyle, (UINT)std::lround(app.scale * 96.f)))
        AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    return POINT{rc.right - rc.left, rc.bottom - rc.top};
}

// ------------------------------------------------------------ ridisegno

void requestRedraw(bool sceneChanged) {
    if (sceneChanged) app.sceneDirty = true;
    if (app.hwnd) InvalidateRect(app.hwnd, nullptr, FALSE);
}

static void paintToScreen(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) { EndPaint(hwnd, &ps); return; }
    int LW = (int)std::floor(W / app.scale), LH = (int)std::floor(H / app.scale);
    auto key = sceneSignature();
    if (!app.scene || app.sceneW != W || app.sceneH != H || app.sceneDirty || key != app.sceneKey) {
        if (app.scene && (app.sceneW != W || app.sceneH != H)) { DeleteObject(app.scene); app.scene = nullptr; }
        if (!app.scene) app.scene = CreateCompatibleBitmap(hdc, W, H);
        HDC sdc = CreateCompatibleDC(hdc);
        HGDIOBJ old = SelectObject(sdc, app.scene);
        {
            Graphics g(sdc);
            g.Clear(Color(255, 244, 247, 248));
            g.ScaleTransform(app.scale, app.scale);
            paintScene(g, LW, LH);
        }
        SelectObject(sdc, old);
        DeleteDC(sdc);
        app.sceneW = W; app.sceneH = H;
        app.sceneDirty = false;
        app.sceneKey = sceneSignature();
    }
    HDC sdc = CreateCompatibleDC(hdc);
    HGDIOBJ oldScene = SelectObject(sdc, app.scene);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP frame = CreateCompatibleBitmap(hdc, W, H);
    HGDIOBJ oldFrame = SelectObject(mem, frame);
    BitBlt(mem, 0, 0, W, H, sdc, 0, 0, SRCCOPY);
    {
        Graphics g(mem);
        g.ScaleTransform(app.scale, app.scale);
        paintOverlay(g, LW, LH);
    }
    BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldFrame); DeleteObject(frame); DeleteDC(mem);
    SelectObject(sdc, oldScene); DeleteDC(sdc);
    EndPaint(hwnd, &ps);
}

// ------------------------------------------------------------ comandi

static void stepCompare(int dir) {
    int lo, hi;
    dailyYears(lo, hi);
    if (hi < lo) { app.status = L"Nessun dato giornaliero per il confronto."; return; }
    int y = app.compareYear ? app.compareYear + dir : app.focusYear - 1;
    if (y == app.focusYear) y += dir;
    y = std::clamp(y, lo, hi);
    if (y == app.focusYear) y = y == lo ? std::min(hi, y + 1) : y - 1;
    app.compareYear = y == app.focusYear ? 0 : y;
    if (app.compareYear && app.view != View::Band && app.view != View::Rain)
        app.status = L"Confronto con il " + std::to_wstring(app.compareYear) + L": visibile in Anno e fascia P5-P95 e in Pioggia cumulata.";
}

static void openHeatWave() {
    const auto& waves = heatWaves();
    if (waves.empty()) return;
    const auto& w = waves[std::clamp(app.heatSel, 0, (int)waves.size() - 1)];
    app.view = View::Band;
    app.bandVar = VTM;
    app.season = Season::Year;
    app.focusYear = w.y0;
    int a = dayIndex(w.m0, w.d0), b = w.y1 == w.y0 ? dayIndex(w.m1, w.d1) : 365;
    app.zoomA = std::max(0, a - 20) / 365.0;
    app.zoomB = std::min(365, b + 20) / 365.0;
    app.status = L"Ondata nell'anno " + std::to_wstring(w.y0) + L": tasto destro sul grafico per vedere l'anno intero.";
}

static void changeStation(int index) {
    if (app.stations.empty()) return;
    int n = (int)app.stations.size();
    index = ((index % n) + n) % n;
    selectStation(index, false);
    if (!st().loaded) startBackgroundLoad({index});
    app.status = L"Stazione: " + st().name + L"   (" + stationPeriod(st()) + L")";
}

void onCommand(int id) {
    app.hoverMonth = -1;
    app.monthTip.clear();
    if (id == 13) return;
    if (id == 20 || id == 21 || id == 22 || id == 23) { app.picker = 0; beginYearEdit(id); return; }
    if (id == 5) { app.picker = app.picker == 1 ? 0 : 1; return; }
    if (id == 6) { app.picker = app.picker == 2 ? 0 : 2; return; }
    if (id == 12) { app.picker = app.picker == 3 ? 0 : 3; return; }
    if (id == 10) { app.picker = app.picker == 4 ? 0 : 4; return; }
    if (id == 19) { app.picker = app.picker == 5 ? 0 : 5; return; }
    if (id >= 300 && id <= 316) {
        int i = id - 300;
        if (i == 0) app.season = Season::Year;
        else if (i == 1) app.season = Season::Winter;
        else if (i == 2) app.season = Season::Spring;
        else if (i == 3) app.season = Season::Summer;
        else if (i == 4) app.season = Season::Autumn;
        else { app.season = Season::Month; app.month = i - 4; }
        app.picker = 0;
        resetZoom();
        saveSession();
        return;
    }
    if (id >= 320 && id <= 322) {
        app.refMode = id == 320 ? RefMode::Station : id == 321 ? RefMode::Visible : RefMode::Clima1991;
        app.picker = 0;
        saveSession();
        return;
    }
    if (id >= 600 && id < 600 + NVAR) {
        app.bandVar = id - 600;
        app.picker = 0;
        if (app.view != View::Band) app.view = View::Band;
        saveSession();
        return;
    }
    if (id >= 2000 && id < 2000 + (int)app.stations.size()) {
        app.picker = 0;
        changeStation(id - 2000);
        if (app.view == View::Overview) app.view = View::Clima;
        saveSession();
        return;
    }
    if (id >= 1000 && id < 1000 + (int)app.stations.size()) {
        changeStation(id - 1000);
        app.view = View::Clima;
        saveSession();
        return;
    }
    app.picker = 0;
    if (id >= 100 && id < 100 + ViewCount) app.view = (View)(id - 100);
    else if (id >= 450 && id <= 453) { app.overviewSort = id - 450; app.overviewScroll = 0; }
    else if (id == 16) stepCompare(-1);
    else if (id == 17) stepCompare(+1);
    else if (id == 18) app.compareYear = 0;
    else if (id >= 400 && id <= 402) { app.heatMode = (HeatMode)(id - 400); app.heatSel = 0; }
    else if (id == 403) { app.heatThreshold = std::max(20, app.heatThreshold - 1); app.heatSel = 0; }
    else if (id == 404) { app.heatThreshold = std::min(42, app.heatThreshold + 1); app.heatSel = 0; }
    else if (id >= 410 && id <= 419) app.heatSel = id - 410;
    else if (id == 420) openHeatWave();
    else if (id == 1) { app.y0--; clampPeriod(); }
    else if (id == 2) { app.y0++; clampPeriod(); }
    else if (id == 3) { app.y1--; clampPeriod(); }
    else if (id == 4) { app.y1++; clampPeriod(); }
    else if (id == 7) saveFiles();
    else if (id == 14) openOutDir();
    else if (id == 8 || id == 9) {
        int lo, hi;
        focusRange(lo, hi);
        app.focusYear = std::clamp(app.focusYear + (id == 8 ? -1 : 1), lo, hi);
    }
    else if (id == 11) updateAllFromOsmer();
    saveSession();
}

int yearForTarget(int target) {
    if (target == 20) return app.y0;
    if (target == 21) return app.y1;
    if (target == 23) return app.compareYear ? app.compareYear : app.focusYear - 1;
    return app.focusYear;
}

void applyTypedYear(int target, int value) {
    if (target == 20) app.y0 = value;
    else if (target == 21) app.y1 = value;
    else if (target == 23) {
        if (value == 0) app.compareYear = 0;
        else if (!dailyYear(value)) {
            int lo, hi;
            dailyYears(lo, hi);
            app.status = L"Il " + std::to_wstring(value) + L" non ha dati per questa stazione (disponibili " + std::to_wstring(lo)
                + L"-" + std::to_wstring(hi) + L").";
            saveSession();
            return;
        } else app.compareYear = value;
        saveSession();
        app.status = app.compareYear ? L"Confronto con il " + std::to_wstring(app.compareYear) + L"." : L"Confronto tolto.";
        return;
    } else {
        int lo, hi;
        focusRange(lo, hi);
        app.focusYear = std::clamp(value, lo, hi);
    }
    if (target == 20 || target == 21) clampPeriod();
    saveSession();
    app.status = L"Anno impostato a " + std::to_wstring(yearForTarget(target)) + L".";
}

void closeYearEdit(bool apply) {
    if (!app.yearEdit || !IsWindowVisible(app.yearEdit) || app.editBusy) return;
    app.editBusy = true;
    if (apply) {
        wchar_t buf[16] = {};
        GetWindowTextW(app.yearEdit, buf, 16);
        bool digits = buf[0] != 0;
        for (wchar_t* p = buf; *p && digits; ++p) digits = *p >= L'0' && *p <= L'9';
        if (digits) applyTypedYear(app.editTarget, _wtoi(buf));
    }
    ShowWindow(app.yearEdit, SW_HIDE);
    if (GetFocus() == app.yearEdit) SetFocus(app.hwnd);
    app.editBusy = false;
    requestRedraw();
}

LRESULT CALLBACK YearEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) { closeYearEdit(true); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { closeYearEdit(false); return 0; }
    return CallWindowProcW(app.yearEditProc, hwnd, msg, wp, lp);
}

void beginYearEdit(int target) {
    if (app.yearEdit && IsWindowVisible(app.yearEdit) && app.editTarget == target) return;
    if (app.yearEdit && IsWindowVisible(app.yearEdit)) closeYearEdit(true);
    RECT logical{70, 0, 142, 34};
    for (const auto& h : app.hits) if (h.id == target) logical = h.rc;
    RECT rc{px((float)logical.left), px((float)logical.top), px((float)logical.right), px((float)logical.bottom)};
    if (!app.yearFont) createYearFont();
    if (!app.yearEdit) {
        app.yearEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_NUMBER | ES_CENTER | ES_AUTOHSCROLL,
            rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, app.hwnd, (HMENU)50, GetModuleHandleW(nullptr), nullptr);
        app.yearEditProc = (WNDPROC)SetWindowLongPtrW(app.yearEdit, GWLP_WNDPROC, (LONG_PTR)YearEditProc);
        SendMessageW(app.yearEdit, WM_SETFONT, (WPARAM)app.yearFont, TRUE);
    } else {
        SetWindowPos(app.yearEdit, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER);
    }
    app.editTarget = target;
    std::wstring text = std::to_wstring(yearForTarget(target));
    SetWindowTextW(app.yearEdit, text.c_str());
    ShowWindow(app.yearEdit, SW_SHOW);
    SetFocus(app.yearEdit);
    SendMessageW(app.yearEdit, EM_SETSEL, 0, -1);
    app.status = L"Scrivi l'anno e premi Invio. Esc annulla.";
    requestRedraw();
}

// ------------------------------------------------------------ zoom e trascinamento

static bool dailyZoomView() { return app.view == View::Band || app.view == View::Rain; }
static bool periodZoomView() {
    return app.view == View::Spei || app.view == View::Temp || app.view == View::Prec || app.view == View::Balance;
}
static int probeAt(POINT p) {
    for (int i = 0; i < (int)app.probes.size(); ++i) if (PtInRect(&app.probes[i].plot, p)) return i;
    return -1;
}
static int probeIndex(int k, POINT p) {
    const auto& pr = app.probes[k];
    int n = (int)pr.tip.size();
    int w = pr.plot.right - pr.plot.left;
    int idx = w > 0 ? (int)std::lround((p.x - pr.plot.left) * (double)(n - 1) / (double)w) : 0;
    return std::clamp(idx, 0, n - 1);
}

static void clickProbe(int k, POINT p) {
    if (k < 0 || k >= (int)app.probes.size()) return;
    const auto& pr = app.probes[k];
    if (pr.year.empty()) return;
    int y = pr.year[probeIndex(k, p)];
    if (!dailyYear(y)) {
        app.status = L"Il " + std::to_wstring(y) + L" non ha dati giornalieri per questa stazione.";
    } else {
        app.focusYear = y;
        app.view = View::Band;
        app.status = L"Anno " + std::to_wstring(y) + L": valori giornalieri e fascia P5-P95.";
        saveSession();
    }
}

static void stationYears(int& lo, int& hi) {
    const auto& s = st();
    lo = s.firstYear; hi = s.lastYear;
}

static void shiftPeriod(int k) {
    int lo, hi;
    stationYears(lo, hi);
    if (hi <= lo) return;
    k = std::clamp(k, lo - app.y0, hi - app.y1);
    app.y0 += k;
    app.y1 += k;
}

static void panBy(int dx, const RECT& plot) {
    double w = (double)std::max<LONG>(1, plot.right - plot.left);
    if (dailyZoomView()) {
        double span = app.zoomB - app.zoomA;
        double shift = std::clamp(-dx / w * span, -app.zoomA, 1.0 - app.zoomB);
        app.zoomA += shift;
        app.zoomB += shift;
    } else if (periodZoomView()) {
        static double carry = 0;
        carry += -dx / w * (app.y1 - app.y0);
        int k = (int)carry;
        if (k) { shiftPeriod(k); carry -= k; }
    }
}

static void zoomAt(POINT p, const RECT& plot, bool in) {
    double w = (double)std::max<LONG>(1, plot.right - plot.left);
    double f = std::clamp((p.x - plot.left) / w, 0.0, 1.0);
    double factor = in ? 0.8 : 1.25;
    if (dailyZoomView()) {
        double span = app.zoomB - app.zoomA;
        double anchor = app.zoomA + f * span;
        double ns = std::clamp(span * factor, 0.03, 1.0);
        double a = anchor - f * ns, b = a + ns;
        if (a < 0) { b -= a; a = 0; }
        if (b > 1) { a -= b - 1; b = 1; }
        app.zoomA = std::max(0.0, a);
        app.zoomB = std::min(1.0, b);
        if (ns >= 1.0) resetZoom();
    } else if (periodZoomView()) {
        int lo, hi;
        stationYears(lo, hi);
        if (hi - lo < 2) return;
        int span = app.y1 - app.y0;
        double anchor = app.y0 + f * span;
        int ns = (int)std::lround(span * factor);
        if (ns == span) ns += in ? -1 : 1;
        ns = std::clamp(ns, std::min(4, hi - lo), hi - lo);
        int y0 = (int)std::lround(anchor - f * ns);
        y0 = std::clamp(y0, lo, hi - ns);
        app.y0 = y0;
        app.y1 = y0 + ns;
    }
}

static void resetChartZoom() {
    if (dailyZoomView()) resetZoom();
    else if (periodZoomView()) fullPeriod();
}

// ------------------------------------------------------------ finestra

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_STATION_LOADED:
        onStationLoaded(lp);
        return 0;
    case WM_TIMER:
        if (wp == WatchTimer) checkSourceFiles();
        if (wp == OsmerTimer) pollOsmerUpdate();
        return 0;
    case WM_PAINT:
        paintToScreen(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_GETMINMAXINFO: {
        auto* info = (MINMAXINFO*)lp;
        POINT minSize = minimumWindowSize(hwnd);
        info->ptMinTrackSize.x = std::max(info->ptMinTrackSize.x, minSize.x);
        info->ptMinTrackSize.y = std::max(info->ptMinTrackSize.y, minSize.y);
        return 0;
    }
    case WM_DPICHANGED: {
        applyDpi(HIWORD(wp));
        const RECT* suggested = (const RECT*)lp;
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        requestRedraw();
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == 50 && HIWORD(wp) == EN_KILLFOCUS) closeYearEdit(true);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p = toLogical(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        bool hit = false;
        for (auto it = app.hits.rbegin(); it != app.hits.rend(); ++it) {
            if (PtInRect(&it->rc, p)) { onCommand(it->id); requestRedraw(); hit = true; break; }
        }
        if (!hit && app.picker == 0) {
            int k = probeAt(p);
            if (k >= 0) {
                app.mouseDown = true;
                app.dragging = false;
                app.dragStart = app.dragLast = p;
                app.dragProbe = k;
                SetCapture(hwnd);
                hit = true;
            }
        }
        if (!hit) {
            closeYearEdit(true);
            if (app.picker) { app.picker = 0; requestRedraw(); }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!app.mouseDown) return 0;
        bool wasDrag = app.dragging;
        int k = app.dragProbe;
        POINT start = app.dragStart;
        app.mouseDown = app.dragging = false;
        ReleaseCapture();
        if (!wasDrag) clickProbe(k, start);
        else saveSession();
        requestRedraw();
        return 0;
    }
    case WM_CAPTURECHANGED:
        app.mouseDown = app.dragging = false;
        return 0;
    case WM_RBUTTONUP: {
        POINT p = toLogical(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (app.picker == 0 && probeAt(p) >= 0 && (dailyZoomView() || periodZoomView())) {
            resetChartZoom();
            saveSession();
            app.status = L"Zoom annullato.";
            requestRedraw();
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p = toLogical(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (app.mouseDown) {
            if (!app.dragging && (std::abs(p.x - app.dragStart.x) > 4 || std::abs(p.y - app.dragStart.y) > 4)
                && (dailyZoomView() || periodZoomView())) {
                app.dragging = true;
                app.hoverA = app.hoverB = -1;
            }
            if (app.dragging && app.dragProbe >= 0 && app.dragProbe < (int)app.probes.size()) {
                RECT plot = app.probes[app.dragProbe].plot;
                panBy(p.x - app.dragLast.x, plot);
                app.dragLast = p;
                requestRedraw();
                return 0;
            }
        }
        int hot = 0;
        for (const auto& h : app.hits) if ((h.id >= 20 && h.id <= 23) && PtInRect(&h.rc, p)) hot = h.id;
        int a = -1, b = -1, month = -1;
        if (app.picker == 0) {
            int k = probeAt(p);
            if (k >= 0) { a = k; b = probeIndex(k, p); }
            for (int i = 0; i < (int)app.monthHits.size(); ++i)
                if (PtInRect(&app.monthHits[i].rc, p)) { month = i; break; }
        }
        if (hot != app.hotId || a != app.hoverA || b != app.hoverB || month != app.hoverMonth) {
            bool sceneChanged = hot != app.hotId;
            app.hotId = hot;
            app.hoverA = a;
            app.hoverB = b;
            app.hoverMonth = month;
            app.monthTip.clear();
            if (month >= 0) app.monthTip = app.monthHits[month].tip;
            app.hoverPt = p;
            requestRedraw(sceneChanged);
        } else if (month >= 0 || a >= 0) {
            app.hoverPt = p;
            requestRedraw(false);
        }
        TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
        return 0;
    }
    case WM_MOUSELEAVE:
        app.hoverA = app.hoverB = app.hoverMonth = -1;
        app.monthTip.clear();
        requestRedraw(false);
        return 0;
    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &p);
        p = toLogical(p);
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        app.hoverMonth = -1;
        app.monthTip.clear();
        if (p.x < 340) {
            app.menuScroll += delta > 0 ? -1 : 1;
            requestRedraw();
        } else if (app.picker == 0 && app.view == View::Overview) {
            app.overviewScroll += delta > 0 ? -3 : 3;
            requestRedraw();
        } else if (app.picker == 0 && (dailyZoomView() || periodZoomView())) {
            int k = probeAt(p);
            if (k >= 0) {
                zoomAt(p, app.probes[k].plot, delta > 0);
                app.hoverA = app.hoverB = -1;
                saveSession();
                requestRedraw();
            }
        }
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, RGB(32, 54, 72));
        SetBkColor(dc, RGB(255, 255, 255));
        static HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
        return (LRESULT)brush;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(hwnd, &p); p = toLogical(p);
            if (app.dragging) { SetCursor(LoadCursor(nullptr, IDC_SIZEWE)); return TRUE; }
            for (const auto& h : app.hits) if (h.id >= 20 && h.id <= 23 && PtInRect(&h.rc, p)) {
                SetCursor(LoadCursor(nullptr, IDC_IBEAM));
                return TRUE;
            }
            for (const auto& h : app.hits) if (h.id != 13 && PtInRect(&h.rc, p)) {
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                return TRUE;
            }
            for (const auto& pr : app.probes) if (!pr.year.empty() && PtInRect(&pr.plot, p)) {
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    }
    case WM_KEYDOWN: {
        app.hoverMonth = -1;
        app.monthTip.clear();
        int step = std::max(2, (app.y1 - app.y0) / 6);
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (wp == VK_ESCAPE) {
            if (app.picker) { app.picker = 0; requestRedraw(); return 0; }
            DestroyWindow(hwnd);
            return 0;
        }
        if (wp >= '1' && wp <= '9') app.view = (View)(wp - '0');
        else if (wp == '0') app.view = View::Overview;
        else if (wp == 'T') app.view = View::Thermal;
        else if (wp == 'P') app.view = View::RainIdx;
        else if (wp == 'H') app.view = View::Heat;
        else if (wp == 'S') saveFiles();
        else if (wp == 'U') updateAllFromOsmer();
        else if (wp == 'M') { cycleSeason(); resetZoom(); }
        else if (wp == 'V') {
            do { app.bandVar = (app.bandVar + 1) % NVAR; } while (app.bandVar == VDIR || (!st().hasVar[app.bandVar] && app.bandVar != VTM));
            app.view = View::Band;
        }
        else if (wp == 'C' && ctrl) copyChartToClipboard();
        else if (wp == 'C') app.refMode = app.refMode == RefMode::Station ? RefMode::Visible : app.refMode == RefMode::Visible ? RefMode::Clima1991 : RefMode::Station;
        else if (wp == 'Z') { fullPeriod(); resetZoom(); }
        else if (wp == VK_PRIOR) changeStation(app.current - 1);
        else if (wp == VK_NEXT) changeStation(app.current + 1);
        else if (wp == VK_LEFT) { app.y0 -= step; app.y1 -= step; clampPeriod(); }
        else if (wp == VK_RIGHT) { app.y0 += step; app.y1 += step; clampPeriod(); }
        else if (wp == VK_OEM_PLUS || wp == VK_ADD) { int m = (app.y0 + app.y1) / 2; int h = std::max(2, (app.y1 - app.y0) / 3); app.y0 = m - h; app.y1 = m + h; clampPeriod(); }
        else if (wp == VK_OEM_MINUS || wp == VK_SUBTRACT) { app.y0 -= step; app.y1 += step; clampPeriod(); }
        else if (wp == 'G') {
            app.view = View::Year;
            int lo, hi;
            focusRange(lo, hi);
            int y = app.focusYear + 1;
            if (y > hi) y = lo;
            app.focusYear = y;
        }
        saveSession();
        requestRedraw();
        return 0;
    }
    case WM_SIZE:
        if (app.yearEdit && IsWindowVisible(app.yearEdit)) closeYearEdit(true);
        requestRedraw();
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, WatchTimer);
        cancelOsmerUpdate();
        saveSession();
        app.hwnd = nullptr;
        stopBackgroundLoad();
        if (app.yearFont) DeleteObject(app.yearFont);
        if (app.scene) DeleteObject(app.scene);
        app.scene = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Cartella osmer: quella che contiene dati_stazioni, salendo dall'eseguibile.
static std::wstring findRoot() {
    std::filesystem::path p(exeDir());
    for (int i = 0; i < 4; ++i) {
        std::error_code ec;
        if (std::filesystem::is_directory(p / L"dati_stazioni", ec)) return p.wstring();
        if (!p.has_parent_path() || p.parent_path() == p) break;
        p = p.parent_path();
    }
    return std::filesystem::path(exeDir()).parent_path().wstring();
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    enableDpiAwareness();
    app.root = findRoot();
    app.outDir = joinPath(app.root, L"grafici_console");
    app.stations = discoverStations(joinPath(app.root, L"dati_stazioni"));
    // Prima installazione: nessun CSV. La finestra si apre lo stesso e invita
    // a scaricare lo storico con "Aggiorna tutte da OSMER".
    {
        std::error_code ec;
        std::filesystem::create_directories(joinPath(app.root, L"dati_stazioni"), ec);
    }
    app.current = 0;
    for (int i = 0; i < (int)app.stations.size(); ++i) if (app.stations[i]->slug == L"udine_s_o") app.current = i;
    bool session = loadSession();
    bool exportMode = wcsstr(GetCommandLineW(), L"--export") != nullptr;
    // --station=slug sceglie la stazione all'avvio (utile con --export).
    if (const wchar_t* arg = wcsstr(GetCommandLineW(), L"--station=")) {
        std::wstring slug(arg + 10);
        slug = slug.substr(0, slug.find_first_of(L" 	\""));
        for (int i = 0; i < (int)app.stations.size(); ++i) if (app.stations[i]->slug == slug) { app.current = i; session = false; }
    }
    // La stazione scelta si legge subito; le altre in background.
    if (haveStation()) loadStation(st());
    else app.status = L"Nessun dato in dati_stazioni: premi AGGIORNA TUTTE DA OSMER per scaricare lo storico (prima installazione).";
    if (haveStation()) {
        int y0 = app.y0, y1 = app.y1, fy = app.focusYear;
        selectStation(app.current, false);
        if (session) { app.y0 = y0; app.y1 = y1; clampPeriod(); int lo, hi; focusRange(lo, hi); app.focusYear = std::clamp(fy, lo, hi); }
        else app.focusYear = st().lastYear;
    }
    GdiplusStartupInput in; ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &in, nullptr) != Ok) return 1;
    if (exportMode) {
        // Prova senza finestra: carica tutto e salva ogni vista della stazione.
        for (auto& s : app.stations) if (!s->loaded) loadStation(*s);
        std::error_code ec;
        std::filesystem::create_directories(app.outDir, ec);
        bool ok = true;
        for (int v = 0; v < ViewCount && haveStation(); ++v) { app.view = (View)v; ok = saveFiles() && ok; }
        GdiplusShutdown(token);
        return ok ? 0 : 1;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"OsmerConsoleGrafica";
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"OSMER FVG - console climatica delle stazioni", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1400, 900, nullptr, nullptr, hi, nullptr);
    app.hwnd = hwnd;
    applyDpi(windowDpi(hwnd));
    {
        POINT minSize = minimumWindowSize(hwnd);
        RECT work{0, 0, 1400, 900};
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) work = mi.rcWork;
        int w = std::min<int>(std::max<int>(px(1400), minSize.x), work.right - work.left);
        int h = std::min<int>(std::max<int>(px(900), minSize.y), work.bottom - work.top);
        SetWindowPos(hwnd, nullptr, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2,
            w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);
    std::vector<int> rest;
    for (int i = 0; i < (int)app.stations.size(); ++i) if (!app.stations[i]->loaded) rest.push_back(i);
    startBackgroundLoad(rest);
    SetTimer(hwnd, WatchTimer, 3000, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    GdiplusShutdown(token);
    return 0;
}
