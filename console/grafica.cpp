// Grafica: disegno della finestra e dei grafici con GDI+.
// Stile e logica dei grafici ripresi dalla Console Udine.
#include "osmer_console.h"

static Color C(BYTE r, BYTE g, BYTE b, BYTE a = 255) { return Color(a, r, g, b); }

static void text(Graphics& g, const std::wstring& s, const Font& font, Color col, const RectF& rc,
          StringAlignment h = StringAlignmentNear, StringAlignment v = StringAlignmentCenter) {
    SolidBrush b(col);
    StringFormat fmt;
    fmt.SetAlignment(h);
    fmt.SetLineAlignment(v);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(s.c_str(), -1, &font, rc, &fmt, &b);
}
static void fillRound(Graphics& g, const RectF& r, Color fill, Color line) {
    GraphicsPath path; float d = 12;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
    SolidBrush b(fill); g.FillPath(&b, &path);
    Pen p(line, 1.f); g.DrawPath(&p, &path);
}
static void addHit(const RectF& r, int id) {
    app.hits.push_back(Hit{RECT{(LONG)r.X, (LONG)r.Y, (LONG)r.GetRight(), (LONG)r.GetBottom()}, id});
}
static void addProbe(const RectF& plot, std::vector<std::wstring> tip, std::vector<int> years = {}) {
    if (tip.size() < 2 || plot.Width < 2) return;
    App::Probe pr;
    pr.plot = RECT{(LONG)plot.X, (LONG)plot.Y, (LONG)plot.GetRight(), (LONG)plot.GetBottom()};
    pr.tip = std::move(tip);
    pr.year = std::move(years);
    app.probes.push_back(std::move(pr));
}
static void addRowTip(const RectF& r, std::vector<std::wstring> tip, int year = 0, int month = 0) {
    app.monthHits.push_back(App::MonthHit{RECT{(LONG)r.X, (LONG)r.Y, (LONG)r.GetRight(), (LONG)r.GetBottom()}, year, month, std::move(tip)});
}

static void button(Graphics& g, const RectF& r, const std::wstring& label, bool on, int id, const Font& font) {
    addHit(r, id);
    fillRound(g, r, on ? C(32, 54, 72) : C(255, 255, 255), on ? C(32, 54, 72) : C(206, 216, 222));
    text(g, label, font, on ? C(255, 255, 255) : C(32, 54, 72), r, StringAlignmentCenter, StringAlignmentCenter);
}

struct Bounds { double lo, hi; };

struct AxisScale {
    double lo = 0, hi = 1, step = 1;
    int intervals = 1, decimals = 0;
    double tick(int index) const {
        double value = lo + index * step;
        return std::abs(value) < step * 1e-9 ? 0 : value;
    }
};

// Limiti esterni ai dati e passi 1, 2, 5 moltiplicati per una potenza di dieci.
static AxisScale niceScale(double lo, double hi, double minimumStep = 0) {
    if (!finite(lo) || !finite(hi)) { lo = 0; hi = 1; }
    if (hi < lo) std::swap(lo, hi);
    if (hi == lo) {
        double pad = std::max(minimumStep, std::max(0.5, std::abs(lo) * 0.05));
        lo -= pad; hi += pad;
    }
    double target = (hi - lo) / 6.0;
    double unit = std::pow(10.0, std::floor(std::log10(target)));
    double step = unit, best = std::numeric_limits<double>::infinity();
    for (double multiple : {1.0, 2.0, 5.0, 10.0}) {
        double candidate = std::max(minimumStep, multiple * unit);
        double distance = std::abs(std::log(candidate / target));
        if (distance < best) { best = distance; step = candidate; }
    }
    double first = std::floor(lo / step + 1e-10);
    double last = std::ceil(hi / step - 1e-10);
    if (last <= first) last = first + 1;
    AxisScale axis;
    axis.lo = first * step; axis.hi = last * step; axis.step = step;
    axis.intervals = (int)std::llround(last - first);
    axis.decimals = std::clamp(-(int)std::floor(std::log10(step) + 1e-10), 0, 10);
    return axis;
}

static Bounds boundsOf(const std::vector<Sample>& pts, bool spei) {
    if (spei) {
        double limit = 3;
        for (const auto& p : pts) if (finite(p.value)) limit = std::max(limit, std::ceil(std::abs(p.value)));
        return {-limit, limit};
    }
    if (pts.empty()) return {-1, 1};
    double lo = 0, hi = 0;
    for (auto& p : pts) { lo = std::min(lo, p.value); hi = std::max(hi, p.value); }
    double pad = std::max(0.2, (hi - lo) * 0.1);
    return {lo - pad, hi + pad};
}
static std::wstring statsLine(const std::vector<Sample>& pts) {
    if (pts.empty()) return L"Nessun dato nel periodo selezionato.";
    double mn = pts[0].value, mx = pts[0].value;
    int ymn = pts[0].year, ymx = pts[0].year;
    for (auto& p : pts) {
        if (p.value < mn) { mn = p.value; ymn = p.year; }
        if (p.value > mx) { mx = p.value; ymx = p.year; }
    }
    std::wstring when = std::to_wstring(pts.back().year);
    if (pts.back().month) when += L"-" + std::to_wstring(pts.back().month);
    return L"Ultimo " + f2(pts.back().value) + L" (" + when + L")    minimo " + f2(mn) + L" (" +
           std::to_wstring(ymn) + L")    massimo " + f2(mx) + L" (" + std::to_wstring(ymx) + L")";
}

static Color kCard() { return C(10, 24, 40); }
static Color kCardEdge() { return C(42, 86, 124); }
static Color kGrid() { return C(32, 68, 102); }
static Color kTick() { return C(166, 198, 220); }
static Color kTitle() { return C(236, 244, 250); }
static Color kSub() { return C(154, 186, 210); }
static Color kLine() { return C(236, 246, 252); }
static Color kGold() { return C(240, 196, 112); }
static Color kIce() { return C(120, 214, 228); }
static Color kAmber() { return C(224, 164, 96); }
static Color kMean() { return C(184, 204, 216, 180); }
static Color kCompare() { return C(206, 150, 255); }
static Color kHotColor() { return C(255, 154, 122); }
static Color kGreen() { return C(132, 214, 150); }
static Color kRow() { return C(16, 38, 60); }

static void drawLegend(Graphics& g, const RectF& card, const wchar_t* gold, const wchar_t* ice, const wchar_t* amber,
                const wchar_t* dashed = nullptr, const wchar_t* compare = nullptr) {
    FontFamily ff(L"Segoe UI");
    Font f(&ff, 12, FontStyleRegular, UnitPixel);
    float w = 236.f, h = (dashed ? 82.f : 64.f) + (compare ? 18.f : 0.f);
    RectF box(card.GetRight() - w - 14.f, card.Y + 12.f, w, h);
    fillRound(g, box, C(8, 20, 34), kCardEdge());
    const wchar_t* labels[] = {gold, ice, amber};
    Color cols[] = {kGold(), kIce(), kAmber()};
    for (int i = 0; i < 3; ++i) {
        float y = box.Y + 8.f + i * 18.f;
        Pen sw(cols[i], 2.6f);
        g.DrawLine(&sw, box.X + 12.f, y + 8.f, box.X + 34.f, y + 8.f);
        text(g, labels[i], f, kTitle(), RectF(box.X + 42.f, y, box.Width - 52.f, 16.f));
    }
    if (dashed) {
        float y = box.Y + 62.f;
        Pen sw(kMean(), 1.15f);
        sw.SetDashStyle(DashStyleDash);
        g.DrawLine(&sw, box.X + 12.f, y + 8.f, box.X + 34.f, y + 8.f);
        text(g, dashed, f, kSub(), RectF(box.X + 42.f, y, box.Width - 52.f, 16.f));
    }
    if (compare) {
        float y = box.Y + (dashed ? 80.f : 62.f);
        Pen sw(kCompare(), 2.2f);
        g.DrawLine(&sw, box.X + 12.f, y + 8.f, box.X + 34.f, y + 8.f);
        text(g, compare, f, kTitle(), RectF(box.X + 42.f, y, box.Width - 52.f, 16.f));
    }
}

static std::wstring stationCredit() {
    const auto& s = st();
    std::wstring t = L"Stazione " + s.name;
    if (finite(s.alt)) t += L" (" + plainNum(s.alt, 0) + L" m)";
    t += L"   |   fonte: OSMER FVG, dati giornalieri " + stationPeriod(s);
    if (app.view != View::Band && app.view != View::Rain && app.view != View::Heat && app.view != View::Thermal && app.view != View::RainIdx)
        t += L"   |   climatologia solo dai dati della stazione";
    return t;
}
static void drawCredit(Graphics& g, const RectF& card) {
    FontFamily ff(L"Segoe UI");
    Font foot(&ff, 12, FontStyleRegular, UnitPixel);
    text(g, stationCredit(), foot, kSub(), RectF(card.X + 18, card.GetBottom() - 22, card.Width - 36, 16));
}
static void drawNote(Graphics& g, const RectF& page, const std::wstring& note) {
    FontFamily ff(L"Segoe UI");
    Font f(&ff, 12, FontStyleRegular, UnitPixel);
    text(g, note, f, kSub(), RectF(page.X + 18, page.GetBottom() - 42, page.Width - 36, 18));
}

static void drawYAxis(Graphics& g, const RectF& plot, const AxisScale& axis, float labelWidth = 56.f) {
    Pen grid(kGrid(), 1.f);
    FontFamily ff(L"Segoe UI");
    Font tick(&ff, 12, FontStyleRegular, UnitPixel);
    for (int index = 0; index <= axis.intervals; ++index) {
        float y = plot.GetBottom() - plot.Height * (float)index / axis.intervals;
        g.DrawLine(&grid, plot.X, y, plot.GetRight(), y);
        text(g, plainNum(axis.tick(index), axis.decimals), tick, kTick(),
            RectF(plot.X - labelWidth - 6, y - 9, labelWidth, 18), StringAlignmentFar);
    }
}

static void drawSeries(Graphics& g, const RectF& plot, const std::vector<Sample>& pts, Bounds bd, bool spei, const wchar_t* probeName = nullptr, bool warm = false) {
    if (pts.size() < 2) return;
    auto scale = niceScale(bd.lo, bd.hi, spei ? 1.0 : 0.0);
    bd = {scale.lo, scale.hi};
    // Ascissa sul tempo reale (anni o mesi), cosi' i buchi restano visibili.
    auto tOf = [](const Sample& p) { return p.year + (p.month ? (p.month - 0.5) / 12.0 : 0.0); };
    double ta = tOf(pts.front()), tb = tOf(pts.back());
    if (tb <= ta) tb = ta + 1;
    // Serie annuali a barre: mezzo anno di margine, cosi' le barre estreme restano dentro.
    if (!pts.front().month && pts.size() < 80) { ta -= 0.5; tb += 0.5; }
    auto X = [&](const Sample& p) { return plot.X + plot.Width * (float)((tOf(p) - ta) / (tb - ta)); };
    auto Y = [&](double v) { return plot.Y + plot.Height * (float)(1.0 - (v - bd.lo) / (bd.hi - bd.lo)); };
    if (spei) {
        auto bandRect = [&](double loV, double hiV, Color col) {
            float yA = Y(hiV), yB = Y(loV);
            float top = std::max(plot.Y, std::min(yA, yB));
            float bot = std::min(plot.GetBottom(), std::max(yA, yB));
            if (bot - top < 1.f) return;
            SolidBrush br(col);
            g.FillRectangle(&br, plot.X, top, plot.Width, bot - top);
        };
        bandRect(bd.lo, -2.0, C(176, 98, 58, 88));
        bandRect(-2.0, -1.0, C(196, 140, 78, 52));
        bandRect(-1.0, 1.0, C(24, 52, 78, 70));
        bandRect(1.0, bd.hi, C(64, 170, 196, 58));
    }
    float y0 = Y(0);
    Pen grid(kGrid(), 1.f);
    Pen axis(C(150, 190, 214), 1.15f);
    FontFamily ff(L"Segoe UI");
    Font tick(&ff, 12, FontStyleRegular, UnitPixel);
    drawYAxis(g, plot, scale);
    if (spei) {
        Pen dash(kGold(), 1.15f);
        dash.SetDashStyle(DashStyleDash);
        g.DrawLine(&dash, plot.X, Y(-1), plot.GetRight(), Y(-1));
        g.DrawLine(&dash, plot.X, Y(-2), plot.GetRight(), Y(-2));
    }
    g.DrawLine(&axis, plot.X, y0, plot.GetRight(), y0);
    // Un salto di piu' di un passo interrompe la linea (dati mancanti).
    double gap = pts.front().month ? 1.5 / 12.0 : 1.5;
    auto broken = [&](size_t i) { return i > 0 && tOf(pts[i]) - tOf(pts[i - 1]) > gap; };
    bool yearly = !pts.front().month;
    if (yearly && pts.size() < 80) {
        // Serie annuali: barre, piu' leggibili con pochi anni.
        float bw = std::max(2.f, plot.Width / (float)(tb - ta + 1) * 0.66f);
        for (const auto& p : pts) {
            // warm: scarto di temperatura, positivo = piu' caldo (ambra).
            bool up = p.value >= 0;
            SolidBrush br(up != warm ? C(72, 196, 214, 170) : C(224, 164, 96, 190));
            float yv = Y(p.value);
            g.FillRectangle(&br, X(p) - bw / 2, std::min(yv, y0), bw, std::max(1.f, std::abs(y0 - yv)));
        }
    } else {
        auto fillSide = [&](bool positive, Color col) {
            std::vector<PointF> poly;
            auto flush = [&]() {
                if (poly.size() < 2) { poly.clear(); return; }
                poly.push_back(PointF(poly.back().X, y0));
                poly.push_back(PointF(poly.front().X, y0));
                SolidBrush br(col);
                g.FillPolygon(&br, poly.data(), (INT)poly.size());
                poly.clear();
            };
            for (size_t i = 0; i < pts.size(); ++i) {
                bool keep = positive ? pts[i].value >= 0 : pts[i].value < 0;
                if (!keep || broken(i)) flush();
                if (!keep) continue;
                poly.push_back(PointF(X(pts[i]), Y(pts[i].value)));
            }
            flush();
        };
        fillSide(false, C(224, 164, 96, 115));
        fillSide(true, C(72, 196, 214, 105));
    }
    Pen stroke(spei ? kLine() : kGold(), pts.size() > 900 ? 1.15f : 1.85f);
    stroke.SetLineJoin(LineJoinRound);
    std::vector<PointF> line;
    for (size_t i = 0; i <= pts.size() && !(yearly && pts.size() < 80); ++i) {
        if (i == pts.size() || broken(i)) {
            if (line.size() > 1) g.DrawLines(&stroke, line.data(), (INT)line.size());
            line.clear();
            if (i == pts.size()) break;
        }
        line.push_back(PointF(X(pts[i]), Y(pts[i].value)));
    }
    int yA = pts.front().year, yB = pts.back().year, span = std::max(1, yB - yA);
    int step = span > 80 ? 10 : span > 30 ? 5 : span > 12 ? 2 : 1;
    for (int year = ((yA + step - 1) / step) * step; year <= yB; year += step) {
        float x = plot.X + plot.Width * (float)((year - ta) / (tb - ta));
        if (x < plot.X - 1 || x > plot.GetRight() + 1) continue;
        g.DrawLine(&grid, x, plot.GetBottom(), x, plot.GetBottom() + 6);
        text(g, std::to_wstring(year), tick, kTick(), RectF(x - 30, plot.GetBottom() + 8, 60, 18), StringAlignmentCenter);
    }
    if (probeName) {
        // Tooltip: una voce per ogni posizione orizzontale del periodo.
        int slots = std::max(2, (int)std::lround((tb - ta) * (yearly ? 1 : 12)) + 1);
        std::vector<std::wstring> tips(slots);
        std::vector<int> years(slots, 0);
        size_t j = 0;
        for (int k = 0; k < slots; ++k) {
            double t = ta + (tb - ta) * k / (slots - 1);
            while (j + 1 < pts.size() && std::abs(tOf(pts[j + 1]) - t) <= std::abs(tOf(pts[j]) - t)) ++j;
            const auto& p = pts[j];
            std::wstring s = probeName;
            s += L"  " + std::to_wstring(p.year);
            if (p.month) s += std::wstring(p.month < 10 ? L"-0" : L"-") + std::to_wstring(p.month);
            s += L"   " + f2(p.value);
            if (!p.month && p.n) s += L"   (" + std::to_wstring(p.n) + L" mesi)";
            tips[k] = s;
            years[k] = p.year;
        }
        addProbe(plot, std::move(tips), std::move(years));
    }
}

