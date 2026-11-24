// Dati: stazioni, lettura dei CSV OSMER, mesi, climatologia, SPEI e serie per i grafici.
//
// La climatologia di ogni stazione usa SOLO i dati di quella stazione:
// nessuna serie esterna (HISTALP, 1803...) come nella Console Udine.
#include "osmer_console.h"

App app;

bool finite(double v) { return std::isfinite(v); }

static Station emptyStation;
bool haveStation() { return app.current >= 0 && app.current < (int)app.stations.size(); }
Station& st() { return haveStation() ? *app.stations[app.current] : emptyStation; }

std::wstring exeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    auto s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}
std::wstring joinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    return (a.back() == L'\\' || a.back() == L'/') ? a + b : a + L"\\" + b;
}
std::wstring widen(const std::string& text) {
    if (text.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), n);
    return out;
}
std::string narrow(const std::wstring& text) {
    if (text.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::string fileStamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return "";
    std::ostringstream o;
    o << a.nFileSizeHigh << ':' << a.nFileSizeLow << ':' << a.ftLastWriteTime.dwHighDateTime << ':' << a.ftLastWriteTime.dwLowDateTime;
    return o.str();
}

// ------------------------------------------------------------ calendario

int dayIndex(int m, int d) {
    static const int cum[12] = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335};
    return cum[m - 1] + d - 1;
}
bool isLeap(int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
int daysIn(int y, int m) {
    static const int dim[13] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && isLeap(y) ? 29 : dim[m];
}
long dayNumber(int y, int m, int d) {
    // Giorni dal calendario civile (algoritmo di H. Hinnant).
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe;
}
void ymdFromIndex(int ix, int& m, int& d) {
    static const int cum[13] = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366};
    for (m = 1; m <= 12; ++m) if (ix < cum[m]) { d = ix - cum[m - 1] + 1; return; }
    m = 12; d = 31;
}
const wchar_t* monthName(int m) {
    static const wchar_t* n[] = {L"", L"Gennaio", L"Febbraio", L"Marzo", L"Aprile", L"Maggio", L"Giugno",
                                  L"Luglio", L"Agosto", L"Settembre", L"Ottobre", L"Novembre", L"Dicembre"};
    return (m >= 1 && m <= 12) ? n[m] : L"";
}
const wchar_t* monthAbbr(int m) {
    static const wchar_t* n[] = {L"", L"gen", L"feb", L"mar", L"apr", L"mag", L"giu", L"lug", L"ago", L"set", L"ott", L"nov", L"dic"};
    return (m >= 1 && m <= 12) ? n[m] : L"";
}
const wchar_t* varName(int k) {
    static const wchar_t* n[] = {L"Pioggia", L"Temperatura minima", L"Temperatura media", L"Temperatura massima",
        L"Umidita minima", L"Umidita media", L"Umidita massima", L"Vento medio", L"Vento massimo (raffica)",
        L"Direzione del vento massimo", L"Radiazione globale", L"Pressione media"};
    return k >= 0 && k < NVAR ? n[k] : L"";
}
const wchar_t* varUnit(int k) {
    static const wchar_t* u[] = {L"mm", L"C", L"C", L"C", L"%", L"%", L"%", L"km/h", L"km/h", L"N", L"kJ/m2", L"hPa"};
    return k >= 0 && k < NVAR ? u[k] : L"";
}
int varDigits(int k) {
    return (k == VRAD || k == VUN || k == VUM || k == VUX || k == VDIR) ? 0 : 1;
}
std::wstring dateText(int y, int m, int d) {
    std::wostringstream out;
    out << std::setfill(L'0') << std::setw(2) << d << L'/' << std::setw(2) << m << L'/' << y;
    return out.str();
}

// ------------------------------------------------------------ elenco delle stazioni

static bool transientPath(const std::filesystem::path& p) {
    for (const auto& part : p) {
        auto s = part.wstring();
        if (s == L"_backup_riallineamento" || s.rfind(L".riallineamento_", 0) == 0) return true;
    }
    return false;
}

