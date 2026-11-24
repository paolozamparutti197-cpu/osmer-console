#!/usr/bin/env python3
"""
Aggiorna in modo incrementale i CSV giornalieri OSMER.

Legge le stazioni dagli script `scarica_osmer_*_tutti_gli_anni.py`, riscarica
una finestra recente dall'endpoint OSMER e aggiorna i CSV per data, senza
duplicati. Se una stazione non ha ancora un CSV combinato, puo' inizializzarlo
scaricando gli anni disponibili.
"""

from __future__ import annotations

import argparse
import ast
import csv
import os
import tempfile
import time
from dataclasses import dataclass
from datetime import date, datetime, timedelta
from pathlib import Path
from urllib.parse import urlencode

import requests
from bs4 import BeautifulSoup


PAGE_URL = "https://www.osmer.fvg.it/archivio.php?ln=&p=dati"
AJAX_URL = "https://www.osmer.fvg.it/ajax/getStationData.php"
DEFAULT_REFRESH_DAYS = 10
USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
)


@dataclass(frozen=True)
class Station:
    name: str
    value: str
    slug: str
    script_path: Path
    combined_path: Path


def is_transient_data_path(path: Path) -> bool:
    """True for data that must never be selected as a station's source."""
    return any(
        part == "_backup_riallineamento" or part.startswith(".riallineamento_")
        for part in path.parts
    )


def canonical_combined_by_name(root: Path) -> dict[str, Path]:
    """Return the sole non-transient combined CSV for every filename.

    A backup has the same filename as its canonical CSV. Treating a recursive
    scan as a ``name -> path`` dictionary therefore made the last scanned
    backup become the active destination. Ambiguous non-transient files are an
    explicit error instead of an arbitrary choice.
    """
    candidates: dict[str, list[Path]] = {}
    for path in root.rglob("*_giornalieri_tutti_gli_anni.csv"):
        if "anni_test" in path.name or is_transient_data_path(path):
            continue
        candidates.setdefault(path.name, []).append(path.resolve())

    ambiguous = {
        name: paths
        for name, paths in candidates.items()
        if len(paths) > 1
    }
    if ambiguous:
        details = "; ".join(
            f"{name}: {', '.join(str(path) for path in paths)}"
            for name, paths in sorted(ambiguous.items())
        )
        raise RuntimeError(f"CSV canonici ambigui: {details}")
    return {name: paths[0] for name, paths in candidates.items()}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Aggiorna i dati OSMER mancanti.")
    parser.add_argument(
        "--until",
        default=date.today().isoformat(),
        help="Ultimo giorno da includere, in formato YYYY-MM-DD.",
    )
    parser.add_argument(
        "--sleep",
        type=float,
        default=1.0,
        help="Pausa in secondi tra una stazione e la successiva.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Mostra cosa verrebbe aggiornato senza scrivere file.",
    )
    parser.add_argument(
        "--skip-init-missing",
        action="store_true",
        help="Non inizializza le stazioni prive di CSV combinato.",
    )
    parser.add_argument(
        "--refresh-days",
        type=int,
        default=DEFAULT_REFRESH_DAYS,
        help="Numero di giorni finali da riscaricare e sostituire (default: 10).",
    )
    return parser.parse_args()


def ajax_headers() -> dict[str, str]:
    return {
        "accept": "application/json, text/javascript, */*; q=0.01",
        "content-type": "application/x-www-form-urlencoded; charset=UTF-8",
        "origin": "https://www.osmer.fvg.it",
        "referer": PAGE_URL,
        "connection": "close",
        "user-agent": USER_AGENT,
        "x-requested-with": "XMLHttpRequest",
    }


def make_session() -> requests.Session:
    session = requests.Session()
    session.headers.update({"User-Agent": USER_AGENT})
    return session


def load_archive_page(session: requests.Session) -> BeautifulSoup:
    response = session.get(PAGE_URL, timeout=30)
    response.raise_for_status()
    session.cookies.set("meteofvg_cookie", "1", domain="www.osmer.fvg.it", path="/")
    return BeautifulSoup(response.text, "html.parser")


def read_page_years(soup: BeautifulSoup) -> list[int]:
    years: list[int] = []
    for option in soup.select("#anno option"):
        value = option.get("value", "").strip()
        if value.isdigit():
            years.append(int(value))
    return sorted(set(years))


def read_default_day(soup: BeautifulSoup) -> str:
    selected = soup.select_one("#giorno option[selected]")
    if selected and selected.get("value"):
        return selected["value"]
    first = soup.select_one("#giorno option")
    if first and first.get("value"):
        return first["value"]
    return "1"


