#!/usr/bin/env python3
"""Fetch a Pryon wake-word model set from Amazon's DAVS, the way the Echo's assetmgrd does (seen with src/tools/curlspy.c):
  GET https://api.amazonalexa.com/v2/deviceArtifacts/?artifactFilter=<url-quoted base64 of the request JSON>
  Authorization: Bearer <access token of a registered device>
The JSON answer carries a signed CloudFront downloadUrl that expires within minutes, so the request is the thing to keep.
  tools/davs-fetch.py [--ecids 1,2,...] <map.db> <key> [locale] [outdir]
    key: alexa echo computer amazon ziggy, or aed for the sound detection model (docs/re-aed.md); locale default de-DE
    --ecids: the wake word engine compatibility ids to ask for (default: donut's NS65741 engine).  An older engine lacks
             some (radar's has no 36, 37) and gets a model set it can load only when it asks with its own list;
             `pryon_test` prints it ("wakeword_ecids" in its attributes line).
aed: the request PuffinApp's AEDInventory::createRequest (0x43d898) builds: filters filterVersion "2", the engine's
     aed_ecids, modelClass "class-10" and the region (NA, EU or FE; here from the locale). The artifact type and key are
     two global strings "AED" (0xf4274, 0xf42c0); the spelling DAVS wants is not known, so the variants are tried in turn.
     --ecids then means the aed_ecids (`aed_test` / `pryon_test` print them in the attributes line).
map.db is /data/ace/kvstorage/map.db of the registered Echo; its access token lasts an hour after the device fetched it.
"""
import base64, json, pathlib, shutil, sqlite3, sys, tarfile, urllib.error, urllib.parse, urllib.request

ENGINE_IDS = [str(i) for i in (1, 10, 11, 12, 13, 14, 15, 16, 17, 19, 2, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33,
                               34, 35, 36, 37, 4, 5, 6, 7, 8, 9)]      # what NS65741's PuffinApp sends
AED_IDS = ["1", "2", "3", "5", "6", "7"]                                 # NS65741 libpryon's aed_ecids
AED_NAMES = [("AED", "AED"), ("aed", "aed"), ("AED", "aed"), ("aed", "AED")]
# DAVS region by locale; PuffinApp picks one of NA, EU, FE
REGION = {"en-US": "NA", "en-CA": "NA", "fr-CA": "NA", "es-MX": "NA", "pt-BR": "NA", "ja-JP": "FE", "en-AU": "FE", "en-IN": "FE"}

def ask(token, req):
    enc = urllib.parse.quote(base64.b64encode(json.dumps(req, separators=(",", ":")).encode()).decode(), safe="")
    url = "https://api.amazonalexa.com/v2/deviceArtifacts/?artifactFilter=" + enc
    with urllib.request.urlopen(urllib.request.Request(url, headers={"Authorization": "Bearer " + token}), timeout=30) as r:
        return json.load(r)

def main():
    args, ecids = sys.argv[1:], None
    if args[:1] == ["--ecids"] and len(args) > 1:
        ecids, args = [i for i in args[1].split(",") if i], args[2:]
    if len(args) < 2: sys.exit(__doc__)
    db, key = args[0], args[1]
    locale = args[2] if len(args) > 2 else "de-DE"
    out = pathlib.Path(args[3] if len(args) > 3 else "device-logs/models")
    token = sqlite3.connect(db).execute("select cast(value as text) from deviceData where key='access_token'").fetchone()[0]
    if key == "aed":
        region = REGION.get(locale, "EU")
        info = None
        for typ, k in AED_NAMES:
            req = {"artifactType": typ, "artifactKey": k,
                   "filters": {"filterVersion": ["2"], "engineCompatibilityIdList": ecids or AED_IDS,
                               "modelClass": ["class-10"], "location": [region]}}
            try:
                info = ask(token, req); break
            except urllib.error.HTTPError as e:
                print(f"aed: type {typ} key {k}: HTTP {e.code} {e.read()[:200]!r}", file=sys.stderr)
        if info is None: sys.exit("aed: DAVS has no model under any of the names tried")
        dest = out / f"aed-{region}"
    else:
        req = {"artifactType": "wakeword", "artifactKey": key,
               "filters": {"engineCompatibilityIdList": ecids or ENGINE_IDS, "locale": [locale], "modelClass": ["B"]}}
        info = ask(token, req)
        dest = out / f"{key}-{locale}"
    # fetched before: Amazon may have published a newer one since; the artifact id says whether it did
    old = json.loads((dest / "davs.json").read_text()).get("artifactIdentifier") if (dest / "davs.json").exists() else None
    new = info.get("artifactIdentifier", "?")
    if old == new and (dest / "unpacked").is_dir():
        print(f"{key} {locale}: unchanged, id {new} -> {dest}")
        return
    dest.mkdir(parents=True, exist_ok=True)
    (dest / "davs.json").write_text(json.dumps(info, indent=1))
    tgz = dest / "artifact.tar.gz"
    with urllib.request.urlopen(info["downloadUrl"], timeout=120) as r: tgz.write_bytes(r.read())
    shutil.rmtree(dest / "unpacked", ignore_errors=True)     # an update: no file of the old one may stay
    with tarfile.open(tgz) as t: t.extractall(dest / "unpacked", filter="data")
    print(f"{key} {locale}: {tgz.stat().st_size} bytes, id {new}{f' (updated, was {old})' if old else ''} -> {dest}")

if __name__ == "__main__":
    main()
