#!/usr/bin/env python3
"""
doc_meta.py — metadatos de las guias para la navegacion de la documentacion.

Lee docs/guides/*.md y escribe docs/html/doc_meta.js, que carga
docs/theme/haruka_docs.js (script tag, para que funcione tambien con file://).
De cada guia saca:

  * id      -- la pagina de Doxygen: `# Titulo {#id}` (la guia principal es `index`)
  * title   -- el titulo
  * path    -- la ruta del .md en el repo
  * date    -- la ultima modificacion: el commit que la toco, o la fecha del fichero
               si tiene cambios sin commitear
  * pin     -- `<!-- doc: pin -->` en el .md

Y COMPRUEBA la plantilla, que es lo que evita que una guia se quede vieja sin que
nadie lo note:

  * estan las secciones de la plantilla, en orden;
  * cada fichero citado en "Archivos principales" existe.

Uso:  python3 tools/doc_meta.py [--check]
      --check: sale con error si alguna guia no cumple (para CI o antes de commitear).
"""
import argparse
import datetime
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GUIDES = os.path.join("docs", "guides")
OUT = os.path.join(ROOT, "docs", "html", "doc_meta.js")
MAINPAGE = os.path.join(GUIDES, "ARQUITECTURA.md")   # USE_MDFILE_AS_MAINPAGE del Doxyfile

SECTIONS = ["Cómo funciona", "Archivos principales", "Opciones", "Cómo usarlo"]
FILE_SECTION = "Archivos principales"

RE_TITLE = re.compile(r"^#\s+(.+?)\s*\{#([A-Za-z0-9_]+)\}\s*$")
RE_PIN = re.compile(r"<!--\s*doc:\s*([^>]*?)\s*-->")
RE_H2 = re.compile(r"^##\s+(.+?)\s*$")
RE_FILECELL = re.compile(r"^\|\s*`([^`]+)`")


def git(*args):
    try:
        return subprocess.run(["git", *args], cwd=ROOT, capture_output=True,
                              text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def modified(path):
    """Fecha del ultimo commit; la del fichero si hay cambios locales o no esta en git."""
    dirty = git("status", "--porcelain", "--", path)
    if not dirty:
        d = git("log", "-1", "--format=%cs", "--", path)
        if d:
            return d
    ts = os.path.getmtime(os.path.join(ROOT, path))
    return datetime.date.fromtimestamp(ts).isoformat()


def parse(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        raw = f.read().splitlines()
    # Lo que va dentro de ``` es ejemplo, no estructura: no cuenta como seccion ni como fichero.
    lines, fenced = [], False
    for ln in raw:
        if ln.lstrip().startswith("```"):
            fenced = not fenced
            continue
        if not fenced:
            lines.append(ln)

    problems = []
    title = page = None
    for ln in lines:
        m = RE_TITLE.match(ln)
        if m:
            title, page = m.group(1), m.group(2)
            break
    if not title:
        problems.append("falta la primera linea `# Titulo {#id}`")
        title = os.path.splitext(os.path.basename(path))[0]
        page = title.lower()
    if path == MAINPAGE:
        page = "index"

    flags = set()
    for m in RE_PIN.finditer("\n".join(lines)):
        flags.update(m.group(1).split())

    heads = [m.group(1) for m in map(RE_H2.match, lines) if m]
    found = [h for h in heads if h in SECTIONS]
    if found != SECTIONS:
        missing = [s for s in SECTIONS if s not in heads]
        if missing:
            problems.append("faltan secciones: " + ", ".join(missing))
        else:
            problems.append("secciones fuera de orden: " + " / ".join(found))

    section = None
    for ln in lines:
        m = RE_H2.match(ln)
        if m:
            section = m.group(1)
            continue
        if section != FILE_SECTION:
            continue
        c = RE_FILECELL.match(ln)
        if c:
            ref = c.group(1).split(":")[0]
            if not any(os.path.exists(os.path.join(ROOT, p)) for p in expand(ref)):
                problems.append(f"no existe: {ref}")

    return {"id": page, "title": title, "path": path, "date": modified(path),
            "pin": "pin" in flags}, problems


def html_paths(html_dir):
    """id -> ruta del .html relativa a html/ (CREATE_SUBDIRS reparte las paginas en d0/d1a/...)."""
    out = {}
    for dp, _, fs in os.walk(html_dir):
        for f in fs:
            if f.endswith(".html"):
                rel = os.path.relpath(os.path.join(dp, f), html_dir).replace(os.sep, "/")
                out.setdefault(f[:-5], rel)
    return out


def expand(ref):
    """`Foo.{h,cpp}` -> Foo.h, Foo.cpp (basta con que exista uno)."""
    m = re.match(r"^(.*)\{([^}]+)\}(.*)$", ref)
    if not m:
        return [ref]
    return [m.group(1) + alt + m.group(3) for alt in m.group(2).split(",")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="error si una guia no cumple")
    args = ap.parse_args()

    files = sorted(os.path.join(GUIDES, f) for f in os.listdir(os.path.join(ROOT, GUIDES))
                   if f.endswith(".md"))
    pages, bad = [], 0
    for p in files:
        meta, problems = parse(p)
        pages.append(meta)
        for pr in problems:
            print(f"[doc_meta] {p}: {pr}")
        bad += bool(problems)

    pages.sort(key=lambda m: (not m["pin"], m["title"].lower()))
    if os.path.isdir(os.path.dirname(OUT)):
        hrefs = html_paths(os.path.dirname(OUT))
        for m in pages:
            m["href"] = hrefs.get(m["id"], m["id"] + ".html")
        with open(OUT, "w", encoding="utf-8") as f:
            f.write("harukaDocsRegister(" + json.dumps(pages, ensure_ascii=False) + ");\n")
        print(f"[doc_meta] {len(pages)} guias -> {os.path.relpath(OUT, ROOT)}")
    else:
        print("[doc_meta] sin docs/html (falta doxygen): solo se comprueban las guias")

    if bad:
        print(f"[doc_meta] {bad} guia(s) no cumplen la plantilla (ver @ref guia_docs)")
    return 1 if (bad and args.check) else 0


if __name__ == "__main__":
    sys.exit(main())