std::vector<std::unique_ptr<Station>> discoverStations(const std::wstring& dataDir) {
    std::vector<std::unique_ptr<Station>> out;
    std::error_code ec;
    const std::wstring suffix = L"_giornalieri_tutti_gli_anni.csv";
    for (auto it = std::filesystem::recursive_directory_iterator(dataDir, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        auto name = it->path().filename().wstring();
        if (name.size() <= suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        if (name.rfind(L"osmer_", 0) != 0 || name.find(L"anni_test") != std::wstring::npos) continue;
        if (transientPath(it->path())) continue;
        auto s = std::make_unique<Station>();
        s->file = it->path().wstring();
        s->slug = name.substr(6, name.size() - 6 - suffix.size());
        s->name = s->slug;
        // Il nome vero sta nel CSV: si legge solo la seconda riga, cosi' l'elenco e' subito pronto.
        std::ifstream in{it->path()};
        std::string header, row;
        if (std::getline(in, header) && std::getline(in, row)) {
            std::vector<std::string> c;
            std::string cur;
            for (char ch : row) { if (ch == ';') { c.push_back(cur); cur.clear(); } else if (ch != '\r') cur.push_back(ch); }
            c.push_back(cur);
            if (c.size() > 15 && !c[15].empty()) s->name = widen(c[15]);
        }
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return _wcsicmp(a->name.c_str(), b->name.c_str()) < 0;
    });
    return out;
}

std::wstring stationLabel(const Station& s) { return s.name; }

std::wstring stationPeriod(const Station& s) {
    if (!s.loaded) return L"in caricamento...";
    if (s.days.empty()) return L"nessun dato";
    return std::to_wstring(s.days.front().y) + L"-" + std::to_wstring(s.lastY);
}

// ------------------------------------------------------------ lettura del CSV

static float parseCell(const char* a, const char* b) {
    while (a < b && (*a == ' ' || *a == '"')) ++a;
    while (b > a && (b[-1] == ' ' || b[-1] == '"' || b[-1] == '\r')) --b;
    if (a >= b || (b - a == 1 && *a == '-')) return NAN;
    char buf[40];
    size_t n = std::min<size_t>(b - a, sizeof(buf) - 1);
    for (size_t i = 0; i < n; ++i) buf[i] = a[i] == ',' ? '.' : a[i];
    buf[n] = 0;
    char* end = nullptr;
    double v = strtod(buf, &end);
    if (end == buf || *end != 0 || !std::isfinite(v)) return NAN;
    return (float)v;
}

// Limiti fisici: fuori da questi il valore e' considerato senza misura.
// (Negli anni 1991-1994 alcune stazioni hanno colonne spostate nell'umidita'.)
static bool plausible(int k, double v) {
    switch (k) {
        case VP: return v >= 0 && v <= 600;
        case VTN: case VTM: case VTX: return v >= -45 && v <= 48;
        case VUN: case VUM: case VUX: return v >= 0 && v <= 100;
        case VVM: case VVX: return v >= 0 && v <= 250;
        case VDIR: return v >= 0 && v <= 360;
        case VRAD: return v >= 0 && v <= 40000;
        case VPRES: return v >= 500 && v <= 1100;
    }
    return true;
}

static double numberOf(std::string s) {
    for (auto& c : s) if (c == ',') c = '.';
    char* end = nullptr;
    double v = strtod(s.c_str(), &end);
    return end != s.c_str() ? v : NaN;
}

// ------------------------------------------------------------ PET e SPEI

// Ore di luce medie del mese alla latitudine data (giorno centrale del mese).
static double daylightHours(double latDeg, int m) {
    static const int mid[13] = {0, 15, 46, 75, 105, 135, 162, 198, 228, 258, 288, 318, 344};
    double phi = latDeg * 3.14159265358979 / 180.0;
    double decl = 0.409 * std::sin(2 * 3.14159265358979 * mid[m] / 365.0 - 1.39);
    double x = -std::tan(phi) * std::tan(decl);
    x = std::clamp(x, -1.0, 1.0);
    return 24.0 / 3.14159265358979 * std::acos(x);
}

static double normalFromProb(double F) {
    // Approssimazione usata da Vicente-Serrano per lo SPEI.
    F = std::clamp(F, 1e-7, 1 - 1e-7);
    double P = 1 - F;
    double sign = 1;
    if (P > 0.5) { P = 1 - P; sign = -1; }
    double W = std::sqrt(-2 * std::log(P));
    const double c0 = 2.515517, c1 = 0.802853, c2 = 0.010328, d1 = 1.432788, d2 = 0.189269, d3 = 0.001308;
    double z = W - (c0 + c1 * W + c2 * W * W) / (1 + d1 * W + d2 * W * W + d3 * W * W * W);
    return sign * z;
}

// Distribuzione logistica generalizzata stimata con gli L-momenti (Hosking),
// per mese di calendario: e' la "log-logistica" del pacchetto R SPEI e
// funziona anche con asimmetria negativa (mesi estivi).
static void speiScale(Station& s, int k) {
    const size_t n = s.months.size();
    std::vector<double> acc(n, NaN);
    for (size_t i = (size_t)k - 1; i < n; ++i) {
        double sum = 0; bool ok = true;
        for (size_t j = i + 1 - k; j <= i && ok; ++j) {
            const auto& mo = s.months[j];
            ok = mo.okP && finite(mo.pet) && !mo.partial;
            if (ok) sum += mo.p - mo.pet;
        }
        if (ok) acc[i] = sum;
    }
    const double pi = 3.14159265358979;
    for (int cal = 1; cal <= 12; ++cal) {
        std::vector<double> x;
        for (size_t i = 0; i < n; ++i) if (s.months[i].m == cal && finite(acc[i])) x.push_back(acc[i]);
        if (x.size() < 10) continue;
        std::sort(x.begin(), x.end());
        double N = (double)x.size(), b0 = 0, b1 = 0, b2 = 0;
        for (size_t i = 0; i < x.size(); ++i) {
            double j = (double)i;               // i-1 con indici da 1
            b0 += x[i];
            b1 += j / (N - 1) * x[i];
            b2 += j * (j - 1) / ((N - 1) * (N - 2)) * x[i];
        }
        b0 /= N; b1 /= N; b2 /= N;
        double l1 = b0, l2 = 2 * b1 - b0, l3 = 6 * b2 - 6 * b1 + b0;
        if (!(l2 > 0)) continue;
        double t3 = l3 / l2;
        if (!(std::abs(t3) < 0.95)) continue;
        double kk = -t3, alpha, xi;
        if (std::abs(kk) < 1e-6) { alpha = l2; xi = l1; }
        else {
            double r = kk * pi / std::sin(kk * pi);
            alpha = l2 / r;
            xi = l1 - alpha * (1.0 / kk - pi / std::sin(kk * pi));
        }
        for (size_t i = 0; i < n; ++i) {
            if (s.months[i].m != cal || !finite(acc[i])) continue;
            double F;
            if (std::abs(kk) < 1e-6) F = 1.0 / (1.0 + std::exp(-(acc[i] - xi) / alpha));
            else {
                double arg = 1 - kk * (acc[i] - xi) / alpha;
                if (arg <= 0) F = kk > 0 ? 1.0 : 0.0;
                else F = 1.0 / (1.0 + std::exp(std::log(arg) / kk));
            }
            double z = normalFromProb(F);
            if (k == 3) s.months[i].spei3 = z;
            else if (k == 6) s.months[i].spei6 = z;
            else s.months[i].spei12 = z;
        }
    }
}

static void computeMonths(Station& s) {
    s.months.clear();
    if (s.days.empty()) return;
    int y = s.days.front().y, m = s.days.front().m;
    size_t i = 0;
    while (y < s.lastY || (y == s.lastY && m <= s.lastM)) {
        Month mo;
        mo.y = y; mo.m = m; mo.dim = daysIn(y, m);
        double st = 0, sp = 0, sn = 0, sx = 0;
        while (i < s.days.size() && (s.days[i].y < y || (s.days[i].y == y && s.days[i].m < m))) ++i;
        while (i < s.days.size() && s.days[i].y == y && s.days[i].m == m) {
            const auto& d = s.days[i];
            if (d.has(VTM)) { st += d.v[VTM]; ++mo.nt; }
            if (d.has(VP)) { sp += d.v[VP]; ++mo.np; }
            if (d.has(VTN)) { sn += d.v[VTN]; ++mo.nn; }
            if (d.has(VTX)) { sx += d.v[VTX]; ++mo.nx; }
            mo.last = d.d;
            ++i;
        }
        if (mo.nt) mo.t = st / mo.nt;
        if (mo.np) mo.p = sp;
        if (mo.nn) mo.tn = sn / mo.nn;
        if (mo.nx) mo.tx = sx / mo.nx;
        mo.partial = y == s.lastY && m == s.lastM && s.lastD < mo.dim;
        // Regola di completezza per la climatologia: temperatura con al massimo
        // 5 giorni mancanti, pioggia con al massimo 2 (una pioggia mancante non e' zero).
        mo.okT = !mo.partial && mo.nt >= mo.dim - 5;
        mo.okP = !mo.partial && mo.np >= mo.dim - 2;
        s.months.push_back(mo);
        if (++m > 12) { m = 1; ++y; }
    }
    // PET di Thornthwaite con l'indice di calore dalle medie mensili della stazione.
    double clim[13] = {}; int cnt[13] = {};
    for (const auto& mo : s.months) if (mo.okT) { clim[mo.m] += mo.t; cnt[mo.m]++; }
    double I = 0; bool allMonths = true;
    for (int k = 1; k <= 12; ++k) {
        if (!cnt[k]) { allMonths = false; continue; }
        double t = clim[k] / cnt[k];
        if (t > 0) I += std::pow(t / 5.0, 1.514);
    }
    double lat = finite(s.lat) ? s.lat : 46.0;
    if (allMonths && I > 0) {
        double a = 6.75e-7 * I * I * I - 7.71e-5 * I * I + 1.792e-2 * I + 0.49239;
        for (auto& mo : s.months) {
            // Per la PET basta la temperatura di almeno meta' dei giorni.
            if (mo.partial || mo.nt < mo.dim / 2) continue;
            double K = daylightHours(lat, mo.m) / 12.0 * mo.dim / 30.0;
            mo.pet = mo.t <= 0 ? 0 : 16.0 * K * std::pow(10.0 * std::min(mo.t, 26.5) / I, a);
        }
        speiScale(s, 3);
        speiScale(s, 6);
        speiScale(s, 12);
    }
}

bool loadStation(Station& s) {
    s.error.clear();
    std::string stamp = fileStamp(s.file);
    std::ifstream in{std::filesystem::path(s.file), std::ios::binary};
    if (!in) { s.error = L"File non leggibile"; s.loaded = true; return false; }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<Day> days;
    days.reserve(text.size() / 110 + 16);
    size_t pos = 0;
    bool header = true;
    std::string nameField, valueField;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const char* a = text.data() + pos;
        const char* b = text.data() + eol;
        pos = eol + 1;
        if (header) { header = false; continue; }
        if (b - a < 10) continue;
        // Celle separate da ';': 0 mese, 1 giorno, 2-13 grandezze, 14 anno, 15 nome, 16 codice.
        const char* cells[19]; const char* ends[19];
        int k = 0; const char* c = a;
        for (const char* p = a; p <= b && k < 19; ++p) {
            if (p == b || *p == ';') { cells[k] = c; ends[k] = p; ++k; c = p + 1; }
        }
        if (k < 15) continue;
        float fm = parseCell(cells[0], ends[0]), fd = parseCell(cells[1], ends[1]), fy = parseCell(cells[14], ends[14]);
        if (!std::isfinite(fm) || !std::isfinite(fd) || !std::isfinite(fy)) continue;
        Day d;
        d.y = (int)fy; d.m = (int)fm; d.d = (int)fd;
        if (d.m < 1 || d.m > 12 || d.d < 1 || d.d > daysIn(d.y, d.m)) continue;
        bool any = false;
        for (int v = 0; v < NVAR; ++v) {
            float x = parseCell(cells[2 + v], ends[2 + v]);
            if (std::isfinite(x) && !plausible(v, x)) x = NAN;
            d.v[v] = x;
            any = any || std::isfinite(x);
        }
        if (nameField.empty() && k > 16) { nameField.assign(cells[15], ends[15]); valueField.assign(cells[16], ends[16]); }
        if (!any) continue;
        d.n = dayNumber(d.y, d.m, d.d);
        days.push_back(d);
    }
    std::stable_sort(days.begin(), days.end(), [](const Day& x, const Day& y) { return x.n < y.n; });
    // Date doppie: vale l'ultima riga del file.
    std::vector<Day> unique;
    unique.reserve(days.size());
    for (const auto& d : days) {
        if (!unique.empty() && unique.back().n == d.n) unique.back() = d;
        else unique.push_back(d);
    }
    s.days.swap(unique);
    if (!nameField.empty()) s.name = widen(nameField);
    if (!valueField.empty()) {
        std::vector<std::string> parts;
        std::string cur;
        for (char ch : valueField) { if (ch == '@') { parts.push_back(cur); cur.clear(); } else if (ch != '\r') cur.push_back(ch); }
        parts.push_back(cur);
        if (!parts.empty()) s.code = widen(parts[0]);
        if (parts.size() >= 6) { s.lat = numberOf(parts[3]); s.lon = numberOf(parts[4]); s.alt = numberOf(parts[5]); }
    }
    for (int v = 0; v < NVAR; ++v) s.hasVar[v] = false;
    s.measuredT = s.measuredP = 0;
    for (const auto& d : s.days) {
        for (int v = 0; v < NVAR; ++v) if (d.has(v)) s.hasVar[v] = true;
        if (d.has(VTM)) ++s.measuredT;
        if (d.has(VP)) ++s.measuredP;
    }
    if (!s.days.empty()) {
        s.firstYear = s.days.front().y;
        s.lastY = s.days.back().y; s.lastM = s.days.back().m; s.lastD = s.days.back().d;
        s.lastYear = s.lastY;
        s.expectedDays = s.days.back().n - s.days.front().n + 1;
    } else {
        s.firstYear = s.lastYear = s.lastY = 0;
        s.expectedDays = 0;
    }
    computeMonths(s);
    s.stamp = stamp;
    s.loaded = true;
    return true;
}

