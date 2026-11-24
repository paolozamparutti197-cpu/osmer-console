#!/usr/bin/env python3
"""
Scarica tutti gli anni disponibili dei dati giornalieri OSMER/ARPA FVG
per la stazione Udine S.O..

Uso:
  python scarica_osmer_udine_s_o_tutti_gli_anni.py
  python scarica_osmer_udine_s_o_tutti_gli_anni.py --output output_udine_s_o_tutti_gli_anni --sleep 3
"""

from __future__ import annotations

import argparse
import calendar
import csv
import logging
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from urllib.parse import urlencode

import requests
from bs4 import BeautifulSoup


PAGE_URL = "https://www.osmer.fvg.it/archivio.php?ln=&p=dati"
AJAX_URL = "https://www.osmer.fvg.it/ajax/getStationData.php"
LEGAL_URL = "https://www.osmer.fvg.it/note-legali.php"

STATION_NAME = 'Udine S.O.'
STATION_VALUE = 'UDI@Udine S.O.@syn@46.035212@13.226672@91'
STATION_SLUG = "udine_s_o"
LICENSE_TEXT = "Creative Commons attribuzione 3.0 Italia, CC BY-SA 3.0 IT"

USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
)


@dataclass
class YearResult:
    year: int
    http_status: int | None = None
    status: str = "pending"
    rows: list[list[str]] = field(default_factory=list)
    headers: list[str] = field(default_factory=list)
    csv_path: Path | None = None
    warnings: list[str] = field(default_factory=list)
    error: str | None = None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Scarica tutti gli anni disponibili OSMER per Udine S.O.."
    )
    parser.add_argument(
        "--output",
        default="output_udine_s_o_tutti_gli_anni",
        help="Cartella di output.",
    )
    parser.add_argument(
        "--sleep",
        type=float,
        default=3.0,
        help="Pausa in secondi tra richieste annuali. Minimo effettivo: 3.",
    )
    return parser.parse_args()


def setup_logging(output_dir: Path) -> Path:
    output_dir.mkdir(parents=True, exist_ok=True)
    log_path = output_dir / "download_osmer_udine_s_o.log"
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        handlers=[
            logging.StreamHandler(sys.stdout),
            logging.FileHandler(log_path, encoding="utf-8"),
        ],
        force=True,
    )
    return log_path


