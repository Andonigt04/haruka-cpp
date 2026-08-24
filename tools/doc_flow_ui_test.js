/*
 * doc_flow_ui_test.js — prueba de integracion de docs/theme/haruka_flow.js.
 *
 *   node tools/doc_flow_ui_test.js
 *
 * Monta un DOM minimo con la misma estructura que genera Doxygen
 * (h2.memtitle + div.memitem > div.memdoc), carga el fichero de datos de una
 * pagina real de docs/html/flow y comprueba que el panel se inyecta, que los
 * tres niveles de detalle filtran distinto y que los enlaces apuntan a ficheros
 * y anclas que existen de verdad en docs/html.
 *
 * Requiere haber generado antes la documentacion:  ./tools/build_docs.sh
 */
const fs = require('fs');
class El {
  constructor(tag){ this.tagName=tag.toUpperCase(); this.children=[]; this.attrs={};
    this.dataset={}; this._cls=new Set(); this._text=''; this.handlers={}; this.parent=null; }
  set className(v){ this._cls=new Set(String(v).split(/\s+/).filter(Boolean)); }
  get className(){ return [...this._cls].join(' '); }
  get classList(){ const s=this._cls; return {
    add:(c)=>s.add(c), remove:(c)=>s.delete(c), contains:(c)=>s.has(c),
    toggle:(c,f)=>{ const on = f===undefined ? !s.has(c) : !!f; on?s.add(c):s.delete(c); return on; } }; }
  set textContent(v){ this._text=String(v); this.children=[]; }
  get textContent(){ return this._text + this.children.map(c=>c.textContent).join(''); }
  set innerHTML(v){ this.children=[]; this._text = v ? '[html]' : ''; }
  appendChild(c){ c.parent=this; this.children.push(c); return c; }
  set href(v){ this.attrs.href=v; }
  get href(){ return this.attrs.href; }
  setAttribute(k,v){ this.attrs[k]=v; }
  getAttribute(k){ return this.attrs[k]; }
  addEventListener(t,f){ (this.handlers[t]=this.handlers[t]||[]).push(f); }
  click(){ (this.handlers.click||[]).forEach(f=>f.call(this,{})); }
  get nextElementSibling(){ if(!this.parent) return null;
    const i=this.parent.children.indexOf(this); return this.parent.children[i+1]||null; }
  walk(out=[]){ out.push(this); this.children.forEach(c=>c.walk(out)); return out; }
  matches(sel){ // "h2.memtitle", ".hf-row", "a[href^='#']"
    if (sel.startsWith('a[href^=')) return this.tagName==='A' && (this.attrs.href||'').startsWith('#');
    const m=sel.match(/^([a-z0-9]*)((?:\.[-\w]+)*)$/i);
    if(!m) return false;
    if(m[1] && this.tagName!==m[1].toUpperCase()) return false;
    return m[2].split('.').filter(Boolean).every(c=>this._cls.has(c));
  }
  querySelectorAll(sel){ const scoped=sel.startsWith(':scope >');
    const parts=sel.replace(':scope >','').trim();
    const pool = scoped ? this.children : this.walk().slice(1);
    if (parts.includes('>')) { // ":scope > .hf-row > .hf-caret..."
      const segs=parts.split('>').map(s=>s.trim());
      let cur=[this];
      for (const s of segs){ const neg=s.includes(':not(');
        const base=s.replace(/:not\([^)]*\)/,''); const negCls=neg?s.match(/:not\(\.([^)]+)\)/)[1]:null;
        cur=cur.flatMap(n=>n.children).filter(n=>n.matches(base) && (!negCls || !n._cls.has(negCls))); }
      return cur; }
    return pool.filter(n=>n.matches(parts)); }
  querySelector(sel){ return this.querySelectorAll(sel)[0]||null; }
}
const scripts=[];
const document = {
  readyState:'complete', head:new El('head'), body:new El('body'),
  createElement:(t)=>new El(t),
  addEventListener:()=>{},
  querySelectorAll:(sel)=>document.body.querySelectorAll(sel),
};
global.document=document;
global.location={ pathname:'/docs/html/db/df5/classPhysicsEngine.html' };
global.window=global; global.harukaRelPath='../../';
// Object.defineProperty para capturar el <script> que inyecta el modulo
const origAppend=document.head.appendChild.bind(document.head);
document.head.appendChild=(c)=>{ if(c.tagName==='SCRIPT') scripts.push(c); return origAppend(c); };

