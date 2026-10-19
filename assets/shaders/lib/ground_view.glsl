/**
 * @file ground_view.glsl
 * @brief EL SUELO DIBUJADO en la GPU: la cota que `terrain_node.vert` pinta bajo un punto de la cara
 *        del cubo. Gemelo de `World/Planet/GroundView.h` (la tabla que lo alimenta) y de
 *        `TerrainNodeRenderer::nearNodes` (quien la rellena). Lo usa cualquier cosa que se apoye en
 *        el suelo que se VE (hierba, piedrecillas, decals…).
 *
 * ⚠️ NO es el campo de altura (bake + detalle): eso es lo que pisa la física. Lo que se ve es el mapa
 * del nodo hoja dibujado, y entre los dos hay centímetros (`terrain_node_drawn_vs_field`). Algo
 * apoyado en el campo flotaría o se hundiría por ese error.
 *
 * El INCLUYENTE declara (con los nombres de aquí; los bindings son suyos):
 *   struct NearNode { ivec4 node; ivec4 slot; };          // node = cara/nivel/i/j · slot = hueco/(kUp|zancada<<8)/subI/subJ
 *   buffer ... { NearNode uNodes[]; };                     // la tabla de `GroundView::nodes`
 *   buffer ... { float uHeights[]; };                      // `TerrainNodeRenderer::heightsBuffer()`
 *   buffer ... { uint uIdxOff[]; };  buffer ... { uint uIdxList[]; };   // el índice espacial (abajo)
 *   ivec4 uNodeGrid;   // x = texeles por nodo (129) · y = celdas por nodo (128) · z = nodos en la tabla
 *   dvec4 uIdx;        // x,y = esquina mínima de la rejilla del índice (lx,ly) · z = lado de celda · w = lado total
 *   ivec4 uIdxGrid;    // x,y = celdas de la rejilla del índice
 *
 * ── EL ÍNDICE ────────────────────────────────────────────────────────────────────────────────────
 * Barrer la tabla entera por punto es O(puntos × nodos). La CPU reparte los nodos por una rejilla
 * (lx,ly) de la cara de la cámara y el punto solo prueba los candidatos de su celda (1-3 casi
 * siempre). Si la celda no da ninguno —o el punto cae fuera de la rejilla— se barre la tabla entera:
 * el índice cambia CUÁNTOS nodos se prueban, nunca CUÁL se elige. (Sin ese respaldo, una celda vacía
 * por un desajuste al límite dejó un anillo sin hierba que seguía al jugador.)
 */
#ifndef HARUKA_GROUND_VIEW_GLSL
#define HARUKA_GROUND_VIEW_GLSL

/// Cota del mapa del nodo `nd` en el texel FRACCIONARIO (tu, tv) de la hoja, con la MISMA bilineal
/// que `terrain_node.vert` (incluida la caída a ancestro `kUp`). `dhdt` en metros por téxel del dato.
float harukaGroundNodeTexel(const NearNode nd, const float tu, const float tv, out vec2 dhdt) {
    const int   N1    = uNodeGrid.x;
    const float cells = float(uNodeGrid.y);
    const int   kUp   = nd.slot.y & 0xFF;
    const float inv = 1.0 / float(1 << kUp);
    const float au = clamp((float(nd.slot.z) * cells + tu) * inv, 0.0, cells);
    const float av = clamp((float(nd.slot.w) * cells + tv) * inv, 0.0, cells);
    const uint base = uint(nd.slot.x) * uint(N1) * uint(N1);
    const int iu = int(floor(au)), iv = int(floor(av));
    const int ju = min(iu + 1, N1 - 1), jv = min(iv + 1, N1 - 1);
    const float fu = au - float(iu), fv = av - float(iv);
    const float h00 = uHeights[base + uint(iv) * uint(N1) + uint(iu)];
    const float h10 = uHeights[base + uint(iv) * uint(N1) + uint(ju)];
    const float h01 = uHeights[base + uint(jv) * uint(N1) + uint(iu)];
    const float h11 = uHeights[base + uint(jv) * uint(N1) + uint(ju)];
    dhdt = vec2(mix(h10 - h00, h11 - h01, fv), mix(h01 - h00, h11 - h10, fu));
    return mix(mix(h00, h10, fu), mix(h01, h11, fu), fv);
}