def read_station_start_years(soup: BeautifulSoup, fallback_year: int) -> dict[str, int]:
    start_years: dict[str, int] = {}
    for option in soup.find_all("option"):
        value = option.get("value", "").strip()
        if "@syn@" not in value:
            continue
        raw_start = option.get("data-start", "").strip()
        start_years[value] = int(raw_start) if raw_start.isdigit() else fallback_year
    return start_years


def clean_cell(cell) -> str:
    return cell.get_text(" ", strip=True)


def extract_first_valid_table(html: str) -> tuple[list[str], list[list[str]]]:
    soup = BeautifulSoup(html, "html.parser")
    for table in soup.find_all("table"):
        table_rows = table.find_all("tr")
        if not table_rows:
            continue
        headers = [clean_cell(cell) for cell in table_rows[0].find_all(["th", "td"])]
        if not headers:
            continue
        expected_cols = len(headers)
        rows: list[list[str]] = []
        for row in table_rows[1:]:
            values = [clean_cell(cell) for cell in row.find_all(["th", "td"])]
            if len(values) != expected_cols or not any(values):
                continue
            try:
                month = int(values[0])
                day = int(values[1])
            except (IndexError, ValueError):
                # OSMER talvolta restituisce una tabella HTML di errore con
                # una riga apparentemente valida (ad esempio ``found\"}``).
                continue
            if 1 <= month <= 12 and 1 <= day <= 31:
                rows.append(values)
        if rows:
            return headers, rows
    return [], []


def source_constants(script_path: Path) -> dict[str, str]:
    tree = ast.parse(script_path.read_text(encoding="utf-8"))
    constants: dict[str, str] = {}
    wanted = {"STATION_NAME", "STATION_VALUE", "STATION_SLUG"}
    for node in tree.body:
        if not isinstance(node, ast.Assign):
            continue
        if len(node.targets) != 1 or not isinstance(node.targets[0], ast.Name):
            continue
        key = node.targets[0].id
        if key in wanted and isinstance(node.value, ast.Constant):
            constants[key] = str(node.value.value)
    return constants


def discover_stations(root: Path) -> list[Station]:
    combined_by_name = canonical_combined_by_name(root)
    stations: list[Station] = []
    for script_path in sorted(root.rglob("scarica_osmer*_tutti_gli_anni.py")):
        constants = source_constants(script_path)
        if not {"STATION_NAME", "STATION_VALUE", "STATION_SLUG"} <= constants.keys():
            continue
        slug = constants["STATION_SLUG"]
        combined_name = f"osmer_{slug}_giornalieri_tutti_gli_anni.csv"
        data_dir = root / "dati_stazioni"
        fallback_dir = data_dir if data_dir.exists() else script_path.parent
        combined_path = combined_by_name.get(
            combined_name,
            fallback_dir / f"output_{slug}_tutti_gli_anni" / combined_name,
        )
        stations.append(
            Station(
                name=constants["STATION_NAME"],
                value=constants["STATION_VALUE"],
                slug=slug,
                script_path=script_path,
                combined_path=combined_path,
            )
        )
    return stations


def detect_delimiter(path: Path) -> str:
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        first_line = handle.readline()
    return ";" if first_line.count(";") >= first_line.count(",") else ","


def read_csv(path: Path) -> tuple[list[str], list[list[str]]]:
    delimiter = detect_delimiter(path)
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.reader(handle, delimiter=delimiter)
        header = next(reader)
        return header, list(reader)


def write_csv(path: Path, header: list[str], rows: list[list[str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.",
        suffix=".tmp",
        dir=path.parent,
    )
    temporary_path = Path(temporary_name)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="") as handle:
            writer = csv.writer(handle, delimiter=";", lineterminator="\n")
            writer.writerow(header)
            writer.writerows(rows)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary_path, path)
    except BaseException:
        temporary_path.unlink(missing_ok=True)
        raise


def row_date(row: list[str], year_index: int | None) -> date:
    year = int(row[year_index]) if year_index is not None else 2026
    return date(year, int(row[0]), int(row[1]))


def month_starts(start: date, end: date) -> list[date]:
    months: list[date] = []
    cursor = date(start.year, start.month, 1)
    while cursor <= end:
        months.append(cursor)
        if cursor.month == 12:
            cursor = date(cursor.year + 1, 1, 1)
        else:
            cursor = date(cursor.year, cursor.month + 1, 1)
    return months


