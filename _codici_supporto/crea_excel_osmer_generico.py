#!/usr/bin/env python3
"""
Genera workbook Excel riepilogativi dai CSV combinati OSMER.

Se viene indicato `--csv`, lavora su un solo file. Altrimenti cerca i soli CSV
combinati canonici sotto la cartella base: backup e staging sono esclusi.
"""

from __future__ import annotations

import argparse
import ast
import os
import re
import tempfile
import unicodedata
from pathlib import Path

import pandas as pd


MONTH_NAMES = {
    1: "gennaio",
    2: "febbraio",
    3: "marzo",
    4: "aprile",
    5: "maggio",
    6: "giugno",
    7: "luglio",
    8: "agosto",
    9: "settembre",
    10: "ottobre",
    11: "novembre",
    12: "dicembre",
}

NON_NUMERIC_COLUMNS = {
    "mese",
    "giorno*",
    "anno_richiesto",
    "stazione_nome",
    "stazione_valore",
    "data_estrazione",
}


def is_transient_data_path(path: Path) -> bool:
    return any(
        part == "_backup_riallineamento" or part.startswith(".riallineamento_")
        for part in path.parts
    )


def require_canonical_path(path: Path) -> Path:
    resolved = path.resolve()
    if is_transient_data_path(resolved):
        raise ValueError(f"I backup e gli staging non possono essere sorgenti o destinazioni: {resolved}")
    return resolved


def parse_args() -> argparse.Namespace:
    base_dir = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(
        description="Genera un workbook Excel riepilogativo da CSV OSMER."
    )
    parser.add_argument(
        "--csv",
        default=None,
        help="CSV sorgente. Se omesso, cerca tutti i CSV combinati sotto la cartella base.",
    )
    parser.add_argument(
        "--xlsx",
        default=None,
        help="File Excel da creare. Valido solo insieme a --csv. Se omesso, deriva il nome dal CSV.",
    )
    parser.add_argument(
        "--skip-existing",
        action="store_true",
        help="Salta gli XLSX gia' esistenti e piu' recenti del CSV sorgente.",
    )
    parser.add_argument("--base-dir", default=str(base_dir), help=argparse.SUPPRESS)
    return parser.parse_args()


def find_csvs(base_dir: Path, explicit_csv: str | None) -> list[Path]:
    if explicit_csv:
        return [require_canonical_path(Path(explicit_csv))]

    direct_matches = sorted(
        path for path in base_dir.glob("osmer_*_giornalieri_tutti_gli_anni.csv")
        if not is_transient_data_path(path)
    )
    if len(direct_matches) == 1:
        return [direct_matches[0].resolve()]

    matches = sorted(
        path.resolve()
        for path in base_dir.rglob("osmer_*_giornalieri_tutti_gli_anni.csv")
        if "anni_test" not in path.name and not is_transient_data_path(path)
    )
    if not matches:
        raise FileNotFoundError(
            "Nessun CSV osmer_*_giornalieri_tutti_gli_anni.csv trovato sotto la cartella base."
        )
    return matches


def output_path_for(csv_path: Path, explicit_xlsx: str | None) -> Path:
    csv_path = require_canonical_path(csv_path)
    if explicit_xlsx:
        return require_canonical_path(Path(explicit_xlsx))
    stem = csv_path.name.replace("_giornalieri_tutti_gli_anni.csv", "")
    return csv_path.with_name(f"{stem}_riepilogo_dashboard.xlsx")


def source_constants(script_path: Path) -> dict[str, str]:
    tree = ast.parse(script_path.read_text(encoding="utf-8"))
    constants: dict[str, str] = {}
    wanted = {"STATION_NAME", "STATION_SLUG"}
    for node in tree.body:
        if not isinstance(node, ast.Assign):
            continue
        if len(node.targets) != 1 or not isinstance(node.targets[0], ast.Name):
            continue
        key = node.targets[0].id
        if key in wanted and isinstance(node.value, ast.Constant):
            constants[key] = str(node.value.value)
    return constants


