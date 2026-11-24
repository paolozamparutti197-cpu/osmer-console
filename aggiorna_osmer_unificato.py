#!/usr/bin/env python3
"""
Pannello unico per gestire i dati OSMER del progetto.

Questo file non sostituisce fisicamente gli script esistenti: li lascia al loro
posto e li coordina. In questo modo puoi provarlo senza perdere nulla.

Esempi:
  python aggiorna_osmer_unificato.py
  python aggiorna_osmer_unificato.py --list
  python aggiorna_osmer_unificato.py --action incremental --station codroipo
  python aggiorna_osmer_unificato.py --action incremental --station all
  python aggiorna_osmer_unificato.py --action excel --station lignano --skip-existing-xlsx
  python aggiorna_osmer_unificato.py --action full --station codroipo
  python aggiorna_osmer_unificato.py --action reports
  python aggiorna_osmer_unificato.py --action all --station all
"""

from __future__ import annotations

import argparse
import contextlib
import io
import queue
import shutil
import subprocess
import sys
import threading
import time
import unicodedata
from dataclasses import dataclass
from datetime import date, datetime, timedelta
from pathlib import Path
from tkinter import BOTH, END, LEFT, RIGHT, VERTICAL, Button, Checkbutton, Entry, Frame
from tkinter import IntVar, Label, Listbox, Scrollbar, StringVar, Tk, Text, Toplevel
from tkinter import messagebox, ttk


ROOT = Path(__file__).resolve().parent
SUPPORT_DIR = ROOT / "_codici_supporto"
DATA_DIR = ROOT / "dati_stazioni"
REPORT_DIR = ROOT / "report_html"
if SUPPORT_DIR.exists():
    sys.path.insert(0, str(SUPPORT_DIR))

import aggiorna_osmer_mancanti as missing
import crea_excel_osmer_generico as excel

REPORT_SCRIPTS = [
    SUPPORT_DIR / "genera_top5_temperature_maggio.py",
    SUPPORT_DIR / "genera_html_maggio_caldo.py",
]


@dataclass(frozen=True)
class RunResult:
    label: str
    returncode: int


class QueueWriter(io.TextIOBase):
    def __init__(self, messages: queue.Queue[str]) -> None:
        self.messages = messages

    def write(self, value: str) -> int:
        if value:
            self.messages.put(value)
        return len(value)

    def flush(self) -> None:
        return None


def normalize(value: str) -> str:
    text = unicodedata.normalize("NFKD", value)
    text = "".join(ch for ch in text if not unicodedata.combining(ch))
    text = "".join(ch.lower() if ch.isalnum() else " " for ch in text)
    return " ".join(text.split())


def discover_stations() -> list[missing.Station]:
    stations = missing.discover_stations(ROOT)
    return sorted(stations, key=lambda station: (station.name.casefold(), station.slug))


def station_choices_text(stations: list[missing.Station]) -> str:
    lines = []
    width = len(str(len(stations)))
    for index, station in enumerate(stations, 1):
        rel_script = station.script_path.relative_to(ROOT)
        lines.append(
            f"{index:>{width}}. {station.name} [{station.slug}] - {rel_script}"
        )
    return "\n".join(lines)


def print_station_list(stations: list[missing.Station]) -> None:
    print(f"Stazioni trovate: {len(stations)}")
    print(station_choices_text(stations))


def split_station_queries(raw_values: list[str] | None) -> list[str]:
    if not raw_values:
        return ["all"]
    queries: list[str] = []
    for raw in raw_values:
        queries.extend(part.strip() for part in raw.split(",") if part.strip())
    return queries or ["all"]


