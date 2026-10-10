#!/usr/bin/env python3
"""Extrae el arbol de ejecucion de cada funcion del motor y lo publica junto a
la documentacion de Doxygen.

Fuentes de datos:
  * docs/xml   -> XML de Doxygen: que miembro corresponde a cada pagina HTML.
  * libclang   -> AST real de cada unidad de traduccion (build/compile_commands.json):
                  llamadas, condicionales, bucles, declaraciones y asignaciones.

Salida: docs/html/flow/<compound>.js, un fichero por pagina, cargado bajo demanda
por docs/theme/haruka_flow.js (script tag, para que funcione tambien con file://).

Uso:  python3 tools/doc_flow.py [--jobs N] [--build build] [--limit N]
Requiere:  pip install --user libclang
"""

from __future__ import annotations

import argparse
import glob
import json
import multiprocessing as mp
import os
import re
import sys
import time
import xml.etree.ElementTree as ET

try:
    import clang.cindex as ci
    from clang.cindex import CursorKind as CK
except ImportError:  # pragma: no cover
    sys.exit("Falta libclang. Instalalo con:  pip install --user libclang")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_ROOT = os.path.join(ROOT, "src")
XML_DIR = os.path.join(ROOT, "docs", "xml")
HTML_DIR = os.path.join(ROOT, "docs", "html")
OUT_DIR = os.path.join(HTML_DIR, "flow")

MAX_LABEL = 140

# --------------------------------------------------------------------------
# libclang: cabeceras internas del compilador
# --------------------------------------------------------------------------


def builtin_include_args() -> list[str]:
    """libclang de pip no trae sus cabeceras (stddef.h y companyia)."""
    cands = sorted(glob.glob("/usr/lib/clang/*/include"), reverse=True)
    cands += sorted(glob.glob("/usr/lib64/clang/*/include"), reverse=True)
    for d in cands:
        if os.path.exists(os.path.join(d, "stddef.h")):
            return ["-isystem", d]
    return []


BUILTIN_ARGS = builtin_include_args()


def tu_args(cmd) -> list[str]:
    """Argumentos de compile_commands.json depurados para libclang."""
    args, skip = [], False
    raw = list(cmd.arguments)[1:]
    for a in raw:
        if skip:
            skip = False
            continue
        if a == "-o":
            skip = True
            continue
        if a in ("-c", "-pipe"):
            continue
        if a.startswith("-M"):  # dependencias: irrelevantes y rompen el parse
            continue
        if os.path.abspath(os.path.join(cmd.directory, a)) == os.path.abspath(
            os.path.join(cmd.directory, cmd.filename)
        ):
            continue
        args.append(a)
    return args + BUILTIN_ARGS + ["-ferror-limit=0", "-Wno-everything"]


# --------------------------------------------------------------------------
# Doxygen XML: miembros, anclas y paginas
# --------------------------------------------------------------------------


class DoxIndex:
    """Indice del XML de Doxygen: donde vive cada funcion y a que pagina apunta.

    Con CREATE_SUBDIRS los refid ya incluyen el subdirectorio ("d5/d4c/classFoo"),
    asi que la URL de una pagina es directamente <refid>.html.
    """

    def __init__(self) -> None:
        # (fichero_real, linea_final_del_cuerpo) -> (pagina, ancla, nombre)
        self.by_body: dict[tuple[str, int], tuple[str, str, str]] = {}
        # (fichero_real, linea_de_declaracion) -> url relativa al raiz de html
        self.by_decl: dict[tuple[str, int], str] = {}
        # fichero_real -> pagina de codigo fuente
        self.src_page: dict[str, str] = {}

    def load(self) -> None:
        index = os.path.join(XML_DIR, "index.xml")
        if not os.path.exists(index):
            sys.exit(
                "No existe docs/xml/index.xml. Ejecuta antes: doxygen Doxyfile "
                "(con GENERATE_XML = YES)"
            )
        for comp in ET.parse(index).getroot().findall("compound"):
            refid = comp.get("refid")
            path = os.path.join(XML_DIR, refid + ".xml")
            if os.path.exists(path):
                self._load_compound(path, refid)

    def _load_compound(self, path: str, refid: str) -> None:
        try:
            root = ET.parse(path).getroot()
        except ET.ParseError:
            return
        cdef = root.find("compounddef")
        if cdef is None:
            return
        if cdef.get("kind") == "file":
            loc = cdef.find("location")
            if loc is not None and loc.get("file"):
                real = os.path.realpath(os.path.join(ROOT, loc.get("file")))
                self.src_page[real] = refid + "_source.html"
        for m in cdef.iter("memberdef"):
            if m.get("kind") not in ("function", "slot", "signal"):
                continue
            mid = m.get("id") or ""
            if "_1" not in mid:
                continue
            owner, anchor = mid.rsplit("_1", 1)
            page = os.path.basename(owner)
            name = (m.findtext("name") or "").strip()
            loc = m.find("location")
            if loc is None:
                continue
            if loc.get("file") and loc.get("line"):
                real = os.path.realpath(os.path.join(ROOT, loc.get("file")))
                self.by_decl.setdefault(
                    (real, int(loc.get("line"))), owner + ".html#" + anchor
                )
            if loc.get("bodyfile") and loc.get("bodyend"):
                try:
                    end = int(loc.get("bodyend"))
                except ValueError:
                    continue
                if end <= 0:
                    continue
                realb = os.path.realpath(os.path.join(ROOT, loc.get("bodyfile")))
                self.by_body[(realb, end)] = (page, anchor, name)