def ajax_headers() -> dict[str, str]:
    return {
        "accept": "application/json, text/javascript, */*; q=0.01",
        "content-type": "application/x-www-form-urlencoded; charset=UTF-8",
        "origin": "https://www.osmer.fvg.it",
        "referer": PAGE_URL,
        "sec-ch-ua": '"Chromium";v="124", "Google Chrome";v="124", "Not-A.Brand";v="99"',
        "sec-ch-ua-mobile": "?0",
        "sec-ch-ua-platform": '"Windows"',
        "sec-fetch-dest": "empty",
        "sec-fetch-mode": "cors",
        "sec-fetch-site": "same-origin",
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


def station_data_start(soup: BeautifulSoup) -> int:
    option = soup.find("option", attrs={"value": STATION_VALUE})
    if option is None:
        raise RuntimeError("La stazione Udine S.O. non e' presente nel selettore.")
    raw = option.get("data-start", "").strip()
    if raw.isdigit():
        return int(raw)
    return min(read_page_years(soup))


def clean_cell(cell) -> str:
    return cell.get_text(" ", strip=True)


def extract_first_valid_table(html: str) -> tuple[list[str], list[list[str]]]:
    soup = BeautifulSoup(html, "html.parser")
    for table in soup.find_all("table"):
        rows = table.find_all("tr")
        if not rows:
            continue
        headers = [clean_cell(cell) for cell in rows[0].find_all(["th", "td"])]
        if not headers:
            continue
        expected_cols = len(headers)
        data_rows: list[list[str]] = []
        for row in rows[1:]:
            values = [clean_cell(cell) for cell in row.find_all(["th", "td"])]
            if len(values) == expected_cols and any(values):
                data_rows.append(values)
        if data_rows:
            return headers, data_rows
    return [], []


def request_year(
    session: requests.Session,
    year: int,
    default_day: str,
) -> tuple[int, list[str], list[list[str]]]:
    payload = {
        "a": str(year),
        "m": "99",
        "g": default_day,
        "s": STATION_VALUE,
        "t": "H_3",
        "ln": "",
        "o": "visualizza",
    }
    body = urlencode(payload)
    response = session.post(AJAX_URL, headers=ajax_headers(), data=body, timeout=90)
    if response.status_code != 200:
        return response.status_code, [], []
    html = response.json()
    if not isinstance(html, str):
        raise RuntimeError("La risposta JSON non contiene HTML come stringa.")
    headers, rows = extract_first_valid_table(html)
    return response.status_code, headers, rows


def write_csv(path: Path, headers: list[str], rows: list[list[str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle, delimiter=";", lineterminator="\n")
        writer.writerow(headers)
        writer.writerows(rows)


def expected_rows_for_complete_year(year: int) -> int:
    return 366 if calendar.isleap(year) else 365


def quality_warnings(year: int, rows_count: int, latest_year: int) -> list[str]:
    warnings: list[str] = []
    expected = expected_rows_for_complete_year(year)
    if year == 2024 and rows_count != 366:
        warnings.append(f"2024 completo atteso a 366 righe, trovate {rows_count}.")
    if year == 2025 and rows_count != 365:
        warnings.append(f"2025 completo atteso a 365 righe, trovate {rows_count}.")
    if year < latest_year and abs(rows_count - expected) > 3:
        kind = "bisestile" if calendar.isleap(year) else "non bisestile"
        warnings.append(
            f"Anno completo {kind}: righe molto diverse da {expected} "
            f"(trovate {rows_count})."
        )
    if year == latest_year and rows_count < expected:
        warnings.append(
            f"Anno piu' recente probabilmente incompleto: {rows_count}/{expected} righe."
        )
    return warnings


def fetch_license_note(session: requests.Session) -> str:
    try:
        response = session.get(LEGAL_URL, timeout=30)
        response.raise_for_status()
    except requests.RequestException as exc:
        return f"{LICENSE_TEXT}; note legali non rilette per errore: {exc}"
    text = BeautifulSoup(response.text, "html.parser").get_text(" ", strip=True)
    if "CC BY-SA 3.0 IT" in text or "attribuzione 3.0 Italia" in text:
        return LICENSE_TEXT
    return f"{LICENSE_TEXT}; verificare pagina note legali"


def write_readme(
    path: Path,
    page_years: list[int],
    station_start_year: int,
    years: list[int],
    results: list[YearResult],
    combined_rows_count: int,
    extraction_time: str,
    license_note: str,
    default_day: str,
    sleep_seconds: float,
) -> None:
    with_data = [r for r in results if r.status == "ok"]
    without_data = [r for r in results if r.status == "no_data"]
    errors = [r for r in results if r.status == "error"]
    incomplete = [r for r in results if r.warnings]
    lines = [
        "# Download OSMER Udine S.O.",
        "",
        f"- URL origine: {PAGE_URL}",
        f"- Endpoint usato: {AJAX_URL}",
        f"- Stazione: {STATION_NAME}",
        f"- Valore stazione: `{STATION_VALUE}`",
        f"- Data-start stazione letto dalla pagina: {station_start_year}",
        f"- Anni presenti nel selettore pagina: {', '.join(map(str, page_years))}",
        f"- Anni disponibili usati per la stazione: {', '.join(map(str, years))}",
        f"- Anni scaricati: {', '.join(str(r.year) for r in results)}",
        f"- Anni con dati: {', '.join(str(r.year) for r in with_data) if with_data else 'nessuno'}",
        f"- Anni senza dati: {', '.join(str(r.year) for r in without_data) if without_data else 'nessuno'}",
        f"- Righe totali nel CSV combinato: {combined_rows_count}",
        f"- Data e ora dell'estrazione: {extraction_time}",
        f"- Licenza indicata dalla pagina: {license_note}",
        f"- Pausa tra richieste annuali: {sleep_seconds} secondi",
        "",
        "## Numero di righe per anno",
        "",
    ]
    for result in results:
        detail = f"- {result.year}: {len(result.rows)} righe, stato={result.status}"
        if result.csv_path:
            detail += f", file={result.csv_path.name}"
        if result.warnings:
            detail += f", warning={' | '.join(result.warnings)}"
        if result.error:
            detail += f", errore={result.error}"
        lines.append(detail)

    lines.extend(["", "## Anni incompleti o da controllare", ""])
    if incomplete:
        lines.extend(f"- {r.year}: {' | '.join(r.warnings)}" for r in incomplete)
    else:
        lines.append("- nessuno")

    lines.extend(["", "## Problemi incontrati", ""])
    if errors:
        lines.extend(f"- {r.year}: {r.error}" for r in errors)
    else:
        lines.append("- nessuno")

    lines.extend(
        [
            "",
            "## Metodo usato",
            "",
            "La pagina di origine e' stata letta per individuare gli anni effettivamente",
            "presenti nel selettore `anno` e il `data-start` della stazione Udine S.O..",
            "Gli anni scaricati sono l'intersezione tra il selettore pagina e gli anni",
            "maggiori o uguali al `data-start`, ordinati dal piu' vecchio al piu'",
            "recente. Il valore predefinito valido del selettore `giorno` e' stato letto",
            f"dalla pagina ed e' risultato `{default_day}`.",
            "",
            "Per ogni anno e' stata inviata una richiesta `POST` non aggressiva all'endpoint",
            "`ajax/getStationData.php` con `m=99` per tutti i mesi e `t=H_3` per i dati",
            "giornalieri. Tra una richiesta annuale e la successiva e' stata rispettata",
            "una pausa minima di 3 secondi.",
            "",
            "La risposta dell'endpoint e' JSON contenente HTML. Dal primo elemento `<table>`",
            "valido sono state estratte le intestazioni originali e solo le righe con lo",
            "stesso numero di colonne dell'intestazione. Note legali, righe testuali,",
            "timestamp e righe non tabellari sono state scartate. I valori sono stati",
            "salvati come testo UTF-8 con separatore `;`, senza conversioni numeriche",
            "aggressive e senza sostituire valori mancanti.",
        ]
    )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    args = parse_args()
    sleep_seconds = max(3.0, args.sleep)
    output_dir = Path(args.output)
    log_path = setup_logging(output_dir)

    extraction_time = datetime.now().astimezone().isoformat(timespec="seconds")
    session = make_session()

    try:
        soup = load_archive_page(session)
        page_years = read_page_years(soup)
        default_day = read_default_day(soup)
        start_year = station_data_start(soup)
        years = [year for year in page_years if year >= start_year]
    except Exception:
        logging.exception("Errore iniziale: impossibile leggere la pagina archivio.")
        return 2

    if not years:
        logging.error("Nessun anno valido trovato per Udine S.O..")
        return 2

    logging.info("Anni presenti nel selettore pagina: %s", page_years)
    logging.info("Data-start stazione Udine S.O.: %s", start_year)
    logging.info("Anni disponibili usati per Udine S.O.: %s", years)
    logging.info("Primo anno disponibile: %s", years[0])
    logging.info("Ultimo anno disponibile: %s", years[-1])
    logging.info("Giorno predefinito valido: %s", default_day)
    logging.info("Log: %s", log_path)

    results: list[YearResult] = []
    combined_headers: list[str] | None = None
    combined_rows: list[list[str]] = []

    for index, year in enumerate(years):
        result = YearResult(year=year)
        try:
            logging.info("Anno richiesto: %s", year)
            http_status, headers, rows = request_year(session, year, default_day)
            result.http_status = http_status
            logging.info("Anno %s: esito HTTP %s", year, http_status)

            if http_status != 200:
                result.status = "error"
                result.error = f"HTTP {http_status}"
                logging.error("Anno %s: errore %s", year, result.error)
            elif not headers or not rows:
                result.status = "no_data"
                result.headers = headers
                logging.warning("Anno %s: nessun dato tabellare valido", year)
            else:
                result.status = "ok"
                result.headers = headers
                result.rows = rows
                result.warnings = quality_warnings(year, len(rows), years[-1])
                filename = f"osmer_{STATION_SLUG}_{year}_giornalieri.csv"
                result.csv_path = output_dir / filename
                write_csv(result.csv_path, headers, rows)
                logging.info("Anno %s: numero righe estratte %s", year, len(rows))
                logging.info("Anno %s: file salvato %s", year, result.csv_path)
                for warning in result.warnings:
                    logging.warning("Anno %s: %s", year, warning)

                if combined_headers is None:
                    combined_headers = headers[:]
                elif headers != combined_headers:
                    warning = "Intestazioni diverse dal primo anno; combinazione per posizione."
                    result.warnings.append(warning)
                    logging.warning("Anno %s: %s", year, warning)

                for row in rows:
                    combined_rows.append(
                        row
                        + [
                            str(year),
                            STATION_NAME,
                            STATION_VALUE,
                            extraction_time,
                        ]
                    )
        except Exception as exc:
            result.status = "error"
            result.error = str(exc)
            logging.exception("Anno %s: errore, continuo con l'anno successivo", year)
        results.append(result)

        if index < len(years) - 1:
            time.sleep(sleep_seconds)

    combined_path = output_dir / "osmer_udine_s_o_giornalieri_tutti_gli_anni.csv"
    if combined_headers and combined_rows:
        write_csv(
            combined_path,
            combined_headers
            + ["anno_richiesto", "stazione_nome", "stazione_valore", "data_estrazione"],
            combined_rows,
        )
        logging.info("CSV combinato salvato %s", combined_path)
        logging.info("Righe totali CSV combinato: %s", len(combined_rows))
    else:
        logging.warning("CSV combinato non creato: nessuna riga dati disponibile.")

    license_note = fetch_license_note(session)
    readme_path = output_dir / "README_osmer_udine_s_o_download.md"
    write_readme(
        readme_path,
        page_years,
        start_year,
        years,
        results,
        len(combined_rows),
        extraction_time,
        license_note,
        default_day,
        sleep_seconds,
    )
    logging.info("README salvato %s", readme_path)

    years_with_data = [r.year for r in results if r.status == "ok"]
    years_without_data = [r.year for r in results if r.status == "no_data"]
    errors = [r for r in results if r.status == "error"]

    print("\nRIEPILOGO")
    print(f"Primo anno disponibile per Udine S.O.: {years[0]}")
    print(f"Ultimo anno disponibile per Udine S.O.: {years[-1]}")
    print(f"Totale anni processati: {len(results)}")
    print(
        "Anni con dati: "
        f"{len(years_with_data)} ({years_with_data[0]}-{years_with_data[-1] if years_with_data else 'n/a'})"
    )
    print(f"Anni senza dati: {years_without_data if years_without_data else 'nessuno'}")
    print(f"Righe totali nel CSV combinato: {len(combined_rows)}")
    print(f"Eventuali errori: {[f'{r.year}: {r.error}' for r in errors] if errors else 'nessuno'}")
    print(f"Output: {output_dir.resolve()}")

    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
