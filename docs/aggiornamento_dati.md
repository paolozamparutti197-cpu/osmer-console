# Aggiornamento dati OSMER

I dati persistiti sono CSV UTF-8 separati da `;`: un file per anno e un file
combinato `osmer_<stazione>_giornalieri_tutti_gli_anni.csv`. Nel combinato la
chiave naturale di una riga è `(anno_richiesto, mese, giorno)`.

## Aggiornamento ordinario

Il comando ordinario riscarica sempre gli ultimi 10 giorni fino alla data
indicata. Le righe remote sostituiscono le righe locali con la stessa data e le
date nuove vengono aggiunte. La riscrittura è atomica e non crea duplicati.

```powershell
python aggiorna_osmer_unificato.py --action incremental --station all --until 2026-07-30
```

Per verificare senza modificare file:

```powershell
python aggiorna_osmer_unificato.py --action incremental --station all --until 2026-07-30 --dry-run
```

La finestra predefinita è 10 giorni. Solo per esigenze diagnostiche può essere
variata con `--refresh-days N`.

## Riallineamento completo

Il comando `full` scarica in staging, verifica e poi pubblica esclusivamente
nei CSV canonici della stazione. Al termine rigenera anche il relativo Excel.
Per ogni stazione:

1. scarica l'intero storico disponibile in una cartella `.riallineamento_*`;
2. verifica colonne, ordinamento, unicità delle date e coerenza tra CSV annuali
   e combinato;
3. pubblica i nuovi CSV solo dopo il superamento dei controlli;
4. rimuove lo staging temporaneo, anche in caso di errore.

```powershell
python aggiorna_osmer_unificato.py --action full --station all --until 2026-07-30 --yes
```

Se una stazione fallisce, i suoi dati locali restano invariati. Backup e staging
non sono mai selezionati come sorgenti dati né come destinazioni degli Excel.

## Rimozione di residui storici

Per eliminare i backup creati dalle versioni precedenti e gli staging falliti:

```powershell
python aggiorna_osmer_unificato.py --action cleanup
```

Per provare prima su una singola stazione:

```powershell
python aggiorna_osmer_unificato.py --action full --station codroipo --until 2026-07-30
```
