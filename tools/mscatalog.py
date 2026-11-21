#!/usr/bin/env python3
"""Find an update's download links in the Microsoft Update Catalog.

    python3 tools/mscatalog.py <search words> [--match <text>]...

Prints one line per update found: its title, then its file URL(s). --match
keeps only titles containing every given text (case-insensitive). Reads the
catalog's own pages (Search.aspx, DownloadDialog.aspx); downloads nothing.
"""
import html, json, re, sys, urllib.parse, urllib.request

BASE = 'https://www.catalog.update.microsoft.com/'
UA = {'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)'}


def get(url, data=None):
    req = urllib.request.Request(url, data=data, headers=UA)
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read().decode('utf-8', 'replace')


def search(q):
    page = get(BASE + 'Search.aspx?q=' + urllib.parse.quote(q))
    rows = []
    for m in re.finditer(r'<a id=[\'"]([0-9a-f-]{36})_link[\'"][^>]*>\s*(.*?)\s*</a>', page, re.S):
        rows.append((m.group(1), html.unescape(re.sub(r'\s+', ' ', m.group(2))).strip()))
    return rows


def files(uid):
    body = urllib.parse.urlencode({'updateIDs': json.dumps([{'size': 0, 'languages': '', 'uidInfo': uid, 'updateID': uid}])})
    page = get(BASE + 'DownloadDialog.aspx', body.encode())
    return re.findall(r"downloadInformation\[\d+\]\.files\[\d+\]\.url = '([^']+)'", page)


def main():
    args = sys.argv[1:]
    match = []
    while '--match' in args:
        i = args.index('--match')
        match.append(args[i + 1].lower())
        del args[i:i + 2]
    for uid, title in search(' '.join(args)):
        if all(m in title.lower() for m in match):
            print(title)
            for u in files(uid):
                print('   ', u)


if __name__ == "__main__":
    main()