// ------------------------------------------------------------ stagione e riferimento

bool monthInSeason(int m) {
    if (app.season == Season::Year) return true;
    if (app.season == Season::Month) return m == app.month;
    if (app.season == Season::Winter) return m == 12 || m <= 2;
    if (app.season == Season::Spring) return m >= 3 && m <= 5;
    if (app.season == Season::Summer) return m >= 6 && m <= 8;
    return m >= 9 && m <= 11;
}
static int seasonMonths() {
    int n = 0;
    for (int m = 1; m <= 12; ++m) if (monthInSeason(m)) ++n;
    return n;
}
std::wstring seasonLabel() {
    switch (app.season) {
        case Season::Year: return L"Anno intero";
        case Season::Month: return monthName(app.month);
        case Season::Winter: return L"Inverno";
        case Season::Spring: return L"Primavera";
        case Season::Summer: return L"Estate";
        case Season::Autumn: return L"Autunno";
    }
    return L"";
}
void refRange(const Station& s, int& a, int& b) {
    if (app.refMode == RefMode::Clima1991) { a = 1991; b = 2020; return; }
    if (app.refMode == RefMode::Visible) { a = app.y0; b = app.y1; return; }
    a = s.firstYear; b = s.lastYear;
}
std::wstring refLabel() {
    if (app.refMode == RefMode::Clima1991) return L"1991-2020";
    if (app.refMode == RefMode::Visible) return L"Periodo a schermo";
    return L"Tutti i dati della stazione";
}
bool yearInRef(const Station& s, int y) {
    int a, b;
    refRange(s, a, b);
    return y >= a && y <= b;
}

std::wstring plainNum(double v, int prec) {
    if (!finite(v)) return L"-";
    std::wostringstream o;
    o << std::fixed << std::setprecision(prec) << v;
    auto s = o.str();
    for (auto& c : s) if (c == L'.') c = L',';
    if (s == L"-0" || s == L"-0,0" || s == L"-0,00") s.erase(0, 1);
    return s;
}
std::wstring f2(double v) {
    if (!finite(v)) return L"-";
    auto s = plainNum(v, 2);
    if (v > 0 && s != L"0,00") s = L"+" + s;
    return s;
}

const Month* findMonth(const Station& s, int y, int m) {
    if (s.months.empty()) return nullptr;
    int i = (y - s.months.front().y) * 12 + (m - s.months.front().m);
    if (i < 0 || i >= (int)s.months.size()) return nullptr;
    return &s.months[i];
}

// Media climatologica mensile della stazione nel riferimento scelto.
// Si usano solo i mesi completi; servono almeno 3 anni per mese.
constexpr int MinClimYears = 3;
void monthClimate(const Station& s, double climT[13], double climP[13], int cntT[13], int cntP[13]) {
    for (int m = 0; m <= 12; ++m) { climT[m] = climP[m] = 0; cntT[m] = cntP[m] = 0; }
    for (const auto& mo : s.months) {
        if (!yearInRef(s, mo.y)) continue;
        if (mo.okT) { climT[mo.m] += mo.t; cntT[mo.m]++; }
        if (mo.okP) { climP[mo.m] += mo.p; cntP[mo.m]++; }
    }
    for (int m = 1; m <= 12; ++m) {
        climT[m] = cntT[m] >= MinClimYears ? climT[m] / cntT[m] : NaN;
        climP[m] = cntP[m] >= MinClimYears ? climP[m] / cntP[m] : NaN;
    }
}

std::vector<std::wstring> monthDetails(int year, int month) {
    const auto& s = st();
    const Month* mo = findMonth(s, year, month);
    std::vector<std::wstring> tip = {std::wstring(monthName(month)) + L" " + std::to_wstring(year)
        + (mo && mo->partial ? L" - mese parziale" : L"")};
    if (!mo || (!mo->nt && !mo->np)) { tip.push_back(L"Nessuna misura in questo mese."); return tip; }
    double cT[13], cP[13]; int nT[13], nP[13];
    monthClimate(s, cT, cP, nT, nP);
    tip.push_back(L"Temperatura media: " + (mo->nt ? plainNum(mo->t, 2) + L" C" : L"non disponibile")
        + (finite(cT[month]) ? L"   (media " + plainNum(cT[month], 2) + L" C su " + std::to_wstring(nT[month]) + L" anni)" : L""));
    tip.push_back(L"Tmin media " + plainNum(mo->tn, 1) + L" C   Tmax media " + plainNum(mo->tx, 1) + L" C");
    tip.push_back(L"Pioggia cumulata: " + (mo->np ? plainNum(mo->p, 1) + L" mm" : L"non disponibile")
        + (finite(cP[month]) ? L"   (media " + plainNum(cP[month], 1) + L" mm su " + std::to_wstring(nP[month]) + L" anni)" : L""));
    tip.push_back(L"Giorni con misure: temperatura " + std::to_wstring(mo->nt) + L" / pioggia " + std::to_wstring(mo->np)
        + L" su " + std::to_wstring(mo->dim));
    if (!mo->partial && (!mo->okT || !mo->okP))
        tip.push_back(std::wstring(L"Escluso dalla climatologia: ") + (!mo->okT ? L"temperatura" : L"") + (!mo->okT && !mo->okP ? L" e " : L"")
            + (!mo->okP ? L"pioggia" : L"") + L" incompleta.");
    if (mo->partial) {
        tip.push_back(L"Dati al " + dateText(year, month, mo->last));
        tip.push_back(L"Scarti rispetto alla media del mese intero.");
    }
    return tip;
}

