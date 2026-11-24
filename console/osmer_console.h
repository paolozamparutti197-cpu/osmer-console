// Console OSMER - dichiarazioni comuni a tutti i moduli.
//
// Versione multi-stazione della Console Udine: legge
// direttamente i CSV OSMER di tutte le stazioni in ..\dati_stazioni e
// ricava la climatologia di ogni stazione SOLO dai suoi dati.
//
//   dati.cpp            stazioni, lettura CSV, mesi, climatologia, SPEI, serie
//   grafica.cpp         disegno della finestra e dei grafici (GDI+)
//   esportazione.cpp    salvataggio PNG, EMF e CSV, copia negli appunti
//   aggiornamento.cpp   caricamento in background, controllo dei file,
//                       "Aggiorna tutte le stazioni da OSMER"
//   osmer_console.cpp   finestra, mouse, tastiera, DPI e avvio
#pragma once

#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commdlg.h>
#include <gdiplus.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#endif

using namespace Gdiplus;

constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

// ---------------------------------------------------------------- dati della stazione

// Colonne dei CSV OSMER, nello stesso ordine del file.
enum Var { VP = 0, VTN, VTM, VTX, VUN, VUM, VUX, VVM, VVX, VDIR, VRAD, VPRES, NVAR };

struct Day {
    int y = 0, m = 0, d = 0;
    long n = 0;                 // numero del giorno (calendario civile)
    float v[NVAR];              // NaN = senza misura
    bool has(int k) const { return std::isfinite(v[k]); }
    double get(int k) const { return v[k]; }
};

// Riepilogo di un mese ricavato dai giorni della stazione.
struct Month {
    int y = 0, m = 0;
    int dim = 30;               // giorni del mese
    int nt = 0, np = 0, nn = 0, nx = 0, last = 0;
    double t = NaN, p = NaN, tn = NaN, tx = NaN;
    bool okT = false, okP = false;   // mese abbastanza completo per la climatologia
    bool partial = false;            // ultimo mese, non ancora chiuso
    double pet = NaN;                // evapotraspirazione Thornthwaite
    double spei3 = NaN, spei6 = NaN, spei12 = NaN;
};

struct Station {
    std::wstring name;          // dal campo stazione_nome
    std::wstring slug;          // dal nome del file
    std::wstring file;          // CSV combinato
    std::wstring code;          // prima parte di stazione_valore
    double lat = NaN, lon = NaN, alt = NaN;
    std::string stamp;          // dimensione e data del file letto
    bool loaded = false;
    std::wstring error;
    std::vector<Day> days;      // solo i giorni con almeno una misura, in ordine
    std::vector<Month> months;  // continui dal primo all'ultimo mese con dati
    int firstYear = 0, lastYear = 0;
    int lastY = 0, lastM = 0, lastD = 0;   // ultimo giorno con misure
    bool hasVar[NVAR] = {};
    long expectedDays = 0, measuredT = 0, measuredP = 0;
};

struct Sample { int year = 0, month = 0; double value = 0; int n = 0; };
enum class View {
    Overview, Clima, Year, Band, Rain, Temp, Prec, Spei, Balance, Rank, Heat, Thermal, RainIdx, Table
};
constexpr int ViewCount = 14;

// Colonna della Tabella dati: una grandezza di una stazione.
struct TableColumn { int station = 0; int var = VTM; };
enum class Season { Year, Month, Winter, Spring, Summer, Autumn };
enum class RefMode { Station, Visible, Clima1991 };
struct Hit { RECT rc{}; int id = 0; };

struct RainPt {
    int x = 0, mark = 0;
    double v = 0, lo = 0, hi = 0, mean = NaN;
    bool ok = false, loOk = false, hiOk = false;
};

struct BandPt {
    int month = 0, day = 0;
    double t = 0, p5 = 0, p95 = 0;
    double mean = NaN;
    bool ht = false;
};

struct HeatWave {
    int y0 = 0, m0 = 0, d0 = 0, y1 = 0, m1 = 0, d1 = 0, days = 0;
    double meanT = 0, meanTx = 0, maxTx = 0, meanTn = 0, minTn = 0;
    size_t first = 0;
};
// Caldo: 5 e 10 giorni piu' caldi, serie con Tmax oltre soglia.
// Freddo: 5 e 10 giorni piu' freddi, serie con Tmin sotto soglia.
enum class HeatMode { Five, Ten, Streak, Cold5, Cold10, ColdStreak };
inline bool coldMode(HeatMode m) { return m == HeatMode::Cold5 || m == HeatMode::Cold10 || m == HeatMode::ColdStreak; }
inline bool streakMode(HeatMode m) { return m == HeatMode::Streak || m == HeatMode::ColdStreak; }

