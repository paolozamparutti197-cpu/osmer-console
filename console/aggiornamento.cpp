// Aggiornamento: caricamento delle stazioni in background, controllo dei file
// e pulsante "Aggiorna tutte le stazioni da OSMER".
#include "osmer_console.h"

// ------------------------------------------------------------ caricamento in background
// Il thread legge i CSV in oggetti Station propri e li consegna alla finestra
// con PostMessage: i dati in uso non vengono mai toccati da due thread.

namespace {
struct Loader {
    std::thread worker;
    std::mutex mu;
    std::vector<std::wstring> queue;
    bool running = false;
    std::atomic<bool> stop{false};
    std::atomic<int> pending{0};
} loader;

void loaderMain() {
    for (;;) {
        std::wstring file;
        {
            std::lock_guard<std::mutex> lock(loader.mu);
            if (loader.queue.empty() || loader.stop) { loader.running = false; return; }
            file = loader.queue.front();
            loader.queue.erase(loader.queue.begin());
        }
        auto* s = new Station();
        s->file = file;
        loadStation(*s);
        if (loader.stop || !app.hwnd || !PostMessageW(app.hwnd, WM_STATION_LOADED, 0, (LPARAM)s)) {
            delete s;
            --loader.pending;
        }
    }
}
}

void startBackgroundLoad(const std::vector<int>& which) {
    std::vector<std::wstring> files;
    for (int i : which) if (i >= 0 && i < (int)app.stations.size()) files.push_back(app.stations[i]->file);
    if (files.empty()) return;
    bool start = false;
    {
        std::lock_guard<std::mutex> lock(loader.mu);
        for (auto& f : files) {
            if (std::find(loader.queue.begin(), loader.queue.end(), f) != loader.queue.end()) continue;
            loader.queue.push_back(f);
            ++loader.pending;
        }
        if (!loader.running) { loader.running = true; start = true; }
    }
    if (start) {
        if (loader.worker.joinable()) loader.worker.join();
        loader.stop = false;
        loader.worker = std::thread(loaderMain);
    }
}

void stopBackgroundLoad() {
    loader.stop = true;
    if (loader.worker.joinable()) loader.worker.join();
}

bool loadingStations() { return loader.pending > 0; }

int stationsLoaded() {
    int n = 0;
    for (const auto& s : app.stations) if (s->loaded) ++n;
    return n;
}

void onStationLoaded(LPARAM lp) {
    std::unique_ptr<Station> fresh((Station*)lp);
    --loader.pending;
    for (int i = 0; i < (int)app.stations.size(); ++i) {
        if (app.stations[i]->file != fresh->file) continue;
        fresh->slug = app.stations[i]->slug;
        app.stations[i] = std::move(fresh);
        if (i == app.current) {
            ++app.dataVersion;
            selectStation(i, true);
        }
        break;
    }
    requestRedraw();
}

// Ogni pochi secondi: un CSV cambiato (per esempio aggiornato da un altro
// programma) viene riletto. Si confrontano solo dimensione e data del file.
void checkSourceFiles() {
    if (osmerRunning()) return;
    std::vector<int> changed;
    for (int i = 0; i < (int)app.stations.size(); ++i) {
        const auto& s = *app.stations[i];
        if (!s.loaded) continue;
        if (fileStamp(s.file) != s.stamp) changed.push_back(i);
    }
    if (!changed.empty()) {
        startBackgroundLoad(changed);
        app.status = L"Rilevati " + std::to_wstring(changed.size()) + L" file di stazione cambiati: rilettura in corso.";
        requestRedraw();
    }
}

// ------------------------------------------------------------ aggiornamento da OSMER