def expected_combined_csvs(base_dir: Path) -> dict[Path, str]:
    expected: dict[Path, str] = {}
    combined_by_name = {
        path.name: path.resolve()
        for path in base_dir.rglob("osmer_*_giornalieri_tutti_gli_anni.csv")
        if "anni_test" not in path.name and not is_transient_data_path(path)
    }
    for script_path in sorted(base_dir.rglob("scarica_osmer*_tutti_gli_anni.py")):
        constants = source_constants(script_path)
        if not {"STATION_NAME", "STATION_SLUG"} <= constants.keys():
            continue
        slug = constants["STATION_SLUG"]
        combined_name = f"osmer_{slug}_giornalieri_tutti_gli_anni.csv"
        path = combined_by_name.get(
            combined_name,
            (script_path.parent / f"output_{slug}_tutti_gli_anni" / combined_name).resolve(),
        )
        expected[path] = constants["STATION_NAME"]
    return expected


def station_title(raw: pd.DataFrame, csv_path: Path) -> str:
    if "stazione_nome" in raw.columns and raw["stazione_nome"].dropna().size:
        value = str(raw["stazione_nome"].dropna().iloc[0]).strip()
        if value:
            return value
    name = csv_path.stem.replace("osmer_", "").replace("_giornalieri_tutti_gli_anni", "")
    return name.replace("_", " ").title()


def normalize(text: str) -> str:
    text = unicodedata.normalize("NFKD", str(text))
    text = "".join(ch for ch in text if not unicodedata.combining(ch))
    return re.sub(r"[^a-z0-9]+", " ", text.lower()).strip()


def to_number(series: pd.Series) -> pd.Series:
    cleaned = (
        series.astype("string")
        .str.strip()
        .replace({"": pd.NA, "-": pd.NA, "nan": pd.NA, "None": pd.NA})
        .str.replace(".", "", regex=False)
        .str.replace(",", ".", regex=False)
    )
    return pd.to_numeric(cleaned, errors="coerce")


def sum_with_missing(series: pd.Series) -> float:
    return series.sum(min_count=1)


sum_with_missing.__name__ = "sum"


def numeric_columns(raw: pd.DataFrame) -> list[str]:
    cols: list[str] = []
    for column in raw.columns:
        if column in NON_NUMERIC_COLUMNS:
            continue
        converted = to_number(raw[column])
        if converted.notna().any():
            cols.append(column)
    return cols


def flatten_columns(df: pd.DataFrame) -> pd.DataFrame:
    df = df.copy()
    df.columns = [
        col if isinstance(col, str) else f"{col[0]} | {col[1]}" for col in df.columns
    ]
    return df.reset_index()


def build_numeric_frame(raw: pd.DataFrame, numeric_cols: list[str]) -> pd.DataFrame:
    calc = raw.copy()
    calc["anno"] = pd.to_numeric(calc["anno_richiesto"], errors="coerce").astype("Int64")
    calc["mese_num"] = pd.to_numeric(calc["mese"], errors="coerce").astype("Int64")
    calc["giorno_num"] = pd.to_numeric(calc["giorno*"], errors="coerce").astype("Int64")
    calc["data"] = pd.to_datetime(
        {
            "year": calc["anno"],
            "month": calc["mese_num"],
            "day": calc["giorno_num"],
        },
        errors="coerce",
    )
    calc["mese_nome"] = calc["mese_num"].map(MONTH_NAMES)
    for column in numeric_cols:
        calc[column] = to_number(calc[column])
    return calc


def build_monthly_summary(calc: pd.DataFrame, numeric_cols: list[str]) -> pd.DataFrame:
    grouped = calc.groupby(["anno", "mese_num", "mese_nome"], dropna=False)
    summaries = grouped[numeric_cols].agg(["mean", "min", "max", sum_with_missing])
    monthly = flatten_columns(summaries)
    monthly = monthly.rename(
        columns={"anno": "anno", "mese_num": "mese", "mese_nome": "mese_nome"}
    )
    counts = grouped.size().reset_index(name="giorni_rilevati")
    monthly.insert(3, "giorni_rilevati", counts["giorni_rilevati"])
    return monthly.sort_values(["anno", "mese"], kind="stable")