def fetch_month(
    session: requests.Session,
    station: Station,
    year: int,
    month: int,
) -> tuple[list[str], list[list[str]]]:
    payload = {
        "a": str(year),
        "m": str(month),
        "g": "1",
        "s": station.value,
        "t": "H_3",
        "ln": "",
        "o": "visualizza",
    }
    response = session.post(
        AJAX_URL,
        headers=ajax_headers(),
        data=urlencode(payload),
        timeout=90,
    )
    response.raise_for_status()
    html = response.json()
    if not isinstance(html, str):
        raise RuntimeError("Risposta OSMER inattesa: JSON non stringa.")
    return extract_first_valid_table(html)


def fetch_year(
    session: requests.Session,
    station: Station,
    year: int,
    default_day: str,
) -> tuple[list[str], list[list[str]]]:
    payload = {
        "a": str(year),
        "m": "99",
        "g": default_day,
        "s": station.value,
        "t": "H_3",
        "ln": "",
        "o": "visualizza",
    }
    response = session.post(
        AJAX_URL,
        headers=ajax_headers(),
        data=urlencode(payload),
        timeout=90,
    )
    response.raise_for_status()
    html = response.json()
    if not isinstance(html, str):
        raise RuntimeError("Risposta OSMER inattesa: JSON non stringa.")
    return extract_first_valid_table(html)


def has_decimal_comma_style(header: list[str], rows: list[list[str]]) -> bool:
    if any("Temp," in col or "Dir," in col or "Radiaz," in col for col in header):
        return True
    sample = rows[-20:] if len(rows) > 20 else rows
    return any("," in value for row in sample for value in row[2:14])


def convert_row_style(row: list[str], decimal_comma: bool) -> list[str]:
    if not decimal_comma:
        return row
    return [value.replace(".", ",") for value in row]


def initialize_station(
    session: requests.Session,
    station: Station,
    until: date,
    extraction_time: str,
    page_years: list[int],
    station_start_years: dict[str, int],
    default_day: str,
    dry_run: bool,
    sleep_seconds: float,
) -> tuple[int, date | None]:
    if not page_years:
        raise RuntimeError("Nessun anno trovato nel selettore OSMER.")

    start_year = station_start_years.get(station.value, page_years[0])
    years = [year for year in page_years if start_year <= year <= until.year]
    if not years:
        return 0, None

    combined_header: list[str] | None = None
    combined_rows: list[list[str]] = []
    latest: date | None = None
    added = 0

    for index, year in enumerate(years):
        headers, rows = fetch_year(session, station, year, default_day)
        if not headers or not rows:
            if index < len(years) - 1:
                time.sleep(sleep_seconds)
            continue

        rows_for_year: list[list[str]] = []
        for row in rows:
            current = date(year, int(row[0]), int(row[1]))
            if current > until:
                continue
            rows_for_year.append(row)
            combined_rows.append(
                row + [str(year), station.name, station.value, extraction_time]
            )
            latest = current if latest is None or current > latest else latest
            added += 1

        if rows_for_year and not dry_run:
            annual_path = station.combined_path.parent / f"osmer_{station.slug}_{year}_giornalieri.csv"
            write_csv(annual_path, headers, rows_for_year)

        if combined_header is None:
            combined_header = headers[:]
        elif headers != combined_header:
            print(f"ATTENZIONE {station.slug}: intestazioni OSMER diverse tra gli anni.")

        if index < len(years) - 1:
            time.sleep(sleep_seconds)

    if combined_header and combined_rows and not dry_run:
        write_csv(
            station.combined_path,
            combined_header
            + ["anno_richiesto", "stazione_nome", "stazione_valore", "data_estrazione"],
            combined_rows,
        )

    return added, latest