/// Cota DIBUJADA del nodo `nd` en (fx, fy) ∈ [0,1]² de la cara. false si el nodo no cubre el punto.
/// `dhdt` = pendiente en metros por téxel del dato leído.
///
/// ⚠️ EN LOS TRIANGULOS DE LA MALLA, NO EN EL TEXEL FINO (17-10). Aquí se devolvía la bilineal del mapa
/// en cada téxel, pero el nodo se dibuja con ZANCADA: un vértice cada 2^z téxeles y triángulos planos
/// entre ellos. Donde el relieve se curva, la hierba quedaba sobre baches que no se dibujan —medido en
/// `v5 F3` por zancada: 0,075 m con 1, 0,126 con 2, 0,386 con 4— y SOBRESALÍA del terreno. Ahora se
/// leen las cuatro esquinas de la celda de zancada y se interpola en el MISMO triángulo que el index
/// buffer (`(a,b,c)` / `(c,b,d)`, diagonal de (u+s, v) a (u, v+s)). Lo que no se replica (la rampa
/// de mapas y el morph del vértice) son centímetros: ver la nota de `terrain_node.vert`.
bool harukaGroundNodeAt(const NearNode nd, const double fx, const double fy, out float h, out vec2 dhdt) {
    const float  cells  = float(uNodeGrid.y);
    const double cellsL = double(1 << nd.node.y);
    const int i = int(floor(fx * cellsL)), j = int(floor(fy * cellsL));
    if (i != nd.node.z || j != nd.node.w) { h = 0.0; dhdt = vec2(0.0); return false; }
    const float tu = float((fx * cellsL - double(i)) * double(cells));
    const float tv = float((fy * cellsL - double(j)) * double(cells));
    const float st = float(1 << ((nd.slot.y >> 8) & 0xF));
    if (st <= 1.0) { h = harukaGroundNodeTexel(nd, tu, tv, dhdt); return true; }
    // La celda de zancada que contiene el punto (la última se cierra en el borde del nodo).
    const float u0 = min(floor(tu / st) * st, cells - st), v0 = min(floor(tv / st) * st, cells - st);
    const float fu = clamp((tu - u0) / st, 0.0, 1.0), fv = clamp((tv - v0) / st, 0.0, 1.0);
    vec2 g;
    const float ha = harukaGroundNodeTexel(nd, u0,      v0,      g);
    const float hb = harukaGroundNodeTexel(nd, u0 + st, v0,      g);
    const float hc = harukaGroundNodeTexel(nd, u0,      v0 + st, g);
    const float hd = harukaGroundNodeTexel(nd, u0 + st, v0 + st, g);
    vec2 dLeaf;   // pendiente del triángulo, en metros por téxel de la HOJA
    if (fu + fv <= 1.0) { h = ha + fu * (hb - ha) + fv * (hc - ha);                 dLeaf = vec2(hb - ha, hc - ha) / st; }
    else                { h = hd + (1.0 - fu) * (hc - hd) + (1.0 - fv) * (hb - hd); dLeaf = vec2(hd - hc, hd - hb) / st; }
    dhdt = dLeaf * float(1 << (nd.slot.y & 0xFF));   // al téxel del dato, como la bilineal
    return true;
}

/// La cota DIBUJADA bajo (cara, lx, ly), lx/ly ∈ [-1,1]. false si ningún nodo de la tabla lo cubre.
bool harukaGroundHeight(int face, double lx, double ly, out float h, out vec2 dhdt) {
    const double fx = (lx + 1.0LF) * 0.5LF, fy = (ly + 1.0LF) * 0.5LF;
    h = 0.0; dhdt = vec2(0.0);
    const double ix = uIdx.x, iy = uIdx.y, iL = uIdx.z, iS = uIdx.w;
    if (iS > 0.0LF && lx >= ix && lx < ix + iS && ly >= iy && ly < iy + iS) {
        const int gx = int(clamp(floor((lx - ix) / iL), 0.0LF, double(uIdxGrid.x - 1)));
        const int gy = int(clamp(floor((ly - iy) / iL), 0.0LF, double(uIdxGrid.y - 1)));
        const int gid = gy * uIdxGrid.x + gx;
        for (int t = int(uIdxOff[gid]); t < int(uIdxOff[gid + 1]); ++t) {
            const NearNode nd = uNodes[uIdxList[t]];
            if (nd.node.x == face && harukaGroundNodeAt(nd, fx, fy, h, dhdt)) return true;
        }
    }
    for (int n = 0; n < uNodeGrid.z; ++n) {   // respaldo: la tabla entera (ver la nota del índice)
        const NearNode nd = uNodes[n];
        if (nd.node.x == face && harukaGroundNodeAt(nd, fx, fy, h, dhdt)) return true;
    }
    return false;
}

#endif // HARUKA_GROUND_VIEW_GLSL