def resolve_stations(
    stations: list[missing.Station],
    raw_values: list[str] | None,
) -> list[missing.Station]:
    queries = split_station_queries(raw_values)
    if any(query.casefold() in {"all", "tutte", "*"} for query in queries):
        return stations

    selected: list[missing.Station] = []
    selected_slugs: set[str] = set()
    by_number = {str(index): station for index, station in enumerate(stations, 1)}

    searchable: list[tuple[missing.Station, str]] = []
    for station in stations:
        rel_script = station.script_path.relative_to(ROOT).as_posix()
        haystack = " ".join(
            [
                station.slug,
                station.name,
                station.value.split("@", 1)[0],
                rel_script,
            ]
        )
        searchable.append((station, normalize(haystack)))

    for query in queries:
        if query in by_number:
            matches = [by_number[query]]
        else:
            wanted = normalize(query)
            exact = [
                station
                for station, _haystack in searchable
                if wanted
                in {
                    normalize(station.slug),
                    normalize(station.name),
                    normalize(station.value.split("@", 1)[0]),
                }
            ]
            matches = exact or [
                station for station, haystack in searchable if wanted and wanted in haystack
            ]

        if not matches:
            raise ValueError(f"Nessuna stazione trovata per: {query}")
        if len(matches) > 1:
            names = ", ".join(f"{station.name} [{station.slug}]" for station in matches[:10])
            raise ValueError(f"Stazione ambigua '{query}'. Corrispondenze: {names}")

        station = matches[0]
        if station.slug not in selected_slugs:
            selected.append(station)
            selected_slugs.add(station.slug)

    return selected


def ask(text: str, default: str = "") -> str:
    suffix = f" [{default}]" if default else ""
    value = input(f"{text}{suffix}: ").strip()
    return value or default


def ask_yes_no(text: str, default: bool = False) -> bool:
    marker = "S/n" if default else "s/N"
    while True:
        value = input(f"{text} ({marker}): ").strip().casefold()
        if not value:
            return default
        if value in {"s", "si", "y", "yes"}:
            return True
        if value in {"n", "no"}:
            return False
        print("Rispondi con s oppure n.")


def choose_action() -> str:
    actions = [
        ("incremental", "Aggiorna gli ultimi 10 giorni"),
        ("full", "Riallineamento completo e rigenerazione Excel"),
        ("excel", "Rigenera i file Excel riepilogativi"),
        ("cleanup", "Rimuovi backup e staging obsoleti"),
        ("reports", "Rigenera i report HTML"),
        ("all", "Aggiorna dati, Excel e report"),
        ("list", "Mostra elenco stazioni"),
        ("quit", "Esci"),
    ]
    print("")
    print("Cosa vuoi fare?")
    for index, (_action, label) in enumerate(actions, 1):
        print(f"{index}. {label}")
    while True:
        raw = ask("Scelta", "1")
        if raw.isdigit() and 1 <= int(raw) <= len(actions):
            return actions[int(raw) - 1][0]
        for action, _label in actions:
            if raw.casefold() == action:
                return action
        print("Scelta non valida.")