def build_annual_summary(calc: pd.DataFrame, numeric_cols: list[str]) -> pd.DataFrame:
    grouped = calc.groupby(["anno"], dropna=False)
    summaries = grouped[numeric_cols].agg(["mean", "min", "max", sum_with_missing])
    annual = flatten_columns(summaries)
    counts = grouped.size().reset_index(name="giorni_rilevati")
    annual.insert(1, "giorni_rilevati", counts["giorni_rilevati"])
    return annual.sort_values(["anno"], kind="stable")


def find_column(columns: list[str], *terms: str) -> str | None:
    normalized = [(column, normalize(column)) for column in columns]
    normalized_terms = [normalize(term) for term in terms]
    for column, norm in normalized:
        if all(term in norm for term in normalized_terms):
            return column
    return None


def build_extremes(calc: pd.DataFrame, numeric_cols: list[str]) -> pd.DataFrame:
    rows: list[dict[str, object]] = []
    for column in numeric_cols:
        values = calc[column].dropna()
        if values.empty:
            rows.append({"variabile": column, "valori_validi": 0})
            continue
        min_idx = values.idxmin()
        max_idx = values.idxmax()
        rows.append(
            {
                "variabile": column,
                "valori_validi": int(values.count()),
                "media_periodo": values.mean(),
                "minimo_assoluto": values.min(),
                "data_minimo": calc.loc[min_idx, "data"].date()
                if pd.notna(calc.loc[min_idx, "data"])
                else "",
                "anno_minimo": calc.loc[min_idx, "anno"],
                "mese_minimo": calc.loc[min_idx, "mese_num"],
                "massimo_assoluto": values.max(),
                "data_massimo": calc.loc[max_idx, "data"].date()
                if pd.notna(calc.loc[max_idx, "data"])
                else "",
                "anno_massimo": calc.loc[max_idx, "anno"],
                "mese_massimo": calc.loc[max_idx, "mese_num"],
                "somma_periodo": values.sum(min_count=1),
            }
        )
    return pd.DataFrame(rows)


def fmt_number(value: object, suffix: str = "") -> str:
    if pd.isna(value):
        return ""
    return f"{float(value):.1f}{suffix}"