static void cardTitle(Graphics& g, const RectF& card, const std::wstring& title, const std::wstring& sub) {
    fillRound(g, card, kCard(), kCardEdge());
    FontFamily ff(L"Segoe UI");
    Font titleF(&ff, 18, FontStyleBold, UnitPixel);
    Font subF(&ff, 13, FontStyleRegular, UnitPixel);
    text(g, title, titleF, kTitle(), RectF(card.X + 18, card.Y + 12, card.Width - 270, 24), StringAlignmentNear, StringAlignmentNear);
    text(g, sub, subF, kSub(), RectF(card.X + 18, card.Y + 36, card.Width - 270, 20), StringAlignmentNear, StringAlignmentNear);
}

static void emptyMessage(Graphics& g, const RectF& page, const std::wstring& msg) {
    FontFamily ff(L"Segoe UI");
    Font body(&ff, 15, FontStyleRegular, UnitPixel);
    text(g, msg, body, kSub(), RectF(page.X + 24, page.Y + 100, page.Width - 48, 30));
}

static std::wstring stationTitle() { return st().name + L" - "; }

// ------------------------------------------------------------ pannelli di indici annuali

struct IndexPanel {
    const wchar_t* title; const wchar_t* unit; Color col; int digits;
    std::vector<double> values;
    bool fromZero = true;
};
struct IndexYearInfo { int year; bool partial; int measured, expected; };

static void drawIndexPanels(Graphics& g, const RectF& page, std::vector<IndexPanel>& panels,
                            const std::vector<IndexYearInfo>& years, const wchar_t* measuredLabel) {
    FontFamily ff(L"Segoe UI");
    Font smallF(&ff, 13, FontStyleBold, UnitPixel);
    Font tick(&ff, 12, FontStyleRegular, UnitPixel);
    int cols = panels.size() > 4 ? 3 : 2;
    int rows = ((int)panels.size() + cols - 1) / cols;
    float gx = page.X + 18, gy = page.Y + 80;
    float gw = (page.Width - 36 - 24 * (cols - 1)) / cols, gh = (page.Height - 80 - 50 - 20 * (rows - 1)) / rows;
    int n = (int)years.size();
    for (int k = 0; k < (int)panels.size(); ++k) {
        const auto& pn = panels[k];
        RectF cell(gx + (k % cols) * (gw + 24), gy + (k / cols) * (gh + 20), gw, gh);
        double sx = 0, sy = 0, sxx = 0, sxy = 0; int m = 0;
        double hi = 0, lo = pn.fromZero ? 0 : 1e9;
        for (int i = 0; i < n; ++i) {
            double v = pn.values[i];
            if (!finite(v)) continue;
            hi = std::max(hi, v); lo = std::min(lo, v);
            if (years[i].partial) continue;
            double x = years[i].year;
            sx += x; sy += v; sxx += x * x; sxy += x * v; ++m;
        }
        std::wstring trend;
        double slope = 0, icpt = 0;
        bool hasTrend = m >= 10 && (m * sxx - sx * sx) != 0;
        if (hasTrend) {
            slope = (m * sxy - sx * sy) / (m * sxx - sx * sx);
            icpt = (sy - slope * sx) / m;
            trend = L"tendenza " + f2(slope * 10) + L" " + pn.unit + L"/decennio";
        } else trend = L"tendenza: servono 10 anni completi";
        text(g, pn.title, smallF, kTitle(), RectF(cell.X, cell.Y, cell.Width * 0.6f, 20));
        text(g, trend, tick, kSub(), RectF(cell.X + cell.Width * 0.4f, cell.Y, cell.Width * 0.6f, 20), StringAlignmentFar);
        RectF plot(cell.X + 40, cell.Y + 28, cell.Width - 48, cell.Height - 50);
        if (lo > hi) { lo = 0; hi = 1; }
        auto scale = pn.fromZero ? niceScale(0, std::max(1.0, hi), 1.0) : niceScale(lo, hi, 0.5);
        auto Y = [&](double v) { return plot.Y + plot.Height * (float)(1.0 - (v - scale.lo) / (scale.hi - scale.lo)); };
        drawYAxis(g, plot, scale, 34.f);
        float slot = plot.Width / std::max(1, n);
        float bw = std::max(2.f, slot * 0.7f);
        std::vector<std::wstring> tips;
        std::vector<int> ys;
        int labelStep = n > 30 ? 10 : n > 12 ? 5 : n > 6 ? 2 : 1;
        for (int i = 0; i < n; ++i) {
            double v = pn.values[i];
            float cx = plot.X + slot * (i + 0.5f);
            if (finite(v)) {
                Color c = pn.col;
                SolidBrush br(years[i].partial ? Color(90, c.GetR(), c.GetG(), c.GetB()) : c);
                float base = Y(pn.fromZero ? 0.0 : scale.lo);
                float yv = Y(v);
                g.FillRectangle(&br, cx - bw / 2, std::min(yv, base), bw, std::max(1.f, std::abs(base - yv)));
            }
            if (years[i].year % labelStep == 0)
                text(g, std::to_wstring(years[i].year), tick, kTick(), RectF(cx - 22, plot.GetBottom() + 4, 44, 16), StringAlignmentCenter);
            tips.push_back(std::to_wstring(years[i].year) + (years[i].partial ? L"*" : L"") + L":  "
                + plainNum(v, pn.digits) + L" " + pn.unit + L"    " + measuredLabel + L": " + std::to_wstring(years[i].measured)
                + L"/" + std::to_wstring(years[i].expected) + L"    clic: fascia giornaliera");
            ys.push_back(years[i].year);
        }
        if (hasTrend && n > 1) {
            Pen tp(kTitle(), 1.4f);
            tp.SetDashStyle(DashStyleDash);
            float xa = plot.X + slot * 0.5f, xb = plot.X + slot * (n - 0.5f);
            g.DrawLine(&tp, xa, Y(icpt + slope * years.front().year), xb, Y(icpt + slope * years.back().year));
        }
        addProbe(RectF(plot.X + slot * 0.5f, plot.Y, std::max(2.f, plot.Width - slot), plot.Height), std::move(tips), std::move(ys));
    }
}

// ------------------------------------------------------------ viste

static void paintOverview(Graphics& g, const RectF& page, bool controls) {
    FontFamily ff(L"Segoe UI");
    Font head(&ff, 12, FontStyleBold, UnitPixel);
    Font body(&ff, 14, FontStyleRegular, UnitPixel);
    Font smallFt(&ff, 12, FontStyleRegular, UnitPixel);
    std::wstring when = seasonLabel() + L" " + std::to_wstring(app.focusYear);
    cardTitle(g, page, L"Panoramica delle stazioni - " + when,
              L"Scarti dalla climatologia di ciascuna stazione (" + refLabel() + L"), solo mesi completi   |   clic su una riga: apre la stazione");
    drawLegend(g, page, L"scarto di temperatura", L"piu fresco / piu piovoso", L"piu caldo / piu secco");
    auto rows = overviewRows();
    long newest = 0;
    for (const auto& s : app.stations) if (s->loaded && !s->days.empty()) newest = std::max(newest, s->days.back().n);
    float top = page.Y + 96;
    float x0 = page.X + 18, w = page.Width - 36;
    // Colonne in frazioni della larghezza.
    const float fx[] = {0.f, 0.27f, 0.34f, 0.46f, 0.57f, 0.66f, 0.83f, 1.f};
    const wchar_t* heads[] = {L"Stazione", L"Quota", L"Periodo", L"Ultimo dato", L"Completezza T/P", L"Scarto temperatura", L"Pioggia rispetto alla media"};
    const int sortCol[] = {0, -1, -1, 3, -1, 1, 2};
    for (int i = 0; i < 7; ++i) {
        RectF r(x0 + w * fx[i], top, w * (fx[i + 1] - fx[i]) - 6, 20);
        bool sorted = sortCol[i] >= 0 && sortCol[i] == app.overviewSort;
        text(g, std::wstring(heads[i]) + (sorted ? L"  v" : L""), head, sorted ? kGold() : kSub(), r);
        if (controls && sortCol[i] >= 0) addHit(r, 450 + sortCol[i]);
    }
    float rowTop = top + 26;
    float pitch = 26.f;
    int fit = std::max(1, (int)((page.GetBottom() - 50 - rowTop) / pitch));
    int maxScroll = std::max(0, (int)rows.size() - fit);
    app.overviewScroll = std::clamp(app.overviewScroll, 0, maxScroll);
    for (int k = 0; k < fit && k + app.overviewScroll < (int)rows.size(); ++k) {
        const auto& r = rows[k + app.overviewScroll];
        const auto& s = *app.stations[r.index];
        RectF row(x0 - 6, rowTop + k * pitch, w + 12, pitch - 3);
        bool cur = r.index == app.current;
        SolidBrush band(cur ? C(40, 72, 100) : (k % 2 ? kRow() : C(12, 30, 48)));
        g.FillRectangle(&band, row);
        if (controls) {
            addHit(row, 1000 + r.index);
            std::vector<std::wstring> tip = {s.name + (s.code.empty() ? L"" : L"  [" + s.code + L"]")};
            if (finite(s.lat)) tip.push_back(L"Lat " + plainNum(s.lat, 4) + L"  Lon " + plainNum(s.lon, 4) + L"  quota " + plainNum(s.alt, 0) + L" m");
            tip.push_back(L"File: " + s.file.substr(app.root.size() + 1));
            if (s.loaded) tip.push_back(L"Giorni con misure: " + std::to_wstring(s.days.size()) + L"   mesi: " + std::to_wstring(s.months.size()));
            if (r.nMonths) tip.push_back(L"Scarto calcolato su " + std::to_wstring(r.nMonths) + L" mesi completi");
            addRowTip(row, std::move(tip));
        }
        float y = row.Y;
        auto cell = [&](int i) { return RectF(x0 + w * fx[i], y, w * (fx[i + 1] - fx[i]) - 6, row.Height); };
        text(g, s.name, body, kTitle(), cell(0));
        text(g, finite(s.alt) ? plainNum(s.alt, 0) + L" m" : L"-", smallFt, kSub(), cell(1));
        text(g, stationPeriod(s), smallFt, kSub(), cell(2));
        if (s.loaded && !s.days.empty()) {
            long age = newest - s.days.back().n;
            text(g, dateText(s.lastY, s.lastM, s.lastD), smallFt, age > 7 ? kAmber() : kGreen(), cell(3));
            double cT = s.expectedDays ? 100.0 * s.measuredT / s.expectedDays : NaN;
            double cP = s.expectedDays ? 100.0 * s.measuredP / s.expectedDays : NaN;
            text(g, plainNum(cT, 0) + L"% / " + plainNum(cP, 0) + L"%", smallFt, kSub(), cell(4));
        } else text(g, s.loaded ? L"nessun dato" : L"...", smallFt, kSub(), cell(3));
        // Scarto T: barra centrata, scala +-3 C.
        RectF ct = cell(5);
        if (finite(r.tAn)) {
            float mid = ct.X + ct.Width * 0.62f, half = ct.Width * 0.36f;
            float len = (float)std::clamp(r.tAn / 3.0, -1.0, 1.0) * half;
            SolidBrush br(r.tAn >= 0 ? C(224, 164, 96, 200) : C(120, 214, 228, 200));
            g.FillRectangle(&br, std::min(mid, mid + len), y + 6, std::abs(len), row.Height - 12);
            Pen axis(kGrid(), 1.f);
            g.DrawLine(&axis, mid, y + 3, mid, y + row.Height - 3);
            text(g, f2(r.tAn) + L" C", smallFt, r.tAn >= 0 ? kAmber() : kIce(), RectF(ct.X, y, ct.Width * 0.24f + 20, row.Height));
        } else text(g, L"-", smallFt, kSub(), ct);
        RectF cp = cell(6);
        if (finite(r.pPct)) {
            float x = cp.X + cp.Width * 0.30f, full = cp.Width * 0.66f;
            float len = (float)std::clamp(r.pPct / 200.0, 0.0, 1.0) * full;
            SolidBrush br(r.pPct >= 100 ? C(72, 196, 214, 190) : C(224, 164, 96, 190));
            g.FillRectangle(&br, x, y + 6, len, row.Height - 12);
            Pen axis(kGrid(), 1.f);
            g.DrawLine(&axis, x + full / 2, y + 3, x + full / 2, y + row.Height - 3);
            text(g, plainNum(r.pPct, 0) + L"%", smallFt, r.pPct >= 100 ? kIce() : kAmber(), RectF(cp.X, y, cp.Width * 0.3f, row.Height));
        } else text(g, L"-", smallFt, kSub(), cp);
    }
    std::wstring foot = L"Righe " + std::to_wstring(app.overviewScroll + 1) + L"-" + std::to_wstring(std::min((int)rows.size(), app.overviewScroll + fit))
        + L" di " + std::to_wstring(rows.size()) + (maxScroll ? L"   |   rotella per scorrere" : L"")
        + L"   |   data verde: aggiornata, ambra: ferma da piu di 7 giorni rispetto alla stazione piu recente   |   barra pioggia: centro = 100%";
    text(g, foot, smallFt, kSub(), RectF(page.X + 18, page.GetBottom() - 26, page.Width - 36, 18));
}