def interactive_args(stations: list[missing.Station]) -> argparse.Namespace:
    action = choose_action()
    if action == "quit":
        raise SystemExit(0)
    if action == "list":
        return argparse.Namespace(
            action="list",
            station=["all"],
            until=date.today().isoformat(),
            sleep=1.0,
            dry_run=False,
            skip_init_missing=False,
            skip_existing_xlsx=False,
            yes=False,
            refresh_days=missing.DEFAULT_REFRESH_DAYS,
        )

    print("")
    print("Puoi indicare 'all', un numero, uno slug o parte del nome.")
    print("Esempi: all, 12, codroipo, lignano")
    station_query = ask("Stazione", "all")
    until = ask("Aggiorna fino alla data YYYY-MM-DD", date.today().isoformat())
    sleep = float(ask("Pausa fra stazioni/richieste in secondi", "1"))
    dry_run = False
    skip_init_missing = False
    skip_existing_xlsx = False

    if action in {"incremental", "all"}:
        dry_run = ask_yes_no("Vuoi fare solo una prova senza scrivere file?", False)
        skip_init_missing = ask_yes_no(
            "Saltare le stazioni senza CSV combinato gia' presente?", False
        )
    if action in {"excel", "full", "all"}:
        skip_existing_xlsx = ask_yes_no(
            "Saltare gli Excel gia' aggiornati rispetto al CSV?", False
        )

    selected = resolve_stations(stations, [station_query])
    if action == "full" and len(selected) > 1:
        print("")
        print(
            "Attenzione: il download completo di molte stazioni puo' richiedere molto tempo "
            "e riscrive i CSV dentro le cartelle di output."
        )
        if not ask_yes_no("Continuare?", False):
            raise SystemExit(0)

    return argparse.Namespace(
        action=action,
        station=[station_query],
        until=until,
        sleep=sleep,
        dry_run=dry_run,
        skip_init_missing=skip_init_missing,
        skip_existing_xlsx=skip_existing_xlsx,
        yes=True,
        refresh_days=missing.DEFAULT_REFRESH_DAYS,
    )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Pannello unico per aggiornare dati, Excel e report OSMER.",
    )
    parser.add_argument(
        "--action",
        choices=["incremental", "full", "excel", "cleanup", "reports", "all"],
        help="Azione da eseguire. Se omessa si apre il menu interattivo.",
    )
    parser.add_argument(
        "--station",
        action="append",
        help="Stazione: all, numero elenco, slug, codice o parte del nome. Ripetibile.",
    )
    parser.add_argument(
        "--until",
        default=date.today().isoformat(),
        help="Ultimo giorno da includere nell'aggiornamento.",
    )
    parser.add_argument(
        "--refresh-days",
        type=int,
        default=missing.DEFAULT_REFRESH_DAYS,
        help="Giorni finali da riscaricare e sostituire (default: 10).",
    )
    parser.add_argument(
        "--sleep",
        type=float,
        default=1.0,
        help="Pausa in secondi tra stazioni/richieste.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Per incremental: mostra cosa farebbe senza scrivere file.",
    )
    parser.add_argument(
        "--skip-init-missing",
        action="store_true",
        help="Per incremental: non inizializza stazioni prive di CSV combinato.",
    )
    parser.add_argument(
        "--skip-existing-xlsx",
        action="store_true",
        help="Per excel: salta XLSX gia' piu' recenti del CSV.",
    )
    parser.add_argument(
        "--yes",
        action="store_true",
        help="Non chiedere conferma per operazioni lunghe da CLI.",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="Mostra le stazioni disponibili ed esce.",
    )
    parser.add_argument(
        "--gui",
        action="store_true",
        help="Apre la finestra grafica.",
    )
    parser.add_argument(
        "--no-gui",
        action="store_true",
        help="Usa il menu testuale se non viene indicata un'azione.",
    )
    args = parser.parse_args(argv)
    if args.list:
        args.action = "list"
    return args


def update_incremental(
    stations: list[missing.Station],
    until_text: str,
    sleep_seconds: float,
    dry_run: bool,
    skip_init_missing: bool,
    refresh_days: int = missing.DEFAULT_REFRESH_DAYS,
) -> int:
    until = date.fromisoformat(until_text)
    extraction_time = datetime.now().astimezone().isoformat(timespec="seconds")
    station_sleep_seconds = max(0.0, sleep_seconds)
    init_request_sleep_seconds = max(3.0, sleep_seconds)
    init_missing = not skip_init_missing

    print("")
    if refresh_days < 1:
        raise ValueError("--refresh-days deve essere almeno 1.")
    window_start = until - timedelta(days=refresh_days - 1)
    print(
        f"Aggiornamento idempotente: finestra {window_start.isoformat()} - "
        f"{until.isoformat()} ({refresh_days} giorni)"
    )
    print(f"Stazioni selezionate: {len(stations)}")
    if dry_run:
        print("Modalita' prova: nessun file verra' modificato.")

    session = missing.make_session()
    soup = missing.load_archive_page(session)
    page_years = missing.read_page_years(soup)
    default_day = missing.read_default_day(soup)
    fallback_year = page_years[0] if page_years else until.year
    station_start_years = missing.read_station_start_years(soup, fallback_year)

    total_added = 0
    errors = 0
    for index, station in enumerate(stations, 1):
        try:
            existed_before = station.combined_path.exists()
            added, latest = missing.append_missing(
                session,
                station,
                until,
                extraction_time,
                page_years,
                station_start_years,
                default_day,
                dry_run,
                init_missing,
                init_request_sleep_seconds,
                refresh_days,
            )
            total_added += added
            if not existed_before and not init_missing:
                status = "CSV mancante, inizializzazione saltata"
            elif not existed_before and dry_run:
                status = "da inizializzare"
            elif not existed_before:
                status = "inizializzata" if added else "CSV mancante, nessun dato"
            else:
                status = "gia' aggiornata" if added == 0 else f"+{added} righe"
            latest_text = latest.isoformat() if latest else "n/d"
            print(f"{index:02d}. {station.name} [{station.slug}]: {status}, ultimo={latest_text}")
        except Exception as exc:
            errors += 1
            print(f"{index:02d}. {station.name} [{station.slug}]: ERRORE {exc}")

        if index < len(stations):
            time.sleep(station_sleep_seconds)

    print(f"Righe aggiunte totali: {total_added}")
    return 1 if errors else 0