// ------------------------------------------------------------ periodo e sessione

void clampPeriod() {
    const auto& s = st();
    if (s.days.empty()) return;
    int a = s.firstYear, b = s.lastYear;
    app.y0 = std::clamp(app.y0, a, b);
    app.y1 = std::clamp(app.y1, a, b);
    if (app.y1 <= app.y0) { app.y1 = std::min(b, app.y0 + 1); if (app.y1 <= app.y0) app.y0 = std::max(a, app.y1 - 1); }
}
void fullPeriod() {
    const auto& s = st();
    if (s.days.empty()) return;
    app.y0 = s.firstYear;
    app.y1 = s.lastYear;
}
void focusRange(int& lo, int& hi) {
    const auto& s = st();
    if (!s.days.empty()) { lo = s.firstYear; hi = s.lastYear; }
    else { lo = app.y0; hi = app.y1; }
    if (app.view == View::Overview) {
        lo = 9999; hi = 0;
        for (const auto& p : app.stations) if (p->loaded && !p->days.empty()) { lo = std::min(lo, p->firstYear); hi = std::max(hi, p->lastYear); }
        if (hi < lo) { lo = app.y0; hi = app.y1; }
    }
    if (hi < lo) hi = lo;
}

void selectStation(int index, bool keepPeriod) {
    if (index < 0 || index >= (int)app.stations.size()) return;
    app.current = index;
    ++app.dataVersion;
    resetZoom();
    app.heatSel = 0;
    const auto& s = st();
    if (!s.days.empty()) {
        // Ogni stazione ha il suo periodo: si mostra tutto, salvo quando si
        // ricarica la stessa stazione (keepPeriod) e il periodo resta valido.
        if (!keepPeriod) fullPeriod();
        else clampPeriod();
        int lo, hi;
        View v = app.view;
        if (v == View::Overview) app.view = View::Clima;
        focusRange(lo, hi);
        app.view = v;
        app.focusYear = std::clamp(app.focusYear, lo, hi);
        if (app.compareYear && !dailyYear(app.compareYear)) app.compareYear = 0;
    }
}

void saveSession() {
    std::ofstream out(std::filesystem::path(joinPath(exeDir(), L"ultima_vista.txt")));
    if (!out) return;
    out << (int)app.view << ' ' << app.y0 << ' ' << app.y1 << ' ' << app.focusYear << ' '
        << app.month << ' ' << (int)app.season << ' ' << (int)app.refMode << ' '
        << app.compareYear << ' ' << (int)app.heatMode << ' ' << app.heatThreshold << ' ' << app.bandVar << ' '
        << app.overviewSort << '\n';
    out << narrow(st().slug) << '\n';
}
bool loadSession() {
    std::ifstream in(std::filesystem::path(joinPath(exeDir(), L"ultima_vista.txt")));
    int view, y0, y1, fy, month, season, ref, compare, heatMode, threshold, bandVar, sort;
    if (!(in >> view >> y0 >> y1 >> fy >> month >> season >> ref >> compare >> heatMode >> threshold >> bandVar >> sort)) return false;
    std::string slug;
    std::getline(in, slug);
    std::getline(in, slug);
    if (!slug.empty() && slug.back() == '\r') slug.pop_back();
    if (view < 0 || view >= ViewCount || season < 0 || season > 5 || ref < 0 || ref > 2) return false;
    for (int i = 0; i < (int)app.stations.size(); ++i) if (narrow(app.stations[i]->slug) == slug) app.current = i;
    app.view = (View)view;
    app.y0 = y0;
    app.y1 = y1;
    app.month = std::clamp(month, 1, 12);
    app.season = (Season)season;
    app.refMode = (RefMode)ref;
    app.focusYear = fy;
    app.compareYear = compare;
    app.heatMode = (HeatMode)std::clamp(heatMode, 0, 2);
    app.heatThreshold = std::clamp(threshold, 20, 42);
    app.bandVar = std::clamp(bandVar, 0, NVAR - 1);
    if (app.bandVar == VDIR) app.bandVar = VTM;
    app.overviewSort = std::clamp(sort, 0, 3);
    return true;
}
void cycleSeason() {
    if (app.season == Season::Year) app.season = Season::Winter;
    else if (app.season == Season::Winter) app.season = Season::Spring;
    else if (app.season == Season::Spring) app.season = Season::Summer;
    else if (app.season == Season::Summer) app.season = Season::Autumn;
    else if (app.season == Season::Autumn) { app.season = Season::Month; app.month = 1; }
    else if (app.month < 12) app.month++;
    else app.season = Season::Year;
}

// ------------------------------------------------------------ serie annuali

// Scarto annuale dalla climatologia mensile della stazione. Un anno entra
// se ha almeno il 75% dei mesi della stagione completi (l'ultimo anno
// della stazione entra comunque: e' lo scarto "da inizio anno").
static std::vector<Sample> yearlyAnomaly(int kind) {
    const auto& s = st();
    std::vector<Sample> pts;
    if (s.months.empty() || app.y1 < app.y0) return pts;
    double clim[13] = {}; int cnt[13] = {};
    auto value = [&](const Month& mo, double& v) -> bool {
        if (kind == 0) { v = mo.t; return mo.okT; }
        if (kind == 1) { v = mo.p; return mo.okP; }
        v = mo.p - mo.pet; return mo.okP && mo.okT && finite(mo.pet);
    };
    for (const auto& mo : s.months) {
        double v;
        if (!yearInRef(s, mo.y) || !monthInSeason(mo.m) || !value(mo, v)) continue;
        clim[mo.m] += v; cnt[mo.m]++;
    }
    for (int m = 1; m <= 12; ++m) clim[m] = cnt[m] >= MinClimYears ? clim[m] / cnt[m] : NaN;
    std::vector<double> acc(app.y1 - app.y0 + 1, 0.0);
    std::vector<int> n(acc.size(), 0);
    for (const auto& mo : s.months) {
        double v;
        if (mo.y < app.y0 || mo.y > app.y1 || !monthInSeason(mo.m) || !finite(clim[mo.m]) || !value(mo, v)) continue;
        acc[mo.y - app.y0] += v - clim[mo.m];
        ++n[mo.y - app.y0];
    }
    int need = (int)std::ceil(seasonMonths() * 0.75);
    for (size_t i = 0; i < acc.size(); ++i) {
        int y = app.y0 + (int)i;
        if (!n[i] || (n[i] < need && y != s.lastYear)) continue;
        pts.push_back(Sample{y, 0, kind == 0 ? acc[i] / n[i] : acc[i], n[i]});
    }
    return pts;
}
std::vector<Sample> anomalySeries(bool precip) { return yearlyAnomaly(precip ? 1 : 0); }
std::vector<Sample> balanceSeries() { return yearlyAnomaly(2); }

