"""Takip edilen markalar. Yeni marka eklemek / ID düzeltmek için sadece burayı düzenle."""

MARKALAR = [
    {
        "ad": "TokenFlex",
        "appstore_id": 1576193179,
        "play_id": "com.tokenflexapp",
        "sikayetvar": "token-flex",
        "kampanya": {
            "url": "https://www.tokenflex.com.tr/kampanyalar",
            "link": r"/kampanyalar/[^/?#]+$",
        },
    },
    {
        "ad": "Setcard",
        "appstore_id": 882408961,
        "play_id": "com.setcard.nerede",
        "sikayetvar": "setcard",
        "kampanya": {
            "url": "https://www.setcard.com.tr/kampanyalar",
            "link": r"^/kampanyalar/[a-z0-9-]+/?$",
            "sayfalar": "https://www.setcard.com.tr/kampanyalar?page={n}",  # sonda / olmamalı: sayfadaki göreli linkler buna göre çözülüyor
        },
    },
    {
        "ad": "iWallet",
        "appstore_id": 1499316720,
        "play_id": "tr.com.iwallet.bireysel",
        "sikayetvar": "iwallet",
        "kampanya": None,  # web sitesinde kampanya sayfası yok, kampanyalar uygulama içinde
    },
    {
        "ad": "Moneypay / Migros Money",
        "appstore_id": 1541353571,
        "play_id": "com.colendi.money_pay",
        "sikayetvar": "moneypay",
        "kampanya": {
            "url": "https://www.money.com.tr/mc/kampanyalara-bak/tum-kampanyalar/53",
            "link": r"/Kampanyalar/Detay/",
        },
    },
    {
        "ad": "Edenred",
        "appstore_id": 553023473,
        "play_id": "com.edenred.TicketRestaurant",
        "sikayetvar": "ticket-restaurant",
        "kampanya": {
            "url": "https://www.edenred.com.tr/red-club",
            "link": r"/red-club(-business)?/[^/?#]+$",
        },
    },
    {
        "ad": "Multinet (MultiPay)",
        "appstore_id": 680381668,
        "play_id": "com.mobisoft.multimobil",
        "sikayetvar": "multinet",
        "kampanya": {
            "url": "https://multinet.com.tr/bireysel-kampanyalar",
            "link": r"/bireysel-kampanyalar/[^/?#]+$",
        },
    },
]

UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36"
