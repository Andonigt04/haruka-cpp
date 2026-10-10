/*
 * haruka_flow.js — arbol de ejecucion reactivo para la documentacion de Doxygen.
 *
 * Los datos los genera tools/doc_flow.py a partir del AST real de Clang y se
 * publican en html/flow/<pagina>.js. Cada fichero llama a harukaFlowRegister().
 * Se carga con una etiqueta <script> (no fetch) para que funcione tambien
 * abriendo la documentacion con file://.
 */
(function () {
  "use strict";

  var DATA = null;          // ancla -> { name, sig, nodes, stats, src }
  var PENDING = [];         // botones esperando a que lleguen los datos

  // Modos de detalle. "cada nodo -> se ve?"
  var MODES = {
    steps: {
      label: "Pasos grandes",
      hint: "Solo el esqueleto: condicionales, bucles y llamadas.",
      keep: function (k) {
        return k === "if" || k === "loop" || k === "switch" || k === "case" ||
               k === "block" || k === "call" || k === "return";
      }
    },
    calls: {
      label: "Solo funciones",
      hint: "Únicamente las funciones que se ejecutan, en orden.",
      keep: function (k) { return k === "call"; }
    },
    all: {
      label: "Variables y cálculos",
      hint: "Todo: también las variables que se declaran y se modifican.",
      keep: function () { return true; }
    }
  };

  var ICON = {
    call: "ƒ", assign: "=", var: "□", if: "?", loop: "↺",
    switch: "⑂", case: "•", return: "↩", flow: "↳", block: "◈"
  };

  var relpath = (window.harukaRelPath || "");

  // ---------------------------------------------------------------- datos
  window.harukaFlowRegister = function (page, data) {
    DATA = data;
    PENDING.splice(0).forEach(function (fn) { fn(); });
  };

  function pageId() {
    var f = location.pathname.split("/").pop() || "";
    return f.replace(/\.html?$/, "");
  }

  // ------------------------------------------------------------- filtrado
  /**
   * Aplica el modo de detalle. Los nodos ocultos no se pierden: sus hijos
   * suben de nivel, de forma que "solo funciones" sigue mostrando las
   * llamadas que ocurren dentro de un bucle o de un calculo.
   */
  function filter(nodes, mode) {
    var out = [];
    nodes.forEach(function (n) {
      var kids = filter(n.c || [], mode);
      if (mode.keep(n.k)) {
        var copy = { k: n.k, t: n.t, n: n.n, l: n.l, u: n.u };
        if (kids.length) copy.c = kids;
        out.push(copy);
      } else {
        out = out.concat(kids);
      }
    });
    return out;
  }

  function countNodes(nodes) {
    var n = 0;
    nodes.forEach(function (x) { n += 1 + countNodes(x.c || []); });
    return n;
  }

  // --------------------------------------------------------------- pintado
  function el(tag, cls, txt) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (txt !== undefined) e.textContent = txt;
    return e;
  }

  function renderNodes(nodes, entry, depth) {
    var ul = el("ul", "hf-list");
    nodes.forEach(function (n) {
      var li = el("li", "hf-node hf-" + n.k);
      var row = el("div", "hf-row");

      var kids = n.c || [];
      if (kids.length) {
        var caret = el("button", "hf-caret", "▸");
        caret.setAttribute("aria-label", "Desplegar");
        row.appendChild(caret);
        caret.addEventListener("click", function () {
          li.classList.toggle("hf-open");
          caret.textContent = li.classList.contains("hf-open") ? "▾" : "▸";
        });
      } else {
        row.appendChild(el("span", "hf-caret hf-caret-empty", ""));
      }

      row.appendChild(el("span", "hf-icon", ICON[n.k] || "•"));

      var label = el("span", "hf-label");
      if (n.k === "call" && n.u) {
        var a = el("a", "hf-link", n.t);
        a.href = relpath + n.u;
        a.title = "Ir a la documentación de " + (n.n || n.t);
        label.appendChild(a);
      } else {
        label.textContent = n.t;
      }
      row.appendChild(label);

      if (n.l && entry.src) {
        var src = el("a", "hf-line", ":" + n.l);
        src.href = relpath + entry.src + "#l" + String(n.l).padStart(5, "0");
        src.title = "Ver la línea " + n.l + " en el código";
        row.appendChild(src);
      } else if (n.l) {
        row.appendChild(el("span", "hf-line", ":" + n.l));
      }

      li.appendChild(row);
      if (kids.length) {
        li.appendChild(renderNodes(kids, entry, depth + 1));
        if (depth < 1) {           // los dos primeros niveles nacen abiertos
          li.classList.add("hf-open");
          row.querySelector(".hf-caret").textContent = "▾";
        }
      }
      ul.appendChild(li);
    });
    return ul;
  }

  function buildPanel(entry) {
    var panel = el("div", "hf-panel");
    var state = { mode: "steps" };

    var bar = el("div", "hf-bar");
    var seg = el("div", "hf-seg");
    var body = el("div", "hf-body");
    var hint = el("span", "hf-hint");

    function draw() {
      var mode = MODES[state.mode];
      var nodes = filter(entry.nodes || [], mode);
      body.innerHTML = "";
      if (!nodes.length) {
        body.appendChild(el("div", "hf-empty",
          "Nada que mostrar en este nivel de detalle."));
      } else {
        body.appendChild(renderNodes(nodes, entry, 0));
      }
      hint.textContent = mode.hint + "  — " + countNodes(nodes) + " nodos";
      Array.prototype.forEach.call(seg.children, function (b) {
        b.classList.toggle("hf-on", b.dataset.mode === state.mode);
      });
    }

    Object.keys(MODES).forEach(function (key) {
      var b = el("button", "hf-segbtn", MODES[key].label);
      b.dataset.mode = key;
      b.addEventListener("click", function () { state.mode = key; draw(); });
      seg.appendChild(b);
    });

    var expand = el("button", "hf-mini", "Desplegar todo");
    expand.addEventListener("click", function () {
      var open = expand.dataset.on !== "1";
      expand.dataset.on = open ? "1" : "0";
      expand.textContent = open ? "Plegar todo" : "Desplegar todo";
      body.querySelectorAll(".hf-node").forEach(function (li) {
        if (!li.querySelector(":scope > .hf-row > .hf-caret:not(.hf-caret-empty)")) return;
        li.classList.toggle("hf-open", open);
        li.querySelector(":scope > .hf-row > .hf-caret").textContent =
          open ? "▾" : "▸";
      });
    });

    bar.appendChild(seg);
    bar.appendChild(expand);
    bar.appendChild(hint);
    panel.appendChild(bar);
    panel.appendChild(body);
    draw();
    return panel;
  }

  // ------------------------------------------------------------- inyeccion
  function attach(anchor, host) {
    var entry = DATA && DATA[anchor];
    if (!entry) return;

    var wrap = el("div", "hf-wrap");
    var stats = entry.stats || {};
    var btn = el("button", "hf-toggle");
    btn.appendChild(el("span", "hf-toggle-caret", "▸"));
    btn.appendChild(el("span", "hf-toggle-title", "Flujo de ejecución"));
    btn.appendChild(el("span", "hf-badge", (stats.calls || 0) + " llamadas"));
    btn.appendChild(el("span", "hf-badge", (stats.steps || 0) + " pasos"));
    wrap.appendChild(btn);

    var panel = null;
    btn.addEventListener("click", function () {
      if (!panel) {                       // se construye la primera vez que se abre
        panel = buildPanel(entry);
        wrap.appendChild(panel);
      }
      var open = wrap.classList.toggle("hf-expanded");
      btn.querySelector(".hf-toggle-caret").textContent = open ? "▾" : "▸";
    });

    host.appendChild(wrap);
  }

  function inject() {
    document.querySelectorAll("h2.memtitle").forEach(function (h2) {
      var link = h2.querySelector("a[href^='#']");
      if (!link) return;
      var anchor = link.getAttribute("href").slice(1);
      var item = h2.nextElementSibling;
      while (item && !item.classList.contains("memitem")) item = item.nextElementSibling;
      if (!item) return;
      var host = item.querySelector(".memdoc") || item;
      if (host.querySelector(".hf-wrap")) return;
      attach(anchor, host);
    });
  }

  function boot() {
    var s = document.createElement("script");
    s.src = relpath + "flow/" + pageId() + ".js";
    s.async = true;
    s.onload = inject;
    s.onerror = function () { /* pagina sin datos de flujo: nada que hacer */ };
    document.head.appendChild(s);
    if (DATA) inject(); else PENDING.push(inject);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", boot);
  } else {
    boot();
  }
})();
