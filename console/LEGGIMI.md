# Console OSMER - tutte le stazioni

Versione multi-stazione della Console Udine dell'autore.
Avvio: `console\osmer_console.exe`.

## Dati

- Legge direttamente i CSV `osmer_<stazione>_giornalieri_tutti_gli_anni.csv` in `..\dati_stazioni`
  (tutte le sottocartelle; esclusi backup e staging `_backup_riallineamento`, `.riallineamento_*`).
  Nessun Excel, nessun Python per leggere i dati.
- Accetta sia il punto sia la virgola decimale (Talmassons), `-` = senza misura.
- Valori fuori dai limiti fisici sono scartati (per esempio l'umidita' oltre 100% delle colonne
  spostate negli anni 1991-1994 di alcune stazioni).
- La stazione scelta si legge subito, le altre in background. Ogni 3 secondi si controlla
  dimensione e data dei CSV: un file cambiato viene riletto da solo.

## Climatologia

Ogni stazione usa SOLO i propri dati (niente HISTALP o serie dal 1803 come la Console Udine).
Riferimento scelto con il pulsante "Clima" (tasto C):
- tutti i dati della stazione (predefinito),
- periodo a schermo (INIZIO-FINE),
- 1991-2020, limitato agli anni che la stazione ha davvero.

Un mese entra nelle medie se e' completo: temperatura con al massimo 5 giorni mancanti,
pioggia con al massimo 2 (una pioggia mancante non e' uno zero). Servono almeno 3 anni per mese.
Il mese in corso e' "parziale": si vede, ma non entra nelle medie.

SPEI-3/6/12: calibrato sui soli dati della stazione, PET di Thornthwaite alla latitudine della
stazione, distribuzione logistica generalizzata con L-momenti (come il pacchetto R SPEI).
Servono almeno 10 anni di mesi completi.

## Viste (menu a sinistra, tasti tra parentesi)

0 Panoramica stazioni: tutte le stazioni, ultimo dato, completezza, scarto T e pioggia %
  dell'anno/stagione scelti rispetto alla climatologia di ciascuna. Clic su una riga = apre la stazione.
1 Climatologia: medie mensili (Tmedia, Tmin, Tmax, pioggia, giorni piovosi, umidita', vento,
  radiazione, pressione), record di Tmax, Tmin e pioggia giornaliera, climogramma.
2 Dettaglio anno, 3 Anno e fascia P5-P95 (qualsiasi grandezza, tasto V), 4 Pioggia cumulata
  (con media degli anni completi), 5 Temperatura, 6 Precipitazioni, 7 SPEI, 8 Bilancio P-PET,
  9 Classifiche, H Ondate di caldo, T Tmin e Tmax (6 indici), P Indici di pioggia (6 indici).

PagSu / PagGiu cambiano stazione; "STAZIONE" apre l'elenco completo.

## Aggiorna tutte le stazioni da OSMER

Un solo pulsante (tasto U) per tutte le stazioni. Lancia in background
`py -3 aggiorna_osmer_unificato.py --action incremental --station all --until <oggi>`
(ultimi 10 giorni, riscrittura atomica dei CSV). Alla domanda: "No" aggiunge
`--skip-init-missing` (solo aggiornamento, pochi minuti); "Si" scarica anche lo storico
delle stazioni che non hanno ancora un CSV. Con `dati_stazioni` vuota parte il primo
scaricamento completo (oltre un'ora) e le stazioni compaiono man mano. Avanzamento in basso, registro in
`console\osmer_aggiornamento.log`, esito in `console\ultimo_aggiornamento_osmer.txt`.
Al termine le stazioni cambiate vengono rilette.

## Esportazione e prova

"Scarica PNG e CSV" salva in `..\grafici_console` PNG, PNG 2x, EMF e CSV.
Prova senza finestra: `osmer_console.exe --export [--station=slug]` salva tutte le viste.

## Compilazione

`build.bat` (Visual Studio 2022 Community), esito in `build_log.txt`.
