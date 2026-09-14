#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""從 EasyEDA Pro 專案檔（.eprj，其實是 SQLite）取出每個符號的腳位座標。

為什麼需要：easyeda2kicad 轉符號庫時，**有些符號把 Y 取負、有些沒有**。
猜錯的後果不是「接不到」，而是接到同一顆的另一支腳 —— 電容 1/2 腳對調、
TJA1051 的 CANH/CANL 互換 —— 網表看起來還很合理。與其實測猜慣例，不如
直接把原稿的腳位拿出來和符號庫比一比。

回傳 {符號名: {腳號: (x, y)}}，單位同 .esch（1 單位 = 10 mil）。
"""
import base64, collections, io, json, os, re, sqlite3, zlib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EPRJ = os.path.join(REPO, "docs", "PCB", "TES_Controller_V1.3.eprj")


def key(name):
    """符號名正規化 —— KiCad 庫把 '/' '(' 之類換成底線，比對前要抹平。"""
    return re.sub(r"[^a-z0-9]", "", (name or "").lower())


def load(path=EPRJ):
    db = sqlite3.connect(path)
    out = {}
    for title, data in db.execute(
            "select title, dataStr from components where docType=2"):
        if not data or not data.startswith("base64"):
            continue
        raw = zlib.decompress(base64.b64decode(data[6:]),
                              16 + zlib.MAX_WBITS).decode("utf-8")
        pins, number = {}, {}
        for ln in raw.splitlines():
            ln = ln.strip()
            if not ln.startswith('["PIN"') and not ln.startswith('["ATTR"'):
                continue
            try:
                r = json.loads(ln)
            except Exception:
                continue
            if r[0] == "PIN":
                pins[r[1]] = (r[4], r[5])
            elif len(r) > 4 and r[3] == "NUMBER" and r[2] in pins:
                number[r[2]] = str(r[4])
        if pins:
            out[key(title)] = {number.get(eid, eid): xy
                               for eid, xy in pins.items()}
    db.close()
    return out


def device_attrs(path=EPRJ):
    """{裝置 uuid: {屬性: 值}}。圖上顯示的文字是 "={Value}" 這種樣板，
    要靠這張表才解得開。"""
    db = sqlite3.connect(path)
    out = collections.defaultdict(dict)
    for k, v, u in db.execute("select key, value, device_uuid from attributes"):
        out[u][k] = v
    db.close()
    return out


def resolve(text, attrs):
    """把 "={Value}" 這種樣板換成實際內容；換不出來就原樣回去。"""
    if not text:
        return text
    m = re.fullmatch(r"=\{(.+)\}", text.strip())
    if m:
        return attrs.get(m.group(1)) or ""
    return text


if __name__ == "__main__":
    import sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import sch_gen as G
    U = 0.254
    eda = load()
    _l, _b, by_name = G.load_lib()
    same = neg = odd = miss = 0
    for name, info in sorted(by_name.items()):
        e = eda.get(key(name))
        if not e or not info["pins"]:
            miss += 1
            continue
        s_same = s_neg = True
        for pad, (px, py, _r) in info["pins"].items():
            if pad not in e:
                s_same = s_neg = False
                break
            ex, ey = e[pad][0] * U, e[pad][1] * U
            if abs(px - ex) > 0.01 or abs(py - ey) > 0.01:
                s_same = False
            if abs(px - ex) > 0.01 or abs(py + ey) > 0.01:
                s_neg = False
        if s_same and s_neg:
            same += 1              # 全部 y=0，兩者相同
        elif s_same:
            same += 1
        elif s_neg:
            neg += 1
        else:
            odd += 1
            print("  對不上 %s" % name)
    print("Y 相同 %d、Y 取負 %d、對不上 %d、找不到原稿 %d"
          % (same, neg, odd, miss))