static void paintClima(Graphics& g, const RectF& page, bool controls) {
    FontFamily ff(L"Segoe UI");
    Font head(&ff, 12, FontStyleBold, UnitPixel);
    Font body(&ff, 14, FontStyleRegular, UnitPixel);
    Font bold(&ff, 14, FontStyleBold, UnitPixel);
    Font smallFt(&ff, 12, FontStyleRegular, UnitPixel);
    const auto& s = st();
    int a, b;
    refRange(s, a, b);
    a = std::max(a, s.firstYear); b = std::min(b, s.lastYear);
    cardTitle(g, page, L"Climatologia - " + s.name,
              L"Medie " + std::to_wstring(a) + L"-" + std::to_wstring(b) + L" (" + refLabel() + L") sui mesi completi   |   record su tutto il periodo " + stationPeriod(s));
    drawLegend(g, page, L"temperatura", L"pioggia", L"record");
    if (s.months.empty()) { emptyMessage(g, page, L"Nessun dato per questa stazione."); drawCredit(g, page); return; }
    const auto& rows = climaTable();
    float x0 = page.X + 18, w = page.Width - 36;
    float top = page.Y + 96;
    const float fx[] = {0.f, 0.085f, 0.125f, 0.185f, 0.245f, 0.305f, 0.37f, 0.42f, 0.47f, 0.525f, 0.595f, 0.66f, 0.775f, 0.89f, 1.f};
    const wchar_t* heads[] = {L"Mese", L"Anni", L"Tmedia", L"Tmin", L"Tmax", L"Pioggia", L"Gg >=1", L"Umid.", L"Vento", L"Radiaz.", L"Press.",
                              L"Tmax record", L"Tmin record", L"Pioggia max 24h"};
    const wchar_t* units[] = {L"", L"", L"C", L"C", L"C", L"mm", L"giorni", L"%", L"km/h", L"kJ/m2", L"hPa", L"C", L"C", L"mm"};
    for (int i = 0; i < 14; ++i) {
        text(g, heads[i], head, i >= 11 ? kHotColor() : kSub(), RectF(x0 + w * fx[i], top, w * (fx[i + 1] - fx[i]), 18));
        text(g, units[i], smallFt, kGrid(), RectF(x0 + w * fx[i], top + 16, w * (fx[i + 1] - fx[i]), 14));
    }
    float rowH = std::clamp((page.Height - 96 - 40 - 60 - 160) / 13.f, 20.f, 28.f);
    float rowTop = top + 36;
    auto recText = [](double v, int y, int digits) { return finite(v) ? plainNum(v, digits) + L"  (" + std::to_wstring(y) + L")" : std::wstring(L"-"); };
    for (int k = 1; k <= 13; ++k) {
        int m = k <= 12 ? k : 0;
        const auto& r = rows[m];
        float y = rowTop + (k - 1) * rowH + (k == 13 ? 6.f : 0.f);
        RectF row(x0 - 6, y, w + 12, rowH - 2);
        SolidBrush band(k == 13 ? C(40, 72, 100) : (k % 2 ? kRow() : C(12, 30, 48)));
        g.FillRectangle(&band, row);
        auto cell = [&](int i) { return RectF(x0 + w * fx[i], y, w * (fx[i + 1] - fx[i]) - 4, rowH - 2); };
        const Font& f = k == 13 ? bold : body;
        text(g, k == 13 ? L"Anno" : monthName(m), f, kTitle(), cell(0));
        text(g, r.years ? std::to_wstring(r.years) : L"-", smallFt, kSub(), cell(1));
        text(g, plainNum(r.t, 1), f, kGold(), cell(2));
        text(g, plainNum(r.tn, 1), f, kIce(), cell(3));
        text(g, plainNum(r.tx, 1), f, kAmber(), cell(4));
        text(g, plainNum(r.p, 0), f, kIce(), cell(5));
        text(g, plainNum(r.wet, 1), f, kTitle(), cell(6));
        text(g, plainNum(r.um, 0), f, kTitle(), cell(7));
        text(g, plainNum(r.vm, 1), f, kTitle(), cell(8));
        text(g, plainNum(r.rad, 0), f, kTitle(), cell(9));
        text(g, plainNum(r.pres, 1), f, kTitle(), cell(10));
        text(g, recText(r.recTx, r.recTxY, 1), smallFt, kHotColor(), cell(11));
        text(g, recText(r.recTn, r.recTnY, 1), smallFt, kIce(), cell(12));
        text(g, recText(r.recP, r.recPY, 1), smallFt, kTitle(), cell(13));
        if (controls) {
            auto full = [&](double v, int y, int md, int mm) {
                if (!finite(v)) return std::wstring(L"-");
                return plainNum(v, 1) + L" il " + dateText(y, mm, md);
            };
            int mTx = k == 13 ? r.recM : m;
            int mTn = k == 13 ? r.recTnD / 100 : m, dTn = k == 13 ? r.recTnD % 100 : r.recTnD;
            int mP = k == 13 ? r.recPD / 100 : m, dP = k == 13 ? r.recPD % 100 : r.recPD;
            std::vector<std::wstring> tip = {
                std::wstring(k == 13 ? L"Anno" : monthName(m)) + L" - " + s.name,
                L"Tmax piu alta: " + full(r.recTx, r.recTxY, r.recTxD, mTx) + L" C",
                L"Tmin piu bassa: " + full(r.recTn, r.recTnY, dTn, mTn) + L" C",
                L"Pioggia massima in un giorno: " + full(r.recP, r.recPY, dP, mP) + L" mm",
                L"Medie su " + std::to_wstring(r.years) + L" anni con il mese completo (" + std::to_wstring(a) + L"-" + std::to_wstring(b) + L")"};
            addRowTip(row, std::move(tip));
        }
    }
    // Climogramma sotto la tabella.
    float gTop = rowTop + 13 * rowH + 24;
    RectF plot(page.X + 70, gTop + 10, page.Width - 140, page.GetBottom() - 60 - gTop - 10);
    if (plot.Height >= 110) {
        double pmax = 1, tlo = 1e9, thi = -1e9;
        for (int m = 1; m <= 12; ++m) {
            if (finite(rows[m].p)) pmax = std::max(pmax, rows[m].p);
            for (double v : {rows[m].tn, rows[m].tx, rows[m].t}) if (finite(v)) { tlo = std::min(tlo, v); thi = std::max(thi, v); }
        }
        if (tlo > thi) { tlo = 0; thi = 1; }
        auto ps = niceScale(0, pmax, 1.0);
        auto ts = niceScale(std::min(0.0, tlo), thi, 1.0);
        auto YP = [&](double v) { return plot.Y + plot.Height * (float)(1.0 - (v - ps.lo) / (ps.hi - ps.lo)); };
        auto YT = [&](double v) { return plot.Y + plot.Height * (float)(1.0 - (v - ts.lo) / (ts.hi - ts.lo)); };
        drawYAxis(g, plot, ts, 44.f);
        for (int i = 0; i <= ps.intervals; ++i) {
            float yy = plot.GetBottom() - plot.Height * (float)i / ps.intervals;
            text(g, plainNum(ps.tick(i), ps.decimals), smallFt, kIce(), RectF(plot.GetRight() + 6, yy - 9, 60, 18));
        }
        text(g, L"C", head, kGold(), RectF(plot.X - 60, plot.Y - 22, 50, 16), StringAlignmentFar);
        text(g, L"mm", head, kIce(), RectF(plot.GetRight() + 6, plot.Y - 22, 50, 16));
        float slot = plot.Width / 12.f;
        std::vector<PointF> lt, ln, lx;
        std::vector<std::wstring> tips;
        for (int m = 1; m <= 12; ++m) {
            float cx = plot.X + slot * (m - 0.5f);
            if (finite(rows[m].p)) {
                SolidBrush br(C(72, 168, 210, 120));
                g.FillRectangle(&br, cx - slot * 0.3f, YP(rows[m].p), slot * 0.6f, YP(0) - YP(rows[m].p));
            }
            if (finite(rows[m].t)) lt.push_back(PointF(cx, YT(rows[m].t)));
            if (finite(rows[m].tn)) ln.push_back(PointF(cx, YT(rows[m].tn)));
            if (finite(rows[m].tx)) lx.push_back(PointF(cx, YT(rows[m].tx)));
            text(g, monthAbbr(m), smallFt, kTick(), RectF(cx - 20, plot.GetBottom() + 4, 40, 16), StringAlignmentCenter);
            tips.push_back(std::wstring(monthName(m)) + L":  Tmedia " + plainNum(rows[m].t, 1) + L"  Tmin " + plainNum(rows[m].tn, 1)
                + L"  Tmax " + plainNum(rows[m].tx, 1) + L"  pioggia " + plainNum(rows[m].p, 0) + L" mm");
        }
        auto lineOf = [&](std::vector<PointF>& pts, Color c, float wdt) {
            if (pts.size() < 2) return;
            Pen p(c, wdt); p.SetLineJoin(LineJoinRound);
            g.DrawLines(&p, pts.data(), (INT)pts.size());
            SolidBrush d(c);
            for (auto& q : pts) g.FillEllipse(&d, q.X - 3.f, q.Y - 3.f, 6.f, 6.f);
        };
        lineOf(ln, kIce(), 1.6f);
        lineOf(lx, kAmber(), 1.6f);
        lineOf(lt, kGold(), 2.4f);
        addProbe(RectF(plot.X + slot * 0.5f, plot.Y, plot.Width - slot, plot.Height), std::move(tips));
    }
    drawCredit(g, page);
}

