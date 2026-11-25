// Controllo qualita' dei dati giornalieri ed esclusione dei dati scelti dall'utente.
//
// I controlli segnalano, non correggono: un dato si toglie dai calcoli solo se
// l'utente lo esclude dalla vista Controllo qualita'. Le esclusioni stanno in
// console\dati_esclusi.csv e vengono applicate alla lettura dei CSV.
#include "osmer_console.h"

#include <map>
#include <set>
#include <tuple>

// ------------------------------------------------------------ esclusioni

namespace {
std::mutex exclMutex;
std::set<std::tuple<std::wstring, long, int>> exclusions;   // slug, giorno, grandezza
bool exclLoaded = false;

std::wstring exclFile() { return joinPath(exeDir(), L"dati_esclusi.csv"); }

void loadExclusionsLocked() {
    if (exclLoaded) return;
    exclLoaded = true;
    std::ifstream in{std::filesystem::path(exclFile())};
    std::string line;
    std::getline(in, line);   // intestazione
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // slug;aaaa-mm-gg;indice grandezza;nome grandezza;valore
        std::vector<std::string> c;
        std::string cur;
        for (char ch : line) { if (ch == ';') { c.push_back(cur); cur.clear(); } else cur.push_back(ch); }
        c.push_back(cur);
        if (c.size() < 3 || c[1].size() < 10) continue;
        int y = atoi(c[1].c_str()), m = atoi(c[1].c_str() + 5), d = atoi(c[1].c_str() + 8);
        int var = atoi(c[2].c_str());
        if (m < 1 || m > 12 || d < 1 || d > 31 || var < 0 || var >= NVAR) continue;
        exclusions.insert({widen(c[0]), dayNumber(y, m, d), var});
    }
}

void saveExclusionsLocked() {
    std::ofstream out{std::filesystem::path(exclFile()), std::ios::binary};
    out << "stazione;data;indice_grandezza;grandezza\n";
    for (const auto& e : exclusions) {
        int y, m, d;
        civilFromDays(std::get<1>(e), y, m, d);
        char date[16];
        snprintf(date, sizeof(date), "%04d-%02d-%02d", y, m, d);
        out << narrow(std::get<0>(e)) << ';' << date << ';' << std::get<2>(e) << ';' << narrow(varShort(std::get<2>(e))) << '\n';
    }
}
}

void loadExclusions() {
    std::lock_guard<std::mutex> lock(exclMutex);
    loadExclusionsLocked();
}

// Chiamata anche dal thread di caricamento: protetta dal mutex.
bool isExcluded(const std::wstring& slug, long n, int var) {
    std::lock_guard<std::mutex> lock(exclMutex);
    loadExclusionsLocked();
    return exclusions.count({slug, n, var}) > 0;
}

int exclusionCount() {
    std::lock_guard<std::mutex> lock(exclMutex);
    loadExclusionsLocked();
    return (int)exclusions.size();
}

bool toggleExclusion(const std::wstring& slug, long n, int var) {
    std::lock_guard<std::mutex> lock(exclMutex);
    loadExclusionsLocked();
    auto key = std::make_tuple(slug, n, var);
    bool now;
    if (exclusions.count(key)) { exclusions.erase(key); now = false; }
    else { exclusions.insert(key); now = true; }
    saveExclusionsLocked();
    return now;
}

// ------------------------------------------------------------ controlli

const wchar_t* qcKindName(int k) {
    static const wchar_t* n[] = {L"Incoerenza Tmin/Tmedia/Tmax", L"Fuori dal clima della stazione", L"Diverso dalle stazioni vicine",
                                 L"Salto isolato", L"Valore bloccato", L"Valore impossibile (scartato)", L"Escluso dall'utente"};
    return k >= 0 && k < QcKindCount ? n[k] : L"";
}

