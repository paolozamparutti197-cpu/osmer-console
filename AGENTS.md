# Istruzioni per assistenti AI (Claude, Codex, Copilot, ...)

Questo file spiega a un agente AI come far funzionare bene la Console OSMER e come
modificarla senza romperla. Leggerlo per intero prima di agire.

## In breve

- Programma **Windows nativo in C++17** (Win32 + GDI+), sorgenti in `console/`.
- Legge i CSV OSMER in `dati_stazioni/` e calcola tutto in memoria: nessun database,
  nessun Excel, nessun Python per leggere i dati.
- Python serve solo per **scaricare/aggiornare** i dati dal sito OSMER
  (`aggiorna_osmer_unificato.py` + `_codici_supporto/`).
- L'interfaccia, i messaggi, i commenti e i nomi dei file sono in **italiano**: mantenerli
  in italiano.

## Far partire il progetto (checklist)

1. Windows 10/11. Verificare Python: `py -3 --version` (>= 3.10).
2. `py -3 -m pip install -r requirements.txt` (requests, beautifulsoup4, pandas, openpyxl).
3. Eseguibile: usare quello della Release oppure compilare con `console\build.bat`.
   - `build.bat` trova Visual Studio con `vswhere`; serve il componente
     "MSVC x64/x86 build tools" (carico di lavoro "Sviluppo desktop C++").
   - L'esito e' in `console\build_log.txt`: cercare `ESITO: OK` ed eventuali `error`/`warning`.
   - Il messaggio `"vswhere.exe" non e' riconosciuto` stampato da `vcvars64.bat` e' innocuo.
   - Se lanci `build.bat` da Git Bash usa `cmd //c build.bat` oppure PowerShell
     `cmd /c console\build.bat`.