def append_missing(
    session: requests.Session,
    station: Station,
    until: date,
    extraction_time: str,
    page_years: list[int],
    station_start_years: dict[str, int],
    default_day: str,
    dry_run: bool,
    init_missing: bool,
    sleep_seconds: float,
    refresh_days: int = DEFAULT_REFRESH_DAYS,
) -> tuple[int, date | None]:
    if not station.combined_path.exists():
        if not init_missing:
            return 0, None
        if dry_run:
            return 0, None
        return initialize_station(
            session,
            station,
            until,
            extraction_time,
            page_years,
            station_start_years,
            default_day,
            dry_run,
            sleep_seconds,
        )

    combined_header, combined_rows = read_csv(station.combined_path)
    year_index = combined_header.index("anno_richiesto")
    existing_by_date = {
        row_date(row, year_index): row
        for row in combined_rows
    }
    existing_dates = set(existing_by_date)
    if not existing_dates:
        if not init_missing:
            return 0, None
        return initialize_station(
            session,
            station,
            until,
            extraction_time,
            page_years,
            station_start_years,
            default_day,
            dry_run,
            sleep_seconds,
        )

    if refresh_days < 1:
        raise ValueError("refresh_days deve essere almeno 1.")

    latest = max(existing_dates)
    # La finestra termina alla data richiesta e include esattamente
    # refresh_days giorni. Se il CSV e' arretrato, non lasciamo un buco:
    # partiamo anche dal giorno successivo all'ultima data locale.
    window_start = until - timedelta(days=refresh_days - 1)
    start = min(window_start, latest + timedelta(days=1))
    if start > until:
        return 0, latest

    fetched_by_date: dict[date, list[str]] = {}
    for month_start in month_starts(start, until):
        headers, rows = fetch_month(session, station, month_start.year, month_start.month)
        if headers and headers != combined_header[: len(headers)]:
            print(f"ATTENZIONE {station.slug}: intestazioni OSMER diverse dal CSV combinato.")
        for row in rows:
            current = date(month_start.year, int(row[0]), int(row[1]))
            if start <= current <= until:
                fetched_by_date[current] = row

    if not fetched_by_date:
        return 0, latest

    decimal_comma = has_decimal_comma_style(combined_header, combined_rows)
    replacement_rows = {
        current: convert_row_style(
            row + [str(current.year), station.name, station.value, extraction_time],
            decimal_comma,
        )
        for current, row in fetched_by_date.items()
    }
    # Normalizza anche eventuali duplicati storici: per una data remota la
    # risposta appena scaricata prevale, altrimenti resta l'ultima riga locale.
    merged_by_date = dict(existing_by_date)
    merged_by_date.update(replacement_rows)
    merged_rows = [merged_by_date[current] for current in sorted(merged_by_date)]
    affected = len(replacement_rows)
    new_latest = max(latest, max(replacement_rows))

    if dry_run:
        return affected, new_latest

    write_csv(station.combined_path, combined_header, merged_rows)

    for year in sorted({current.year for current in replacement_rows}):
        annual_path = station.combined_path.parent / f"osmer_{station.slug}_{year}_giornalieri.csv"
        if annual_path.exists():
            annual_header, annual_rows = read_csv(annual_path)
        else:
            annual_header = combined_header[: len(next(iter(fetched_by_date.values())))]
            annual_rows = []
        year_replacements = {
            current: fetched_by_date[current]
            for current in replacement_rows
            if current.year == year
        }
        annual_by_date = {
            date(year, int(row[0]), int(row[1])): row
            for row in annual_rows
        }
        annual_by_date.update(year_replacements)
        merged_annual = [annual_by_date[current] for current in sorted(annual_by_date)]
        write_csv(annual_path, annual_header, merged_annual)

    return affected, new_latest


def main() -> int:
    args = parse_args()
    until = date.fromisoformat(args.until)
    root = Path(".")
    stations = discover_stations(root)
    if not stations:
        print("Nessuna stazione trovata.")
        return 2

    extraction_time = datetime.now().astimezone().isoformat(timespec="seconds")
    session = make_session()
    soup = load_archive_page(session)
    page_years = read_page_years(soup)
    default_day = read_default_day(soup)
    station_start_years = read_station_start_years(soup, page_years[0] if page_years else until.year)

    total = 0
    init_missing = not args.skip_init_missing
    station_sleep_seconds = max(0.0, args.sleep)
    init_request_sleep_seconds = max(3.0, args.sleep)
    print(f"Stazioni trovate: {len(stations)}")
    print(f"Aggiornamento fino a: {until.isoformat()}")
    for index, station in enumerate(stations, 1):
        try:
            existed_before = station.combined_path.exists()
            added, latest = append_missing(
                session,
                station,
                until,
                extraction_time,
                page_years,
                station_start_years,
                default_day,
                args.dry_run,
                init_missing,
                init_request_sleep_seconds,
                args.refresh_days,
            )
            total += added
            if not existed_before and not init_missing:
                status = "CSV mancante, inizializzazione saltata"
            elif not existed_before and args.dry_run:
                status = "da inizializzare"
            elif not existed_before:
                status = "inizializzata" if added else "CSV mancante, nessun dato"
            else:
                status = "gia' aggiornata" if added == 0 else f"+{added} righe"
            latest_text = latest.isoformat() if latest else "n/d"
            print(f"{index:02d}. {station.slug}: {status}, ultimo={latest_text}")
        except Exception as exc:
            print(f"{index:02d}. {station.slug}: ERRORE {exc}")
        if index < len(stations):
            time.sleep(station_sleep_seconds)

    print(f"Righe aggiunte totali: {total}")
    if args.dry_run:
        print("Dry run: nessun file modificato.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
