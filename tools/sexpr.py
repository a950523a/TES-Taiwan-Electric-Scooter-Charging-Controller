#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""KiCad s-expression 的最小解析／輸出。

只支援 KiCad 檔案實際會出現的語法：巢狀括號、雙引號字串（\\" 與 \\ 跳脫）、
裸符號（symbol）與數字。夠用，而且不必為了讀一個檔案拉一整包相依。
"""
import io, re

_TOKEN = re.compile(r'\s*(?:(\()|(\))|"((?:[^"\\]|\\.)*)"|([^\s()"]+))')


class Sym(str):
    """裸符號。和字串分開，輸出時才知道不要加引號。"""
    __slots__ = ()


def unescape(s):
    return s.replace(chr(92) + chr(34), chr(34)).replace(chr(92) * 2, chr(92))


def escape(s):
    return s.replace(chr(92), chr(92) * 2).replace(chr(34), chr(92) + chr(34))


def loads(text):
    pos, stack, cur = 0, [], None
    n = len(text)
    while pos < n:
        m = _TOKEN.match(text, pos)
        if not m:
            break
        pos = m.end()
        if m.group(1):
            new = []
            if cur is not None:
                cur.append(new)
            stack.append(cur)
            cur = new
        elif m.group(2):
            done = cur
            cur = stack.pop()
            if cur is None:
                return done
        elif m.group(3) is not None:
            cur.append(unescape(m.group(3)))
        else:
            cur.append(Sym(m.group(4)))
    return cur


def load(path):
    return loads(io.open(path, encoding="utf-8").read())


def dumps(node, indent=0):
    if isinstance(node, Sym):
        return str(node)
    if isinstance(node, str):
        return chr(34) + escape(node) + chr(34)
    if isinstance(node, float):
        s = ("%.4f" % node).rstrip("0").rstrip(".")
        return s if s not in ("", "-0") else "0"
    if isinstance(node, int):
        return str(node)
    # 串列：子節點全是原子就排成一行，否則每個子節點自己一行
    parts = [dumps(c, indent + 1) for c in node]
    flat = all(not isinstance(c, list) for c in node)
    if flat or sum(len(p) for p in parts) < 70:
        return "(" + " ".join(parts) + ")"
    pad = "\t" * (indent + 1)
    head = parts[0]
    rest = "".join("\n" + pad + p for p in parts[1:])
    return "(" + head + rest + "\n" + "\t" * indent + ")"


def find(node, tag):
    """回傳第一個 (tag ...) 子節點。"""
    for c in node:
        if isinstance(c, list) and c and c[0] == tag:
            return c
    return None


def findall(node, tag):
    return [c for c in node
            if isinstance(c, list) and c and c[0] == tag]