// Indici annuali da Tmin e Tmax.
struct ThermalYear {
    int year = 0;
    int expected = 0, measured = 0;
    int tropical = 0, frost = 0, ice = 0, summer = 0, hot30 = 0;
    double range = NaN;
    bool partial = false;
};

// Indici annuali di pioggia.
struct RainYear {
    int year = 0;
    int expected = 0, measured = 0;
    int wet = 0, heavy = 0, dryRun = 0;
    double maxDay = NaN, total = NaN;
    bool partial = false;
};

// Climatologia di un mese (riga della vista Climatologia).
struct ClimaRow {
    double t = NaN, tn = NaN, tx = NaN, p = NaN, wet = NaN, um = NaN, vm = NaN, rad = NaN, pres = NaN;
    int years = 0;
    double recTx = NaN, recTn = NaN, recP = NaN;
    int recTxY = 0, recTxD = 0, recTnY = 0, recTnD = 0, recPY = 0, recPD = 0, recM = 0;
};

// Riga della panoramica: una per stazione.
struct OverviewRow {
    int index = 0;
    double tAn = NaN, pAn = NaN, pPct = NaN;
    int nMonths = 0;
};

struct App {
    std::vector<std::unique_ptr<Station>> stations;
    int current = 0;
    unsigned long dataVersion = 1;
    View view = View::Overview;
    int y0 = 1991, y1 = 2026, focusYear = 2026, month = 1;
    Season season = Season::Year;
    RefMode refMode = RefMode::Station;
    int bandVar = VTM;
    std::wstring root;          // cartella osmer
    std::wstring outDir;
    std::wstring status = L"PagSu / PagGiu: stazione precedente / successiva    Tasti: elenco completo    Esc: chiudi";
    std::vector<Hit> hits;
    HWND hwnd = nullptr;
    HWND yearEdit = nullptr;
    HFONT yearFont = nullptr;
    WNDPROC yearEditProc = nullptr;
    int editTarget = 0;
    bool editBusy = false;
    int menuScroll = 0;
    int overviewScroll = 0;
    int overviewSort = 0;       // 0 nome, 1 scarto T, 2 scarto P, 3 ultimo dato
    int picker = 0;             // 1 stagione, 2 riferimento, 3 tasti, 4 stazioni, 5 grandezza
    int hotId = 0;
    int hoverA = -1, hoverB = -1;
    POINT hoverPt{};
    // Righe di tabella con un riquadro di dettagli al passaggio del mouse.
    struct MonthHit { RECT rc; int year, month; std::vector<std::wstring> tip; };
    std::vector<MonthHit> monthHits;
    std::vector<std::wstring> monthTip;
    int hoverMonth = -1;
    RectF pageRc{};
    double zoomA = 0, zoomB = 1;
    bool mouseDown = false, dragging = false;
    POINT dragStart{}, dragLast{};
    int dragProbe = -1;
    int compareYear = 0;
    HeatMode heatMode = HeatMode::Five;
    int heatThreshold = 33;
    int coldThreshold = -5;     // soglia della Tmin per le serie fredde
    int heatSel = 0;
    // Tabella dati: periodo (numeri di giorno, 0 = da impostare), stazioni
    // scelte (vuoto = stazione corrente), grandezza se le stazioni sono piu' di una.
    long tableFrom = 0, tableTo = 0;
    int tableVar = VTM;
    std::vector<std::wstring> tableSlugs;
    int tableScroll = 0;
    struct Probe { RECT plot; std::vector<std::wstring> tip; std::vector<int> year; };
    std::vector<Probe> probes;
    float scale = 1.f;
    HBITMAP scene = nullptr;
    int sceneW = 0, sceneH = 0;
    bool sceneDirty = true;
    std::wstring sceneKey;
};
extern App app;

Station& st();                       // stazione selezionata
bool haveStation();

constexpr int MinClientWidth = 1080;
constexpr int MinClientHeight = 600;

// ---------------------------------------------------------------- dati.cpp