def build_dashboard(
    raw: pd.DataFrame,
    calc: pd.DataFrame,
    monthly: pd.DataFrame,
    annual: pd.DataFrame,
    numeric_cols: list[str],
    csv_path: Path,
    title: str,
) -> pd.DataFrame:
    latest_year = int(calc["anno"].max())
    first_date = calc["data"].min()
    last_date = calc["data"].max()
    completed = calc[calc["anno"] < latest_year]

    rain_col = find_column(numeric_cols, "pioggia")
    temp_min_col = find_column(numeric_cols, "temp", "min")
    temp_med_col = find_column(numeric_cols, "temp", "med")
    temp_max_col = find_column(numeric_cols, "temp", "max")
    wind_max_col = find_column(numeric_cols, "vento", "max")

    rows: list[tuple[str, object, str]] = [
        ("Stazione", title, ""),
        ("CSV sorgente", csv_path.name, "Aggiorna il CSV e rilancia lo script."),
        ("Righe dati", len(raw), "Righe lette dal CSV riepilogativo."),
        (
            "Periodo",
            f"{first_date.date()} - {last_date.date()}",
            "Date ricostruite da anno/mese/giorno.",
        ),
        ("Anni coperti", f"{int(calc['anno'].min())} - {latest_year}", ""),
        (
            "Righe ultimo anno",
            int((calc["anno"] == latest_year).sum()),
            "L'ultimo anno puo' essere parziale.",
        ),
        ("Variabili numeriche", len(numeric_cols), ", ".join(numeric_cols)),
    ]

    if rain_col:
        rain_sum = annual[f"{rain_col} | sum"]
        wettest_year = annual.loc[rain_sum.idxmax()]
        rain_month_sum = monthly[f"{rain_col} | sum"]
        wettest_month = monthly.loc[rain_month_sum.idxmax()]
        rows.extend(
            [
                ("Pioggia totale periodo", calc[rain_col].sum(min_count=1), rain_col),
                (
                    "Pioggia media annua anni completi",
                    completed.groupby("anno")[rain_col].sum(min_count=1).mean(),
                    "Esclude l'ultimo anno.",
                ),
                (
                    "Anno piu' piovoso",
                    f"{int(wettest_year['anno'])} ({fmt_number(wettest_year[f'{rain_col} | sum'], ' mm')})",
                    "Da somma annuale.",
                ),
                (
                    "Mese piu' piovoso",
                    f"{int(wettest_month['anno'])}-{int(wettest_month['mese']):02d} ({fmt_number(wettest_month[f'{rain_col} | sum'], ' mm')})",
                    "Da somma mensile.",
                ),
            ]
        )

    if temp_max_col and calc[temp_max_col].dropna().size:
        hottest = calc.loc[calc[temp_max_col].idxmax()]
        rows.append(
            (
                "Temperatura massima assoluta",
                f"{fmt_number(hottest[temp_max_col], ' C')} il {hottest['data'].date()}",
                temp_max_col,
            )
        )
    if temp_min_col and calc[temp_min_col].dropna().size:
        coldest = calc.loc[calc[temp_min_col].idxmin()]
        rows.append(
            (
                "Temperatura minima assoluta",
                f"{fmt_number(coldest[temp_min_col], ' C')} il {coldest['data'].date()}",
                temp_min_col,
            )
        )
    if wind_max_col and calc[wind_max_col].dropna().size:
        windy = calc.loc[calc[wind_max_col].idxmax()]
        rows.append(
            (
                "Vento massimo assoluto",
                f"{fmt_number(windy[wind_max_col], ' km/h')} il {windy['data'].date()}",
                wind_max_col,
            )
        )
    if temp_med_col:
        rows.append(
            (
                "Temperatura media periodo",
                calc[temp_med_col].mean(),
                "Media dei valori giornalieri disponibili.",
            )
        )
    return pd.DataFrame(rows, columns=["indicatore", "valore", "nota"])


def add_table(
    worksheet,
    dataframe: pd.DataFrame,
    table_name: str,
    startrow: int = 0,
    startcol: int = 0,
) -> None:
    rows, cols = dataframe.shape
    if rows == 0 or cols == 0:
        return
    headers = [{"header": str(col)} for col in dataframe.columns]
    worksheet.add_table(
        startrow,
        startcol,
        startrow + rows,
        startcol + cols - 1,
        {"name": table_name, "columns": headers},
    )


def set_common_format(worksheet, dataframe: pd.DataFrame) -> None:
    worksheet.freeze_panes(1, 0)
    for idx, column in enumerate(dataframe.columns):
        width = min(max(len(str(column)) + 2, 11), 30)
        worksheet.set_column(idx, idx, width)


def make_table_name(prefix: str, title: str) -> str:
    slug = re.sub(r"[^A-Za-z0-9]", "", title)[:18]
    return f"{prefix}{slug}"