namespace {
struct OsmerRun {
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    ULONGLONG started = 0;
    std::wstring log;
    int total = 0, done = 0, errors = 0;
    bool init = false;          // anche storico delle stazioni senza CSV
    std::wstring lastLine;
} osmer;

std::wstring lastUpdateText;
bool lastUpdateRead = false;

std::wstring memoFile() { return joinPath(exeDir(), L"ultimo_aggiornamento_osmer.txt"); }

std::wstring nowText() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[64];
    swprintf_s(buf, L"%02d/%02d/%04d %02d:%02d", t.wDay, t.wMonth, t.wYear, t.wHour, t.wMinute);
    return buf;
}

std::wstring findPython(bool& launcher) {
    wchar_t buf[MAX_PATH];
    launcher = true;
    if (SearchPathW(nullptr, L"py.exe", nullptr, MAX_PATH, buf, nullptr)) return buf;
    launcher = false;
    if (SearchPathW(nullptr, L"python.exe", nullptr, MAX_PATH, buf, nullptr)) return buf;
    return L"";
}

std::string readAll(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::string out;
    char buf[65536];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n) out.append(buf, n);
    CloseHandle(h);
    return out;
}

// Legge l'avanzamento dal registro dello script: "Stazioni selezionate: N"
// e una riga "NN. Nome [slug]: ..." per ogni stazione elaborata.
void parseProgress() {
    std::string text = readAll(osmer.log);
    std::istringstream in(text);
    std::string line;
    int done = 0, errors = 0, total = osmer.total;
    std::string last;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        auto p = line.find("Stazioni selezionate: ");
        if (p != std::string::npos && atoi(line.c_str() + p + 22) > 0) total = atoi(line.c_str() + p + 22);
        // Avanzamento: "01. Nome [slug]: esito". L'elenco iniziale ha "] - script" e non conta.
        if (line.size() > 4 && isdigit((unsigned char)line[0]) && isdigit((unsigned char)line[1]) && line[2] == '.' && line[3] == ' '
            && line.find("]: ") != std::string::npos) {
            ++done;
            if (line.find("ERRORE") != std::string::npos) ++errors;
        }
        last = line;
    }
    osmer.total = total;
    osmer.done = done;
    osmer.errors = errors;
    osmer.lastLine = widen(last);
}
}

bool osmerRunning() { return osmer.process != nullptr; }

// Aggiunge all'elenco i CSV comparsi in dati_stazioni (primo scaricamento o
// stazioni nuove) e li fa leggere in background. Ritorna quante sono.
static int addNewStations() {
    auto found = discoverStations(joinPath(app.root, L"dati_stazioni"));
    std::vector<int> added;
    for (auto& f : found) {
        bool known = false;
        for (const auto& s : app.stations) if (s->file == f->file) { known = true; break; }
        if (known) continue;
        // Un CSV ancora in scrittura non ha le righe: si riprova al giro dopo.
        if (f->name == f->slug && fileStamp(f->file).empty()) continue;
        app.stations.push_back(std::move(f));
        added.push_back((int)app.stations.size() - 1);
    }
    if (!added.empty()) {
        // L'ordine alfabetico resta quello della prima lettura: le nuove vanno in fondo.
        startBackgroundLoad(added);
        if (app.stations.size() == added.size()) { app.current = 0; ++app.dataVersion; }
    }
    return (int)added.size();
}

std::wstring osmerProgress() {
    if (!osmer.process) return L"";
    ULONGLONG s = (GetTickCount64() - osmer.started) / 1000;
    std::wstring t = L"Aggiornamento OSMER in corso: " + std::to_wstring(osmer.done)
        + (osmer.total ? L" / " + std::to_wstring(osmer.total) : std::wstring(L"")) + L" stazioni   ("
        + std::to_wstring(s / 60) + L" min " + std::to_wstring(s % 60) + L" s)";
    if (osmer.errors) t += L"   errori: " + std::to_wstring(osmer.errors);
    return t;
}