# --------------------------------------------------------------------------
# Analisis del cuerpo de una funcion
# --------------------------------------------------------------------------

ASSIGN_OPS = {
    "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=",
}

LOOPS = {CK.FOR_STMT, CK.WHILE_STMT, CK.DO_STMT, CK.CXX_FOR_RANGE_STMT}
CALLS = {CK.CALL_EXPR, CK.CXX_NEW_EXPR, CK.CXX_DELETE_EXPR}
TRANSPARENT = {
    CK.UNEXPOSED_EXPR,
    CK.PAREN_EXPR,
    CK.CSTYLE_CAST_EXPR,
    CK.CXX_STATIC_CAST_EXPR,
    CK.CXX_CONST_CAST_EXPR,
    CK.CXX_REINTERPRET_CAST_EXPR,
    CK.CXX_FUNCTIONAL_CAST_EXPR,
}


def squash(s: str) -> str:
    s = re.sub(r"//[^\n]*", " ", s)
    s = re.sub(r"/\*.*?\*/", " ", s, flags=re.S)
    s = re.sub(r"\s+", " ", s).strip()
    return s


def snippet(src: bytes, cur, limit: int = MAX_LABEL) -> str:
    e = cur.extent
    try:
        raw = src[e.start.offset : e.end.offset].decode("utf-8", "replace")
    except Exception:
        return cur.spelling or ""
    t = squash(raw)
    return t if len(t) <= limit else t[: limit - 1] + "…"


def head(src: bytes, cur, body=None, limit: int = MAX_LABEL) -> str:
    """Cabecera de una sentencia compuesta, sin arrastrar su cuerpo.

    Con `body` (el cursor del bloque interno) el corte es exacto incluso cuando
    el bucle no lleva llaves; sin el, se corta por la primera llave."""
    start = cur.extent.start.offset
    end = body.extent.start.offset if body is not None else cur.extent.end.offset
    if end <= start:
        end = cur.extent.end.offset
    t = squash(src[start:end].decode("utf-8", "replace"))
    cut = t.find("{")
    if cut > 0:
        t = t[:cut]
    t = t.strip()
    return t if len(t) <= limit else t[: limit - 1] + "…"


def top_level_assign(cur) -> str | None:
    """Operador de asignacion en el nivel superior de la expresion, via tokens."""
    depth = 0
    for tok in cur.get_tokens():
        s = tok.spelling
        if s in "([{":
            depth += 1
        elif s in ")]}":
            depth -= 1
        elif depth == 0 and s in ASSIGN_OPS:
            return s
    return None


def lhs_text(src: bytes, cur, op: str) -> str:
    t = snippet(src, cur, 4000)
    i = t.find(op)
    return (t[:i].strip() if i > 0 else t).strip()


def drop_self_ctor(nodes: list, var: str) -> list:
    """Quita el nodo del constructor de la propia variable ("pos(0.0)"), que es
    ruido en el arbol, pero conserva las llamadas que hubiera dentro."""
    out = []
    for n in nodes:
        t = n.get("t", "")
        if n["k"] == "call" and var and (t == var or t.startswith(var + "(")):
            out.extend(n.get("c", []))
        else:
            out.append(n)
    return out