std::vector<Sample> yearMinSpei12() {
    std::vector<Sample> pts;
    for (const auto& mo : st().months) {
        if (mo.y < app.y0 || mo.y > app.y1 || !finite(mo.spei12)) continue;
        if (pts.empty() || pts.back().year != mo.y) pts.push_back(Sample{mo.y, 0, mo.spei12, 1});
        else pts.back().value = std::min(pts.back().value, mo.spei12);
    }
    return pts;
}
std::vector<Sample> speiMonthly(int scale) {
    std::vector<Sample> pts;
    for (const auto& mo : st().months) {
        if (mo.y < app.y0 || mo.y > app.y1) continue;
        double v = scale == 3 ? mo.spei3 : scale == 6 ? mo.spei6 : mo.spei12;
        if (finite(v)) pts.push_back(Sample{mo.y, mo.m, v, 1});
    }
    return pts;
}

std::wstring classeSpei(double z) {
    if (!finite(z)) return L"";
    if (z <= -2) return L"siccita estrema";
    if (z <= -1.5) return L"siccita severa";
    if (z <= -1) return L"siccita moderata";
    if (z >= 2) return L"umidita estrema";
    if (z >= 1.5) return L"molto umido";
    if (z >= 1) return L"moderatamente umido";
    return L"nella norma";
}

// ------------------------------------------------------------ serie giornaliere

static double percentileSorted(std::vector<double>& v, double q) {
    if (v.empty()) return NaN;
    double k = (v.size() - 1) * q;
    size_t f = (size_t)std::floor(k), c = (size_t)std::ceil(k);
    if (f == c) return v[f];
    return v[f] * (c - k) + v[c] * (k - f);
}
static void smooth5(double* v, int n) {
    double tmp[366];
    for (int i = 0; i < n; ++i) {
        double s = 0; int c = 0;
        for (int j = i - 2; j <= i + 2; ++j) if (j >= 0 && j < n && finite(v[j])) { s += v[j]; ++c; }
        tmp[i] = c ? s / c : NaN;
    }
    for (int i = 0; i < n; ++i) v[i] = tmp[i];
}
static double meanOf(const std::vector<double>& values) {
    if (values.empty()) return NaN;
    double sum = 0;
    for (double value : values) sum += value;
    return sum / values.size();
}
static bool keepIndex(int i) {
    int m, d;
    ymdFromIndex(i, m, d);
    return app.season == Season::Year || (app.season != Season::Month && monthInSeason(m));
}

// Cumulata della pioggia di un anno sul calendario di 366 giorni.
// measured = giorni con pioggia misurata.
static void yearCumul(int year, double out[366], bool ok[366], int* measured = nullptr) {
    double raw[366] = {};
    bool has[366] = {};
    bool any = false;
    int count = 0;
    for (const auto& r : st().days) {
        if (r.y != year || !r.has(VP)) continue;
        int ix = dayIndex(r.m, r.d);
        raw[ix] = r.v[VP];
        has[ix] = true;
        any = true;
        ++count;
    }
    if (measured) *measured = count;
    for (int i = 0; i < 366; ++i) { out[i] = 0; ok[i] = false; }
    if (!any) return;
    int i29 = dayIndex(2, 29);
    if (!isLeap(year) && !has[i29]) {
        bool later = false;
        for (int i = dayIndex(3, 1); i < 366; ++i) if (has[i]) later = true;
        if (later) { raw[i29] = 0; has[i29] = true; }
    }
    double cum = 0;
    for (int i = 0; i < 366; ++i) {
        if (has[i]) cum += raw[i];
        ok[i] = has[i];
        out[i] = cum;
    }
}

static std::vector<int> stationYears() {
    std::vector<int> years;
    for (const auto& r : st().days) if (years.empty() || years.back() != r.y) years.push_back(r.y);
    return years;
}

static std::vector<RainPt> computeRainSeries() {
    std::vector<RainPt> pts;
    auto years = stationYears();
    if (app.season == Season::Month) {
        int dim = app.month == 2 ? (isLeap(app.focusYear) ? 29 : 28) : daysIn(2001, app.month);
        std::vector<double> lo(32, 0), hi(32, 0), line(32, 0), sum(32, 0);
        std::vector<int> cnt(32, 0);
        std::vector<bool> loOk(32, false), hiOk(32, false), lineOk(32, false);
        for (int year : years) {
            double raw[32] = {};
            bool has[32] = {};
            int last = 0, n = 0;
            for (const auto& r : st().days) {
                if (r.y != year || r.m != app.month || !r.has(VP)) continue;
                raw[r.d] = r.v[VP]; has[r.d] = true; last = std::max(last, r.d); ++n;
            }
            if (!n) continue;
            // L'inviluppo storico usa solo i mesi quasi completi.
            bool complete = n >= daysIn(year, app.month) - 1;
            double cum = 0;
            for (int d = 1; d <= last; ++d) {
                if (has[d]) cum += raw[d];
                if (!has[d]) continue;
                if (complete && year != app.focusYear) {
                    if (!loOk[d] || cum < lo[d]) { lo[d] = cum; loOk[d] = true; }
                    if (!hiOk[d] || cum > hi[d]) { hi[d] = cum; hiOk[d] = true; }
                    sum[d] += cum; cnt[d]++;
                }
                if (year == app.focusYear) { line[d] = cum; lineOk[d] = true; }
            }
        }
        for (int d = 1; d <= dim; ++d) {
            RainPt p;
            p.x = d; p.mark = d;
            p.v = line[d]; p.ok = lineOk[d];
            p.lo = lo[d]; p.hi = hi[d]; p.loOk = loOk[d]; p.hiOk = hiOk[d];
            p.mean = cnt[d] ? sum[d] / cnt[d] : NaN;
            pts.push_back(p);
        }
        return pts;
    }
    double line[366] = {};
    bool lineOk[366] = {};
    yearCumul(app.focusYear, line, lineOk);
    double lo[366], hi[366], sum[366] = {};
    int cnt[366] = {};
    bool loOk[366] = {}, hiOk[366] = {};
    for (int year : years) {
        if (year == app.focusYear) continue;
        double cum[366];
        bool ok[366];
        int measured = 0;
        yearCumul(year, cum, ok, &measured);
        // Solo anni quasi completi (almeno 95% dei giorni con pioggia misurata).
        if (measured < 0.95 * (isLeap(year) ? 366 : 365)) continue;
        for (int i = 0; i < 366; ++i) {
            if (!ok[i]) continue;
            if (!loOk[i] || cum[i] < lo[i]) { lo[i] = cum[i]; loOk[i] = true; }
            if (!hiOk[i] || cum[i] > hi[i]) { hi[i] = cum[i]; hiOk[i] = true; }
            sum[i] += cum[i]; cnt[i]++;
        }
    }
    for (int i = 0; i < 366; ++i) {
        if (!keepIndex(i)) continue;
        RainPt p;
        p.x = (int)pts.size(); p.mark = i;
        p.v = line[i]; p.ok = lineOk[i];
        p.lo = loOk[i] ? lo[i] : 0; p.hi = hiOk[i] ? hi[i] : 0; p.loOk = loOk[i]; p.hiOk = hiOk[i];
        p.mean = cnt[i] ? sum[i] / cnt[i] : NaN;
        pts.push_back(p);
    }
    return pts;
}

