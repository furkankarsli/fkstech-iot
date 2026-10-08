"""/zk – Yemek kartı rakip takibi (TokenFlex ve rakipleri).

Arka planda YENILEME_SAAT'te bir veri çekilir ve zk/veri/ altına günlük kayıt yazılır.
Render ücretsiz planda disk kalıcı olmadığı için günlük kayıtlar ayrıca
GitHub Action (.github/workflows/zk-veri.yml) tarafından repoya commit edilir.
"""
import io
import threading
import time
from datetime import datetime, timezone

from flask import Blueprint, jsonify, render_template, send_file

from zk.veri_islem import kayitlar, kaydet, son_durum, topla

YENILEME_SAAT = 6
ELLE_YENILEME_ARALIK_DK = 15

zk_bp = Blueprint("zk", __name__, url_prefix="/zk")

_kilit = threading.Lock()
_durum = {"calisiyor": False, "son_deneme": None}


def _son_guncelleme():
    hepsi = kayitlar()
    if not hepsi:
        return None
    return datetime.fromisoformat(hepsi[-1]["guncelleme"])


def _yenile():
    if not _kilit.acquire(blocking=False):
        return False
    try:
        _durum["calisiyor"] = True
        _durum["son_deneme"] = datetime.now(timezone.utc)
        kaydet(topla())
        return True
    except Exception as e:
        print(f"[zk] yenileme hatası: {e}")
        return False
    finally:
        _durum["calisiyor"] = False
        _kilit.release()


def _eski_mi(saat):
    son = _son_guncelleme()
    return son is None or (datetime.now(timezone.utc) - son).total_seconds() > saat * 3600


def _dongu():
    while True:
        if _eski_mi(YENILEME_SAAT):
            _yenile()
        time.sleep(600)


def baslat():
    threading.Thread(target=_dongu, daemon=True, name="zk-yenileme").start()


@zk_bp.route("/")
def sayfa():
    return render_template("zk.html")


@zk_bp.route("/api/data")
def veri():
    d = son_durum()
    return jsonify({"veri": d, "yenileniyor": _durum["calisiyor"]})


@zk_bp.route("/api/yenile", methods=["POST"])
def elle_yenile():
    if _durum["calisiyor"]:
        return jsonify({"durum": "calisiyor"}), 202
    # Sitelere yük bindirmemek için: veri ya da son deneme 15 dk'dan yeniyse tekrar çekme
    son = _durum["son_deneme"]
    yakin_deneme = son and (datetime.now(timezone.utc) - son).total_seconds() < ELLE_YENILEME_ARALIK_DK * 60
    if yakin_deneme or not _eski_mi(ELLE_YENILEME_ARALIK_DK / 60):
        return jsonify({"durum": "guncel", "mesaj": f"Veri {ELLE_YENILEME_ARALIK_DK} dakikadan yeni."}), 200
    threading.Thread(target=_yenile, daemon=True).start()
    return jsonify({"durum": "basladi"}), 202


@zk_bp.route("/rapor.xlsx")
def excel():
    from zk.excel import rapor_olustur

    d = son_durum()
    if not d:
        return "Henüz veri yok", 404
    buf = io.BytesIO()
    rapor_olustur(d, buf)
    buf.seek(0)
    return send_file(
        buf,
        as_attachment=True,
        download_name=f"rakip_takibi_{d['tarih']}.xlsx",
        mimetype="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
    )