class Analyzer:
    def __init__(self, src: bytes, dox: dict) -> None:
        self.src = src
        self.decl_urls = dox  # (file, line) -> url

    # -- expresiones ------------------------------------------------------
    def call_url(self, cur):
        ref = cur.referenced
        if ref is None:
            return None
        loc = ref.location
        if loc is None or loc.file is None:
            return None
        key = (os.path.realpath(loc.file.name), loc.line)
        return self.decl_urls.get(key)

    def calls_in(self, cur, out: list, budget: list) -> None:
        """Llamadas contenidas en una expresion, preservando el anidamiento."""
        for ch in cur.get_children():
            self.visit_expr(ch, out, budget)

    def visit_expr(self, ch, out: list, budget: list) -> None:
        if budget[0] <= 0:
            return
        if ch.kind not in CALLS:
            self.calls_in(ch, out, budget)
            return
        ref = ch.referenced
        name = (ref.spelling if ref else ch.spelling) or ""
        if name.startswith("operator"):
            op = name[len("operator"):].strip()
            if op in ASSIGN_OPS:
                # a *= b sobre un tipo con operador propio sigue siendo una asignacion
                kids: list = []
                self.calls_in(ch, kids, budget)
                out.append({
                    "k": "assign",
                    "t": snippet(self.src, ch),
                    "n": lhs_text(self.src, ch, op),
                    "l": ch.location.line,
                    **({"c": kids} if kids else {}),
                })
                return
            if op != "()":
                self.calls_in(ch, out, budget)
                return
            # operator(): std::function, functores y lambdas si son una llamada real
        if ch.kind == CK.CALL_EXPR and not name:
            # conversiones implicitas y similares: transparentes
            self.calls_in(ch, out, budget)
            return
        budget[0] -= 1
        text = snippet(self.src, ch, 90)
        if name.startswith("operator"):  # operator(): el nombre util es el callable
            name = text.split("(")[0].strip() or name
        node = {"k": "call", "t": text, "n": name, "l": ch.location.line}
        url = self.call_url(ch)
        if url:
            node["u"] = url
        kids: list = []
        self.calls_in(ch, kids, budget)
        if kids:
            node["c"] = kids
        out.append(node)

    def expr_nodes(self, cur, line: int) -> list:
        budget = [40]
        op = top_level_assign(cur) if cur.kind in (
            CK.BINARY_OPERATOR,
            CK.COMPOUND_ASSIGNMENT_OPERATOR,
        ) else None
        if op:
            kids: list = []
            self.calls_in(cur, kids, budget)
            return [{
                "k": "assign",
                "t": snippet(self.src, cur),
                "n": lhs_text(self.src, cur, op),
                "l": line,
                **({"c": kids} if kids else {}),
            }]
        if cur.kind == CK.UNARY_OPERATOR:
            toks = [t.spelling for t in cur.get_tokens()]
            if "++" in toks or "--" in toks:
                return [{
                    "k": "assign",
                    "t": snippet(self.src, cur),
                    "n": snippet(self.src, cur, 60),
                    "l": line,
                }]
        out: list = []
        self.visit_expr(cur, out, budget)
        return out

    # -- sentencias -------------------------------------------------------
    def stmts(self, cur, depth: int) -> list:
        out: list = []
        if depth > 12:
            return out
        for ch in cur.get_children():
            out.extend(self.stmt(ch, depth))
        return out

    def stmt(self, cur, depth: int) -> list:
        k = cur.kind
        line = cur.location.line

        if k == CK.COMPOUND_STMT:
            return self.stmts(cur, depth)

        if k == CK.DECL_STMT:
            out = []
            for d in cur.get_children():
                if d.kind in (CK.VAR_DECL,):
                    kids: list = []
                    self.calls_in(d, kids, [40])
                    kids = drop_self_ctor(kids, d.spelling)
                    out.append({
                        "k": "var",
                        "t": snippet(self.src, d),
                        "n": d.spelling,
                        "l": line,
                        **({"c": kids} if kids else {}),
                    })
                else:
                    out.append({"k": "var", "t": snippet(self.src, d), "l": line})
            return out

        if k == CK.IF_STMT:
            kids = list(cur.get_children())
            cond = kids[0] if kids else None
            then = kids[1] if len(kids) > 1 else None
            els = kids[2] if len(kids) > 2 else None
            node = {
                "k": "if",
                "t": "if (" + (snippet(self.src, cond, 110) if cond is not None else "") + ")",
                "l": line,
            }
            pre: list = []
            if cond is not None:
                self.calls_in(cond, pre, [20])
            body = self.stmt(then, depth + 1) if then is not None else []
            if pre:
                body = pre + body
            if body:
                node["c"] = body
            out = [node]
            if els is not None:
                if els.kind == CK.IF_STMT:
                    sub = self.stmt(els, depth)
                    if sub:
                        sub[0]["t"] = "else " + sub[0]["t"]
                    out.extend(sub)
                else:
                    eb = self.stmt(els, depth + 1)
                    out.append({"k": "if", "t": "else", "l": els.location.line,
                                **({"c": eb} if eb else {})})
            return out

        if k in LOOPS:
            body = list(cur.get_children())
            inner = body[-1] if body else None
            node = {"k": "loop", "t": head(self.src, cur, inner, 120), "l": line}
            kids = self.stmt(inner, depth + 1) if inner is not None else []
            if kids:
                node["c"] = kids
            return [node]

        if k == CK.SWITCH_STMT:
            kids = list(cur.get_children())
            cond = kids[0] if kids else None
            node = {
                "k": "switch",
                "t": "switch (" + (snippet(self.src, cond, 90) if cond is not None else "") + ")",
                "l": line,
            }
            body = self.stmt(kids[-1], depth + 1) if len(kids) > 1 else []
            if body:
                node["c"] = body
            return [node]

        if k in (CK.CASE_STMT, CK.DEFAULT_STMT):
            kids = list(cur.get_children())
            label = "default:" if k == CK.DEFAULT_STMT else (
                "case " + (snippet(self.src, kids[0], 60) if kids else "") + ":"
            )
            rest = kids[1:] if k == CK.CASE_STMT else kids
            body: list = []
            for r in rest:
                body.extend(self.stmt(r, depth + 1))
            return [{"k": "case", "t": label, "l": line, **({"c": body} if body else {})}]

        if k == CK.RETURN_STMT:
            kids: list = []
            self.calls_in(cur, kids, [20])
            return [{
                "k": "return",
                "t": snippet(self.src, cur),
                "l": line,
                **({"c": kids} if kids else {}),
            }]

        if k in (CK.BREAK_STMT, CK.CONTINUE_STMT, CK.GOTO_STMT):
            return [{"k": "flow", "t": snippet(self.src, cur, 40), "l": line}]

        if k == CK.CXX_TRY_STMT:
            body = self.stmts(cur, depth + 1)
            return [{"k": "block", "t": "try", "l": line, **({"c": body} if body else {})}]

        if k == CK.CXX_CATCH_STMT:
            body = self.stmts(cur, depth + 1)
            return [{"k": "block", "t": head(self.src, cur, None, 80), "l": line,
                     **({"c": body} if body else {})}]

        if k == CK.NULL_STMT:
            return []

        return self.expr_nodes(cur, line)