static void paintYear(Graphics& g, const RectF& page, bool controls) {
    FontFamily ff(L"Segoe UI");
    Font h1(&ff, 20, FontStyleBold, UnitPixel);
    Font head(&ff, 12, FontStyleBold, UnitPixel);
    Font body(&ff, 15, FontStyleRegular, UnitPixel);
    Font bold(&ff, 15, FontStyleBold, UnitPixel);
    Font smallFt(&ff, 12, FontStyleRegular, UnitPixel);
    const auto& s = st();
    int y = app.focusYear;
    fillRound(g, page, kCard(), kCardEdge());
    text(g, s.name + L" - anno " + std::to_wstring(y), h1, kTitle(), RectF(page.X + 24, page.Y + 16, page.Width - 300, 28), StringAlignmentNear, StringAlignmentNear);
    text(g, L"Scarti dalla climatologia della stazione (" + refLabel() + L")   |   G: anno successivo   |   mouse su un mese: dettagli",
         smallFt, kSub(), RectF(page.X + 24, page.Y + 48, page.Width - 300, 18));
    drawLegend(g, page, L"pioggia sotto la media", L"piu freddo / piu pioggia", L"piu caldo");
    double cT[13], cP[13]; int nT[13], nP[13];
    monthClimate(s, cT, cP, nT, nP);
    float x0 = page.X + 24, w = page.Width - 48;
    float top = page.Y + 96;
    const float fx[] = {0.f, 0.13f, 0.22f, 0.31f, 0.39f, 0.47f, 0.56f, 0.65f, 0.72f, 0.79f, 0.86f, 0.93f, 1.f};
    const wchar_t* heads[] = {L"Mese", L"Tmedia", L"Scarto", L"Tmin", L"Tmax", L"Pioggia", L"Scarto", L"%", L"Gg >=1", L"SPEI-3", L"SPEI-6", L"SPEI-12"};
    for (int i = 0; i < 12; ++i) text(g, heads[i], head, kSub(), RectF(x0 + w * fx[i], top, w * (fx[i + 1] - fx[i]), 18));
    float rowH = std::clamp((page.Height - 96 - 30 - 90) / 13.f, 20.f, 34.f);
    float rowTop = top + 24;
    double sumT = 0, sumAT = 0, sumP = 0, sumAP = 0, sumCP = 0, sumN = 0, sumX = 0; int kT = 0, kP = 0, kN = 0, kX = 0, wetY = 0; bool anyWet = false;
    double aT[13], pPct[13];
    for (int m = 0; m <= 12; ++m) aT[m] = pPct[m] = NaN;
    for (int k = 1; k <= 13; ++k) {
        float yy = rowTop + (k - 1) * rowH + (k == 13 ? 6.f : 0.f);
        RectF row(x0 - 6, yy, w + 12, rowH - 2);
        SolidBrush band(k == 13 ? C(40, 72, 100) : (k % 2 ? kRow() : C(12, 30, 48)));
        g.FillRectangle(&band, row);
        auto cell = [&](int i) { return RectF(x0 + w * fx[i], yy, w * (fx[i + 1] - fx[i]) - 4, rowH - 2); };
        auto colored = [&](int i, double v, const std::wstring& suffix, bool heat, const Font& f) {
            if (!finite(v)) { text(g, L"-", f, kSub(), cell(i)); return; }
            Color c = heat ? (v >= 0 ? kAmber() : kIce()) : (v >= 0 ? kIce() : kGold());
            text(g, f2(v) + suffix, f, c, cell(i));
        };
        if (k <= 12) {
            int m = k;
            const Month* mo = findMonth(s, y, m);
            std::wstring name = monthName(m);
            if (mo && mo->partial) name += L" *";
            text(g, name, body, kTitle(), cell(0));
            if (controls) addRowTip(row, monthDetails(y, m), y, m);
            if (!mo || (!mo->nt && !mo->np)) { text(g, L"nessuna misura", smallFt, kSub(), cell(1)); continue; }
            bool tOk = mo->nt > 0 && (mo->okT || mo->partial);
            bool pOk = mo->np > 0 && (mo->okP || mo->partial);
            // Mesi incompleti: valore tra parentesi e niente scarto.
            text(g, mo->nt ? (tOk ? plainNum(mo->t, 1) : L"(" + plainNum(mo->t, 1) + L")") : L"-", body, kGold(), cell(1));
            colored(2, tOk ? mo->t - cT[m] : NaN, L"", true, body);
            text(g, plainNum(mo->tn, 1), body, kIce(), cell(3));
            text(g, plainNum(mo->tx, 1), body, kAmber(), cell(4));
            text(g, mo->np ? (pOk ? plainNum(mo->p, 1) : L"(" + plainNum(mo->p, 1) + L")") : L"-", body, kIce(), cell(5));
            colored(6, pOk ? mo->p - cP[m] : NaN, L"", false, body);
            text(g, pOk && finite(cP[m]) && cP[m] > 0 ? plainNum(100 * mo->p / cP[m], 0) + L"%" : L"-", body, kTitle(), cell(7));
            int wet = 0;
            for (const auto& d : s.days) if (d.y == y && d.m == m && d.has(VP) && d.v[VP] >= 1) ++wet;
            text(g, mo->np ? std::to_wstring(wet) : L"-", body, kTitle(), cell(8));
            if (mo->np) { wetY += wet; anyWet = true; }
            text(g, f2(mo->spei3), body, kTitle(), cell(9));
            text(g, f2(mo->spei6), body, kTitle(), cell(10));
            text(g, f2(mo->spei12), body, kTitle(), cell(11));
            if (mo->okT && finite(cT[m])) { sumT += mo->t; sumAT += mo->t - cT[m]; ++kT; aT[m] = mo->t - cT[m]; }
            if (!mo->partial && mo->nn >= mo->dim - 5) { sumN += mo->tn; ++kN; }
            if (!mo->partial && mo->nx >= mo->dim - 5) { sumX += mo->tx; ++kX; }
            if (pOk && finite(cP[m]) && cP[m] > 0) pPct[m] = 100 * mo->p / cP[m];
            if ((mo->okP || mo->partial) && mo->np && finite(cP[m])) { sumP += mo->p; sumAP += mo->p - cP[m]; sumCP += cP[m]; ++kP; }
        } else {
            text(g, L"Anno", bold, kTitle(), cell(0));
            text(g, kT ? plainNum(sumT / kT, 1) : L"-", bold, kGold(), cell(1));
            colored(2, kT ? sumAT / kT : NaN, L"", true, bold);
            text(g, kN ? plainNum(sumN / kN, 1) : L"-", bold, kIce(), cell(3));
            text(g, kX ? plainNum(sumX / kX, 1) : L"-", bold, kAmber(), cell(4));
            text(g, kP ? plainNum(sumP, 0) : L"-", bold, kIce(), cell(5));
            colored(6, kP ? sumAP : NaN, L"", false, bold);
            text(g, kP && sumCP > 0 ? plainNum(100 * sumP / sumCP, 0) + L"%" : L"-", bold, kTitle(), cell(7));
            text(g, anyWet ? std::to_wstring(wetY) : L"-", bold, kTitle(), cell(8));
            text(g, std::to_wstring(kT) + L" mesi T, " + std::to_wstring(kP) + L" mesi P", smallFt, kSub(), RectF(x0 + w * fx[9], yy, w * 0.21f, rowH - 2));
        }
    }
    float ny = rowTop + 13 * rowH + 14;
    text(g, L"* mese parziale: scarti rispetto alla media del mese intero.  (valore) tra parentesi: mese incompleto, escluso dalle medie.  Riga Anno: media degli scarti T e somma degli scarti P sui mesi disponibili.",
         smallFt, kSub(), RectF(page.X + 24, ny, page.Width - 48, 18));
    text(g, L"SPEI calibrato sui soli dati della stazione (logistica generalizzata, PET Thornthwaite): servono almeno 10 anni di mesi completi.",
         smallFt, kSub(), RectF(page.X + 24, ny + 18, page.Width - 48, 18));
    // Sotto la tabella, se c'e' spazio: scarti mensili di temperatura e pioggia in percentuale.
    float gTop = ny + 52, gBottom = page.GetBottom() - 40;
    if (gBottom - gTop >= 120) {
        float half = (page.Width - 48 - 40) / 2.f;
        for (int panel = 0; panel < 2; ++panel) {
            RectF area(page.X + 24 + panel * (half + 40), gTop, half, gBottom - gTop);
            text(g, panel == 0 ? L"Scarto mensile di temperatura (C)" : L"Pioggia del mese in % della media", head, kSub(),
                 RectF(area.X, area.Y, area.Width, 18));
            RectF plot(area.X + 44, area.Y + 26, area.Width - 50, area.Height - 48);
            double lo = 0, hi = 0;
            for (int m = 1; m <= 12; ++m) {
                double v = panel == 0 ? aT[m] : pPct[m] - 100;
                if (finite(v)) { lo = std::min(lo, v); hi = std::max(hi, v); }
            }
            auto sc = niceScale(lo, hi, panel == 0 ? 0.5 : 10.0);
            auto Y = [&](double v) { return plot.Y + plot.Height * (float)(1.0 - (v - sc.lo) / (sc.hi - sc.lo)); };
            Pen grid(kGrid(), 1.f);
            for (int i = 0; i <= sc.intervals; ++i) {
                float yy = plot.GetBottom() - plot.Height * (float)i / sc.intervals;
                g.DrawLine(&grid, plot.X, yy, plot.GetRight(), yy);
                double v = sc.tick(i) + (panel == 0 ? 0 : 100);
                text(g, plainNum(v, panel == 0 ? sc.decimals : 0), smallFt, kTick(), RectF(plot.X - 48, yy - 9, 42, 18), StringAlignmentFar);
            }
            float slot = plot.Width / 12.f, base = Y(0);
            std::vector<std::wstring> tips;
            for (int m = 1; m <= 12; ++m) {
                double v = panel == 0 ? aT[m] : pPct[m] - 100;
                float cx = plot.X + slot * (m - 0.5f);
                if (finite(v)) {
                    Color c = panel == 0 ? (v >= 0 ? kAmber() : kIce()) : (v >= 0 ? kIce() : kAmber());
                    SolidBrush br(c);
                    float yv = Y(v);
                    g.FillRectangle(&br, cx - slot * 0.32f, std::min(yv, base), slot * 0.64f, std::max(1.f, std::abs(base - yv)));
                }
                text(g, monthAbbr(m), smallFt, kTick(), RectF(cx - 20, plot.GetBottom() + 4, 40, 16), StringAlignmentCenter);
                tips.push_back(std::wstring(monthName(m)) + L" " + std::to_wstring(y) + L":  "
                    + (panel == 0 ? f2(aT[m]) + L" C" : plainNum(pPct[m], 0) + L"% della media"));
            }
            Pen axis(C(150, 190, 214), 1.15f);
            g.DrawLine(&axis, plot.X, base, plot.GetRight(), base);
            addProbe(RectF(plot.X + slot * 0.5f, plot.Y, plot.Width - slot, plot.Height), std::move(tips));
        }
    }
    drawCredit(g, page);
}

