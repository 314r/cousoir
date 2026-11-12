#!/usr/bin/env python3
"""Compares the catalogs of i18n/ with the strings Cousoir says.

    tools/i18n.py            what each catalog lacks, and what it has that
                             Cousoir no longer says; fails when English
                             lacks the plural forms of a counted string
    tools/i18n.py --missing fr   the strings i18n/fr.json lacks, as JSON to
                             translate
    tools/i18n.py --prune    drops from every catalog what Cousoir no longer
                             says

The strings are those of qsTr() in the QML and tr() in the C++, read by Qt's
lupdate. A catalog maps each, exactly as written there, to its translation;
a counted string ("%n books") to an object of plural forms.
"""
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parent.parent


def source_strings():
    """The strings Cousoir says: {text: counted}."""
    lupdate = (shutil.which("lupdate") or shutil.which("lupdate-qt6") or shutil.which("lupdate6")
               or shutil.which("lupdate", path="/usr/lib/qt6/bin:/usr/lib64/qt6/bin"))
    if not lupdate:
        # ctest's code for a test skipped.
        print("Qt's lupdate is not installed: nothing compared")
        sys.exit(77)
    sources = sorted(str(path) for pattern in ("*.qml", "*.cpp", "*.h") for path in ROOT.glob(pattern))
    with tempfile.TemporaryDirectory() as folder:
        ts = pathlib.Path(folder) / "cousoir.ts"
        subprocess.run([lupdate, "-no-obsolete", "-locations", "none", *sources, "-ts", str(ts)],
                       check=True, capture_output=True)
        strings = {}
        for message in ET.parse(ts).getroot().iter("message"):
            text = message.find("source").text or ""
            strings[text] = strings.get(text, False) or message.get("numerus") == "yes"
    return strings


def main():
    arguments = sys.argv[1:]
    strings = source_strings()
    counted = {text for text, numerus in strings.items() if numerus}
    failed = False
    for path in sorted((ROOT / "i18n").glob("*.json")):
        code = path.stem
        catalog = json.loads(path.read_text(encoding="utf-8"))
        # English is its own source: it only gives counted strings their forms.
        wanted = counted if code == "en" else set(strings)
        missing = [text for text in strings if text in wanted and text not in catalog]
        stale = [text for text in catalog if text not in wanted]
        if arguments[:1] == ["--missing"]:
            if arguments[1:] == [code]:
                json.dump({text: {"one": "", "other": ""} if text in counted else "" for text in missing},
                          sys.stdout, ensure_ascii=False, indent=2)
                print()
            continue
        if "--prune" in arguments and stale:
            catalog = {text: value for text, value in catalog.items() if text in wanted}
            path.write_text(json.dumps(catalog, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"{code}: {len(catalog)} strings, {len(missing)} missing, {len(stale)} "
              f"{'dropped' if '--prune' in arguments else 'no longer said'}")
        for text in missing[:20]:
            print(f"  missing: {text!r}")
        if len(missing) > 20:
            print(f"  … and {len(missing) - 20} more: tools/i18n.py --missing {code}")
        for text in stale:
            print(f"  no longer said: {text!r}")
        failed = failed or (code == "en" and bool(missing))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
