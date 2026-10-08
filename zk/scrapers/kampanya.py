"""Marka sitelerindeki kampanya listeleri."""
import re
from collections import Counter
from urllib.parse import unquote, urljoin, urlparse

import requests
from bs4 import BeautifulSoup

from zk.config import UA

# Kampanya kartlarındaki buton yazıları; başlık olarak kullanılmaz.
GENEL = re.compile(r"^(detay|kampanya detay|detaylar[ıi] gör|detayl[ıi] bilgi al|incele|keşfet|kampanyay[ıi] keşfet|tıkla)", re.I)
MAX_SAYFA = 10


def _sayfa_linkleri(url, desen):
    r = requests.get(url, headers={"User-Agent": UA}, timeout=30)
    r.raise_for_status()
    soup = BeautifulSoup(r.text, "html.parser")
    adaylar = {}  # link -> olası başlıklar
    for a in soup.find_all("a", href=True):
        link = urljoin(url, a["href"]).split("#")[0]
        yol = urlparse(link).path
        if not re.search(desen, yol):
            continue
        metinler = adaylar.setdefault(link, [])
        img = a.find("img")
        for parca in [*a.stripped_strings, a.get("title"), img and img.get("alt")]:
            if parca:
                metinler.append(re.sub(r"\s+", " ", parca).strip())
    return adaylar


def _detay_basligi(link):
    try:
        r = requests.get(link, headers={"User-Agent": UA}, timeout=20)
        t = BeautifulSoup(r.text, "html.parser").title
        return t.get_text(strip=True).split("|")[0].strip() if t else None
    except requests.RequestException:
        return None


def _basliklar(adaylar):
    # Birden fazla kartta aynen geçen metin (ör. ortak görsel alt yazısı) başlık değildir.
    sayac = Counter(m for metinler in adaylar.values() for m in set(metinler))
    sonuc = []
    for link, metinler in adaylar.items():
        baslik = next(
            (m for m in metinler if len(m) >= 8 and sayac[m] == 1 and not GENEL.match(m)),
            None,
        ) or _detay_basligi(link)
        if not baslik:
            slug = [p for p in urlparse(link).path.split("/") if p and not p.isdigit()][-1]
            baslik = unquote(slug).replace("-", " ").strip().capitalize()
        sonuc.append({"baslik": baslik, "link": link})
    return sonuc


def kampanyalar(ayar):
    adaylar = _sayfa_linkleri(ayar["url"], ayar["link"])
    if ayar.get("sayfalar"):
        for n in range(2, MAX_SAYFA + 1):
            yeni = _sayfa_linkleri(ayar["sayfalar"].format(n=n), ayar["link"])
            if not set(yeni) - set(adaylar):
                break
            for k, v in yeni.items():
                adaylar.setdefault(k, []).extend(v)
    return _basliklar(adaylar)