static void paintBand(Graphics& g, const RectF& page) {
    FontFamily ff(L"Segoe UI");
    int above = 0, below = 0;
    const auto& all = bandSeries(&above, &below);
    const auto& cmpAll = bandCompare();
    size_t i0 = 0, i1 = 0;
    zoomRange(all.size(), i0, i1);
    const bool zoomed = all.size() > 1 && (i0 > 0 || i1 < all.size() - 1);
    std::vector<BandPt> pts;
    std::vector<double> cmp;
    if (!all.empty()) pts.assign(all.begin() + i0, all.begin() + i1 + 1);
    if (cmpAll.size() == all.size() && !all.empty()) cmp.assign(cmpAll.begin() + i0, cmpAll.begin() + i1 + 1);
    const int var = app.bandVar;
    const int dg = varDigits(var);
    std::wstring vname = varName(var), unit = varUnit(var);
    std::wstring title = stationTitle() + vname + (app.season == Season::Month
        ? L" - " + std::wstring(monthName(app.month)) + L" " + std::to_wstring(app.focusYear)
        : L" - anno " + std::to_wstring(app.focusYear));
    std::wstring xlab = app.season == Season::Month ? L"Giorno del mese" : app.season == Season::Year ? L"Mese" : seasonLabel();
    std::wstring sub = L"Fascia P5-P95 di tutti gli anni della stazione   |   sopra P95: " + std::to_wstring(above) +
                       L" giorni   |   sotto P5: " + std::to_wstring(below) + L"   |   " + xlab + L"   |   unita: " + unit;
    std::wstring extra;
    if (zoomed)
        extra = L"Zoom " + std::to_wstring(pts.front().day) + L" " + monthAbbr(pts.front().month) + L" - "
            + std::to_wstring(pts.back().day) + L" " + monthAbbr(pts.back().month) + L" (tasto destro: anno intero)";
    if (!cmp.empty()) {
        double diff = 0; int n = 0;
        for (size_t i = 0; i < pts.size(); ++i) if (pts[i].ht && finite(cmp[i])) { diff += pts[i].t - cmp[i]; ++n; }
        if (!extra.empty()) extra += L"   |   ";
        extra += std::to_wstring(app.focusYear) + L" rispetto al " + std::to_wstring(app.compareYear) + L": "
            + (n ? f2(diff / n) + L" " + unit + L" in media su " + std::to_wstring(n) + L" giorni" : L"nessun giorno con misura in entrambi");
    }
    cardTitle(g, page, title, sub);
    if (!extra.empty()) {
        Font extraF(&ff, 12, FontStyleRegular, UnitPixel);
        text(g, extra, extraF, kGold(), RectF(page.X + 18, page.Y + 56, page.Width - 270, 18), StringAlignmentNear, StringAlignmentNear);
    }
    std::wstring cmpLabel = L"anno " + std::to_wstring(app.compareYear);
    drawLegend(g, page, L"valore dell'anno", L"fascia P5-P95", L"sopra P95", L"media storica giornaliera",
               cmp.empty() ? nullptr : cmpLabel.c_str());
    if (!st().hasVar[var]) { emptyMessage(g, page, L"La stazione non misura questa grandezza: sceglierne un'altra con il pulsante Fascia (tasto V)."); drawCredit(g, page); return; }
    if (pts.size() < 2) return;
    double lo = std::numeric_limits<double>::infinity(), hi = -lo;
    for (auto& p : pts) {
        if (finite(p.p5)) lo = std::min(lo, p.p5);
        if (finite(p.p95)) hi = std::max(hi, p.p95);
        if (finite(p.mean)) { lo = std::min(lo, p.mean); hi = std::max(hi, p.mean); }
        if (p.ht) { lo = std::min(lo, p.t); hi = std::max(hi, p.t); }
    }
    for (double v : cmp) if (finite(v)) { lo = std::min(lo, v); hi = std::max(hi, v); }
    if (!finite(lo) || !finite(hi)) { lo = -1; hi = 1; }
    auto scale = niceScale(lo, hi, dg ? 1.0 : 1.0);
    lo = scale.lo; hi = scale.hi;
    const float shift = cmp.empty() ? 0.f : 18.f;
    RectF plot(page.X + 64, page.Y + 96 + shift, page.Width - 96, page.Height - 194 - shift);
    {
        std::vector<std::wstring> tips;
        tips.reserve(pts.size());
        for (auto& p : pts) {
            std::wstring s = std::to_wstring(p.day) + L" " + monthAbbr(p.month) + L" " + std::to_wstring(app.focusYear);
            if (p.ht) s += L":  " + plainNum(p.t, dg) + L" " + unit;
            else s += L":  senza misura";
            if (finite(p.p5)) s += L"    P5 " + plainNum(p.p5, dg);
            if (finite(p.p95)) s += L"    P95 " + plainNum(p.p95, dg);
            if (finite(p.mean)) s += L"    Media " + plainNum(p.mean, dg);
            if (!cmp.empty()) s += L"    " + std::to_wstring(app.compareYear) + L" " + plainNum(cmp[tips.size()], dg);
            tips.push_back(s);
        }
        addProbe(plot, std::move(tips));
    }
    auto X = [&](int i) { return plot.X + plot.Width * (float)i / (float)(pts.size() - 1); };
    auto Y = [&](double v) { return plot.Y + plot.Height * (float)(1.0 - (v - lo) / (hi - lo)); };
    Font tick(&ff, 12, FontStyleRegular, UnitPixel);
    drawYAxis(g, plot, scale, 52.f);
    std::vector<PointF> band;
    for (int i = 0; i < (int)pts.size(); ++i) if (finite(pts[i].p95)) band.push_back(PointF(X(i), Y(pts[i].p95)));
    for (int i = (int)pts.size() - 1; i >= 0; --i) if (finite(pts[i].p5)) band.push_back(PointF(X(i), Y(pts[i].p5)));
    if (band.size() >= 3) { SolidBrush br(C(72, 168, 210, 70)); g.FillPolygon(&br, band.data(), (INT)band.size()); }
    auto polyline = [&](auto value, const Pen& pen) {
        std::vector<PointF> line;
        for (int i = 0; i <= (int)pts.size(); ++i) {
            double v = i < (int)pts.size() ? value(i) : NaN;
            if (!finite(v)) { if (line.size() > 1) g.DrawLines(&pen, line.data(), (INT)line.size()); line.clear(); continue; }
            line.push_back(PointF(X(i), Y(v)));
        }
    };
    Pen edgePen(kIce(), 1.15f);
    polyline([&](int i) { return pts[i].p95; }, edgePen);
    polyline([&](int i) { return pts[i].p5; }, edgePen);
    Pen average(kMean(), 1.15f);
    average.SetDashStyle(DashStyleDash);
    polyline([&](int i) { return pts[i].mean; }, average);
    if (!cmp.empty()) {
        Pen other(kCompare(), 1.8f);
        other.SetLineJoin(LineJoinRound);
        polyline([&](int i) { return cmp[i]; }, other);
    }
    Pen yearPen(kGold(), 2.3f);
    yearPen.SetLineJoin(LineJoinRound);
    polyline([&](int i) { return pts[i].ht ? pts[i].t : NaN; }, yearPen);
    SolidBrush hot(kAmber()), cold(kIce());
    for (int i = 0; i < (int)pts.size(); ++i) {
        if (!pts[i].ht) continue;
        if (finite(pts[i].p95) && pts[i].t > pts[i].p95) g.FillEllipse(&hot, X(i) - 3.2f, Y(pts[i].t) - 3.2f, 6.4f, 6.4f);
        else if (finite(pts[i].p5) && pts[i].t < pts[i].p5) g.FillEllipse(&cold, X(i) - 3.2f, Y(pts[i].t) - 3.2f, 6.4f, 6.4f);
    }
    if (zoomed && app.season != Season::Month) {
        int step = std::max(1, (int)pts.size() / 10);
        for (int i = 0; i < (int)pts.size(); i += step)
            text(g, std::to_wstring(pts[i].day) + L" " + monthAbbr(pts[i].month), tick, kTick(), RectF(X(i) - 28, plot.GetBottom() + 8, 56, 18), StringAlignmentCenter);
    } else if (app.season == Season::Year) {
        static const int cum[12] = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335};
        for (int m = 0; m < 12; ++m) {
            float x = plot.X + plot.Width * (float)cum[m] / 365.f;
            text(g, monthAbbr(m + 1), tick, kTick(), RectF(x - 4, plot.GetBottom() + 8, 40, 18));
        }
    } else {
        int step = std::max(1, (int)pts.size() / 12);
        for (int i = 0; i < (int)pts.size(); i += step)
            text(g, std::to_wstring(pts[i].day) + (app.season == Season::Month ? L"" : std::wstring(L" ") + monthAbbr(pts[i].month)),
                 tick, kTick(), RectF(X(i) - 28, plot.GetBottom() + 8, 56, 18), StringAlignmentCenter);
    }
    drawNote(g, page, L"Tratteggio: media storica. Fascia e media lisciate su 5 giorni nell'anno intero. Punti: ambra sopra P95, ciano sotto P5. Tasto V: cambia grandezza.");
    drawCredit(g, page);
}

static void paintRain(Graphics& g, const RectF& page) {
    FontFamily ff(L"Segoe UI");
    const auto& all = rainSeries();
    const auto& cmpAll = rainCompare();
    size_t i0 = 0, i1 = 0;
    zoomRange(all.size(), i0, i1);
    const bool zoomed = all.size() > 1 && (i0 > 0 || i1 < all.size() - 1);
    std::vector<RainPt> pts;
    std::vector<double> cmp;
    if (!all.empty()) pts.assign(all.begin() + i0, all.begin() + i1 + 1);
    if (cmpAll.size() == all.size() && !all.empty()) cmp.assign(cmpAll.begin() + i0, cmpAll.begin() + i1 + 1);
    auto dateOf = [&](const RainPt& p) {
        int m = app.month, d = p.mark;
        if (app.season != Season::Month) ymdFromIndex(p.mark, m, d);
        return std::to_wstring(d) + L" " + monthAbbr(m);
    };
    int over = 0, under = 0;
    for (auto& p : all) {
        if (p.ok && p.hiOk && p.v > p.hi + 1e-9) ++over;
        if (p.ok && p.loOk && p.v < p.lo - 1e-9) ++under;
    }
    std::wstring title = stationTitle() + (app.season == Season::Month
        ? L"Pioggia cumulata - " + std::wstring(monthName(app.month)) + L" " + std::to_wstring(app.focusYear)
        : L"Pioggia cumulata - anno " + std::to_wstring(app.focusYear));
    std::wstring sub = L"Fascia minimo-massimo degli anni completi della stazione   |   sopra il massimo: " + std::to_wstring(over) +
                       L" giorni   |   sotto il minimo: " + std::to_wstring(under);
    // Confronto con la media alla data dell'ultimo giorno misurato.
    for (size_t i = all.size(); i-- > 0;) {
        if (!all[i].ok || !finite(all[i].mean) || all[i].mean <= 0) continue;
        sub += L"   |   al " + dateOf(all[i]) + L": " + plainNum(100 * all[i].v / all[i].mean, 0) + L"% della media";
        break;
    }
    std::wstring extra;
    if (zoomed) extra = L"Zoom " + dateOf(pts.front()) + L" - " + dateOf(pts.back()) + L" (tasto destro: anno intero)";
    if (!cmp.empty()) {
        for (size_t i = pts.size(); i-- > 0;) {
            if (!pts[i].ok || !finite(cmp[i])) continue;
            if (!extra.empty()) extra += L"   |   ";
            extra += L"Al " + dateOf(pts[i]) + L": " + plainNum(pts[i].v, 0) + L" mm nel " + std::to_wstring(app.focusYear)
                + L", " + plainNum(cmp[i], 0) + L" mm nel " + std::to_wstring(app.compareYear);
            break;
        }
    }
    cardTitle(g, page, title, sub);
    if (!extra.empty()) {
        Font extraF(&ff, 12, FontStyleRegular, UnitPixel);
        text(g, extra, extraF, kGold(), RectF(page.X + 18, page.Y + 56, page.Width - 270, 18), StringAlignmentNear, StringAlignmentNear);
    }
    std::wstring cmpLabel = L"cumulata " + std::to_wstring(app.compareYear);
    drawLegend(g, page, L"cumulata dell'anno", L"minimo e massimo", L"sopra il massimo", L"media degli anni completi",
               cmp.empty() ? nullptr : cmpLabel.c_str());
    if (!st().hasVar[VP]) { emptyMessage(g, page, L"La stazione non misura la pioggia."); drawCredit(g, page); return; }
    if (pts.size() < 2) return;
    double lo = 0, hi = 1;
    for (auto& p : pts) {
        if (p.hiOk) hi = std::max(hi, p.hi);
        if (p.ok) hi = std::max(hi, p.v);
        if (finite(p.mean)) hi = std::max(hi, p.mean);
    }
    for (double v : cmp) if (finite(v)) hi = std::max(hi, v);
    auto scale = niceScale(lo, hi, 1.0);
    lo = scale.lo; hi = scale.hi;
    const float shift = cmp.empty() ? 0.f : 18.f;
    RectF plot(page.X + 70, page.Y + 96 + shift, page.Width - 102, page.Height - 194 - shift);
    {
        std::vector<std::wstring> tips;
        tips.reserve(pts.size());
        for (auto& p : pts) {
            std::wstring s = dateOf(p) + L" " + std::to_wstring(app.focusYear) + L":  ";
            s += p.ok ? L"cumulata " + plainNum(p.v, 1) + L" mm" : L"senza misura";
            if (p.loOk) s += L"    min " + plainNum(p.lo, 0);
            if (finite(p.mean)) s += L"    media " + plainNum(p.mean, 0);
            if (p.hiOk) s += L"    max " + plainNum(p.hi, 0);
            if (!cmp.empty()) s += L"    " + std::to_wstring(app.compareYear) + L" " + plainNum(cmp[tips.size()], 1);
            tips.push_back(s);
        }
        addProbe(plot, std::move(tips));
    }
    auto X = [&](int i) { return plot.X + plot.Width * (float)i / (float)(pts.size() - 1); };
    auto Y = [&](double v) { return plot.Y + plot.Height * (float)(1.0 - (v - lo) / (hi - lo)); };
    Font tick(&ff, 12, FontStyleRegular, UnitPixel);
    drawYAxis(g, plot, scale);
    std::vector<PointF> band;
    for (int i = 0; i < (int)pts.size(); ++i) if (pts[i].hiOk) band.push_back(PointF(X(i), Y(pts[i].hi)));
    for (int i = (int)pts.size() - 1; i >= 0; --i) if (pts[i].loOk) band.push_back(PointF(X(i), Y(pts[i].lo)));
    if (band.size() >= 3) { SolidBrush br(C(64, 150, 200, 80)); g.FillPolygon(&br, band.data(), (INT)band.size()); }
    auto polyline = [&](auto value, const Pen& pen) {
        std::vector<PointF> line;
        for (int i = 0; i <= (int)pts.size(); ++i) {
            double v = i < (int)pts.size() ? value(i) : NaN;
            if (!finite(v)) { if (line.size() > 1) g.DrawLines(&pen, line.data(), (INT)line.size()); line.clear(); continue; }
            line.push_back(PointF(X(i), Y(v)));
        }
    };
    Pen loPen(C(90, 160, 200), 1.15f), hiPen(kIce(), 1.15f);
    polyline([&](int i) { return pts[i].loOk ? pts[i].lo : NaN; }, loPen);
    polyline([&](int i) { return pts[i].hiOk ? pts[i].hi : NaN; }, hiPen);
    Pen average(kMean(), 1.3f);
    average.SetDashStyle(DashStyleDash);
    polyline([&](int i) { return pts[i].mean; }, average);
    if (!cmp.empty()) {
        Pen other(kCompare(), 2.0f);
        other.SetLineJoin(LineJoinRound);
        polyline([&](int i) { return cmp[i]; }, other);
    }
    Pen yearPen(kGold(), 2.6f);
    yearPen.SetLineJoin(LineJoinRound);
    polyline([&](int i) { return pts[i].ok ? pts[i].v : NaN; }, yearPen);
    SolidBrush overMax(kAmber());
    for (int i = 0; i < (int)pts.size(); ++i)
        if (pts[i].ok && pts[i].hiOk && pts[i].v > pts[i].hi + 1e-9) g.FillEllipse(&overMax, X(i) - 3.2f, Y(pts[i].v) - 3.2f, 6.4f, 6.4f);
    if (zoomed && app.season != Season::Month) {
        int step = std::max(1, (int)pts.size() / 10);
        for (int i = 0; i < (int)pts.size(); i += step)
            text(g, dateOf(pts[i]), tick, kTick(), RectF(X(i) - 28, plot.GetBottom() + 8, 56, 18), StringAlignmentCenter);
    } else if (app.season == Season::Year) {
        static const int cum[12] = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335};
        for (int m = 0; m < 12; ++m) {
            float x = plot.X + plot.Width * (float)cum[m] / 365.f;
            text(g, monthAbbr(m + 1), tick, kTick(), RectF(x - 4, plot.GetBottom() + 8, 40, 18));
        }
    } else {
        int step = std::max(1, (int)pts.size() / 12);
        for (int i = 0; i < (int)pts.size(); i += step)
            text(g, dateOf(pts[i]), tick, kTick(), RectF(X(i) - 28, plot.GetBottom() + 8, 56, 18), StringAlignmentCenter);
    }
    drawNote(g, page, L"Linea oro: cumulata dell'anno. Fascia: minimo e massimo della cumulata negli anni con almeno il 95% dei giorni misurati. Il 29 febbraio vale 0 mm negli anni non bisestili.");
    drawCredit(g, page);
}