4. Dati: se `dati_stazioni/` e' vuota, la console si apre e chiede di scaricare lo storico
   ("AGGIORNA TUTTE DA OSMER"). Da riga di comando:
   `py -3 aggiorna_osmer_unificato.py --action incremental --station all`
   (senza `--skip-init-missing` inizializza le stazioni mancanti: oltre un'ora).
   Per una prova veloce su una stazione:
   `py -3 aggiorna_osmer_unificato.py --action incremental --station udine_s_o`
   e `--dry-run` per non scrivere nulla.
5. Verifica senza finestra: `console\osmer_console.exe --export --station=udine_s_o`
   salva tutte le viste (PNG, PNG 2x, EMF, CSV) in `grafici_console/`. Exit code 0 = ok.
   Guardare i PNG per controllare il risultato: e' il modo piu' rapido di verificare
   una modifica grafica.

## Struttura del codice (console/)

| File | Contenuto |
|---|---|
| `osmer_console.h` | tipi (`Day`, `Month`, `Station`, `App`), enum `View`, dichiarazioni |
| `dati.cpp` | scoperta stazioni, lettura CSV, mesi, climatologia, PET, SPEI, serie dei grafici, cache |
| `grafica.cpp` | disegno di finestra e viste (una funzione `paintXxx` per vista) |
| `esportazione.cpp` | PNG/EMF/CSV e appunti |
| `aggiornamento.cpp` | caricamento in background (thread), controllo file, processo Python OSMER |
| `osmer_console.cpp` | finestra, mouse, tastiera, DPI, `wWinMain`, opzioni `--export` e `--station=` |

## Formato dei dati

- Un CSV per stazione: `dati_stazioni/**/osmer_<slug>_giornalieri_tutti_gli_anni.csv`
  (ricerca ricorsiva; esclusi `_backup_riallineamento` e `.riallineamento_*`).
- Separatore `;`. Colonne in ordine fisso: `mese;giorno*;pioggia;Tmin;Tmed;Tmax;Umin;Umed;Umax;
  Vento med;Vento max;Dir;Radiazione;Pressione;anno_richiesto;stazione_nome;stazione_valore;data_estrazione`.
  Le colonne si leggono **per posizione**, non per nome (le intestazioni variano).
- `-` = dato mancante. Alcune stazioni usano la **virgola decimale** (Talmassons): gestita.
- `stazione_valore` = `CODICE@Nome@syn@lat@lon@quota`.
- Valori fuori dai limiti fisici diventano mancanti (`plausible()` in `dati.cpp`): negli anni
  1991-1994 alcune stazioni hanno colonne di umidita' spostate.
- Il file contiene righe vuote fino a fine anno: l'ultimo dato e' l'ultimo giorno con misure.

## Regole di metodo da NON cambiare senza chiederlo all'utente

- La climatologia di ogni stazione usa **solo i dati di quella stazione**: niente serie
  esterne, niente omogeneizzazioni tra stazioni.
- Mese completo: temperatura con al massimo 5 giorni mancanti, pioggia con al massimo 2
  (`computeMonths`). Una pioggia mancante non e' mai zero. Servono almeno 3 anni per una
  media mensile (`MinClimYears`). Il mese in corso e' "parziale" e non entra nelle medie.
- Scarti annuali: anni con almeno il 75% dei mesi della stagione completi; l'ultimo anno
  della stazione entra comunque (scarto "da inizio anno").
- SPEI: PET di Thornthwaite alla latitudine della stazione; distribuzione logistica
  generalizzata con L-momenti per mese di calendario (come il pacchetto R `SPEI`);
  almeno 10 valori per mese. Verifica: media ~0 e deviazione ~1 sulla serie.
- Fascia P5-P95 e media lisciate su 5 giorni nella vista annuale.

## Trappole note

- In `grafica.cpp` **non chiamare una variabile `small`**: e' una macro di `rpcndr.h`
  (`#define small char`) e produce errori incomprensibili.
- `#define NOMINMAX` sta in `osmer_console.h` prima di `windows.h`: lasciarlo.
- Il thread di caricamento crea oggetti `Station` propri e li consegna con `PostMessage`
  (`WM_STATION_LOADED`): non accedere ai dati in uso da altri thread.
- Le serie giornaliere sono in cache, con chiave `app.dataVersion` + stagione/anno/grandezza:
  dopo aver cambiato dati incrementare `app.dataVersion`.
- Lo stato che cambia l'aspetto della finestra va aggiunto a `sceneSignature()`, altrimenti
  la scena non si ridisegna.
- Gli id dei clic: 24-32 controlli della Tabella dati, 100+ viste, 300+ stagioni, 320+ riferimento,
  400-407 ondate (400-402 caldo, 405-407 freddo, 403/404 soglia), 410+ righe ondate, 450+ ordinamento
  panoramica, 600+ grandezze fascia, 1000+ righe panoramica, 2000+ elenco stazioni,
  3000+ grandezza tabella, 4000+ stazioni della tabella (scelta multipla, il pannello resta aperto).
- `ultima_vista.txt` (sessione): riga 1 numeri della vista, riga 2 stazione, riga 3 periodo della
  tabella (numeri di giorno), grandezza e soglia del freddo, riga 4 stazioni della tabella separate da `|`.
  Le righe 3-4 sono facoltative: i file vecchi devono continuare a caricarsi.
- `aggiorna_osmer_unificato.py` scopre le stazioni dagli script `scarica_osmer_*_tutti_gli_anni.py`
  (costanti `STATION_NAME`, `STATION_VALUE`, `STATION_SLUG`). Per aggiungere una stazione si
  aggiunge uno script copiando uno esistente e cambiando le tre costanti.
- Non mettere copie del progetto dentro la cartella del progetto: lo script cerca gli script
  delle stazioni in modo ricorsivo e troverebbe stazioni doppie.
- La console legge l'avanzamento dal registro dello script: righe `NN. Nome [slug]: esito`,
  `Stazioni selezionate: N`, `Righe aggiunte totali: N`. Se si cambia il formato delle
  stampe dello script, aggiornare `parseProgress()` in `aggiornamento.cpp`.

## Buone pratiche

- Dopo ogni modifica: `build.bat` senza errori ne' avvisi, poi `--export` su almeno una stazione
  completa (`udine_s_o`) e una con buchi o senza temperatura (`chievolis`,
  `boa_paloma_interruzione_dati_dal_06_02_2018_al_29_06_2022`), e guardare i PNG.
- Non committare dati (`dati_stazioni/`), esportazioni, log, `ultima_vista.txt`, eseguibili:
  sono nel `.gitignore`. L'eseguibile si distribuisce nelle Release.
- Non lanciare aggiornamenti completi o scaricamenti ripetuti senza necessita': il sito
  OSMER e' pubblico e va usato con moderazione.
