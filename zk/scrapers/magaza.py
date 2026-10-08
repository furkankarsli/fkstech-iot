"""App Store ve Google Play puanları."""
import requests
from google_play_scraper import app as play_app

from zk.config import UA


def appstore(app_id):
    r = requests.get(
        "https://itunes.apple.com/lookup",
        params={"id": app_id, "country": "tr"},
        headers={"User-Agent": UA},
        timeout=20,
    )
    r.raise_for_status()
    sonuc = r.json()["results"]
    if not sonuc:
        raise ValueError(f"App Store'da bulunamadı: {app_id}")
    a = sonuc[0]
    return {
        "puan": round(a.get("averageUserRating") or 0, 2),
        "oy": a.get("userRatingCount"),
        "surum": a.get("version"),
        "guncelleme": (a.get("currentVersionReleaseDate") or "")[:10],
    }


def google_play(paket):
    a = play_app(paket, lang="tr", country="tr")
    return {
        "puan": round(a.get("score") or 0, 2),
        "oy": a.get("ratings"),
        "yorum": a.get("reviews"),
        "indirme": a.get("installs"),
        "surum": a.get("version"),
    }
