"""Genera console/confini_fvg.h dai confini amministrativi ISTAT.

Fonte: ISTAT, Confini delle unita' amministrative a fini statistici al 1 gennaio 2026
(versione non generalizzata, WGS84 UTM 32N), licenza CC BY 3.0 IT.
https://www.istat.it/notizia/confini-delle-unita-amministrative-a-fini-statistici-al-1-gennaio-2018-2/

Uso (serve solo per rigenerare il file, la console non ne ha bisogno):
    py -3 -m pip install pyshp pyproj shapely
    py -3 genera_confini_fvg.py <cartella con Reg01012026 e ProvCM01012026>
"""
from __future__ import annotations

import sys
from pathlib import Path

import shapefile
from pyproj import Transformer
from shapely.geometry import shape
from shapely.ops import transform

TOLERANCE_M = 250        # semplificazione: qualche centinaio di metri basta a schermo
MIN_AREA_KM2 = 2.0       # isolotti della laguna piu' piccoli: tralasciati


def polygons(geom):
    return list(geom.geoms) if geom.geom_type == "MultiPolygon" else [geom]


def rings_lonlat(geom, to_lonlat):
    out = []
    for poly in polygons(geom.simplify(TOLERANCE_M, preserve_topology=True)):
        if poly.area / 1e6 < MIN_AREA_KM2:
            continue
        ring = transform(to_lonlat, poly).exterior
        out.append([(round(x, 4), round(y, 4)) for x, y in ring.coords])
    return out


def main() -> int:
    base = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    to_lonlat = Transformer.from_crs("EPSG:32632", "EPSG:4326", always_xy=True).transform
    region = None
    for sr in shapefile.Reader(str(base / "Reg01012026" / "Reg01012026_WGS84")).shapeRecords():
        if sr.record[1] == 6:
            region = shape(sr.shape.__geo_interface__)
    provinces = []
    for sr in shapefile.Reader(str(base / "ProvCM01012026" / "ProvCM01012026_WGS84")).shapeRecords():
        if sr.record[1] == 6:
            provinces.append((sr.record[5], shape(sr.shape.__geo_interface__)))
    if region is None or not provinces:
        print("Friuli-Venezia Giulia non trovato nei file ISTAT.")
        return 1

    parts = []   # (tipo, nome, anello)  tipo 0 = regione, 1 = provincia
    for ring in rings_lonlat(region, to_lonlat):
        parts.append((0, "Friuli-Venezia Giulia", ring))
    for name, geom in sorted(provinces):
        for ring in rings_lonlat(geom, to_lonlat):
            parts.append((1, name, ring))

    lines = [
        "// Generato da strumenti/genera_confini_fvg.py: non modificare a mano.",
        "// Confini ISTAT al 1/1/2026 (CC BY 3.0 IT), semplificati a circa "
        f"{TOLERANCE_M} m, longitudine e latitudine WGS84.",
        "#pragma once",
        "",
        "struct GeoPart { int kind; const wchar_t* name; int count; const float* lonlat; };",
        "",
    ]
    for i, (_, _, ring) in enumerate(parts):
        flat = ", ".join(f"{x:.4f}f, {y:.4f}f" for x, y in ring)
        lines.append(f"static const float kGeo{i}[] = {{{flat}}};")
    lines.append("")
    lines.append("static const GeoPart kGeoParts[] = {")
    for i, (kind, name, ring) in enumerate(parts):
        lines.append(f'    {{{kind}, L"{name}", {len(ring)}, kGeo{i}}},')
    lines.append("};")
    lines.append(f"static const int kGeoPartCount = {len(parts)};")
    out = Path(__file__).resolve().parent.parent / "confini_fvg.h"
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    total = sum(len(r) for _, _, r in parts)
    print(f"Scritto {out}: {len(parts)} anelli, {total} punti")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