static std::vector<BandPt> computeBandSeries(int* above, int* below) {
    std::vector<BandPt> pts;
    if (above) *above = 0;
    if (below) *below = 0;
    const int var = app.bandVar;
    auto tally = [&](const BandPt& p) {
        if (above && p.ht && finite(p.p95) && p.t > p.p95) ++*above;
        if (below && p.ht && finite(p.p5) && p.t < p.p5) ++*below;
    };
    if (app.season == Season::Month) {
        int dim = app.month == 2 ? (isLeap(app.focusYear) ? 29 : 28) : daysIn(2001, app.month);
        std::vector<double> bag[32];
        double yt[32] = {};
        bool yh[32] = {};
        for (const auto& r : st().days) {
            if (r.m != app.month || !r.has(var)) continue;
            bag[r.d].push_back(r.v[var]);
            if (r.y == app.focusYear) { yt[r.d] = r.v[var]; yh[r.d] = true; }
        }
        for (int d = 1; d <= dim; ++d) {
            std::sort(bag[d].begin(), bag[d].end());
            BandPt p;
            p.month = app.month; p.day = d; p.ht = yh[d]; p.t = yt[d];
            p.p5 = percentileSorted(bag[d], 0.05);
            p.p95 = percentileSorted(bag[d], 0.95);
            p.mean = meanOf(bag[d]);
            tally(p);
            pts.push_back(p);
        }
        return pts;
    }
    std::vector<double> bag[366];
    double yt[366];
    bool yh[366] = {};
    for (int i = 0; i < 366; ++i) yt[i] = NaN;
    for (const auto& r : st().days) {
        if (!r.has(var)) continue;
        int ix = dayIndex(r.m, r.d);
        bag[ix].push_back(r.v[var]);
        if (r.y == app.focusYear) { yt[ix] = r.v[var]; yh[ix] = true; }
    }
    int i29 = dayIndex(2, 29);
    if (!isLeap(app.focusYear) && !yh[i29]) {
        int i28 = dayIndex(2, 28), i31 = dayIndex(3, 1);
        if (yh[i28] && yh[i31]) { yt[i29] = (yt[i28] + yt[i31]) / 2.0; yh[i29] = true; }
    }
    double p5[366], p95[366], mean[366];
    for (int i = 0; i < 366; ++i) {
        std::sort(bag[i].begin(), bag[i].end());
        p5[i] = percentileSorted(bag[i], 0.05);
        p95[i] = percentileSorted(bag[i], 0.95);
        mean[i] = meanOf(bag[i]);
    }
    smooth5(p5, 366);
    smooth5(p95, 366);
    smooth5(mean, 366);
    for (int i = 0; i < 366; ++i) {
        if (!keepIndex(i)) continue;
        int m = 1, d = 1;
        ymdFromIndex(i, m, d);
        BandPt p;
        p.month = m; p.day = d; p.ht = yh[i]; p.t = yt[i];
        p.p5 = p5[i]; p.p95 = p95[i]; p.mean = mean[i];
        tally(p);
        pts.push_back(p);
    }
    return pts;
}

// ------------------------------------------------------------ cache delle serie giornaliere

struct SeriesKey {
    unsigned long version = ~0ul;
    Season season = Season::Year;
    int month = 0, focusYear = 0, var = 0;
    bool operator==(const SeriesKey& o) const {
        return version == o.version && season == o.season && month == o.month && focusYear == o.focusYear && var == o.var;
    }
};
static SeriesKey currentKey() {
    SeriesKey k;
    k.version = app.dataVersion;
    k.season = app.season;
    k.month = app.month;
    k.focusYear = app.focusYear;
    k.var = app.bandVar;
    return k;
}

const std::vector<RainPt>& rainSeries() {
    static SeriesKey key;
    static std::vector<RainPt> pts;
    auto now = currentKey();
    now.var = 0;
    if (!(now == key)) { pts = computeRainSeries(); key = now; }
    return pts;
}

const std::vector<BandPt>& bandSeries(int* above, int* below) {
    static SeriesKey key;
    static std::vector<BandPt> pts;
    static int cachedAbove = 0, cachedBelow = 0;
    auto now = currentKey();
    if (!(now == key)) { pts = computeBandSeries(&cachedAbove, &cachedBelow); key = now; }
    if (above) *above = cachedAbove;
    if (below) *below = cachedBelow;
    return pts;
}

void zoomRange(size_t n, size_t& i0, size_t& i1) {
    i0 = 0;
    i1 = n ? n - 1 : 0;
    if (n < 2) return;
    double a = std::clamp(app.zoomA, 0.0, 1.0), b = std::clamp(app.zoomB, 0.0, 1.0);
    if (b < a) std::swap(a, b);
    i0 = (size_t)std::floor(a * (double)(n - 1) + 1e-9);
    i1 = (size_t)std::ceil(b * (double)(n - 1) - 1e-9);
    size_t minSpan = std::min<size_t>(n - 1, 6);
    if (i1 < i0 + minSpan) {
        i1 = std::min(n - 1, i0 + minSpan);
        if (i1 < i0 + minSpan) i0 = i1 - minSpan;
    }
}
void resetZoom() { app.zoomA = 0; app.zoomB = 1; }

void dailyYears(int& first, int& last) {
    const auto& s = st();
    first = s.days.empty() ? 0 : s.days.front().y;
    last = s.days.empty() ? -1 : s.days.back().y;
}
bool dailyYear(int year) {
    int a, b;
    dailyYears(a, b);
    return year >= a && year <= b;
}

const std::vector<double>& bandCompare() {
    static SeriesKey key;
    static int keyYear = -1;
    static std::vector<double> out;
    auto now = currentKey();
    if (now == key && keyYear == app.compareYear) return out;
    key = now;
    keyYear = app.compareYear;
    out.clear();
    if (!app.compareYear) return out;
    const auto& pts = bandSeries(nullptr, nullptr);
    double t[13][32];
    bool h[13][32] = {};
    for (const auto& r : st().days)
        if (r.y == app.compareYear && r.has(app.bandVar)) { t[r.m][r.d] = r.v[app.bandVar]; h[r.m][r.d] = true; }
    if (!isLeap(app.compareYear) && app.season != Season::Month && h[2][28] && h[3][1]) {
        t[2][29] = (t[2][28] + t[3][1]) / 2.0;
        h[2][29] = true;
    }
    out.reserve(pts.size());
    for (const auto& p : pts) out.push_back(h[p.month][p.day] ? t[p.month][p.day] : NaN);
    return out;
}

const std::vector<double>& rainCompare() {
    static SeriesKey key;
    static int keyYear = -1;
    static std::vector<double> out;
    auto now = currentKey();
    now.var = 0;
    if (now == key && keyYear == app.compareYear) return out;
    key = now;
    keyYear = app.compareYear;
    out.clear();
    if (!app.compareYear) return out;
    const auto& pts = rainSeries();
    if (app.season == Season::Month) {
        double raw[32] = {};
        bool has[32] = {};
        for (const auto& r : st().days)
            if (r.y == app.compareYear && r.m == app.month && r.has(VP)) { raw[r.d] = r.v[VP]; has[r.d] = true; }
        double cum[32];
        double acc = 0;
        for (int d = 1; d <= 31; ++d) { if (has[d]) acc += raw[d]; cum[d] = has[d] ? acc : NaN; }
        for (const auto& p : pts) out.push_back(p.mark >= 1 && p.mark <= 31 ? cum[p.mark] : NaN);
        return out;
    }
    double cum[366];
    bool ok[366];
    yearCumul(app.compareYear, cum, ok);
    for (const auto& p : pts) out.push_back(p.mark >= 0 && p.mark < 366 && ok[p.mark] ? cum[p.mark] : NaN);
    return out;
}

// ------------------------------------------------------------ ondate di caldo

static HeatWave makeWave(size_t first, size_t count) {
    const auto& days = st().days;
    HeatWave w;
    w.first = first;
    w.days = (int)count;
    const auto& a = days[first];
    const auto& b = days[first + count - 1];
    w.y0 = a.y; w.m0 = a.m; w.d0 = a.d;
    w.y1 = b.y; w.m1 = b.m; w.d1 = b.d;
    double sT = 0, sX = 0, sN = 0;
    int nt = 0, nx = 0, nn = 0;
    w.maxTx = -1e9; w.minTn = 1e9;
    for (size_t i = first; i < first + count; ++i) {
        const auto& r = days[i];
        if (r.has(VTM)) { sT += r.v[VTM]; ++nt; }
        if (r.has(VTX)) { sX += r.v[VTX]; ++nx; w.maxTx = std::max(w.maxTx, (double)r.v[VTX]); }
        if (r.has(VTN)) { sN += r.v[VTN]; ++nn; w.minTn = std::min(w.minTn, (double)r.v[VTN]); }
    }
    w.meanT = nt ? sT / nt : NaN;
    w.meanTx = nx ? sX / nx : NaN;
    w.meanTn = nn ? sN / nn : NaN;
    if (!nx) w.maxTx = NaN;
    if (!nn) w.minTn = NaN;
    return w;
}