def run_subprocess(label: str, command: list[str], cwd: Path) -> RunResult:
    print("")
    print(f"== {label} ==")
    print("Comando:", " ".join(str(part) for part in command))
    completed = subprocess.run(command, cwd=str(cwd), text=True)
    if completed.returncode == 0:
        print(f"{label}: completato.")
    else:
        print(f"{label}: terminato con codice {completed.returncode}.")
    return RunResult(label=label, returncode=completed.returncode)


def validate_staged_station(
    station: missing.Station,
    staged: missing.Station,
) -> tuple[int, date, date]:
    header, rows = missing.read_csv(staged.combined_path)
    required = {"anno_richiesto", "stazione_nome", "stazione_valore", "data_estrazione"}
    if not required.issubset(header):
        raise RuntimeError("CSV combinato di staging privo delle colonne di provenienza.")
    if not rows:
        raise RuntimeError("CSV combinato di staging vuoto.")
    year_index = header.index("anno_richiesto")
    dates = [missing.row_date(row, year_index) for row in rows]
    if len(dates) != len(set(dates)):
        raise RuntimeError("CSV combinato di staging con date duplicate.")
    if dates != sorted(dates):
        raise RuntimeError("CSV combinato di staging non ordinato per data.")

    annual_rows = 0
    for annual_path in staged.combined_path.parent.glob(
        f"osmer_{station.slug}_[0-9][0-9][0-9][0-9]_giornalieri.csv"
    ):
        _annual_header, annual = missing.read_csv(annual_path)
        annual_rows += len(annual)
    if annual_rows != len(rows):
        raise RuntimeError(
            f"Totale righe annuali ({annual_rows}) diverso dal combinato ({len(rows)})."
        )
    return len(rows), dates[0], dates[-1]


def publish_staged_csv(station: missing.Station, staging_dir: Path) -> int:
    destination = station.combined_path.parent
    if missing.is_transient_data_path(destination):
        raise RuntimeError(f"Destinazione non canonica: {destination}")
    staged_files = sorted(staging_dir.glob(f"osmer_{station.slug}_*_giornalieri.csv"))
    combined = staging_dir / f"osmer_{station.slug}_giornalieri_tutti_gli_anni.csv"
    if combined.exists():
        staged_files.append(combined)
    if not staged_files:
        raise RuntimeError("Nessun CSV trovato nello staging.")

    destination.mkdir(parents=True, exist_ok=True)
    for staged_path in staged_files:
        staged_path.replace(destination / staged_path.name)
    return len(staged_files)