static void paintHeat(Graphics& g, const RectF& page, bool controls) {
    FontFamily ff(L"Segoe UI");
    std::wstring title = stationTitle() + (app.heatMode == HeatMode::Five ? L"Ondate di caldo, 5 giorni"
        : app.heatMode == HeatMode::Ten ? L"Ondate di caldo, 10 giorni"
        : L"Serie con Tmax oltre " + std::to_wstring(app.heatThreshold) + L" C");
    int fy, ly;
    dailyYears(fy, ly);
    std::wstring sub = app.heatMode == HeatMode::Streak
        ? L"Giorni consecutivi con Tmax sopra la soglia, ordinati per durata   |   dati " + std::to_wstring(fy) + L"-" + std::to_wstring(ly)
        : L"Media della Tmedia sul periodo, giorni con Tmin, Tmedia e Tmax misurate, periodi non sovrapposti   |   dati "
          + std::to_wstring(fy) + L"-" + std::to_wstring(ly);
    cardTitle(g, page, title, sub);
    drawLegend(g, page, L"Tmedia", L"Tmin - Tmax", L"Tmax");
    Font body(&ff, 15, FontStyleRegular, UnitPixel);
    Font smallF(&ff, 13, FontStyleRegular, UnitPixel);
    Font tick(&ff, 12, FontStyleRegular, UnitPixel);
    float top = page.Y + 70;
    if (controls) {
        const wchar_t* labels[] = {L"5 giorni", L"10 giorni", L"Tmax oltre soglia"};
        float x = page.X + 18;
        for (int i = 0; i < 3; ++i) {
            float w = i == 2 ? 150.f : 96.f;
            RectF r(x, top, w, 28);
            addHit(r, 400 + i);
            bool on = (int)app.heatMode == i;
            fillRound(g, r, on ? C(230, 242, 248) : C(16, 38, 60), kCardEdge());
            text(g, labels[i], smallF, on ? C(24, 48, 66) : kTitle(), r, StringAlignmentCenter, StringAlignmentCenter);
            x += w + 8;
        }
        if (app.heatMode == HeatMode::Streak) {
            RectF minus(x + 6, top, 28, 28), plus(x + 92, top, 28, 28);
            addHit(minus, 403); addHit(plus, 404);
            fillRound(g, minus, C(16, 38, 60), kCardEdge());
            fillRound(g, plus, C(16, 38, 60), kCardEdge());
            text(g, L"-", body, kTitle(), minus, StringAlignmentCenter, StringAlignmentCenter);
            text(g, L"+", body, kTitle(), plus, StringAlignmentCenter, StringAlignmentCenter);
            text(g, std::to_wstring(app.heatThreshold) + L" C", body, kGold(), RectF(x + 34, top, 58, 28), StringAlignmentCenter, StringAlignmentCenter);
        }
        top += 40;
    }
    if (!haveTminTmax()) { emptyMessage(g, page, L"La stazione non ha Tmin e Tmax."); drawCredit(g, page); return; }
    const auto& waves = heatWaves();
    if (waves.empty()) {
        text(g, L"Nessun periodo trovato con questi criteri.", body, kSub(), RectF(page.X + 24, top + 20, page.Width - 48, 30));
        drawCredit(g, page);
        return;
    }
    int sel = std::clamp(app.heatSel, 0, (int)waves.size() - 1);
    auto period = [&](const HeatWave& w) {
        std::wstring s = std::to_wstring(w.d0);
        if (w.m0 != w.m1 || w.y0 != w.y1) s += L" " + std::wstring(monthAbbr(w.m0));
        if (w.y0 != w.y1) s += L" " + std::to_wstring(w.y0);
        return s + L" - " + std::to_wstring(w.d1) + L" " + monthAbbr(w.m1) + L" " + std::to_wstring(w.y1);
    };
    float tableW = std::min(640.f, page.Width * 0.56f);
    float x0 = page.X + 18;
    const wchar_t* heads[] = {L"#", L"Periodo", L"Giorni", L"Tmedia", L"Tmax media", L"Tmax max", L"Tmin media"};
    float cols[] = {0, 30, 222, 282, 352, 446, 528};
    float scaleX = tableW / 610.f;
    for (int i = 0; i < 7; ++i)
        text(g, heads[i], smallF, kSub(), RectF(x0 + cols[i] * scaleX, top, (i == 1 ? 190 : 86) * scaleX, 20));
    float rowTop = top + 26;
    float pitch = std::clamp((page.GetBottom() - 54 - rowTop) / (float)waves.size(), 22.f, 34.f);
    for (int i = 0; i < (int)waves.size(); ++i) {
        const auto& w = waves[i];
        RectF row(x0 - 6, rowTop + i * pitch, tableW + 6, pitch - 4);
        if (controls) addHit(row, 410 + i);
        SolidBrush band(i == sel ? C(40, 72, 100) : (i % 2 ? kRow() : C(12, 30, 48)));
        g.FillRectangle(&band, row);
        float y = row.Y;
        const Font& f = pitch < 28 ? smallF : body;
        text(g, std::to_wstring(i + 1), f, kIce(), RectF(x0 + cols[0] * scaleX, y, 28, row.Height));
        text(g, period(w), f, kTitle(), RectF(x0 + cols[1] * scaleX, y, 190 * scaleX, row.Height));
        text(g, std::to_wstring(w.days), f, kTitle(), RectF(x0 + cols[2] * scaleX, y, 56 * scaleX, row.Height));
        text(g, plainNum(w.meanT, 1), f, kGold(), RectF(x0 + cols[3] * scaleX, y, 66 * scaleX, row.Height));
        text(g, plainNum(w.meanTx, 1), f, kAmber(), RectF(x0 + cols[4] * scaleX, y, 86 * scaleX, row.Height));
        text(g, plainNum(w.maxTx, 1), f, kHotColor(), RectF(x0 + cols[5] * scaleX, y, 78 * scaleX, row.Height));
        text(g, plainNum(w.meanTn, 1), f, kIce(), RectF(x0 + cols[6] * scaleX, y, 80 * scaleX, row.Height));
    }
    const auto& w = waves[sel];
    RectF detail(x0 + tableW + 24, top, page.GetRight() - (x0 + tableW + 24) - 18, page.GetBottom() - 54 - top);
    if (detail.Width > 160 && detail.Height > 140) {
        text(g, period(w), body, kTitle(), RectF(detail.X, detail.Y, detail.Width - 120, 22));
        if (controls) {
            RectF open(detail.GetRight() - 120, detail.Y - 2, 120, 26);
            addHit(open, 420);
            fillRound(g, open, C(230, 242, 248), kCardEdge());
            text(g, L"Apri nell'anno", smallF, C(24, 48, 66), open, StringAlignmentCenter, StringAlignmentCenter);
        }
        RectF plot(detail.X + 44, detail.Y + 40, detail.Width - 52, detail.Height - 70);
        const auto& days = st().days;
        double lo = 1e9, hi = -1e9;
        for (size_t k = w.first; k < w.first + (size_t)w.days; ++k) {
            for (int v : {VTN, VTM, VTX}) if (days[k].has(v)) { lo = std::min(lo, (double)days[k].v[v]); hi = std::max(hi, (double)days[k].v[v]); }
        }
        auto scale = niceScale(lo, hi, 1.0);
        auto Y = [&](double v) { return plot.Y + plot.Height * (float)(1.0 - (v - scale.lo) / (scale.hi - scale.lo)); };
        drawYAxis(g, plot, scale, 36.f);
        int n = w.days;
        float slot = plot.Width / n;
        std::vector<std::wstring> tips;
        std::vector<PointF> mean;
        int labelStep = std::max(1, n / 8);
        for (int k = 0; k < n; ++k) {
            const auto& r = days[w.first + k];
            float cx = plot.X + slot * (k + 0.5f);
            if (r.has(VTN) && r.has(VTX)) {
                SolidBrush bar(C(72, 168, 210, 110));
                g.FillRectangle(&bar, cx - slot * 0.28f, Y(r.v[VTX]), slot * 0.56f, Y(r.v[VTN]) - Y(r.v[VTX]));
            }
            if (r.has(VTX)) { SolidBrush b(kAmber()); g.FillEllipse(&b, cx - 3.5f, Y(r.v[VTX]) - 3.5f, 7.f, 7.f); }
            if (r.has(VTN)) { SolidBrush b(kIce()); g.FillEllipse(&b, cx - 3.f, Y(r.v[VTN]) - 3.f, 6.f, 6.f); }
            if (r.has(VTM)) mean.push_back(PointF(cx, Y(r.v[VTM])));
            if (k % labelStep == 0)
                text(g, std::to_wstring(r.d) + L"/" + std::to_wstring(r.m), tick, kTick(), RectF(cx - 24, plot.GetBottom() + 6, 48, 18), StringAlignmentCenter);
            tips.push_back(std::to_wstring(r.d) + L" " + monthAbbr(r.m) + L" " + std::to_wstring(r.y)
                + L":   Tmin " + plainNum(r.v[VTN], 1) + L"    Tmedia " + plainNum(r.v[VTM], 1) + L"    Tmax " + plainNum(r.v[VTX], 1));
        }
        if (mean.size() > 1) { Pen p(kGold(), 2.3f); p.SetLineJoin(LineJoinRound); g.DrawLines(&p, mean.data(), (INT)mean.size()); }
        SolidBrush dot(kGold());
        for (auto& pt : mean) g.FillEllipse(&dot, pt.X - 3.f, pt.Y - 3.f, 6.f, 6.f);
        addProbe(RectF(plot.X + slot * 0.5f, plot.Y, std::max(2.f, plot.Width - slot), plot.Height), std::move(tips));
    }
    drawNote(g, page, app.heatMode == HeatMode::Streak
            ? L"Un giorno senza Tmax o con Tmax non oltre la soglia interrompe la serie. Clic su una riga per il dettaglio."
            : L"Finestre complete, scelte dalla piu calda senza sovrapposizioni. Clic su una riga per il dettaglio.");
    drawCredit(g, page);
}

static void paintThermal(Graphics& g, const RectF& page) {
    const auto& years = thermalYears();
    std::wstring range = years.empty() ? std::wstring(L"-") : std::to_wstring(years.front().year) + L"-" + std::to_wstring(years.back().year);
    cardTitle(g, page, stationTitle() + L"Tmin e Tmax, indici annuali",
              seasonLabel() + L"   |   anni " + range + L"   |   barre chiare: meno del 90% dei giorni misurati   |   tratteggio: tendenza sugli anni completi");
    if (!haveTminTmax()) { emptyMessage(g, page, L"La stazione non ha Tmin e Tmax."); drawCredit(g, page); return; }
    if (years.size() < 2) { emptyMessage(g, page, L"Servono almeno due anni nel periodo scelto."); drawCredit(g, page); return; }
    std::vector<IndexYearInfo> info;
    std::vector<IndexPanel> panels = {
        {L"Notti tropicali (Tmin > 20 C)", L"notti", kAmber(), 0},
        {L"Giorni di gelo (Tmin < 0 C)", L"giorni", kIce(), 0},
        {L"Giorni di ghiaccio (Tmax < 0 C)", L"giorni", C(170, 200, 255), 0},
        {L"Giorni estivi (Tmax > 25 C)", L"giorni", kGold(), 0},
        {L"Giorni caldi (Tmax > 30 C)", L"giorni", kHotColor(), 0},
        {L"Escursione media Tmax - Tmin", L"C", kGreen(), 1, {}, false},
    };
    for (const auto& t : years) {
        info.push_back({t.year, t.partial, t.measured, t.expected});
        panels[0].values.push_back(t.tropical);
        panels[1].values.push_back(t.frost);
        panels[2].values.push_back(t.ice);
        panels[3].values.push_back(t.summer);
        panels[4].values.push_back(t.hot30);
        panels[5].values.push_back(t.range);
    }
    drawIndexPanels(g, page, panels, info, L"giorni con Tmin e Tmax");
    drawCredit(g, page);
}

static void paintRainIdx(Graphics& g, const RectF& page) {
    const auto& years = rainYears();
    std::wstring range = years.empty() ? std::wstring(L"-") : std::to_wstring(years.front().year) + L"-" + std::to_wstring(years.back().year);
    cardTitle(g, page, stationTitle() + L"Pioggia, indici annuali",
              seasonLabel() + L"   |   anni " + range + L"   |   barre chiare: meno del 90% dei giorni misurati   |   tratteggio: tendenza sugli anni completi");
    if (!st().hasVar[VP]) { emptyMessage(g, page, L"La stazione non misura la pioggia."); drawCredit(g, page); return; }
    if (years.size() < 2) { emptyMessage(g, page, L"Servono almeno due anni nel periodo scelto."); drawCredit(g, page); return; }
    std::vector<IndexYearInfo> info;
    std::vector<IndexPanel> panels = {
        {L"Pioggia totale", L"mm", kIce(), 0},
        {L"Giorni piovosi (>= 1 mm)", L"giorni", C(90, 160, 200), 0},
        {L"Giorni con forte pioggia (>= 20 mm)", L"giorni", kCompare(), 0},
        {L"Pioggia massima in un giorno", L"mm", kGold(), 1},
        {L"Intensita media (mm per giorno piovoso)", L"mm", kGreen(), 1, {}, false},
        {L"Periodo secco piu lungo (< 1 mm)", L"giorni", kAmber(), 0},
    };
    for (const auto& r : years) {
        info.push_back({r.year, r.partial, r.measured, r.expected});
        panels[0].values.push_back(r.total);
        panels[1].values.push_back(r.measured ? r.wet : NaN);
        panels[2].values.push_back(r.measured ? r.heavy : NaN);
        panels[3].values.push_back(r.maxDay);
        panels[4].values.push_back(r.wet ? r.total / r.wet : NaN);
        panels[5].values.push_back(r.measured ? r.dryRun : NaN);
    }
    drawIndexPanels(g, page, panels, info, L"giorni con pioggia misurata");
    drawCredit(g, page);
}

