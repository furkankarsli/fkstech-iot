"""Şikayetvar marka sayfasından dönemsel şikayet istatistikleri."""
import json
import re

import requests

from zk.config import UA

# Şikayetvar'ın kendi dönem kodları
DONEMLER = {"l1y": "1yil", "l1m": "1ay", "all": "tum"}


def sikayetvar(slug):
    url = f"https://www.sikayetvar.com/{slug}"
    r = requests.get(url, headers={"User-Agent": UA}, timeout=30)
    r.raise_for_status()
    h = r.text.replace('\\"', '"')

    m = re.search(r'"companyStats":\[', h)
    if not m:
        raise ValueError(f"Şikayet istatistikleri bulunamadı: {url}")
    stats, _ = json.JSONDecoder().raw_decode(h, m.end() - 1)

    sonuc = {"url": url}
    for s in stats:
        ek = DONEMLER.get(s.get("period"))
        if not ek:
            continue
        sonuc[f"sikayet_{ek}"] = s.get("complaintCount")
        sonuc[f"cozulen_{ek}"] = s.get("resolvedCount")
        sonuc[f"cozum_orani_{ek}"] = s.get("resolveRatio")  # %, şikayetçi onaylı çözüm
    if "sikayet_tum" not in sonuc:
        raise ValueError(f"Şikayet sayısı bulunamadı: {url}")
    return sonuc
