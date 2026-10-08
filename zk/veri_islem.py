"""Veri toplama, kayıt (zk/veri/YYYY-MM-DD.json) ve kampanya karşılaştırması.

Komut satırı (GitHub Action da bunu çalıştırır):  python -m zk.veri_islem
"""
import json
import os
import sys
from datetime import datetime, timezone
from pathlib import Path

from zk.config import MARKALAR
from zk.scrapers.kampanya import kampanyalar
from zk.scrapers.magaza import appstore, google_play
from zk.scrapers.sikayetvar import sikayetvar

VERI = Path(__file__).parent / "veri"


def topla():
    markalar, hatalar = [], []

    def dene(ad, kaynak, fn, *args):
        try:
            return fn(*args)
        except Exception as e:  # tek kaynak bozulunca diğerleri yine gelsin
            print(f"[zk] {ad} / {kaynak}: {e}", file=sys.stderr)
            hatalar.append(f"{ad} / {kaynak}: {type(e).__name__}: {str(e)[:150]}")
            return None

    for m in MARKALAR:
        k = m["kampanya"]
        markalar.append({
            "marka": m["ad"],
            "appstore": dene(m["ad"], "App Store", appstore, m["appstore_id"]),
            "play": dene(m["ad"], "Google Play", google_play, m["play_id"]),
            "sikayetvar": dene(m["ad"], "Şikayetvar", sikayetvar, m["sikayetvar"]),
            "kampanyalar": dene(m["ad"], "Kampanya", kampanyalar, k) if k else None,
            "kampanya_kaynagi": k["url"] if k else None,
        })
    simdi = datetime.now(timezone.utc)
    return {
        "tarih": simdi.date().isoformat(),
        "guncelleme": simdi.isoformat(timespec="seconds"),
        "hatalar": hatalar,  # bu çekimde gelmeyen kaynaklar (aynı günün önceki değeri kullanılır)
        "markalar": markalar,
    }


def kaydet(kayit):
    """Günde bir dosya; aynı gün tekrar çekilirse üzerine yazar.
    Bir kaynak bu sefer gelmediyse aynı günün önceki değeri korunur."""
    VERI.mkdir(exist_ok=True)
    yol = VERI / f"{kayit['tarih']}.json"
    if yol.exists():
        eski = {v["marka"]: v for v in json.loads(yol.read_text())["markalar"]}
        for v in kayit["markalar"]:
            for alan in ("appstore", "play", "sikayetvar", "kampanyalar"):
                if v[alan] is None and eski.get(v["marka"], {}).get(alan) is not None:
                    v[alan] = eski[v["marka"]][alan]
    gecici = yol.with_suffix(f".{os.getpid()}.tmp")
    gecici.write_text(json.dumps(kayit, ensure_ascii=False, indent=1))
    gecici.replace(yol)


def kayitlar():
    """Tarihe göre sıralı tüm günlük kayıtlar."""
    return [json.loads(p.read_text()) for p in sorted(VERI.glob("????-??-??.json"))]


def kampanya_farki(simdi, once):
    """Her marka için {'yeni': [link], 'kalkan': [kampanya]}."""
    onceki = {v["marka"]: v.get("kampanyalar") for v in (once or {}).get("markalar", [])}
    fark = {}
    for v in simdi["markalar"]:
        yeni_k, eski_k = v["kampanyalar"], onceki.get(v["marka"])
        if yeni_k is None or eski_k is None:
            fark[v["marka"]] = {"yeni": [], "kalkan": []}
            continue
        eski_linkler = {x["link"] for x in eski_k}
        yeni_linkler = {x["link"] for x in yeni_k}
        fark[v["marka"]] = {
            "yeni": sorted(yeni_linkler - eski_linkler),
            "kalkan": [x for x in eski_k if x["link"] not in yeni_linkler],
        }
    return fark


def son_durum():
    """Son kayıt + bir önceki güne göre kampanya farkı + özet geçmişi."""
    hepsi = kayitlar()
    if not hepsi:
        return None
    son = hepsi[-1]
    once = hepsi[-2] if len(hepsi) > 1 else None
    gecmis = [
        {
            "tarih": k["tarih"],
            "markalar": {
                v["marka"]: {
                    "kampanya": len(v["kampanyalar"]) if v["kampanyalar"] is not None else None,
                    "appstore": (v["appstore"] or {}).get("puan"),
                    "play": (v["play"] or {}).get("puan"),
                    "sikayet_1yil": (v["sikayetvar"] or {}).get("sikayet_1yil"),
                    "cozum_orani_1yil": (v["sikayetvar"] or {}).get("cozum_orani_1yil"),
                }
                for v in k["markalar"]
            },
        }
        for k in hepsi[-90:]
    ]
    return {**son, "onceki_tarih": once and once["tarih"], "fark": kampanya_farki(son, once), "gecmis": gecmis}


if __name__ == "__main__":
    k = topla()
    kaydet(k)
    for v in k["markalar"]:
        s = v["sikayetvar"] or {}
        print(f"{v['marka']:<26} kampanya={len(v['kampanyalar']) if v['kampanyalar'] is not None else '-':<4}"
              f" appstore={(v['appstore'] or {}).get('puan')} play={(v['play'] or {}).get('puan')}"
              f" sikayet_1yil={s.get('sikayet_1yil')} cozum%={s.get('cozum_orani_1yil')}")