def cleanup_transient_data() -> int:
    """Remove only obsolete backup and failed-staging directories under data."""
    data_root = DATA_DIR.resolve()
    targets = sorted(
        (
            path
            for path in data_root.rglob("*")
            if path.is_dir()
            and (path.name == "_backup_riallineamento" or path.name.startswith(".riallineamento_"))
        ),
        key=lambda path: len(path.parts),
        reverse=True,
    )
    removed_dirs = 0
    removed_files = 0
    removed_bytes = 0
    for path in targets:
        if not path.exists():
            continue
        resolved = path.resolve()
        if data_root not in resolved.parents:
            raise RuntimeError(f"Pulizia fuori dalla cartella dati rifiutata: {resolved}")
        files = [item for item in resolved.rglob("*") if item.is_file()]
        removed_files += len(files)
        removed_bytes += sum(item.stat().st_size for item in files)
        shutil.rmtree(resolved)
        removed_dirs += 1
    print(
        f"Cartelle transitorie rimosse: {removed_dirs}; "
        f"file rimossi: {removed_files}; byte liberati: {removed_bytes}."
    )
    return 0


def download_full(
    stations: list[missing.Station],
    until_text: str,
    sleep_seconds: float,
    yes: bool,
) -> int:
    until = date.fromisoformat(until_text)
    print("")
    print(f"Riallineamento completo sicuro per {len(stations)} stazioni.")
    print("Download in staging, verifica e pubblicazione sicura dei CSV canonici.")
    if len(stations) > 1 and not yes:
        if not ask_yes_no("Continuare?", False):
            return 0

    session = missing.make_session()
    soup = missing.load_archive_page(session)
    page_years = missing.read_page_years(soup)
    default_day = missing.read_default_day(soup)
    fallback_year = page_years[0] if page_years else until.year
    station_start_years = missing.read_station_start_years(soup, fallback_year)
    extraction_time = datetime.now().astimezone().isoformat(timespec="seconds")

    errors = 0
    for index, station in enumerate(stations, 1):
        staging_dir = station.combined_path.parent / (
            f".riallineamento_{station.slug}_{datetime.now().strftime('%Y%m%d_%H%M%S_%f')}"
        )
        staged = missing.Station(
            station.name,
            station.value,
            station.slug,
            station.script_path,
            staging_dir / station.combined_path.name,
        )
        try:
            added, latest = missing.initialize_station(
                session,
                staged,
                until,
                extraction_time,
                page_years,
                station_start_years,
                default_day,
                False,
                max(3.0, sleep_seconds),
            )
            count, first, last = validate_staged_station(station, staged)
            published_files = publish_staged_csv(station, staging_dir)
            print(
                f"{index:02d}. {station.name} [{station.slug}]: pubblicata, "
                f"righe={count}, periodo={first.isoformat()}..{last.isoformat()}, "
                f"CSV pubblicati={published_files}"
            )
            if added != count or latest != last:
                print(f"ATTENZIONE {station.slug}: riepilogo download non coerente.")
        except Exception as exc:
            errors += 1
            print(
                f"{index:02d}. {station.name} [{station.slug}]: ERRORE {exc}; "
                "dati locali invariati."
            )
        finally:
            shutil.rmtree(staging_dir, ignore_errors=True)
    return 1 if errors else 0


def create_excels(stations: list[missing.Station], skip_existing: bool) -> int:
    print("")
    print(f"Creazione/aggiornamento Excel per {len(stations)} stazioni.")
    errors = 0
    created = 0
    skipped = 0

    for index, station in enumerate(stations, 1):
        csv_path = station.combined_path
        if missing.is_transient_data_path(csv_path):
            errors += 1
            print(f"{index:02d}. {station.name} [{station.slug}]: ERRORE CSV non canonico: {csv_path}")
            continue
        if not csv_path.exists():
            xlsx_path = excel.output_path_for(csv_path, None)
            try:
                excel.create_unavailable_workbook(
                    station.name,
                    csv_path,
                    xlsx_path,
                    "OSMER non ha restituito osservazioni giornaliere valide.",
                )
                created += 1
                print(
                    f"{index:02d}. {station.name} [{station.slug}]: "
                    f"creato, sorgente non disponibile, file={xlsx_path}"
                )
            except Exception as exc:
                errors += 1
                print(f"{index:02d}. {station.name} [{station.slug}]: ERRORE {exc}")
            continue

        xlsx_path = excel.output_path_for(csv_path, None)
        try:
            existed_and_current = (
                skip_existing
                and xlsx_path.exists()
                and xlsx_path.stat().st_mtime >= csv_path.stat().st_mtime
            )
            title, rows_count, numeric_count = excel.create_workbook_from_csv(
                csv_path,
                xlsx_path,
                skip_existing,
            )
            if existed_and_current:
                skipped += 1
                status = "saltato, XLSX gia' aggiornato"
            else:
                created += 1
                status = "creato/aggiornato"
            print(
                f"{index:02d}. {title}: {status}, righe={rows_count}, "
                f"variabili={numeric_count}, file={xlsx_path}"
            )
        except Exception as exc:
            errors += 1
            print(f"{index:02d}. {station.name} [{station.slug}]: ERRORE {exc}")

    print("")
    print(f"Excel creati/aggiornati: {created}")
    print(f"Excel saltati: {skipped}")
    print(f"Errori Excel: {errors}")
    return 1 if errors else 0