def count(nodes: list) -> tuple[int, int]:
    calls = steps = 0
    for n in nodes:
        if n["k"] == "call":
            calls += 1
        if n["k"] in ("if", "loop", "switch", "case", "block"):
            steps += 1
        c, s = count(n.get("c", []))
        calls += c
        steps += s
    return calls, steps


FUNC_KINDS = {
    CK.FUNCTION_DECL,
    CK.CXX_METHOD,
    CK.CONSTRUCTOR,
    CK.DESTRUCTOR,
    CK.FUNCTION_TEMPLATE,
    CK.CONVERSION_FUNCTION,
}


def analyze_tu(job):
    filename, args, decl_urls = job
    try:
        tu = ci.Index.create().parse(filename, args=args)
    except Exception as e:  # pragma: no cover
        return {"__error__": [filename, str(e)]}
    if tu is None:
        return {}
    res = {}
    src_cache: dict[str, bytes] = {}

    def visit(cur):
        for ch in cur.get_children():
            loc = ch.location
            if loc.file is None:
                continue
            path = os.path.realpath(loc.file.name)
            if not path.startswith(SRC_ROOT):
                if ch.kind in (CK.NAMESPACE, CK.CLASS_DECL, CK.STRUCT_DECL):
                    continue
                continue
            if ch.kind in FUNC_KINDS and ch.is_definition():
                end = ch.extent.end.line
                key = f"{path}:{end}"
                if key in res:
                    continue
                body = None
                for c2 in ch.get_children():
                    if c2.kind == CK.COMPOUND_STMT:
                        body = c2
                if body is None:
                    continue
                if path not in src_cache:
                    try:
                        with open(path, "rb") as fh:
                            src_cache[path] = fh.read()
                    except OSError:
                        src_cache[path] = b""
                an = Analyzer(src_cache[path], decl_urls)
                try:
                    nodes = an.stmts(body, 0)
                except RecursionError:
                    nodes = []
                res[key] = {
                    "start": ch.extent.start.line,
                    "sig": ch.displayname,
                    "nodes": nodes,
                }
            elif ch.kind in (CK.NAMESPACE, CK.CLASS_DECL, CK.STRUCT_DECL,
                             CK.CLASS_TEMPLATE, CK.UNEXPOSED_DECL,
                             CK.CLASS_TEMPLATE_PARTIAL_SPECIALIZATION):
                visit(ch)

    visit(tu.cursor)
    return res


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 1))
    ap.add_argument("--limit", type=int, default=0, help="solo N unidades (pruebas)")
    ap.add_argument("--filter", default="", help="subcadena del fichero .cpp")
    args = ap.parse_args()

    t0 = time.time()
    dox = DoxIndex()
    dox.load()
    print(f"[flow] Doxygen XML: {len(dox.by_body)} cuerpos, {len(dox.by_decl)} declaraciones "
          f"({time.time()-t0:.1f}s)")

    db = ci.CompilationDatabase.fromDirectory(os.path.join(ROOT, args.build))
    cmds = []
    seen = set()
    for c in db.getAllCompileCommands():
        f = os.path.realpath(os.path.join(c.directory, c.filename))
        if f in seen or not f.startswith(SRC_ROOT):
            continue
        if args.filter and args.filter not in f:
            continue
        seen.add(f)
        cmds.append((f, tu_args(c)))
    # Ficheros de src/ que no compila ningun target (src/main.cpp lo excluye
    # CMakeLists a proposito) tambien estan documentados: se analizan con los
    # flags de otra unidad, que en este proyecto comparten includes y defines.
    if cmds and not args.filter:
        covered = {f for f, _ in cmds}
        borrowed = cmds[0][1]
        extra = []
        for dirpath, _dirs, files in os.walk(SRC_ROOT):
            for fn in files:
                if not fn.endswith(".cpp"):
                    continue
                full = os.path.realpath(os.path.join(dirpath, fn))
                if full not in covered:
                    extra.append((full, borrowed))
        if extra:
            print("[flow] fuera de compile_commands.json (flags prestados): "
                  + ", ".join(os.path.relpath(f, ROOT) for f, _ in extra))
            cmds += extra

    if args.limit:
        cmds = cmds[: args.limit]
    print(f"[flow] {len(cmds)} unidades de traduccion, {args.jobs} procesos")

    decl_urls = dox.by_decl
    jobs = [(f, a, decl_urls) for f, a in cmds]
    merged: dict[str, dict] = {}
    errors = []
    t1 = time.time()
    if args.jobs > 1:
        with mp.Pool(args.jobs) as pool:
            for i, res in enumerate(pool.imap_unordered(analyze_tu, jobs), 1):
                if "__error__" in res:
                    errors.append(res["__error__"])
                    continue
                for k, v in res.items():
                    merged.setdefault(k, v)
                if i % 25 == 0:
                    print(f"[flow]   {i}/{len(jobs)} ({time.time()-t1:.0f}s)")
    else:
        for j in jobs:
            res = analyze_tu(j)
            merged.update({k: v for k, v in res.items() if k != "__error__"})
    print(f"[flow] {len(merged)} funciones analizadas en {time.time()-t1:.0f}s")
    for e in errors[:5]:
        print("[flow] error:", e[0], e[1][:120])

    # union con los miembros de Doxygen
    per_page: dict[str, dict] = {}
    matched = 0
    for key, val in merged.items():
        path, end = key.rsplit(":", 1)
        hit = dox.by_body.get((path, int(end)))
        if not hit:
            continue
        owner, anchor, name = hit
        calls, steps = count(val["nodes"])
        entry = {
            "name": name,
            "sig": val["sig"],
            "line": val["start"],
            "nodes": val["nodes"],
            "stats": {"calls": calls, "steps": steps},
        }
        srcp = dox.src_page.get(path)
        if srcp:
            entry["src"] = srcp
        per_page.setdefault(owner, {})[anchor] = entry
        matched += 1
    print(f"[flow] {matched} funciones enlazadas con {len(per_page)} paginas de Doxygen")

    os.makedirs(OUT_DIR, exist_ok=True)
    for old in glob.glob(os.path.join(OUT_DIR, "*.js")):
        os.remove(old)
    total = 0
    for owner, data in per_page.items():
        path = os.path.join(OUT_DIR, owner + ".js")
        payload = json.dumps(data, ensure_ascii=False, separators=(",", ":"))
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("harukaFlowRegister(" + json.dumps(owner) + "," + payload + ");\n")
        total += os.path.getsize(path)
    print(f"[flow] escritos {len(per_page)} ficheros en docs/html/flow "
          f"({total/1048576:.1f} MB) en {time.time()-t0:.0f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