bool finite(double v);
std::wstring exeDir();
std::wstring joinPath(const std::wstring& a, const std::wstring& b);
std::wstring widen(const std::string& text);
std::string narrow(const std::wstring& text);
std::vector<std::unique_ptr<Station>> discoverStations(const std::wstring& dataDir);
bool loadStation(Station& s);        // legge il CSV e calcola mesi, PET e SPEI
std::string fileStamp(const std::wstring& path);
int dayIndex(int m, int d);
bool isLeap(int y);
int daysIn(int y, int m);
long dayNumber(int y, int m, int d);
void ymdFromIndex(int ix, int& m, int& d);
const wchar_t* monthName(int m);
const wchar_t* monthAbbr(int m);
const wchar_t* varName(int k);
const wchar_t* varUnit(int k);
int varDigits(int k);
std::wstring dateText(int y, int m, int d);
std::wstring stationLabel(const Station& s);
std::wstring stationPeriod(const Station& s);
bool monthInSeason(int m);
std::wstring seasonLabel();
std::wstring refLabel();
void refRange(const Station& s, int& a, int& b);
bool yearInRef(const Station& s, int y);
std::wstring f2(double v);
std::wstring plainNum(double v, int prec);
std::vector<std::wstring> monthDetails(int year, int month);
const Month* findMonth(const Station& s, int y, int m);
void clampPeriod();
void fullPeriod();
void focusRange(int& lo, int& hi);
void saveSession();
bool loadSession();
void cycleSeason();
void selectStation(int index, bool keepPeriod = true);
std::vector<Sample> anomalySeries(bool precip);
std::vector<Sample> balanceSeries();
std::vector<Sample> yearMinSpei12();
std::vector<Sample> speiMonthly(int scale);
std::wstring classeSpei(double z);
void monthClimate(const Station& s, double climT[13], double climP[13], int cntT[13], int cntP[13]);
const std::vector<RainPt>& rainSeries();
const std::vector<BandPt>& bandSeries(int* above, int* below);
const std::vector<double>& bandCompare();
const std::vector<double>& rainCompare();
bool dailyYear(int year);
void dailyYears(int& first, int& last);
const std::vector<HeatWave>& heatWaves();
const std::vector<ThermalYear>& thermalYears();
const std::vector<RainYear>& rainYears();
const std::vector<ClimaRow>& climaTable();   // 13 righe: 12 mesi e anno
std::vector<OverviewRow> overviewRows();
bool haveTminTmax();
void zoomRange(size_t n, size_t& i0, size_t& i1);
void resetZoom();
// Tabella dati.
void civilFromDays(long n, int& y, int& m, int& d);
int weekDay(long n);                                  // 0 = domenica
bool parseDate(const std::wstring& text, long& n);    // gg/mm/aaaa
void ensureTablePeriod();
std::vector<int> tableStations();                     // indici delle stazioni in tabella
std::vector<TableColumn> tableColumns();
double tableValue(const Station& s, long n, int var); // NaN se manca
const wchar_t* varShort(int k);
constexpr int TableMaxStations = 10;

// ---------------------------------------------------------------- grafica.cpp

void paintScene(Graphics& g, int W, int H);
void paintOverlay(Graphics& g, int W, int H);
void paintWindow(Graphics& g, int W, int H);
void paintContent(Graphics& g, const RectF& page, bool controls = false);
std::wstring sceneSignature();

// ---------------------------------------------------------------- esportazione.cpp

int GetEncoderClsid(const WCHAR* format, CLSID* pClsid);
void openOutDir();
bool saveFiles();
bool copyChartToClipboard();

// ---------------------------------------------------------------- aggiornamento.cpp

constexpr UINT WM_STATION_LOADED = WM_APP + 1;
constexpr UINT_PTR WatchTimer = 40;
constexpr UINT_PTR OsmerTimer = 42;

void startBackgroundLoad(const std::vector<int>& which);
void onStationLoaded(LPARAM lp);
bool loadingStations();
int stationsLoaded();
void checkSourceFiles();
void updateAllFromOsmer();
void pollOsmerUpdate();
bool osmerRunning();
void cancelOsmerUpdate();
void stopBackgroundLoad();
std::wstring osmerProgress();
std::wstring footerStatus();
std::wstring lastOsmerUpdate();

// ---------------------------------------------------------------- osmer_console.cpp

void requestRedraw(bool sceneChanged = true);