static void paintRank(Graphics& g, const RectF& page) {
    FontFamily ff(L"Segoe UI");
    Font h1(&ff, 20, FontStyleBold, UnitPixel);
    Font body(&ff, 16, FontStyleRegular, UnitPixel);
    Font labelFont(&ff, 13, FontStyleRegular, UnitPixel);
    fillRound(g, page, kCard(), kCardEdge());
    text(g, stationTitle() + L"classifiche del periodo " + std::to_wstring(app.y0) + L"-" + std::to_wstring(app.y1), h1, kTitle(),
         RectF(page.X + 24, page.Y + 18, page.Width - 300, 28), StringAlignmentNear, StringAlignmentNear);
    text(g, seasonLabel() + L"   |   scarti dalla climatologia della stazione (" + refLabel() + L")   |   anni con almeno il 75% dei mesi completi",
         labelFont, kSub(), RectF(page.X + 24, page.Y + 48, page.Width - 290, 20));
    drawLegend(g, page, L"titolo", L"posto", L"valore");
    auto temp = anomalySeries(false), prec = anomalySeries(true);
    auto yearMin = yearMinSpei12();
    auto topN = [](std::vector<Sample> p, bool low) {
        std::sort(p.begin(), p.end(), [&](auto& a, auto& b) { return low ? a.value < b.value : a.value > b.value; });
        if (p.size() > 10) p.resize(10);
        return p;
    };
    struct Col { const wchar_t* title; std::vector<Sample> rows; const wchar_t* unit; };
    Col cols[5] = {
        {L"Piu caldi", topN(temp, false), L" C"},
        {L"Piu freddi", topN(temp, true), L" C"},
        {L"Piu piovosi", topN(prec, false), L" mm"},
        {L"Piu secchi", topN(prec, true), L" mm"},
        {L"SPEI-12 piu basso", topN(yearMin, true), L""},
    };
    float cw = (page.Width - 48) / 5.f;
    float pitch = std::clamp((page.Height - 124.f - 40.f) / 10.f, 24.f, 40.f);
    float chipH = pitch - 6.f;
    bool compact = cw < 220.f;
    Font compactFont(&ff, 13, FontStyleRegular, UnitPixel);
    const Font& rowFont = compact ? compactFont : body;
    float yearX = compact ? 26.f : 34.f, yearW = compact ? 46.f : 60.f;
    float valueX = compact ? 72.f : 100.f;
    for (int c = 0; c < 5; ++c) {
        float x = page.X + 24 + cw * c;
        text(g, cols[c].title, labelFont, kGold(), RectF(x, page.Y + 86, cw - 12, 22));
        if (cols[c].rows.empty()) text(g, L"nessun dato", labelFont, kSub(), RectF(x, page.Y + 120, cw - 12, 22));
        for (int i = 0; i < (int)cols[c].rows.size(); ++i) {
            float y = page.Y + 120 + i * pitch;
            SolidBrush chip(kRow());
            g.FillRectangle(&chip, x, y, cw - 16, chipH);
            const auto& r = cols[c].rows[i];
            text(g, std::to_wstring(i + 1), rowFont, kIce(), RectF(x + 6, y, 24, chipH));
            text(g, std::to_wstring(r.year) + (r.year == st().lastYear ? L"*" : L""), rowFont, kTitle(), RectF(x + yearX, y, yearW, chipH));
            text(g, f2(r.value) + cols[c].unit, rowFont, kAmber(), RectF(x + valueX, y, cw - valueX - 20, chipH));
        }
    }
    drawNote(g, page, L"* anno in corso o ultimo anno della stazione: valore provvisorio, sui soli mesi disponibili.");
    drawCredit(g, page);
}

void paintContent(Graphics& g, const RectF& page, bool controls) {
    FontFamily ff(L"Segoe UI");
    const auto& s = st();
    if (app.stations.empty()) {
        cardTitle(g, page, L"Nessuna stazione", L"La cartella dati_stazioni e' vuota.");
        emptyMessage(g, page, L"Premi AGGIORNA TUTTE DA OSMER per scaricare lo storico di tutte le stazioni (puo' richiedere piu' di un'ora).");
        return;
    }
    if (app.view == View::Overview) { paintOverview(g, page, controls); return; }
    if (!s.loaded) {
        cardTitle(g, page, s.name, L"Caricamento dei dati in corso...");
        return;
    }
    if (s.days.empty()) {
        cardTitle(g, page, s.name, L"Il file della stazione non contiene misure.");
        emptyMessage(g, page, L"Nessun dato giornaliero in " + s.file);
        return;
    }
    switch (app.view) {
        case View::Clima: paintClima(g, page, controls); return;
        case View::Year: paintYear(g, page, controls); return;
        case View::Band: paintBand(g, page); return;
        case View::Rain: paintRain(g, page); return;
        case View::Heat: paintHeat(g, page, controls); return;
        case View::Thermal: paintThermal(g, page); return;
        case View::RainIdx: paintRainIdx(g, page); return;
        case View::Rank: paintRank(g, page); return;
        default: break;
    }
    if (app.view == View::Spei) {
        auto s3 = speiMonthly(3), s6 = speiMonthly(6), s12 = speiMonthly(12);
        float h = (page.Height - 24) / 2.f;
        RectF a(page.X, page.Y, page.Width, h - 8);
        RectF b(page.X, page.Y + h + 8, page.Width, h - 8);
        cardTitle(g, a, stationTitle() + L"SPEI-6", statsLine(s6));
        cardTitle(g, b, stationTitle() + L"SPEI-12", statsLine(s12));
        if (s6.size() < 2 && s12.size() < 2) {
            emptyMessage(g, a, L"SPEI non disponibile: servono almeno 10 anni con pioggia e temperatura complete per ogni mese.");
        }
        drawSeries(g, RectF(a.X + 58, a.Y + 84, a.Width - 80, a.Height - 140), s6, boundsOf(s6, true), true, L"SPEI-6");
        drawSeries(g, RectF(b.X + 58, b.Y + 84, b.Width - 80, b.Height - 140), s12, boundsOf(s12, true), true, L"SPEI-12");
        drawLegend(g, a, L"soglie -1 e -2", L"umido", L"siccitoso");
        drawLegend(g, b, L"soglie -1 e -2", L"umido", L"siccitoso");
        Font foot(&ff, 12, FontStyleRegular, UnitPixel);
        text(g, L"Calibrazione sui soli dati di " + s.name + L" (" + stationPeriod(s) + L"), logistica generalizzata, PET di Thornthwaite alla latitudine della stazione. Mesi incompleti esclusi.",
             foot, kSub(), RectF(a.X + 18, a.GetBottom() - 22, a.Width - 36, 16));
        drawCredit(g, b);
        return;
    }
    std::vector<Sample> pts;
    std::wstring title, unit;
    bool warm = app.view == View::Temp;
    if (app.view == View::Temp) { pts = anomalySeries(false); title = stationTitle() + L"temperatura media, scarto dalla media"; unit = L"C"; }
    else if (app.view == View::Prec) { pts = anomalySeries(true); title = stationTitle() + L"precipitazione, scarto dalla media"; unit = L"mm"; }
    else { pts = balanceSeries(); title = stationTitle() + L"bilancio pioggia - PET, scarto dalla media"; unit = L"mm"; }
    cardTitle(g, page, title, statsLine(pts) + L" " + unit + L"    |    " + seasonLabel() + L"    |    riferimento " + refLabel() + L"    |    clic su un anno: fascia giornaliera");
    if (warm) drawLegend(g, page, L"scarto", L"piu fresco della media", L"piu caldo della media");
    else drawLegend(g, page, L"scarto", L"sopra la media", L"sotto la media");
    Font unitF(&ff, 13, FontStyleBold, UnitPixel);
    text(g, unit, unitF, kIce(), RectF(page.X + 16, page.Y + 86, 40, 20), StringAlignmentCenter);
    if (pts.size() < 2) emptyMessage(g, page, L"Servono almeno due anni con il 75% dei mesi completi nel periodo e nella stagione scelti.");
    drawSeries(g, RectF(page.X + 72, page.Y + 96, page.Width - 100, page.Height - 160), pts, boundsOf(pts, false), false, L"Scarto", warm);
    drawNote(g, page, L"Anni con almeno il 75% dei mesi della stagione completi; l'ultimo anno della stazione e' lo scarto sui mesi gia' disponibili.");
    drawCredit(g, page);
}

// ------------------------------------------------------------ finestra