def _write_workbook(
    raw: pd.DataFrame,
    monthly: pd.DataFrame,
    annual: pd.DataFrame,
    extremes: pd.DataFrame,
    dashboard: pd.DataFrame,
    numeric_cols: list[str],
    csv_path: Path,
    xlsx_path: Path,
    title: str,
) -> None:
    with pd.ExcelWriter(xlsx_path, engine="xlsxwriter") as writer:
        workbook = writer.book
        workbook.set_properties(
            {
                "title": f"OSMER {title} - riepilogo dashboard",
                "subject": "Riepiloghi mensili e dashboard da CSV OSMER",
                "author": "Codex",
            }
        )
        title_fmt = workbook.add_format(
            {"bold": True, "font_size": 16, "font_color": "#1f4e79"}
        )
        note_fmt = workbook.add_format({"font_color": "#555555", "text_wrap": True})

        dashboard.to_excel(writer, sheet_name="Dashboard", index=False, startrow=3)
        ws = writer.sheets["Dashboard"]
        ws.write(0, 0, f"{title} - dashboard OSMER", title_fmt)
        ws.write(1, 0, f"CSV sorgente: {csv_path.name}", note_fmt)
        add_table(ws, dashboard, make_table_name("tblDash", title), startrow=3)
        ws.freeze_panes(4, 0)
        ws.set_column(0, 0, 32)
        ws.set_column(1, 1, 36)
        ws.set_column(2, 2, 64)

        annual.to_excel(writer, sheet_name="Riepilogo_annuale", index=False)
        ws_annual = writer.sheets["Riepilogo_annuale"]
        add_table(ws_annual, annual, make_table_name("tblAnn", title))
        set_common_format(ws_annual, annual)

        monthly.to_excel(writer, sheet_name="Riepilogo_mensile", index=False)
        ws_monthly = writer.sheets["Riepilogo_mensile"]
        add_table(ws_monthly, monthly, make_table_name("tblMens", title))
        set_common_format(ws_monthly, monthly)

        extremes.to_excel(writer, sheet_name="Estremi_assoluti", index=False)
        ws_extremes = writer.sheets["Estremi_assoluti"]
        add_table(ws_extremes, extremes, make_table_name("tblExtr", title))
        set_common_format(ws_extremes, extremes)

        raw.to_excel(writer, sheet_name="Dati_CSV", index=False)
        ws_raw = writer.sheets["Dati_CSV"]
        add_table(ws_raw, raw, make_table_name("tblDati", title))
        set_common_format(ws_raw, raw)

        readme = pd.DataFrame(
            [
                ["Dashboard", "Indicatori sintetici e grafici annuali."],
                [
                    "Riepilogo_mensile",
                    "Per anno/mese: media, minimo, massimo e somma delle variabili numeriche disponibili.",
                ],
                [
                    "Riepilogo_annuale",
                    "Per anno: media, minimo, massimo e somma delle variabili numeriche disponibili.",
                ],
                [
                    "Estremi_assoluti",
                    "Minimo e massimo assoluto con data/anno/mese per ogni variabile.",
                ],
                [
                    "Dati_CSV",
                    "Copia del CSV sorgente mantenuta come testo, senza alterare i valori originali.",
                ],
                [
                    "Aggiornamento",
                    "Aggiorna il CSV e rilancia aggiorna_excel.bat o python crea_excel_riepilogo.py.",
                ],
            ],
            columns=["File", "Descrizione"],
        )
        readme.to_excel(writer, sheet_name="README", index=False)
        ws_readme = writer.sheets["README"]
        add_table(ws_readme, readme, make_table_name("tblReadme", title))
        ws_readme.set_column(0, 0, 24)
        ws_readme.set_column(1, 1, 96)

        rain_col = find_column(numeric_cols, "pioggia")
        temp_med_col = find_column(numeric_cols, "temp", "med")
        year_col = annual.columns.get_loc("anno")
        last_row = len(annual)
        if rain_col and f"{rain_col} | sum" in annual.columns:
            chart_rain = workbook.add_chart({"type": "column"})
            rain_idx = annual.columns.get_loc(f"{rain_col} | sum")
            chart_rain.add_series(
                {
                    "name": "Pioggia annua",
                    "categories": ["Riepilogo_annuale", 1, year_col, last_row, year_col],
                    "values": ["Riepilogo_annuale", 1, rain_idx, last_row, rain_idx],
                }
            )
            chart_rain.set_title({"name": "Pioggia annua"})
            chart_rain.set_x_axis({"name": "Anno"})
            chart_rain.set_y_axis({"name": "mm"})
            chart_rain.set_legend({"none": True})
            ws.insert_chart("E3", chart_rain, {"x_scale": 1.25, "y_scale": 1.1})

        if temp_med_col and f"{temp_med_col} | mean" in annual.columns:
            chart_temp = workbook.add_chart({"type": "line"})
            temp_idx = annual.columns.get_loc(f"{temp_med_col} | mean")
            chart_temp.add_series(
                {
                    "name": "Temperatura media annua",
                    "categories": ["Riepilogo_annuale", 1, year_col, last_row, year_col],
                    "values": ["Riepilogo_annuale", 1, temp_idx, last_row, temp_idx],
                }
            )
            chart_temp.set_title({"name": "Temperatura media annua"})
            chart_temp.set_x_axis({"name": "Anno"})
            chart_temp.set_y_axis({"name": "C"})
            chart_temp.set_legend({"none": True})
            ws.insert_chart("E20", chart_temp, {"x_scale": 1.25, "y_scale": 1.1})


