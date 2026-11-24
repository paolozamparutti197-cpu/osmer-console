// Esportazione: PNG, PNG 2x, EMF e CSV del grafico corrente; copia negli appunti.
#include "osmer_console.h"

int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (!size) return -1;
    std::vector<BYTE> mem(size);
    auto* info = (ImageCodecInfo*)mem.data();
    GetImageEncoders(num, size, info);
    for (UINT i = 0; i < num; ++i) if (wcscmp(info[i].MimeType, format) == 0) { *pClsid = info[i].Clsid; return (int)i; }
    return -1;
}

void openOutDir() {
    std::error_code ec;
    std::filesystem::create_directories(app.outDir, ec);
    ShellExecuteW(app.hwnd, L"explore", app.outDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    app.status = L"Cartella dei grafici aperta: " + app.outDir;
}

static void renderContent(Graphics& g, float scale) {
    auto hits = app.hits;
    auto probes = app.probes;
    auto monthHits = app.monthHits;
    g.Clear(Color(255, 8, 18, 32));
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    if (scale != 1.f) g.ScaleTransform(scale, scale);
    paintContent(g, RectF(24, 24, 1552, 952));
    g.ResetTransform();
    app.hits.swap(hits);
    app.probes.swap(probes);
    app.monthHits.swap(monthHits);
}

bool copyChartToClipboard() {
    Bitmap bmp(1600, 1000, PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        renderContent(g, 1.f);
    }
    HBITMAP hbm = nullptr;
    if (bmp.GetHBITMAP(Color(255, 8, 18, 32), &hbm) != Ok || !hbm) {
        app.status = L"Non riesco a preparare l'immagine per gli appunti.";
        return false;
    }
    BITMAP info{};
    GetObjectW(hbm, sizeof(info), &info);
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih);
    bih.biWidth = info.bmWidth;
    bih.biHeight = info.bmHeight;
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    DWORD bytes = (DWORD)info.bmWidth * 4 * (DWORD)info.bmHeight;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(bih) + bytes);
    bool ok = false;
    if (mem) {
        auto* dst = (BYTE*)GlobalLock(mem);
        memcpy(dst, &bih, sizeof(bih));
        HDC dc = GetDC(nullptr);
        BITMAPINFO bi{};
        bi.bmiHeader = bih;
        ok = GetDIBits(dc, hbm, 0, (UINT)info.bmHeight, dst + sizeof(bih), &bi, DIB_RGB_COLORS) > 0;
        ReleaseDC(nullptr, dc);
        GlobalUnlock(mem);
        if (ok && OpenClipboard(app.hwnd)) {
            EmptyClipboard();
            ok = SetClipboardData(CF_DIB, mem) != nullptr;
            CloseClipboard();
            if (ok) mem = nullptr;
        } else ok = false;
        if (mem) GlobalFree(mem);
    }
    DeleteObject(hbm);
    app.status = ok ? L"Grafico copiato negli appunti: Ctrl+V per incollarlo." : L"Non riesco a copiare il grafico negli appunti.";
    return ok;
}

static std::string num(double v, int prec = 4) {
    if (!finite(v)) return "";
    std::ostringstream o;
    o << std::fixed << std::setprecision(prec) << v;
    return o.str();
}
static std::string date(int y, int m, int d) {
    std::ostringstream o;
    o << y << '-' << std::setw(2) << std::setfill('0') << m << '-' << std::setw(2) << d;
    return o.str();
}