void paintScene(Graphics& g, int W, int H) {
    app.hits.clear();
    app.probes.clear();
    app.monthHits.clear();
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    SolidBrush bg(C(244, 247, 248));
    g.FillRectangle(&bg, 0, 0, W, H);
    FontFamily ff(L"Segoe UI");
    Font brand(&ff, 22, FontStyleBold, UnitPixel);
    Font quiet(&ff, 13, FontStyleRegular, UnitPixel);
    Font nameF(&ff, 16, FontStyleBold, UnitPixel);
    Font hintF(&ff, 12, FontStyleRegular, UnitPixel);
    Font btn(&ff, 16, FontStyleBold, UnitPixel);
    SolidBrush header(C(32, 54, 72));
    g.FillRectangle(&header, 0, 0, W, 72);
    const auto& s = st();
    std::wstring sub = L"OSMER FVG   |   ";
    if (!s.code.empty()) sub += s.code + L"   ";
    if (finite(s.lat)) sub += plainNum(s.lat, 3) + L" N  " + plainNum(s.lon, 3) + L" E  " + plainNum(s.alt, 0) + L" m   ";
    sub += stationPeriod(s);
    // Pulsanti a destra; il nome della stazione si accorcia se serve.
    const float gap = 14.f, right = (float)W - 250.f;
    const float widths[3] = {200.f, 280.f, 160.f};
    float room = right - 360.f - 2 * gap;
    float k = std::clamp(room / (widths[0] + widths[1] + widths[2]), 0.55f, 1.f);
    float bx = right - (widths[0] + widths[1] + widths[2]) * k - 2 * gap;
    text(g, s.name, brand, C(255, 255, 255), RectF(28, 10, std::max(120.f, bx - 40), 30), StringAlignmentNear, StringAlignmentNear);
    text(g, sub, quiet, C(186, 206, 216), RectF(28, 42, std::max(120.f, bx - 40), 18), StringAlignmentNear, StringAlignmentNear);
    text(g, std::to_wstring(app.y0) + L"  -  " + std::to_wstring(app.y1), brand, C(255, 255, 255),
         RectF((float)W - 240, 20, 216, 32), StringAlignmentFar, StringAlignmentCenter);
    {
        Font smallBtn(&ff, 13, FontStyleBold, UnitPixel);
        const Font& headFont = k < 0.9f ? smallBtn : nameF;
        bool busy = osmerRunning();
        std::wstring labels[3] = {L"STAZIONE  v", busy ? L"AGGIORNAMENTO..." : L"AGGIORNA TUTTE DA OSMER", L"APRI GRAFICI"};
        const int ids[3] = {10, 11, 14};
        float x = bx;
        for (int i = 0; i < 3; ++i) {
            float w = widths[i] * k;
            RectF r(x, 16, w, 40);
            addHit(r, ids[i]);
            bool on = (i == 0 && app.picker == 4) || (i == 1 && busy);
            fillRound(g, r, on ? C(240, 196, 112) : C(255, 255, 255), on ? C(240, 196, 112) : C(255, 255, 255));
            text(g, labels[i], headFont, C(32, 54, 72), r, StringAlignmentCenter, StringAlignmentCenter);
            x += w + gap;
        }
    }

    const float side = 312;
    const float cardTop = 90.f;
    const float cardBottom = (float)H - 88.f;
    const float filterH = 316.f;
    float filterTop = cardBottom - filterH;
    if (filterTop < cardTop + 36) filterTop = cardTop + 36;
    const float menuTop = 124.f;
    const float itemH = 42.f;
    int fit = (int)((filterTop - 8.f - menuTop) / itemH);
    if (fit < 1) fit = 1;
    int maxScroll = std::max(0, ViewCount - fit);
    app.menuScroll = std::clamp(app.menuScroll, 0, maxScroll);
    fillRound(g, RectF(18, cardTop, side, cardBottom - cardTop), C(255, 255, 255), C(220, 228, 232));
    text(g, maxScroll ? L"MENU   rotella per scorrere" : L"MENU", quiet, C(98, 116, 129), RectF(36, 102, 270, 16), StringAlignmentNear, StringAlignmentNear);
    const wchar_t* names[] = {L"Panoramica stazioni", L"Climatologia", L"Dettaglio anno", L"Anno e fascia P5-P95", L"Pioggia cumulata",
                              L"Temperatura", L"Precipitazioni", L"SPEI-6 e SPEI-12", L"Bilancio P - PET", L"Classifiche",
                              L"Ondate di caldo", L"Tmin e Tmax", L"Indici di pioggia"};
    const wchar_t* hints[] = {L"Tutte le stazioni a confronto", L"Medie mensili e record", L"I dodici mesi dell'anno",
                              L"Valore giornaliero e percentili", L"Cumulata giorno per giorno", L"Scarto annuale dalla media",
                              L"Millimetri sopra o sotto", L"Siccita, calibrato sulla stazione", L"Pioggia meno evaporazione",
                              L"Anni estremi del periodo", L"I periodi piu caldi", L"Notti tropicali, gelo, giorni caldi",
                              L"Giorni piovosi, intensita, secche"};
    g.SetClip(RectF(20, menuTop - 2, side - 4, std::max(8.f, filterTop - menuTop)));
    for (int i = 0; i < ViewCount; ++i) {
        float yy = menuTop + (i - app.menuScroll) * itemH;
        if (yy + 36 > filterTop || yy < menuTop - 1) continue;
        RectF r(32, yy, side - 28, 38);
        addHit(r, 100 + i);
        if ((int)app.view == i) {
            fillRound(g, r, C(232, 241, 246), C(32, 54, 72));
            SolidBrush bar(C(32, 54, 72));
            g.FillRectangle(&bar, r.X, r.Y + 11, 4.f, r.Height - 22);
        }
        text(g, names[i], nameF, C(32, 54, 72), RectF(r.X + 18, r.Y + 1, r.Width - 28, 20), StringAlignmentNear, StringAlignmentNear);
        text(g, hints[i], hintF, C(98, 116, 129), RectF(r.X + 18, r.Y + 19, r.Width - 28, 16), StringAlignmentNear, StringAlignmentNear);
    }
    g.ResetClip();

    SolidBrush foot(C(255, 255, 255));
    g.FillRectangle(&foot, 0, H - 76, W, 76);
    Pen line(C(220, 228, 232), 1);
    g.DrawLine(&line, 0, H - 76, W, H - 76);
    bool busy = osmerRunning() || loadingStations();
    SolidBrush stateDot(busy ? C(194, 126, 34) : C(52, 142, 100));
    g.FillEllipse(&stateDot, 24.f, (float)H - 63, 8.f, 8.f);
    text(g, footerStatus(), quiet, C(62, 82, 96), RectF(42, (float)H - 74, (float)W - 66, 30));
    std::wstring msg = osmerRunning() ? osmerProgress() : app.status;
    text(g, msg, quiet, osmerRunning() ? C(154, 96, 20) : C(80, 97, 110), RectF(24, (float)H - 46, (float)W - 48, 46));

    RectF page(18 + side + 16, 90, (float)W - side - 52, (float)H - 184);
    app.pageRc = page;
    paintContent(g, page, true);

    float y = filterTop;
    text(g, L"INIZIO", quiet, C(98, 116, 129), RectF(36, y, 120, 16));
    text(g, L"FINE", quiet, C(98, 116, 129), RectF(176, y, 120, 16));
    y += 20;
    auto yearBox = [&](const RectF& ry, int id, int value) {
        bool hot = app.hotId == id || (app.yearEdit && IsWindowVisible(app.yearEdit) && app.editTarget == id);
        fillRound(g, ry, C(255, 255, 255), hot ? C(32, 54, 72) : C(176, 190, 198));
        addHit(ry, id);
        text(g, value ? std::to_wstring(value) : std::wstring(L"nessuno"), value ? nameF : hintF,
             value ? C(32, 54, 72) : C(98, 116, 129), ry, StringAlignmentCenter, StringAlignmentCenter);
    };
    button(g, RectF(32, y, 36, 34), L"-", false, 1, btn);
    yearBox(RectF(70, y, 72, 34), 20, app.y0);
    button(g, RectF(142, y, 36, 34), L"+", false, 2, btn);
    button(g, RectF(186, y, 36, 34), L"-", false, 3, btn);
    yearBox(RectF(222, y, 64, 34), 21, app.y1);
    button(g, RectF(286, y, 28, 34), L"+", false, 4, btn);
    y += 44;
    button(g, RectF(32, y, 120, 34), seasonLabel(), app.picker == 1, 5, hintF);
    button(g, RectF(158, y, 154, 34), app.refMode == RefMode::Station ? L"Clima: tutta la stazione" : app.refMode == RefMode::Visible ? L"Clima: periodo a schermo" : L"Clima: 1991-2020",
           app.picker == 2, 6, hintF);
    y += 42;
    button(g, RectF(32, y, 280, 34), std::wstring(L"Fascia: ") + varName(app.bandVar), app.picker == 5, 19, hintF);
    y += 42;
    button(g, RectF(32, y, 36, 34), L"-", false, 8, btn);
    yearBox(RectF(70, y, 78, 34), 22, app.focusYear);
    button(g, RectF(150, y, 36, 34), L"+", false, 9, btn);
    text(g, L"anno del grafico", hintF, C(98, 116, 129), RectF(192, y, 120, 34));
    y += 42;
    button(g, RectF(32, y, 36, 34), L"-", false, 16, btn);
    yearBox(RectF(70, y, 78, 34), 23, app.compareYear);
    button(g, RectF(150, y, 36, 34), L"+", false, 17, btn);
    if (app.compareYear) {
        text(g, L"confronto", hintF, C(98, 116, 129), RectF(192, y, 76, 34));
        button(g, RectF(276, y, 36, 34), L"x", false, 18, btn);
    } else {
        text(g, L"anno di confronto", hintF, C(98, 116, 129), RectF(192, y, 120, 34));
    }
    y += 42;
    button(g, RectF(32, y, 196, 38), L"Scarica PNG e CSV", true, 7, nameF);
    button(g, RectF(236, y, 76, 38), L"Tasti", app.picker == 3, 12, nameF);

    auto drawList = [&](float x, float top, float w, const std::vector<std::wstring>& labels, int id0, int selected) {
        float h = 12.f + labels.size() * 30.f;
        if (top + h > (float)H - 56) top = std::max(80.f, (float)H - 56 - h);
        RectF box(x, top, w, h);
        fillRound(g, box, C(255, 255, 255), C(32, 54, 72));
        addHit(box, 13);
        for (int i = 0; i < (int)labels.size(); ++i) {
            RectF row(x + 6, top + 6 + i * 30.f, w - 12, 28);
            addHit(row, id0 + i);
            if (i == selected) fillRound(g, row, C(232, 241, 246), C(232, 241, 246));
            text(g, labels[i], nameF, C(32, 54, 72), RectF(row.X + 8, row.Y, row.Width - 16, row.Height), StringAlignmentNear, StringAlignmentCenter);
        }
    };
    if (app.picker == 1) {
        std::vector<std::wstring> labels = {L"Anno intero", L"Inverno", L"Primavera", L"Estate", L"Autunno"};
        for (int m = 1; m <= 12; ++m) labels.push_back(monthName(m));
        int sel = 0;
        if (app.season == Season::Winter) sel = 1;
        else if (app.season == Season::Spring) sel = 2;
        else if (app.season == Season::Summer) sel = 3;
        else if (app.season == Season::Autumn) sel = 4;
        else if (app.season == Season::Month) sel = 4 + app.month;
        drawList(page.X + 16, page.Y + 16, 240, labels, 300, sel);
    } else if (app.picker == 2) {
        const auto& cs = st();
        std::vector<std::wstring> labels = {L"Tutti i dati della stazione (" + stationPeriod(cs) + L")",
                                            L"Periodo a schermo (" + std::to_wstring(app.y0) + L"-" + std::to_wstring(app.y1) + L")",
                                            L"1991-2020 (solo anni con dati)"};
        int sel = app.refMode == RefMode::Station ? 0 : app.refMode == RefMode::Visible ? 1 : 2;
        drawList(page.X + 16, page.Y + 16, 420, labels, 320, sel);
    } else if (app.picker == 5) {
        std::vector<std::wstring> labels;
        std::vector<int> vars;
        for (int v = 0; v < NVAR; ++v) {
            if (v == VDIR) continue;
            labels.push_back(std::wstring(varName(v)) + (st().hasVar[v] ? L"" : L"   (non misurata)"));
            vars.push_back(v);
        }
        int sel = (int)(std::find(vars.begin(), vars.end(), app.bandVar) - vars.begin());
        drawList(page.X + 16, page.Y + 16, 380, labels, 600, sel);
        // Gli id 600+ seguono l'ordine della lista: si rimappano sulle grandezze.
        for (auto& h : app.hits) if (h.id >= 600 && h.id < 600 + (int)vars.size()) h.id = 600 + vars[h.id - 600];
    } else if (app.picker == 4) {
        // Elenco delle stazioni su piu' colonne, tutte visibili insieme.
        int n = (int)app.stations.size();
        float rowH = 30.f;
        int perCol = std::max(1, (int)((page.Height - 70) / rowH));
        int cols = std::max(1, (n + perCol - 1) / perCol);
        perCol = (n + cols - 1) / cols;
        RectF box(page.X + 8, page.Y + 8, page.Width - 16, 52 + perCol * rowH);
        fillRound(g, box, C(255, 255, 255), C(32, 54, 72));
        addHit(box, 13);
        text(g, L"Scegli la stazione  (" + std::to_wstring(n) + L")   -   PagSu / PagGiu: precedente / successiva", nameF, C(32, 54, 72),
             RectF(box.X + 16, box.Y + 10, box.Width - 32, 24));
        float cw = (box.Width - 24) / cols;
        for (int i = 0; i < n; ++i) {
            const auto& si = *app.stations[i];
            RectF row(box.X + 12 + (i / perCol) * cw, box.Y + 42 + (i % perCol) * rowH, cw - 8, rowH - 3);
            addHit(row, 2000 + i);
            if (i == app.current) fillRound(g, row, C(232, 241, 246), C(32, 54, 72));
            text(g, si.name, quiet, C(32, 54, 72), RectF(row.X + 8, row.Y, row.Width * 0.66f, row.Height));
            text(g, stationPeriod(si), hintF, C(98, 116, 129), RectF(row.X + row.Width * 0.66f, row.Y, row.Width * 0.34f - 6, row.Height), StringAlignmentFar);
        }
    } else if (app.picker == 3) {
        const wchar_t* lines[] = {
            L"1-9    viste del menu,  0 Panoramica,  T Tmin e Tmax,  P indici di pioggia,  H ondate",
            L"PagSu / PagGiu    stazione precedente / successiva",
            L"Frecce    spostano il periodo,   + e -    zoom del periodo",
            L"Rotella sul grafico    zoom,  trascina    sposta",
            L"Tasto destro sul grafico  o  Z    annulla lo zoom",
            L"Ctrl+C    copia il grafico negli appunti",
            L"M    stagione successiva,   C    climatologia di riferimento",
            L"V    grandezza della fascia P5-P95",
            L"G    anno successivo,   S    scarica PNG e CSV",
            L"U    aggiorna tutte le stazioni da OSMER",
            L"Esc    chiude il pannello o il programma",
            L"Clic sull'anno    scrivi il numero, Invio conferma",
            L"Clic sulla curva    apre la fascia di quell'anno"
        };
        const int count = (int)(sizeof(lines) / sizeof(lines[0]));
        float h = 36.f + count * 24.f;
        RectF box(page.X + 16, page.Y + 16, 560, h);
        fillRound(g, box, C(255, 255, 255), C(32, 54, 72));
        addHit(box, 13);
        text(g, L"Tasti", nameF, C(32, 54, 72), RectF(box.X + 16, box.Y + 10, box.Width - 32, 22));
        for (int i = 0; i < count; ++i)
            text(g, lines[i], hintF, C(32, 54, 72), RectF(box.X + 16, box.Y + 36 + i * 24.f, box.Width - 32, 22));
    }
}

void paintOverlay(Graphics& g, int W, int H) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    FontFamily ff(L"Segoe UI");
    Font hintF(&ff, 12, FontStyleRegular, UnitPixel);
    if (app.hoverA >= 0 && app.hoverA < (int)app.probes.size() && app.picker == 0) {
        auto& pr = app.probes[app.hoverA];
        if (app.hoverB >= 0 && app.hoverB < (int)pr.tip.size()) {
            const std::wstring& tip = pr.tip[app.hoverB];
            float tw = std::min(720.f, std::max(180.f, 7.0f * (float)tip.size()));
            float tx = (float)app.hoverPt.x + 18;
            float ty = (float)app.hoverPt.y - 40;
            if (tx + tw > (float)W - 16) tx = (float)W - 16 - tw;
            if (ty < 78) ty = (float)app.hoverPt.y + 16;
            RectF box(tx, ty, tw, 32);
            fillRound(g, box, C(32, 54, 72), C(32, 54, 72));
            text(g, tip, hintF, C(255, 255, 255), box, StringAlignmentCenter, StringAlignmentCenter);
        }
    }
    if (app.hoverMonth >= 0 && !app.monthTip.empty() && app.picker == 0) {
        size_t longest = 0;
        for (const auto& row : app.monthTip) longest = std::max(longest, row.size());
        float tw = std::min((float)W - 32, std::max(350.f, 22.f + 6.7f * (float)longest));
        float th = 18.f + 21.f * (float)app.monthTip.size();
        float tx = std::clamp((float)app.hoverPt.x + 18, 16.f, std::max(16.f, (float)W - 16 - tw));
        float ty = (float)app.hoverPt.y + 18;
        if (ty + th > (float)H - 84) ty = (float)app.hoverPt.y - th - 12;
        ty = std::max(78.f, ty);
        RectF box(tx, ty, tw, th);
        fillRound(g, box, C(32, 54, 72), C(104, 146, 170));
        for (int i = 0; i < (int)app.monthTip.size(); ++i)
            text(g, app.monthTip[i], hintF, C(255, 255, 255), RectF(tx + 12, ty + 9 + i * 21.f, tw - 24, 21));
    }
}

void paintWindow(Graphics& g, int W, int H) {
    paintScene(g, W, H);
    paintOverlay(g, W, H);
}

std::wstring sceneSignature() {
    std::wostringstream k;
    k << (int)app.view << L'|' << app.y0 << L'|' << app.y1 << L'|' << app.focusYear << L'|' << app.month
      << L'|' << (int)app.season << L'|' << (int)app.refMode << L'|' << app.dataVersion << L'|' << app.picker
      << L'|' << app.hotId << L'|' << app.menuScroll << L'|' << app.current << L'|' << app.bandVar
      << L'|' << (app.yearEdit && IsWindowVisible(app.yearEdit) ? app.editTarget : 0)
      << L'|' << app.status << L'|' << footerStatus() << L'|' << osmerProgress() << L'|' << stationsLoaded()
      << L'|' << app.zoomA << L'|' << app.zoomB << L'|' << app.compareYear << L'|' << app.overviewScroll << L'|' << app.overviewSort
      << L'|' << (int)app.heatMode << L'|' << app.heatThreshold << L'|' << app.heatSel << L'|' << loadingStations();
    return k.str();
}