def create_reports() -> int:
    print("")
    print("Rigenerazione report HTML.")
    errors = 0
    for script_path in REPORT_SCRIPTS:
        if not script_path.exists():
            print(f"Report saltato, script mancante: {script_path}")
            continue
        result = run_subprocess(script_path.name, [sys.executable, str(script_path)], ROOT)
        if result.returncode != 0:
            errors += 1
    return 1 if errors else 0


def run_action(args: argparse.Namespace, stations: list[missing.Station]) -> int:
    if args.action == "list":
        print_station_list(stations)
        return 0

    if args.action == "reports":
        return create_reports()
    if args.action == "cleanup":
        return cleanup_transient_data()

    selected = resolve_stations(stations, args.station)
    print("")
    print("Stazioni selezionate:")
    print(station_choices_text(selected))

    returncodes: list[int] = []
    if args.action in {"incremental", "all"}:
        returncodes.append(
            update_incremental(
                selected,
                args.until,
                args.sleep,
                args.dry_run,
                args.skip_init_missing,
                args.refresh_days,
            )
        )

    if args.action == "full":
        returncodes.append(download_full(selected, args.until, args.sleep, args.yes))
        returncodes.append(create_excels(selected, args.skip_existing_xlsx))

    if args.action in {"excel", "all"}:
        returncodes.append(create_excels(selected, args.skip_existing_xlsx))

    if args.action == "all":
        returncodes.append(create_reports())

    return 1 if any(code != 0 for code in returncodes) else 0