const std::vector<HeatWave>& heatWaves() {
    static unsigned long version = ~0ul;
    static HeatMode mode = HeatMode::Five;
    static int threshold = -1000;
    static std::vector<HeatWave> out;
    if (version == app.dataVersion && mode == app.heatMode && threshold == app.heatThreshold) return out;
    version = app.dataVersion; mode = app.heatMode; threshold = app.heatThreshold;
    out.clear();
    const auto& days = st().days;
    const size_t top = 10;
    if (app.heatMode == HeatMode::Streak) {
        std::vector<HeatWave> all;
        size_t start = 0, count = 0;
        long lastDay = 0;
        auto close = [&]() { if (count) all.push_back(makeWave(start, count)); count = 0; };
        for (size_t i = 0; i < days.size(); ++i) {
            if (!days[i].has(VTX)) { close(); continue; }
            bool hot = days[i].v[VTX] > app.heatThreshold;
            if (!hot) { close(); continue; }
            if (count && days[i].n - lastDay != 1) close();
            if (!count) start = i;
            ++count;
            lastDay = days[i].n;
        }
        close();
        std::stable_sort(all.begin(), all.end(), [&](const HeatWave& a, const HeatWave& b) {
            if (a.days != b.days) return a.days > b.days;
            return a.maxTx > b.maxTx;
        });
        if (all.size() > top) all.resize(top);
        out = std::move(all);
        return out;
    }
    const size_t window = app.heatMode == HeatMode::Five ? 5 : 10;
    struct Cand { size_t first; double mean; long d0, d1; };
    std::vector<Cand> cands;
    for (size_t i = 0; i + window <= days.size(); ++i) {
        if (days[i + window - 1].n - days[i].n != (long)window - 1) continue;
        bool complete = true;
        double sum = 0;
        for (size_t j = i; j < i + window && complete; ++j) {
            complete = days[j].has(VTM) && days[j].has(VTN) && days[j].has(VTX);
            if (complete) sum += days[j].v[VTM];
        }
        if (!complete) continue;
        cands.push_back(Cand{i, sum / (double)window, days[i].n, days[i + window - 1].n});
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.mean != b.mean) return a.mean > b.mean;
        return a.d0 < b.d0;
    });
    std::vector<Cand> chosen;
    for (const auto& c : cands) {
        bool overlap = false;
        for (const auto& s : chosen) if (!(c.d1 < s.d0 || c.d0 > s.d1)) { overlap = true; break; }
        if (overlap) continue;
        chosen.push_back(c);
        if (chosen.size() >= top) break;
    }
    for (const auto& c : chosen) out.push_back(makeWave(c.first, window));
    return out;
}

// ------------------------------------------------------------ indici annuali

bool haveTminTmax() { return st().hasVar[VTN] || st().hasVar[VTX]; }

static int expectedDays(int y) {
    int e = 0;
    for (int m = 1; m <= 12; ++m) if (monthInSeason(m)) e += daysIn(y, m);
    return e;
}

const std::vector<ThermalYear>& thermalYears() {
    static unsigned long version = ~0ul;
    static int y0 = 0, y1 = 0, month = 0;
    static Season season = Season::Year;
    static std::vector<ThermalYear> out;
    if (version == app.dataVersion && y0 == app.y0 && y1 == app.y1 && month == app.month && season == app.season) return out;
    version = app.dataVersion; y0 = app.y0; y1 = app.y1; month = app.month; season = app.season;
    out.clear();
    int first, last;
    dailyYears(first, last);
    int a = std::max(first, app.y0), b = std::min(last, app.y1);
    if (b < a) return out;
    std::vector<double> rangeSum(b - a + 1, 0.0);
    std::vector<int> rangeN(b - a + 1, 0);
    for (int y = a; y <= b; ++y) { ThermalYear t; t.year = y; t.expected = expectedDays(y); out.push_back(t); }
    for (const auto& r : st().days) {
        if (r.y < a || r.y > b || !monthInSeason(r.m)) continue;
        auto& t = out[r.y - a];
        bool hn = r.has(VTN), hx = r.has(VTX);
        if (hn && r.v[VTN] > 20) ++t.tropical;
        if (hn && r.v[VTN] < 0) ++t.frost;
        if (hx && r.v[VTX] < 0) ++t.ice;
        if (hx && r.v[VTX] > 25) ++t.summer;
        if (hx && r.v[VTX] > 30) ++t.hot30;
        if (hn && hx) { ++t.measured; rangeSum[r.y - a] += r.v[VTX] - r.v[VTN]; ++rangeN[r.y - a]; }
    }
    for (size_t i = 0; i < out.size(); ++i) {
        out[i].range = rangeN[i] ? rangeSum[i] / rangeN[i] : NaN;
        out[i].partial = out[i].measured < 0.9 * out[i].expected;
    }
    return out;
}

const std::vector<RainYear>& rainYears() {
    static unsigned long version = ~0ul;
    static int y0 = 0, y1 = 0, month = 0;
    static Season season = Season::Year;
    static std::vector<RainYear> out;
    if (version == app.dataVersion && y0 == app.y0 && y1 == app.y1 && month == app.month && season == app.season) return out;
    version = app.dataVersion; y0 = app.y0; y1 = app.y1; month = app.month; season = app.season;
    out.clear();
    int first, last;
    dailyYears(first, last);
    int a = std::max(first, app.y0), b = std::min(last, app.y1);
    if (b < a) return out;
    for (int y = a; y <= b; ++y) { RainYear r; r.year = y; r.expected = expectedDays(y); r.total = 0; r.maxDay = NaN; out.push_back(r); }
    std::vector<int> run(out.size(), 0);
    std::vector<long> prev(out.size(), 0);
    for (const auto& d : st().days) {
        if (d.y < a || d.y > b || !monthInSeason(d.m)) continue;
        auto& r = out[d.y - a];
        size_t k = d.y - a;
        if (!d.has(VP)) { run[k] = 0; continue; }
        double p = d.v[VP];
        ++r.measured;
        r.total += p;
        if (p >= 1) ++r.wet;
        if (p >= 20) ++r.heavy;
        if (!finite(r.maxDay) || p > r.maxDay) r.maxDay = p;
        // Periodo secco: giorni consecutivi misurati con meno di 1 mm.
        if (p < 1) {
            run[k] = (run[k] && d.n - prev[k] == 1) ? run[k] + 1 : 1;
            r.dryRun = std::max(r.dryRun, run[k]);
        } else run[k] = 0;
        prev[k] = d.n;
    }
    for (auto& r : out) {
        r.partial = r.measured < 0.9 * r.expected;
        if (!r.measured) r.total = NaN;
    }
    return out;
}

// ------------------------------------------------------------ climatologia della stazione

