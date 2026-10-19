/*
 * haruka_docs.js — navegacion de las guias y cabecera de cada guia.
 *
 * Los datos los genera tools/doc_meta.py en html/doc_meta.js (titulo, ruta,
 * fecha de modificacion, si esta fijada y la ruta del .html). Se carga con <script> (no fetch) para
 * que funcione tambien abriendo la documentacion con file://.
 *
 *   · Barra izquierda: Fijadas, luego Guias. "Comunes" es HTML fijo de header.html.
 *   · En una guia: debajo del titulo, ruta del .md y fecha de modificacion.
 */
(function () {
  "use strict";

  var PAGES = null;
  var relpath = window.harukaRelPath || "";

  window.harukaDocsRegister = function (pages) {
    PAGES = pages;
    if (document.readyState !== "loading") render();
  };

  function pageId() {
    var f = location.pathname.split("/").pop() || "index.html";
    return f.replace(/\.html?$/, "");
  }

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }

  function section(title, pages, current) {
    var box = el("div", "hd-sec");
    box.appendChild(el("div", "hd-sec-title", title));
    var ul = el("ul", "hd-list");
    pages.forEach(function (p) {
      var li = el("li");
      var a = el("a", p.id === current ? "hd-current" : "", p.title);
      a.href = relpath + (p.href || p.id + ".html");
      li.appendChild(a);
      ul.appendChild(li);
    });
    box.appendChild(ul);
    return box;
  }

  function renderNav(current) {
    var host = document.getElementById("haruka-docnav");
    if (!host) return;
    host.textContent = "";
    var pins = PAGES.filter(function (p) { return p.pin; });
    var rest = PAGES.filter(function (p) { return !p.pin; });
    if (pins.length) host.appendChild(section("📌 Fijadas", pins, current));
    host.appendChild(section("Guías", rest, current));
  }

  function renderMeta(current) {
    var page = PAGES.filter(function (p) { return p.id === current; })[0];
    var title = document.querySelector(".headertitle");
    if (!page || !title || title.querySelector(".hd-meta")) return;
    var meta = el("div", "hd-meta");
    if (page.pin) meta.appendChild(el("span", "hd-pin", "📌 Fijada"));
    meta.appendChild(el("code", "hd-path", page.path));
    meta.appendChild(el("span", "hd-date", "Modificado " + page.date));
    title.appendChild(meta);
  }

  function render() {
    if (!PAGES) return;
    var current = pageId();
    renderNav(current);
    renderMeta(current);
  }

  function boot() {
    var s = document.createElement("script");
    s.src = relpath + "doc_meta.js";
    s.async = true;
    s.onerror = function () { /* sin doc_meta.js (doxygen sin tools/doc_meta.py) */ };
    document.head.appendChild(s);
    render();
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", boot);
  } else {
    boot();
  }
})();