class OsmerGui:
    def __init__(self, root: Tk, stations: list[missing.Station]) -> None:
        self.root = root
        self.stations = stations
        self.filtered = stations[:]
        self.messages: queue.Queue[str] = queue.Queue()
        self.worker: threading.Thread | None = None

        self.search_var = StringVar()
        self.until_var = StringVar(value=date.today().isoformat())
        self.sleep_var = StringVar(value="1")
        self.dry_run_var = IntVar(value=0)
        self.skip_init_var = IntVar(value=0)
        self.skip_xlsx_var = IntVar(value=0)

        self.root.title("OSMER - pannello aggiornamento")
        self.root.geometry("980x680")
        self.root.minsize(820, 560)
        self.build()
        self.refresh_station_list()
        self.root.after(100, self.drain_messages)

    def build(self) -> None:
        main = Frame(self.root, padx=12, pady=12)
        main.pack(fill=BOTH, expand=True)

        top = Frame(main)
        top.pack(fill="x")
        Label(top, text="Cerca stazione").pack(side=LEFT)
        search = Entry(top, textvariable=self.search_var)
        search.pack(side=LEFT, fill="x", expand=True, padx=(8, 8))
        search.bind("<KeyRelease>", lambda _event: self.refresh_station_list())
        Button(top, text="Tutte", command=self.select_all).pack(side=LEFT, padx=(0, 6))
        Button(top, text="Nessuna", command=self.clear_selection).pack(side=LEFT)

        content = Frame(main)
        content.pack(fill=BOTH, expand=True, pady=(10, 10))

        left = Frame(content)
        left.pack(side=LEFT, fill=BOTH, expand=True)
        Label(left, text="Stazioni").pack(anchor="w")
        list_frame = Frame(left)
        list_frame.pack(fill=BOTH, expand=True)
        scrollbar = Scrollbar(list_frame, orient=VERTICAL)
        self.station_list = Listbox(
            list_frame,
            selectmode="extended",
            exportselection=False,
            yscrollcommand=scrollbar.set,
        )
        scrollbar.config(command=self.station_list.yview)
        self.station_list.pack(side=LEFT, fill=BOTH, expand=True)
        scrollbar.pack(side=RIGHT, fill="y")

        right = Frame(content, padx=12)
        right.pack(side=RIGHT, fill="y")
        Label(right, text="Opzioni").pack(anchor="w")

        Label(right, text="Aggiorna fino a").pack(anchor="w", pady=(10, 0))
        Entry(right, textvariable=self.until_var, width=18).pack(anchor="w")

        Label(right, text="Pausa secondi").pack(anchor="w", pady=(10, 0))
        Entry(right, textvariable=self.sleep_var, width=18).pack(anchor="w")

        Checkbutton(
            right,
            text="Prova senza scrivere file",
            variable=self.dry_run_var,
        ).pack(anchor="w", pady=(12, 0))
        Checkbutton(
            right,
            text="Non inizializzare CSV mancanti",
            variable=self.skip_init_var,
        ).pack(anchor="w")
        Checkbutton(
            right,
            text="Salta Excel gia' aggiornati",
            variable=self.skip_xlsx_var,
        ).pack(anchor="w")

        ttk.Separator(right).pack(fill="x", pady=14)
        Button(
            right,
            text="Aggiorna dati mancanti",
            width=28,
            command=lambda: self.start_action("incremental"),
        ).pack(anchor="w", pady=3)
        Button(
            right,
            text="Rigenera Excel",
            width=28,
            command=lambda: self.start_action("excel"),
        ).pack(anchor="w", pady=3)
        Button(
            right,
            text="Aggiorna dati + Excel + report",
            width=28,
            command=lambda: self.start_action("all"),
        ).pack(anchor="w", pady=3)
        Button(
            right,
            text="Rigenera report HTML",
            width=28,
            command=lambda: self.start_action("reports"),
        ).pack(anchor="w", pady=3)
        Button(
            right,
            text="Apri grafici stazione",
            width=28,
            command=self.open_graph_tool,
        ).pack(anchor="w", pady=(12, 3))
        Button(
            right,
            text="Download completo",
            width=28,
            command=lambda: self.start_action("full"),
        ).pack(anchor="w", pady=(16, 3))

        Label(main, text="Log").pack(anchor="w")
        log_frame = Frame(main)
        log_frame.pack(fill=BOTH, expand=False)
        log_scroll = Scrollbar(log_frame, orient=VERTICAL)
        self.log = Text(log_frame, height=12, wrap="word", yscrollcommand=log_scroll.set)
        log_scroll.config(command=self.log.yview)
        self.log.pack(side=LEFT, fill=BOTH, expand=True)
        log_scroll.pack(side=RIGHT, fill="y")

    def refresh_station_list(self) -> None:
        query = normalize(self.search_var.get())
        selected_slugs = {
            self.filtered[index].slug for index in self.station_list.curselection()
        }
        self.filtered = [
            station
            for station in self.stations
            if not query
            or query
            in normalize(
                f"{station.name} {station.slug} {station.value.split('@', 1)[0]}"
            )
        ]
        self.station_list.delete(0, END)
        width = len(str(len(self.stations)))
        for index, station in enumerate(self.filtered, 1):
            self.station_list.insert(
                END,
                f"{index:>{width}}. {station.name} [{station.slug}]",
            )
            if station.slug in selected_slugs:
                self.station_list.selection_set(END)

    def selected_stations(self) -> list[missing.Station]:
        selected = [self.filtered[index] for index in self.station_list.curselection()]
        return selected or self.filtered

    def select_all(self) -> None:
        self.station_list.selection_set(0, END)

    def clear_selection(self) -> None:
        self.station_list.selection_clear(0, END)

    def log_text(self, value: str) -> None:
        self.log.insert(END, value)
        self.log.see(END)

    def drain_messages(self) -> None:
        while True:
            try:
                value = self.messages.get_nowait()
            except queue.Empty:
                break
            self.log_text(value)
        self.root.after(100, self.drain_messages)

    def validate_common(self) -> tuple[str, float] | None:
        try:
            date.fromisoformat(self.until_var.get().strip())
        except ValueError:
            messagebox.showerror("Data non valida", "Usa il formato YYYY-MM-DD.")
            return None
        try:
            sleep_seconds = float(self.sleep_var.get().strip().replace(",", "."))
        except ValueError:
            messagebox.showerror("Pausa non valida", "Inserisci un numero di secondi.")
            return None
        return self.until_var.get().strip(), sleep_seconds

    def start_action(self, action: str) -> None:
        if self.worker and self.worker.is_alive():
            messagebox.showinfo("Operazione in corso", "Aspetta che finisca l'operazione attuale.")
            return

        common = self.validate_common()
        if common is None:
            return
        until_text, sleep_seconds = common
        selected = self.selected_stations()
        if not selected and action != "reports":
            messagebox.showwarning("Nessuna stazione", "Seleziona almeno una stazione.")
            return

        if action == "full":
            ok = messagebox.askyesno(
                "Download completo",
                "Il download completo puo' richiedere molto tempo e riscrive i CSV "
                "nelle cartelle di output. Continuare?",
            )
            if not ok:
                return

        self.log_text("\n")
        self.log_text(f"Avvio: {action}\n")
        self.log_text(f"Stazioni: {len(selected)}\n")
        self.worker = threading.Thread(
            target=self.run_worker,
            args=(action, selected, until_text, sleep_seconds),
            daemon=True,
        )
        self.worker.start()

    def run_worker(
        self,
        action: str,
        selected: list[missing.Station],
        until_text: str,
        sleep_seconds: float,
    ) -> None:
        args = argparse.Namespace(
            action=action,
            station=[station.slug for station in selected] if selected else ["all"],
            until=until_text,
            sleep=sleep_seconds,
            dry_run=bool(self.dry_run_var.get()),
            skip_init_missing=bool(self.skip_init_var.get()),
            skip_existing_xlsx=bool(self.skip_xlsx_var.get()),
            yes=True,
            refresh_days=missing.DEFAULT_REFRESH_DAYS,
        )
        writer = QueueWriter(self.messages)
        with contextlib.redirect_stdout(writer), contextlib.redirect_stderr(writer):
            code = run_action(args, self.stations)
            print("")
            if code == 0:
                print("Operazione completata.")
            else:
                print(f"Operazione terminata con errori. Codice: {code}")

    def open_graph_tool(self) -> None:
        script_path = ROOT / "grafici_clima_stazione.py"
        if not script_path.exists():
            messagebox.showerror("File mancante", f"Non trovo {script_path}")
            return
        subprocess.Popen([sys.executable, str(script_path)], cwd=str(ROOT))


def run_gui(stations: list[missing.Station]) -> int:
    root = Tk()
    OsmerGui(root, stations)
    root.mainloop()
    return 0


def main(argv: list[str] | None = None) -> int:
    argv = argv if argv is not None else sys.argv[1:]
    stations = discover_stations()
    if not stations:
        print("Nessuna stazione trovata dagli script scarica_osmer_*_tutti_gli_anni.py.")
        return 2

    args = parse_args(argv)
    if args.gui or (not argv and not args.no_gui):
        return run_gui(stations)
    if args.action is None:
        args = interactive_args(stations)

    try:
        return run_action(args, stations)
    except KeyboardInterrupt:
        print("\nOperazione interrotta dall'utente.")
        return 130
    except Exception as exc:
        print(f"ERRORE: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