const std::vector<ClimaRow>& climaTable() {
    static unsigned long version = ~0ul;
    static RefMode ref = RefMode::Station;
    static int y0 = 0, y1 = 0;
    static std::vector<ClimaRow> out;
    if (version == app.dataVersion && ref == app.refMode && (ref != RefMode::Visible || (y0 == app.y0 && y1 == app.y1))) return out;
    version = app.dataVersion; ref = app.refMode; y0 = app.y0; y1 = app.y1;
    out.assign(13, ClimaRow{});
    const auto& s = st();
    double sT[13] = {}, sN[13] = {}, sX[13] = {}, sP[13] = {}, sW[13] = {};
    int nT[13] = {}, nN[13] = {}, nX[13] = {}, nP[13] = {};
    for (const auto& mo : s.months) {
        if (!yearInRef(s, mo.y)) continue;
        if (mo.okT) { sT[mo.m] += mo.t; nT[mo.m]++; }
        if (!mo.partial && mo.nn >= mo.dim - 5) { sN[mo.m] += mo.tn; nN[mo.m]++; }
        if (!mo.partial && mo.nx >= mo.dim - 5) { sX[mo.m] += mo.tx; nX[mo.m]++; }
        if (mo.okP) { sP[mo.m] += mo.p; nP[mo.m]++; }
    }
    // Giorni piovosi (almeno 1 mm) nei mesi completi.
    for (const auto& d : s.days) {
        if (!d.has(VP) || d.v[VP] < 1 || !yearInRef(s, d.y)) continue;
        const Month* mo = findMonth(s, d.y, d.m);
        if (mo && mo->okP) sW[d.m] += 1;
    }
    double sU[13] = {}, sV[13] = {}, sR[13] = {}, sPr[13] = {};
    int nU[13] = {}, nV[13] = {}, nR[13] = {}, nPr[13] = {};
    for (const auto& d : s.days) {
        int m = d.m;
        bool inRef = yearInRef(s, d.y);
        auto& rowM = out[m];
        if (d.has(VTX) && (!finite(rowM.recTx) || d.v[VTX] > rowM.recTx)) { rowM.recTx = d.v[VTX]; rowM.recTxY = d.y; rowM.recTxD = d.d; }
        if (d.has(VTN) && (!finite(rowM.recTn) || d.v[VTN] < rowM.recTn)) { rowM.recTn = d.v[VTN]; rowM.recTnY = d.y; rowM.recTnD = d.d; }
        if (d.has(VP) && (!finite(rowM.recP) || d.v[VP] > rowM.recP)) { rowM.recP = d.v[VP]; rowM.recPY = d.y; rowM.recPD = d.d; }
        if (!inRef) continue;
        if (d.has(VUM)) { sU[m] += d.v[VUM]; nU[m]++; }
        if (d.has(VVM)) { sV[m] += d.v[VVM]; nV[m]++; }
        if (d.has(VRAD)) { sR[m] += d.v[VRAD]; nR[m]++; }
        if (d.has(VPRES)) { sPr[m] += d.v[VPRES]; nPr[m]++; }
    }
    auto& yr = out[0];
    double yt = 0, yn = 0, yx = 0, yp = 0, yw = 0, yu = 0, yv = 0, yrr = 0, ypr = 0;
    int ct = 0, cn = 0, cx = 0, cp = 0, cu = 0, cv = 0, cr = 0, cpr = 0;
    for (int m = 1; m <= 12; ++m) {
        auto& r = out[m];
        r.recM = m;
        r.years = std::max(nT[m], nP[m]);
        if (nT[m] >= MinClimYears) { r.t = sT[m] / nT[m]; yt += r.t; ++ct; }
        if (nN[m] >= MinClimYears) { r.tn = sN[m] / nN[m]; yn += r.tn; ++cn; }
        if (nX[m] >= MinClimYears) { r.tx = sX[m] / nX[m]; yx += r.tx; ++cx; }
        if (nP[m] >= MinClimYears) { r.p = sP[m] / nP[m]; r.wet = sW[m] / nP[m]; yp += r.p; yw += r.wet; ++cp; }
        if (nU[m]) { r.um = sU[m] / nU[m]; yu += r.um; ++cu; }
        if (nV[m]) { r.vm = sV[m] / nV[m]; yv += r.vm; ++cv; }
        if (nR[m]) { r.rad = sR[m] / nR[m]; yrr += r.rad; ++cr; }
        if (nPr[m]) { r.pres = sPr[m] / nPr[m]; ypr += r.pres; ++cpr; }
        if (finite(r.recTx) && (!finite(yr.recTx) || r.recTx > yr.recTx)) { yr.recTx = r.recTx; yr.recTxY = r.recTxY; yr.recTxD = r.recTxD; yr.recM = m; }
        if (finite(r.recTn) && (!finite(yr.recTn) || r.recTn < yr.recTn)) { yr.recTn = r.recTn; yr.recTnY = r.recTnY; yr.recTnD = r.recTnD; }
        if (finite(r.recP) && (!finite(yr.recP) || r.recP > yr.recP)) { yr.recP = r.recP; yr.recPY = r.recPY; yr.recPD = r.recPD; }
        yr.years = std::max(yr.years, r.years);
    }
    // Valori annuali solo se tutti i 12 mesi hanno la loro media.
    if (ct == 12) yr.t = yt / 12;
    if (cn == 12) yr.tn = yn / 12;
    if (cx == 12) yr.tx = yx / 12;
    if (cp == 12) { yr.p = yp; yr.wet = yw; }
    if (cu == 12) yr.um = yu / 12;
    if (cv == 12) yr.vm = yv / 12;
    if (cr == 12) yr.rad = yrr / 12;
    if (cpr == 12) yr.pres = ypr / 12;
    // I mesi dei record annuali: recTnD e recPD hanno bisogno del mese.
    for (int m = 1; m <= 12; ++m) {
        if (out[m].recTnY == yr.recTnY && out[m].recTnD == yr.recTnD && out[m].recTn == yr.recTn) yr.recTnD = yr.recTnD + 100 * m;
        if (out[m].recPY == yr.recPY && out[m].recPD == yr.recPD && out[m].recP == yr.recP) yr.recPD = yr.recPD + 100 * m;
    }
    return out;
}

// ------------------------------------------------------------ panoramica delle stazioni

std::vector<OverviewRow> overviewRows() {
    std::vector<OverviewRow> rows;
    for (int i = 0; i < (int)app.stations.size(); ++i) {
        const auto& s = *app.stations[i];
        OverviewRow r;
        r.index = i;
        if (s.loaded && !s.months.empty()) {
            double cT[13], cP[13]; int nT[13], nP[13];
            monthClimate(s, cT, cP, nT, nP);
            double aT = 0, sumP = 0, sumC = 0; int kT = 0, kP = 0;
            for (int m = 1; m <= 12; ++m) {
                if (!monthInSeason(m)) continue;
                const Month* mo = findMonth(s, app.focusYear, m);
                if (!mo) continue;
                if (mo->okT && finite(cT[m])) { aT += mo->t - cT[m]; ++kT; }
                if (mo->okP && finite(cP[m])) { sumP += mo->p; sumC += cP[m]; ++kP; }
            }
            if (kT) r.tAn = aT / kT;
            if (kP) { r.pAn = sumP - sumC; r.pPct = sumC > 0 ? 100.0 * sumP / sumC : NaN; }
            r.nMonths = kT;
        }
        rows.push_back(r);
    }
    auto lastN = [](const Station& s) { return s.days.empty() ? 0L : s.days.back().n; };
    if (app.overviewSort == 1)
        std::stable_sort(rows.begin(), rows.end(), [](const OverviewRow& a, const OverviewRow& b) {
            if (finite(a.tAn) != finite(b.tAn)) return finite(a.tAn);
            return finite(a.tAn) && a.tAn > b.tAn;
        });
    else if (app.overviewSort == 2)
        std::stable_sort(rows.begin(), rows.end(), [](const OverviewRow& a, const OverviewRow& b) {
            if (finite(a.pPct) != finite(b.pPct)) return finite(a.pPct);
            return finite(a.pPct) && a.pPct > b.pPct;
        });
    else if (app.overviewSort == 3)
        std::stable_sort(rows.begin(), rows.end(), [&](const OverviewRow& a, const OverviewRow& b) {
            return lastN(*app.stations[a.index]) > lastN(*app.stations[b.index]);
        });
    return rows;
}