def write_workbook(
    raw: pd.DataFrame,
    monthly: pd.DataFrame,
    annual: pd.DataFrame,
    extremes: pd.DataFrame,
    dashboard: pd.DataFrame,
    numeric_cols: list[str],
    csv_path: Path,
    xlsx_path: Path,
    title: str,
) -> None:
    """Publish a workbook only after its complete temporary copy is written."""
    xlsx_path = require_canonical_path(xlsx_path)
    xlsx_path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(
        prefix=f".{xlsx_path.stem}.", suffix=".tmp.xlsx", dir=xlsx_path.parent
    )
    os.close(fd)
    temporary_path = Path(temporary_name)
    try:
        _write_workbook(
            raw, monthly, annual, extremes, dashboard, numeric_cols,
            csv_path, temporary_path, title,
        )
        os.replace(temporary_path, xlsx_path)
    except BaseException:
        temporary_path.unlink(missing_ok=True)
        raise


def write_raw_only_workbook(
    raw: pd.DataFrame,
    csv_path: Path,
    xlsx_path: Path,
    title: str,
) -> None:
    """Create a valid workbook when a station has no numeric observations."""
    xlsx_path = require_canonical_path(xlsx_path)
    xlsx_path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(
        prefix=f".{xlsx_path.stem}.", suffix=".tmp.xlsx", dir=xlsx_path.parent
    )
    os.close(fd)
    temporary_path = Path(temporary_name)
    try:
        with pd.ExcelWriter(temporary_path, engine="xlsxwriter") as writer:
            workbook = writer.book
            title_fmt = workbook.add_format(
                {"bold": True, "font_size": 16, "font_color": "#1f4e79"}
            )
            note_fmt = workbook.add_format({"font_color": "#555555", "text_wrap": True})
            status = pd.DataFrame(
                [
                    ["Stato", "Nessuna osservazione numerica disponibile nel CSV sorgente."],
                    ["CSV sorgente", csv_path.name],
                    ["Aggiornamento", "Il workbook verra' rigenerato automaticamente quando OSMER fornira' dati validi."],
                ],
                columns=["Voce", "Dettaglio"],
            )
            status.to_excel(writer, sheet_name="Dashboard", index=False, startrow=3)
            dashboard = writer.sheets["Dashboard"]
            dashboard.write(0, 0, f"{title} - dashboard OSMER", title_fmt)
            dashboard.write(1, 0, "Dati non disponibili", note_fmt)
            add_table(dashboard, status, make_table_name("tblDash", title), startrow=3)
            dashboard.set_column(0, 0, 20)
            dashboard.set_column(1, 1, 100)

            raw.to_excel(writer, sheet_name="Dati_CSV", index=False)
            raw_sheet = writer.sheets["Dati_CSV"]
            add_table(raw_sheet, raw, make_table_name("tblDati", title))
            set_common_format(raw_sheet, raw)
        os.replace(temporary_path, xlsx_path)
    except BaseException:
        temporary_path.unlink(missing_ok=True)
        raise