namespace {
constexpr double kPi = 3.14159265358979;

double distanceKm(const Station& a, const Station& b) {
    double la = a.lat * kPi / 180, lb = b.lat * kPi / 180;
    double dl = (b.lon - a.lon) * kPi / 180;
    double x = dl * std::cos((la + lb) / 2), y = lb - la;
    return 6371.0 * std::sqrt(x * x + y * y);
}

// Media e deviazione standard per giorno dell'anno, su una finestra di +-7 giorni.
struct DayClim { double mean[366], sd[366]; int n[366]; };

DayClim dayClimate(const Station& s, int var) {
    DayClim c{};
    double sum[366] = {}, sq[366] = {};
    int cnt[366] = {};
    for (const auto& d : s.days) {
        if (!d.has(var)) continue;
        int ix = dayIndex(d.m, d.d);
        sum[ix] += d.v[var]; sq[ix] += (double)d.v[var] * d.v[var]; ++cnt[ix];
    }
    for (int i = 0; i < 366; ++i) {
        double S = 0, Q = 0; int N = 0;
        for (int k = -7; k <= 7; ++k) { int j = (i + k + 366) % 366; S += sum[j]; Q += sq[j]; N += cnt[j]; }
        c.n[i] = N;
        c.mean[i] = N ? S / N : NaN;
        double var2 = N > 1 ? (Q - S * S / N) / (N - 1) : NaN;
        c.sd[i] = finite(var2) && var2 > 0 ? std::sqrt(var2) : NaN;
    }
    return c;
}

std::wstring num1(double v) { return plainNum(v, 1); }

struct Builder {
    // Un dato segnalato da piu' controlli diventa una riga sola.
    std::map<std::tuple<int, long, int>, size_t> where;
    std::vector<QcItem> items;
    void add(int station, long n, int var, double value, int kind, int severity, const std::wstring& detail) {
        auto key = std::make_tuple(station, n, var);
        auto it = where.find(key);
        if (it != where.end()) {
            auto& q = items[it->second];
            q.severity = std::max(q.severity, severity);
            q.detail += L"   |   " + detail;
            return;
        }
        QcItem q;
        q.station = station; q.n = n; q.var = var; q.value = value; q.kind = kind; q.severity = severity; q.detail = detail;
        where[key] = items.size();
        items.push_back(std::move(q));
    }
};

std::vector<QcItem> computeQc() {
    Builder b;
    const int N = (int)app.stations.size();
    const int tvars[3] = {VTN, VTM, VTX};
    // Per ogni stazione: posizione di ogni giorno nel vettore (accesso diretto) e clima giornaliero.
    struct Aux { long first = 0; std::vector<int> pos; DayClim clim[3]; bool ok = false; };
    std::vector<Aux> aux(N);
    for (int i = 0; i < N; ++i) {
        const auto& s = *app.stations[i];
        if (!s.loaded || s.days.empty()) continue;
        auto& a = aux[i];
        a.ok = true;
        a.first = s.days.front().n;
        a.pos.assign(s.days.back().n - a.first + 1, -1);
        for (int k = 0; k < (int)s.days.size(); ++k) a.pos[s.days[k].n - a.first] = k;
        for (int t = 0; t < 3; ++t) a.clim[t] = dayClimate(s, tvars[t]);
    }
    auto dayAt = [&](int i, long n) -> const Day* {
        const auto& a = aux[i];
        if (!a.ok || n < a.first || n - a.first >= (long)a.pos.size()) return nullptr;
        int k = a.pos[n - a.first];
        return k < 0 ? nullptr : &app.stations[i]->days[k];
    };
    // Stazioni vicine: entro 35 km e 350 m di dislivello.
    std::vector<std::vector<int>> nearby(N);
    for (int i = 0; i < N; ++i) {
        const auto& s = *app.stations[i];
        if (!aux[i].ok || !finite(s.lat) || !finite(s.alt)) continue;
        for (int j = 0; j < N; ++j) {
            const auto& o = *app.stations[j];
            if (j == i || !aux[j].ok || !finite(o.lat) || !finite(o.alt)) continue;
            if (distanceKm(s, o) <= 35 && std::abs(s.alt - o.alt) <= 350) nearby[i].push_back(j);
        }
    }

    // Candidati del confronto con le vicine: si decidono alla fine, guardando tutte le stazioni.
    struct NearCand { int station; long n; int var; double x; int severity; std::wstring detail; };
    std::vector<NearCand> nearCands;
    for (int i = 0; i < N; ++i) {
        const auto& s = *app.stations[i];
        if (!s.loaded) continue;
        for (const auto& f : s.flags)
            b.add(i, f.n, f.var, f.value, f.user ? QcExcluded : QcDiscarded, f.user ? 0 : 2,
                  f.user ? L"escluso dai calcoli dall'utente" : L"fuori dai limiti fisici: ignorato in lettura");
        if (!aux[i].ok) continue;
        const auto& days = s.days;
        // Confronto con le vicine: per ogni giorno lo scostamento tra lo scarto della
        // stazione e la mediana degli scarti delle vicine. Si raccoglie tutto e poi si
        // segnala solo cio' che e' eccezionale PER QUESTA STAZIONE: una stazione sul
        // mare o in fondo a una valle si stacca spesso dalle vicine, e non e' un errore.
        struct NearDiff { long n; double x, dev, med, diff; int cnt; };
        std::vector<NearDiff> nd[3];
        for (size_t k = 0; k < days.size(); ++k) {
            const auto& d = days[k];
            // 1. Coerenza tra Tmin, Tmedia e Tmax dello stesso giorno.
            if (d.has(VTN) && d.has(VTX) && d.v[VTN] > d.v[VTX] + 0.05)
                b.add(i, d.n, VTN, d.v[VTN], QcInconsistent, 2, L"Tmin " + num1(d.v[VTN]) + L" piu alta della Tmax " + num1(d.v[VTX]));
            else if (d.has(VTM) && d.has(VTN) && d.has(VTX) && (d.v[VTM] < d.v[VTN] - 0.5 || d.v[VTM] > d.v[VTX] + 0.5))
                b.add(i, d.n, VTM, d.v[VTM], QcInconsistent, 1,
                      L"Tmedia " + num1(d.v[VTM]) + L" fuori da Tmin " + num1(d.v[VTN]) + L" - Tmax " + num1(d.v[VTX]));
            for (int t = 0; t < 3; ++t) {
                int v = tvars[t];
                if (!d.has(v)) continue;
                double x = d.v[v];
                int ix = dayIndex(d.m, d.d);
                const auto& c = aux[i].clim[t];
                // 2. Fuori dal clima della stazione: oltre 4,5 deviazioni standard.
                if (c.n[ix] >= 60 && finite(c.sd[ix]) && c.sd[ix] > 0.3) {
                    double z = (x - c.mean[ix]) / c.sd[ix];
                    if (std::abs(z) >= 4.5)
                        b.add(i, d.n, v, x, QcClimate, std::abs(z) >= 5.5 ? 2 : 1,
                              num1(x) + L" C contro una media di " + num1(c.mean[ix]) + L" C in questo periodo (" + f2(z) + L" deviazioni)");
                }
                // 3. Salto isolato: lontano sia dal giorno prima sia dal giorno dopo.
                if (k > 0 && k + 1 < days.size() && days[k - 1].n == d.n - 1 && days[k + 1].n == d.n + 1
                    && days[k - 1].has(v) && days[k + 1].has(v)) {
                    double d1 = x - days[k - 1].v[v], d2 = x - days[k + 1].v[v];
                    if (d1 * d2 > 0 && std::min(std::abs(d1), std::abs(d2)) >= 10)
                        b.add(i, d.n, v, x, QcSpike, 2, L"salto di " + num1(d1) + L" C dal giorno prima e " + num1(d2) + L" C dal giorno dopo");
                }
                // 4. Confronto con le stazioni vicine, sugli scarti dal proprio clima.
                if (nearby[i].size() >= 2 && finite(c.mean[ix])) {
                    double dev = x - c.mean[ix];
                    std::vector<double> others;
                    for (int j : nearby[i]) {
                        const Day* o = dayAt(j, d.n);
                        const auto& cj = aux[j].clim[t];
                        if (o && o->has(v) && finite(cj.mean[ix])) others.push_back(o->v[v] - cj.mean[ix]);
                    }
                    if (others.size() >= 2) {
                        std::sort(others.begin(), others.end());
                        double med = others.size() % 2 ? others[others.size() / 2]
                                                       : (others[others.size() / 2 - 1] + others[others.size() / 2]) / 2;
                        nd[t].push_back(NearDiff{d.n, x, dev, med, dev - med, (int)others.size()});
                    }
                }
            }
        }
        for (int t = 0; t < 3; ++t) {
            const auto& list = nd[t];
            if (list.size() < 200) continue;   // serve una storia per sapere cosa e' normale
            double sum = 0, sq = 0;
            for (const auto& e : list) { sum += e.diff; sq += e.diff * e.diff; }
            double mean = sum / list.size();
            double sd = std::sqrt(std::max(1e-9, sq / list.size() - mean * mean));
            for (const auto& e : list) {
                double z = (e.diff - mean) / sd;
                double off = e.diff - mean;
                // Eccezionale per la stazione (5 deviazioni) e comunque almeno 8 C.
                if (std::abs(z) >= 5 && std::abs(off) >= 8)
                    nearCands.push_back(NearCand{i, e.n, tvars[t], e.x, std::abs(z) >= 7 ? 2 : 1,
                          L"scarto dalla norma " + f2(e.dev) + L" C, stazioni vicine " + f2(e.med) + L" C (" + std::to_wstring(e.cnt)
                          + L"); di solito la differenza e' " + f2(mean) + L" +- " + plainNum(sd, 1) + L" C"});
            }
        }
        // 5. Valori bloccati: lo stesso valore per molti giorni consecutivi.
        for (int v : {VTN, VTM, VTX, VP}) {
            size_t start = 0;
            int run = 0;
            for (size_t k = 0; k <= days.size(); ++k) {
                bool same = k < days.size() && run > 0 && days[k].has(v) && days[k].n == days[k - 1].n + 1
                            && days[k].v[v] == days[start].v[v];
                if (same) { ++run; continue; }
                bool rainOk = v == VP && run >= 4 && days[start].v[v] >= 1.0;   // la pioviggine ripetuta e' normale
                if ((v != VP && run >= 5) || rainOk)
                    b.add(i, days[start].n, v, days[start].v[v], QcStuck, run >= 10 ? 2 : 1,
                          L"stesso valore " + num1(days[start].v[v]) + L" per " + std::to_wstring(run) + L" giorni di fila");
                if (k < days.size() && days[k].has(v)) { start = k; run = 1; } else run = 0;
            }
        }
    }
    // Un errore riguarda una stazione sola; se nello stesso giorno si staccano dalle
    // vicine tre o piu' stazioni e' il tempo (fohn, bora, inversioni), non un errore.
    std::map<long, std::set<int>> perDay;
    for (const auto& c : nearCands) perDay[c.n].insert(c.station);
    for (const auto& c : nearCands)
        if (perDay[c.n].size() <= 2) b.add(c.station, c.n, c.var, c.x, QcNeighbors, c.severity, c.detail);
    auto items = std::move(b.items);
    // Le esclusioni dell'utente restano visibili, cosi' si possono annullare.
    for (auto& q : items) {
        const auto& s = *app.stations[q.station];
        q.excluded = q.kind == QcExcluded || isExcluded(s.slug, q.n, q.var);
    }
    std::stable_sort(items.begin(), items.end(), [](const QcItem& a, const QcItem& b) {
        if (a.n != b.n) return a.n > b.n;
        return a.station < b.station;
    });
    return items;
}
}

const std::vector<QcItem>& qcItems() {
    static unsigned long version = ~0ul;
    static std::vector<QcItem> out;
    if (version != app.qcVersion) { out = computeQc(); version = app.qcVersion; }
    return out;
}

std::vector<const QcItem*> qcFiltered() {
    std::vector<const QcItem*> out;
    const auto& all = qcItems();
    for (const auto& q : all) {
        if (!app.qcAll && q.station != app.current) continue;
        if (app.qcKind >= 0 && q.kind != app.qcKind) continue;
        if (app.qcSevere && q.severity < 2 && q.kind != QcExcluded) continue;
        out.push_back(&q);
    }
    return out;
}