std::wstring lastOsmerUpdate() {
    if (!lastUpdateRead) {
        lastUpdateRead = true;
        lastUpdateText = widen(readAll(memoFile()));
        while (!lastUpdateText.empty() && (lastUpdateText.back() == L'\n' || lastUpdateText.back() == L'\r')) lastUpdateText.pop_back();
    }
    return lastUpdateText;
}

void updateAllFromOsmer() {
    if (osmer.process) { app.status = osmerProgress(); return; }
    std::wstring script = joinPath(app.root, L"aggiorna_osmer_unificato.py");
    if (GetFileAttributesW(script.c_str()) == INVALID_FILE_ATTRIBUTES) {
        app.status = L"Non trovo aggiorna_osmer_unificato.py in " + app.root;
        return;
    }
    bool launcher = false;
    std::wstring python = findPython(launcher);
    if (python.empty()) { app.status = L"Python non trovato (py.exe o python.exe)."; return; }
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t until[16];
    swprintf_s(until, L"%04d-%02d-%02d", t.wYear, t.wMonth, t.wDay);
    // Si' = aggiorna e scarica anche lo storico completo delle stazioni che non
    // hanno ancora un CSV (prima installazione o stazioni nuove). No = solo aggiornamento.
    bool init = false;
    if (app.stations.empty()) {
        std::wstring ask = L"La cartella dati_stazioni e' vuota.\n\nScaricare da OSMER lo storico completo di tutte le stazioni fino al "
            + dateText(t.wYear, t.wMonth, t.wDay) + L"?\n\nE' un download lungo (anche piu' di un'ora, con pause tra le richieste "
            L"per non sovraccaricare il sito). La console resta utilizzabile e mostra l'avanzamento.";
        if (MessageBoxW(app.hwnd, ask.c_str(), L"Primo scaricamento da OSMER", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
            app.status = L"Scaricamento annullato.";
            return;
        }
        init = true;
    } else {
        std::wstring ask = L"Aggiornare da OSMER tutte le stazioni (ultimi 10 giorni fino al " + dateText(t.wYear, t.wMonth, t.wDay) + L")?\n\n"
            L"Si: aggiorna e scarica anche lo storico completo delle stazioni che non hanno ancora dati (lento).\n"
            L"No: solo aggiornamento degli ultimi 10 giorni (pochi minuti).\n"
            L"Annulla: non fa nulla.\n\n"
            L"I CSV vengono riscritti in modo atomico; la console resta utilizzabile.";
        int r = MessageBoxW(app.hwnd, ask.c_str(), L"Aggiorna tutte le stazioni da OSMER", MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (r == IDCANCEL) { app.status = L"Aggiornamento OSMER annullato."; return; }
        init = r == IDYES;
    }
    osmer.log = joinPath(exeDir(), L"osmer_aggiornamento.log");
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out = CreateFileW(osmer.log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) { app.status = L"Non riesco a creare il registro " + osmer.log; return; }
    std::wstring cmd = L"\"" + python + L"\"" + (launcher ? L" -3" : L"") + L" -u \"" + script
        + L"\" --action incremental --station all --until " + until + (init ? L"" : L" --skip-init-missing");
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    SetEnvironmentVariableW(L"PYTHONUNBUFFERED", L"1");
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = out;
    si.hStdError = out;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    // Il job chiude anche il processo figlio di py.exe se la console viene chiusa.
    osmer.job = CreateJobObjectW(nullptr, nullptr);
    if (osmer.job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(osmer.job, JobObjectExtendedLimitInformation, &info, sizeof(info));
    }
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                             nullptr, app.root.c_str(), &si, &pi);
    CloseHandle(out);
    if (!ok) {
        if (osmer.job) { CloseHandle(osmer.job); osmer.job = nullptr; }
        app.status = L"Non riesco ad avviare Python per l'aggiornamento OSMER.";
        return;
    }
    if (osmer.job) AssignProcessToJobObject(osmer.job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    osmer.process = pi.hProcess;
    osmer.started = GetTickCount64();
    osmer.init = init;
    osmer.total = (int)app.stations.size();
    osmer.done = osmer.errors = 0;
    SetTimer(app.hwnd, OsmerTimer, 1000, nullptr);
    app.status = L"Aggiornamento OSMER avviato. Registro: console\\osmer_aggiornamento.log";
}

void cancelOsmerUpdate() {
    if (!osmer.process) return;
    TerminateProcess(osmer.process, 1);
    CloseHandle(osmer.process);
    osmer.process = nullptr;
    if (osmer.job) { CloseHandle(osmer.job); osmer.job = nullptr; }
    if (app.hwnd) KillTimer(app.hwnd, OsmerTimer);
}

void pollOsmerUpdate() {
    if (!osmer.process) return;
    parseProgress();
    if (WaitForSingleObject(osmer.process, 0) != WAIT_OBJECT_0) {
        // Nel primo scaricamento le stazioni compaiono man mano che sono pronte.
        static int tick = 0;
        if (osmer.init && ++tick % 15 == 0) addNewStations();
        if (GetTickCount64() - osmer.started > (osmer.init ? 8ull * 60 : 45ull) * 60 * 1000) {
            cancelOsmerUpdate();
            app.status = L"Aggiornamento OSMER interrotto: tempo massimo superato. Vedere console\\osmer_aggiornamento.log";
        }
        requestRedraw();
        return;
    }
    DWORD code = 1;
    GetExitCodeProcess(osmer.process, &code);
    CloseHandle(osmer.process);
    osmer.process = nullptr;
    if (osmer.job) { CloseHandle(osmer.job); osmer.job = nullptr; }
    KillTimer(app.hwnd, OsmerTimer);
    parseProgress();
    // Righe aggiunte: l'ultima riga "Righe aggiunte totali: N".
    std::string text = readAll(osmer.log);
    int added = -1;
    auto p = text.rfind("Righe aggiunte totali: ");
    if (p != std::string::npos) added = atoi(text.c_str() + p + 23);
    std::wstring summary = nowText() + L"  -  " + std::to_wstring(osmer.done) + L" stazioni"
        + (added >= 0 ? L", " + std::to_wstring(added) + L" righe nuove" : L"")
        + (osmer.errors ? L", " + std::to_wstring(osmer.errors) + L" errori" : L"");
    if (code == 0 || osmer.done > 0) {
        std::ofstream m{std::filesystem::path(memoFile())};
        m << narrow(summary) << '\n';
        lastUpdateText = summary;
        lastUpdateRead = true;
    }
    if (code == 0) app.status = L"Aggiornamento OSMER completato: " + summary + L". Rilettura delle stazioni cambiate.";
    else app.status = L"Aggiornamento OSMER terminato con errori (codice " + std::to_wstring(code) + L"): " + osmer.lastLine
        + L"   Dettagli in console\\osmer_aggiornamento.log";
    // Nuove stazioni comparse e file cambiati.
    addNewStations();
    std::vector<int> changed;
    for (int i = 0; i < (int)app.stations.size(); ++i)
        if (!app.stations[i]->loaded || fileStamp(app.stations[i]->file) != app.stations[i]->stamp) changed.push_back(i);
    startBackgroundLoad(changed);
    requestRedraw();
}

std::wstring footerStatus() {
    std::wstring s = std::to_wstring(app.stations.size()) + L" stazioni";
    int loaded = stationsLoaded();
    if (loaded < (int)app.stations.size()) s += L" (caricate " + std::to_wstring(loaded) + L")";
    const auto& cur = st();
    if (cur.loaded && !cur.days.empty())
        s += L"   |   " + cur.name + L": dati fino al " + dateText(cur.lastY, cur.lastM, cur.lastD);
    auto last = lastOsmerUpdate();
    s += L"   |   ultimo aggiornamento OSMER: " + (last.empty() ? std::wstring(L"mai dalla console") : last);
    return s;
}