// pagina falsa: h2.memtitle + div.memitem > div.memdoc
function member(anchor){
  const h2=new El('h2'); h2.className='memtitle';
  const a=new El('a'); a.attrs.href='#'+anchor; h2.appendChild(a);
  const item=new El('div'); item.className='memitem';
  const doc=new El('div'); doc.className='memdoc'; item.appendChild(doc);
  document.body.appendChild(h2); document.body.appendChild(item);
  return doc;
}
// --- eleccion de la pagina a probar: la que tiene la funcion mas rica -------
const path=require('path');
const flowDir='docs/html/flow';
if (!fs.existsSync(flowDir)) {
  console.error('No hay docs/html/flow. Ejecuta antes: ./tools/build_docs.sh');
  process.exit(2);
}
function parseFlow(file){
  const t=fs.readFileSync(file,'utf8');
  return JSON.parse(t.slice(t.indexOf(',{')+1, t.lastIndexOf(');')));
}
let best={calls:-1};
for (const f of fs.readdirSync(flowDir)) {
  const data=parseFlow(path.join(flowDir,f));
  for (const [anchor,e] of Object.entries(data)) {
    if (e.stats.calls > best.calls) best={ calls:e.stats.calls, file:f, anchor, data, entry:e };
  }
}
const pageName=best.file.replace(/\.js$/,'');
const flowSrc=fs.readFileSync(path.join(flowDir,best.file),'utf8');
const anchors=Object.keys(best.data);

global.location={ pathname:'/docs/html/dd/dd/'+pageName+'.html' };
global.window=global; global.harukaRelPath='../../';

anchors.forEach(member);
member('anclaSinDatosDeFlujo');

// --- ejecucion -------------------------------------------------------------
eval(fs.readFileSync('docs/theme/haruka_flow.js','utf8'));
const wanted='../../flow/'+pageName+'.js';
eval(flowSrc);
scripts[0].onload && scripts[0].onload();

// --- comprobaciones --------------------------------------------------------
let fails=0;
function check(name, cond, extra){
  if (cond) { console.log('  ok   ' + name + (extra?'  ('+extra+')':'')); }
  else { console.log('  FALLA ' + name + (extra?'  ('+extra+')':'')); fails++; }
}
console.log('pagina: ' + pageName + '  funcion: ' + best.entry.name);

check('pide el fichero de datos correcto', scripts[0].src === wanted, scripts[0].src);

const wraps=document.body.querySelectorAll('.hf-wrap');
check('un panel por miembro con datos, y solo esos',
      wraps.length === anchors.length, wraps.length + ' de ' + anchors.length);

const w=wraps[anchors.indexOf(best.anchor)];
check('el boton resume las cifras de la funcion',
      w.querySelector('.hf-toggle').textContent.includes(best.entry.stats.calls + ' llamadas'),
      w.querySelector('.hf-toggle').textContent);
check('el panel no se construye hasta abrirlo', w.querySelectorAll('.hf-panel').length === 0);

w.querySelector('.hf-toggle').click();
check('al pulsar se expande', w.classList.contains('hf-expanded'));

function nodesFor(mode){
  w.querySelectorAll('.hf-segbtn').find(b=>b.dataset.mode===mode).click();
  return w.querySelectorAll('.hf-node').length;
}
const steps=nodesFor('steps'), calls=nodesFor('calls'), all=nodesFor('all');
check('los tres niveles filtran distinto', calls < steps && steps < all,
      'pasos ' + steps + ' / funciones ' + calls + ' / todo ' + all);
check('"solo funciones" muestra exactamente las llamadas contadas',
      calls === best.entry.stats.calls, calls + ' vs ' + best.entry.stats.calls);
check('ningun nivel se queda vacio', calls > 0);

nodesFor('all');
const links=w.querySelectorAll('.hf-link').filter(a=>a.attrs.href);
const lines=w.querySelectorAll('.hf-line').filter(a=>a.attrs.href);
check('hay enlaces a la documentacion de las funciones llamadas', links.length > 0, links.length);
check('cada nodo enlaza a su linea de codigo', lines.length === all, lines.length);

// los enlaces deben existir de verdad en docs/html
const cache={};
// se resuelve como lo haria el navegador desde la pagina simulada:
// docs/html/dd/dd/<pagina>.html  ->  el prefijo relativo tiene que estar bien
function resolve(href){ return path.join('docs/html', 'dd/dd', href); }
let broken=[];
for (const a of links.concat(lines)) {
  const [f,anc]=a.attrs.href.split('#');
  const fp=resolve(f);
  if (!fs.existsSync(fp)) { broken.push(a.attrs.href + ' (fichero)'); continue; }
  if (!(fp in cache)) cache[fp]=fs.readFileSync(fp,'utf8');
  if (!cache[fp].includes('"'+anc+'"')) broken.push(a.attrs.href + ' (ancla)');
}
check('todos los enlaces apuntan a algo que existe', broken.length === 0,
      broken.slice(0,3).join(', ') || (links.length+lines.length) + ' comprobados');

const exp=w.querySelector('.hf-mini');
exp.click();
const opened=w.querySelectorAll('.hf-node').filter(n=>n.classList.contains('hf-open')).length;
exp.click();
const closed=w.querySelectorAll('.hf-node').filter(n=>n.classList.contains('hf-open')).length;
check('"desplegar todo" abre y "plegar todo" cierra', opened > 0 && closed === 0,
      opened + ' abiertos -> ' + closed);

// contraprueba: un miembro sin datos no debe recibir panel
const last=document.body.querySelectorAll('.memdoc').slice(-1)[0];
check('un miembro sin datos de flujo no recibe boton',
      last.querySelectorAll('.hf-wrap').length === 0);

console.log(fails ? '\nFALLOS: ' + fails : '\nTodo correcto');
process.exit(fails ? 1 : 0);