bool saveFiles() {
    const auto& s = st();
    if (app.view != View::Overview && !s.loaded) { app.status = L"Dati della stazione non ancora caricati."; return false; }
    std::error_code ec;
    std::filesystem::create_directories(app.outDir, ec);
    const wchar_t* names[] = {L"panoramica", L"climatologia", L"anno", L"fascia_p5_p95", L"pioggia_cumulata", L"temperatura_scarto",
                              L"precipitazione_scarto", L"spei_6_12", L"bilancio_scarto", L"classifiche", L"ondate",
                              L"tmin_tmax", L"indici_pioggia", L"tabella"};
    std::wstring stem = (app.view == View::Overview ? std::wstring(L"tutte_le_stazioni") : s.slug) + L"_" + names[(int)app.view];
    if (app.view == View::Overview || app.view == View::Year || app.view == View::Band || app.view == View::Rain) stem += L"_" + std::to_wstring(app.focusYear);
    else if (app.view == View::Table) {
        ensureTablePeriod();
        auto sts = tableStations();
        int y, m, d;
        auto ymd = [&](long n) { civilFromDays(n, y, m, d); return std::to_wstring(y * 10000 + m * 100 + d); };
        stem = (sts.size() > 1 ? std::to_wstring(sts.size()) + L"_stazioni_" + varShort(app.tableVar) : app.stations[sts.empty() ? 0 : sts[0]]->slug)
            + L"_tabella_" + ymd(app.tableFrom) + L"_" + ymd(app.tableTo);
        for (auto& c : stem) if (c == L' ' || c == L'.') c = L'_';
    }
    else if (app.view != View::Clima && app.view != View::Heat) stem += L"_" + std::to_wstring(app.y0) + L"_" + std::to_wstring(app.y1);
    if (app.view == View::Band) {
        static const wchar_t* vs[] = {L"pioggia", L"tmin", L"tmedia", L"tmax", L"umin", L"umed", L"umax", L"vento", L"raffica", L"dir", L"radiazione", L"pressione"};
        stem += std::wstring(L"_") + vs[app.bandVar];
    }
    if (app.view == View::Heat)
        stem += app.heatMode == HeatMode::Five ? L"_5gg" : app.heatMode == HeatMode::Ten ? L"_10gg"
              : app.heatMode == HeatMode::Streak ? L"_tmax_oltre_" + std::to_wstring(app.heatThreshold)
              : app.heatMode == HeatMode::Cold5 ? L"_freddo_5gg" : app.heatMode == HeatMode::Cold10 ? L"_freddo_10gg"
              : L"_tmin_sotto_" + std::to_wstring(app.coldThreshold);
    if (app.compareYear && (app.view == View::Band || app.view == View::Rain)) stem += L"_vs_" + std::to_wstring(app.compareYear);

    CLSID png;
    bool havePng = GetEncoderClsid(L"image/png", &png) >= 0;
    {
        Bitmap bmp(1600, 1000, PixelFormat32bppARGB);
        Graphics g(&bmp);
        renderContent(g, 1.f);
        if (havePng) bmp.Save(joinPath(app.outDir, stem + L".png").c_str(), &png, nullptr);
    }
    {
        Bitmap bmp(3200, 2000, PixelFormat32bppARGB);
        Graphics g(&bmp);
        renderContent(g, 2.f);
        if (havePng) bmp.Save(joinPath(app.outDir, stem + L"_2x.png").c_str(), &png, nullptr);
    }
    {
        HDC ref = GetDC(nullptr);
        Metafile mf(joinPath(app.outDir, stem + L".emf").c_str(), ref, RectF(0, 0, 1600, 1000),
                    MetafileFrameUnitPixel, EmfTypeEmfPlusDual, L"Console OSMER");
        {
            Graphics g(&mf);
            renderContent(g, 1.f);
        }
        ReleaseDC(nullptr, ref);
    }

    std::ofstream csv{std::filesystem::path(joinPath(app.outDir, stem + L".csv")), std::ios::binary};
    csv << "\xEF\xBB\xBF";   // UTF-8 con BOM: Excel e Calc riconoscono gli accenti
    csv << "# " << narrow(app.view == View::Overview ? L"Tutte le stazioni" : s.name) << " - " << narrow(seasonLabel())
        << " - climatologia: " << narrow(refLabel()) << "\n";
    if (app.view == View::Overview) {
        csv << "stazione;quota_m;primo_anno;ultimo_dato;giorni_T;giorni_P;giorni_attesi;scarto_T_C;scarto_P_mm;pioggia_perc;mesi_T\n";
        for (const auto& r : overviewRows()) {
            const auto& x = *app.stations[r.index];
            csv << narrow(x.name) << ';' << num(x.alt, 0) << ';' << x.firstYear << ';'
                << (x.days.empty() ? std::string() : date(x.lastY, x.lastM, x.lastD)) << ';' << x.measuredT << ';' << x.measuredP << ';'
                << x.expectedDays << ';' << num(r.tAn) << ';' << num(r.pAn) << ';' << num(r.pPct, 1) << ';' << r.nMonths << '\n';
        }
    } else if (app.view == View::Clima) {
        csv << "mese;anni;tmedia;tmin;tmax;pioggia_mm;giorni_pioggia_1mm;umidita_media;vento_medio;radiazione;pressione;"
               "tmax_record;anno_tmax;tmin_record;anno_tmin;pioggia_max_24h;anno_pioggia_max\n";
        const auto& rows = climaTable();
        for (int k = 1; k <= 13; ++k) {
            const auto& r = rows[k <= 12 ? k : 0];
            csv << (k <= 12 ? std::to_string(k) : "anno") << ';' << r.years << ';' << num(r.t) << ';' << num(r.tn) << ';' << num(r.tx) << ';'
                << num(r.p) << ';' << num(r.wet) << ';' << num(r.um) << ';' << num(r.vm) << ';' << num(r.rad) << ';' << num(r.pres) << ';'
                << num(r.recTx, 1) << ';' << r.recTxY << ';' << num(r.recTn, 1) << ';' << r.recTnY << ';' << num(r.recP, 1) << ';' << r.recPY << '\n';
        }
    } else if (app.view == View::Year) {
        double cT[13], cP[13]; int nT[13], nP[13];
        monthClimate(s, cT, cP, nT, nP);
        csv << "anno;mese;tmedia;clima_t;tmin;tmax;pioggia_mm;clima_p;giorni_t;giorni_p;parziale;spei3;spei6;spei12\n";
        for (int m = 1; m <= 12; ++m) {
            const Month* mo = findMonth(s, app.focusYear, m);
            if (!mo) continue;
            csv << app.focusYear << ';' << m << ';' << num(mo->nt ? mo->t : NaN) << ';' << num(cT[m]) << ';' << num(mo->tn) << ';' << num(mo->tx) << ';'
                << num(mo->np ? mo->p : NaN) << ';' << num(cP[m]) << ';' << mo->nt << ';' << mo->np << ';' << (mo->partial ? 1 : 0) << ';'
                << num(mo->spei3) << ';' << num(mo->spei6) << ';' << num(mo->spei12) << '\n';
        }
    } else if (app.view == View::Spei) {
        csv << "anno;mese;pioggia_mm;tmedia;pet_mm;SPEI3;SPEI6;SPEI12\n";
        for (const auto& mo : s.months) {
            if (mo.y < app.y0 || mo.y > app.y1) continue;
            csv << mo.y << ';' << mo.m << ';' << num(mo.okP ? mo.p : NaN) << ';' << num(mo.okT ? mo.t : NaN) << ';' << num(mo.pet) << ';'
                << num(mo.spei3) << ';' << num(mo.spei6) << ';' << num(mo.spei12) << '\n';
        }
    } else if (app.view == View::Band) {
        const auto& cmp = bandCompare();
        csv << "data;" << narrow(varName(app.bandVar)) << ";p5;p95;media_storica";
        if (!cmp.empty()) csv << ";valore_" << app.compareYear;
        csv << "\n";
        size_t row = 0;
        for (auto& p : bandSeries(nullptr, nullptr)) {
            csv << date(app.focusYear, p.month, p.day) << ';' << num(p.ht ? p.t : NaN) << ';' << num(p.p5) << ';' << num(p.p95) << ';' << num(p.mean);
            if (!cmp.empty()) csv << ';' << num(cmp[row]);
            ++row;
            csv << '\n';
        }
    } else if (app.view == View::Rain) {
        const auto& cmp = rainCompare();
        csv << "giorno;cumulata_mm;min_mm;media_mm;max_mm";
        if (!cmp.empty()) csv << ";cumulata_" << app.compareYear << "_mm";
        csv << "\n";
        size_t row = 0;
        for (auto& p : rainSeries()) {
            int m = app.month, d = p.mark;
            if (app.season != Season::Month) ymdFromIndex(p.mark, m, d);
            csv << date(app.focusYear, m, d) << ';' << num(p.ok ? p.v : NaN) << ';' << num(p.loOk ? p.lo : NaN) << ';' << num(p.mean) << ';'
                << num(p.hiOk ? p.hi : NaN);
            if (!cmp.empty()) csv << ';' << num(cmp[row]);
            ++row;
            csv << '\n';
        }
    } else if (app.view == View::Temp || app.view == View::Prec || app.view == View::Balance) {
        auto pts = app.view == View::Temp ? anomalySeries(false) : app.view == View::Prec ? anomalySeries(true) : balanceSeries();
        csv << "anno;scarto;mesi\n";
        for (auto& p : pts) csv << p.year << ';' << num(p.value) << ';' << p.n << '\n';
    } else if (app.view == View::Rank) {
        csv << "anno;scarto_T_C;scarto_P_mm;spei12_minimo\n";
        auto t = anomalySeries(false), p = anomalySeries(true), sp = yearMinSpei12();
        for (int y = app.y0; y <= app.y1; ++y) {
            auto find = [&](const std::vector<Sample>& v) { for (auto& x : v) if (x.year == y) return x.value; return NaN; };
            csv << y << ';' << num(find(t)) << ';' << num(find(p)) << ';' << num(find(sp)) << '\n';
        }
    } else if (app.view == View::Heat) {
        csv << "posizione;inizio;fine;giorni;tmedia_media;tmax_media;tmax_max;tmin_media;tmin_min\n";
        int rank = 0;
        for (const auto& w : heatWaves())
            csv << ++rank << ';' << date(w.y0, w.m0, w.d0) << ';' << date(w.y1, w.m1, w.d1) << ';' << w.days << ';'
                << num(w.meanT) << ';' << num(w.meanTx) << ';' << num(w.maxTx) << ';' << num(w.meanTn) << ';' << num(w.minTn) << '\n';
    } else if (app.view == View::Thermal) {
        csv << "anno;giorni_attesi;giorni_con_tmin_tmax;notti_tropicali;giorni_gelo;giorni_ghiaccio;giorni_estivi;giorni_tmax_oltre_30;escursione_media;incompleto\n";
        for (const auto& t : thermalYears())
            csv << t.year << ';' << t.expected << ';' << t.measured << ';' << t.tropical << ';' << t.frost << ';' << t.ice << ';' << t.summer << ';'
                << t.hot30 << ';' << num(t.range) << ';' << (t.partial ? 1 : 0) << '\n';
    } else if (app.view == View::Table) {
        // Tutte le righe del periodo, non solo quelle a schermo.
        auto cols = tableColumns();
        bool multi = tableStations().size() > 1;
        csv << "data";
        for (const auto& c : cols) {
            const auto& x = *app.stations[c.station];
            csv << ';' << narrow(multi ? x.name + L" " + varShort(c.var) : std::wstring(varShort(c.var))) << " (" << narrow(varUnit(c.var)) << ')';
        }
        csv << '\n';
        std::vector<double> sum(cols.size(), 0), mn(cols.size(), NaN), mx(cols.size(), NaN);
        std::vector<int> cnt(cols.size(), 0);
        for (long n = app.tableFrom; n <= app.tableTo; ++n) {
            int y, m, d;
            civilFromDays(n, y, m, d);
            csv << date(y, m, d);
            for (size_t i = 0; i < cols.size(); ++i) {
                double v = tableValue(*app.stations[cols[i].station], n, cols[i].var);
                csv << ';' << num(v, varDigits(cols[i].var));
                if (finite(v)) {
                    sum[i] += v; ++cnt[i];
                    if (!finite(mn[i]) || v < mn[i]) mn[i] = v;
                    if (!finite(mx[i]) || v > mx[i]) mx[i] = v;
                }
            }
            csv << '\n';
        }
        const char* labels[] = {"media", "minimo", "massimo", "giorni_con_dato", "totale"};
        for (int r = 0; r < 5; ++r) {
            csv << labels[r];
            for (size_t i = 0; i < cols.size(); ++i) {
                csv << ';';
                if (r == 0 && cnt[i]) csv << num(sum[i] / cnt[i], 2);
                if (r == 1) csv << num(mn[i], 1);
                if (r == 2) csv << num(mx[i], 1);
                if (r == 3) csv << cnt[i];
                if (r == 4 && cols[i].var == VP && cnt[i]) csv << num(sum[i], 1);
            }
            csv << '\n';
        }
    } else if (app.view == View::RainIdx) {
        csv << "anno;giorni_attesi;giorni_misurati;pioggia_mm;giorni_1mm;giorni_20mm;massimo_giornaliero;periodo_secco_max;incompleto\n";
        for (const auto& r : rainYears())
            csv << r.year << ';' << r.expected << ';' << r.measured << ';' << num(r.total) << ';' << r.wet << ';' << r.heavy << ';'
                << num(r.maxDay) << ';' << r.dryRun << ';' << (r.partial ? 1 : 0) << '\n';
    }
    app.status = L"Salvato in " + app.outDir + L"\\" + stem + L"  (PNG, PNG 2x, EMF vettoriale e CSV)";
    return true;
}