def create_unavailable_workbook(
    title: str,
    csv_path: Path,
    xlsx_path: Path,
    reason: str,
) -> None:
    """Create the single station workbook while transparently reporting no data."""
    xlsx_path = require_canonical_path(xlsx_path)
    xlsx_path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(
        prefix=f".{xlsx_path.stem}.", suffix=".tmp.xlsx", dir=xlsx_path.parent
    )
    os.close(fd)
    temporary_path = Path(temporary_name)
    try:
        with pd.ExcelWriter(temporary_path, engine="xlsxwriter") as writer:
            workbook = writer.book
            title_fmt = workbook.add_format(
                {"bold": True, "font_size": 16, "font_color": "#1f4e79"}
            )
            note_fmt = workbook.add_format({"font_color": "#555555", "text_wrap": True})
            status = pd.DataFrame(
                [
                    ["Stato", "Dati temporaneamente non disponibili."],
                    ["CSV atteso", str(csv_path)],
                    ["Motivo", reason],
                ],
                columns=["Voce", "Dettaglio"],
            )
            status.to_excel(writer, sheet_name="Dashboard", index=False, startrow=3)
            dashboard = writer.sheets["Dashboard"]
            dashboard.write(0, 0, f"{title} - dashboard OSMER", title_fmt)
            dashboard.write(1, 0, "Sorgente dati non disponibile", note_fmt)
            add_table(dashboard, status, make_table_name("tblDash", title), startrow=3)
            dashboard.set_column(0, 0, 20)
            dashboard.set_column(1, 1, 100)
        os.replace(temporary_path, xlsx_path)
    except BaseException:
        temporary_path.unlink(missing_ok=True)
        raise


def create_workbook_from_csv(
    csv_path: Path,
    xlsx_path: Path,
    skip_existing: bool,
) -> tuple[str, int, int]:
    if (
        skip_existing
        and xlsx_path.exists()
        and xlsx_path.stat().st_mtime >= csv_path.stat().st_mtime
    ):
        raw = pd.read_csv(csv_path, sep=";", dtype=str, encoding="utf-8-sig")
        return station_title(raw, csv_path), len(raw), len(numeric_columns(raw))

    raw = pd.read_csv(csv_path, sep=";", dtype=str, encoding="utf-8-sig")
    title = station_title(raw, csv_path)
    numeric_cols = numeric_columns(raw)
    if not numeric_cols:
        write_raw_only_workbook(raw, csv_path, xlsx_path, title)
        return title, len(raw), 0

    calc = build_numeric_frame(raw, numeric_cols)
    monthly = build_monthly_summary(calc, numeric_cols)
    annual = build_annual_summary(calc, numeric_cols)
    extremes = build_extremes(calc, numeric_cols)
    dashboard = build_dashboard(raw, calc, monthly, annual, numeric_cols, csv_path, title)

    write_workbook(
        raw, monthly, annual, extremes, dashboard, numeric_cols, csv_path, xlsx_path, title
    )
    return title, len(raw), len(numeric_cols)


def main() -> int:
    args = parse_args()
    base_dir = Path(args.base_dir).resolve()
    csv_paths = find_csvs(base_dir, args.csv)
    if args.xlsx and len(csv_paths) > 1:
        raise RuntimeError("--xlsx puo' essere usato solo quando specifichi un solo --csv.")

    expected = expected_combined_csvs(base_dir) if not args.csv else {}
    expected_paths = set(expected)
    found_paths = {path.resolve() for path in csv_paths}
    missing_expected = sorted(expected_paths - found_paths)

    created = 0
    skipped = 0
    errors: list[tuple[Path, str]] = []

    print(f"CSV combinati trovati: {len(csv_paths)}")
    for index, csv_path in enumerate(csv_paths, 1):
        xlsx_path = output_path_for(csv_path, args.xlsx)
        try:
            existed_and_current = (
                args.skip_existing
                and xlsx_path.exists()
                and xlsx_path.stat().st_mtime >= csv_path.stat().st_mtime
            )
            title, rows_count, numeric_count = create_workbook_from_csv(
                csv_path, xlsx_path, args.skip_existing
            )
            if existed_and_current:
                skipped += 1
                status = "saltato, XLSX gia' aggiornato"
            else:
                created += 1
                status = "creato"
            print(
                f"{index:02d}. {title}: {status}, righe={rows_count}, "
                f"variabili={numeric_count}, file={xlsx_path}"
            )
        except Exception as exc:
            errors.append((csv_path, str(exc)))
            print(f"{index:02d}. {csv_path}: ERRORE {exc}")

    if missing_expected:
        print("")
        print(f"Stazioni con script ma senza CSV combinato: {len(missing_expected)}")
        for path in missing_expected:
            print(f"- {expected[path]}: {path}")

    print("")
    print(f"Workbook creati/aggiornati: {created}")
    print(f"Workbook saltati: {skipped}")
    print(f"Errori: {len(errors)}")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
