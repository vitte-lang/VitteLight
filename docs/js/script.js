(function(){
  "use strict";

  /* ========== CONFIG ==========\ */
  const CRAWL_LIMIT = 500;
  const FETCH_CONCURRENCY = 8;
  const INDEX_TTL_MS = 24 * 3600 * 1000;
  const INDEX_KEY = "vl.site.index.v3";
  const STOP_FR = ["le","la","les","de","des","du","un","une","et","ou","au","aux","a","à","en","dans","pour","par","avec","sans","sur","sous","chez","est","sont","être","été","avoir","que","qui","quoi","dont","où","ne","pas","plus","moins","ce","cet","cette","ces","se","sa","son","ses","nos","vos","leurs","il","elle","on","nous","vous","ils","elles","d","l","n"];
  const STOP_EN = ["the","a","an","and","or","to","in","of","for","on","at","as","is","are","be","been","was","were","by","from","this","that","these","those","it","its","with","without","not"];
  const STOP = new Set([...STOP_FR, ...STOP_EN]);

  /* ========== STORAGE ==========\ */
  const storage = {
    get(k, d = null){ try{ const v = localStorage.getItem(k); return v===null ? d : JSON.parse(v) }catch{ return d } },
    set(k, v){ try{ localStorage.setItem(k, JSON.stringify(v)) }catch{} },
    del(k){ try{ localStorage.removeItem(k) }catch{} },
  };

  /* ========== UI BASICS ==========\ */
  const html    = document.documentElement;
  const q       = document.getElementById("q");
  const hitsEl  = document.getElementById("hits");
  const toolbar = document.querySelector(".toolbar");
  const srchRoot= document.querySelector(".srch");
  const searchBtn = document.getElementById("searchBtn");
  const sommaire  = document.getElementById("sommaire");
  const fabToc    = document.getElementById("fabToc");
  const content   = document.getElementById("content");

  /* ===== A11y & mobile ===== */
  const prefersReduced = window.matchMedia?.("(prefers-reduced-motion: reduce)") ?? { matches:false };
  const scrollBehavior = prefersReduced.matches ? "auto" : "smooth";
  hitsEl?.setAttribute("role","status");
  hitsEl?.setAttribute("aria-live","polite");
  q && q.setAttribute("inputmode","search");
  q && q.setAttribute("enterkeyhint","search");

  // style pour le match courant + scroll-margin
  (function addFindStyle(){
    const st=document.createElement("style");
    st.textContent=`
      mark[data-cur]{outline:2px solid var(--accent,#5da0ff);border-radius:3px}
      [id],h1[id],h2[id],h3[id]{scroll-margin-top:calc(var(--tbH,48px)+8px)}
    `;
    document.head.appendChild(st);
  })();

  // crée/garantit la box de suggestions (portée dans <body>) + ARIA
  function ensureSuggBox(){
    let box = document.getElementById("suggestions");
    if(!box){
      box = document.createElement("div");
      box.id = "suggestions";
      box.setAttribute("role","listbox");
      box.setAttribute("aria-label","Suggestions de recherche");
      document.body.appendChild(box);
    }
    Object.assign(box.style, {
      position: "fixed",
      left: "0", top: "0",
      transform: "translate(-9999px,-9999px)",
      zIndex: "2147483647",
      background: "var(--card, #0f141b)",
      border: "1px solid var(--line, #202938)",
      borderRadius: "8px",
      maxHeight: "50vh",
      overflow: "auto",
      minWidth: "20rem",
      maxWidth: "min(48rem, 95vw)",
      padding: "6px",
      boxShadow: "0 8px 24px rgba(0,0,0,.2)"
    });
    return box;
  }

  // VisualViewport-aware pour éviter le clavier virtuel
  function viewportOffsets(){
    const vv = window.visualViewport;
    if(!vv) return {vx:0, vy:0};
    return { vx: vv.offsetLeft||0, vy: vv.offsetTop||0 };
  }

  // place la box sous le champ de recherche
  function positionSuggBox(){
    const box = document.getElementById("suggestions");
    if(!box || box.hasAttribute("hidden")) return;
    const anchor = (q && q.offsetParent) ? q : (srchRoot || toolbar || document.body);
    const r = anchor.getBoundingClientRect();
    const width = Math.max(r.width || 0, 280);
    box.style.width = width + "px";
    const bw = box.offsetWidth || width;
    const bh = box.offsetHeight || 0;
    const vw = Math.max(document.documentElement.clientWidth, window.innerWidth || 0);
    const vh = Math.max(document.documentElement.clientHeight, window.innerHeight || 0);
    const {vx,vy} = viewportOffsets();
    const x = Math.min(Math.max(8 + vx, r.left + vx), vx + vw - bw - 8);
    const y = Math.min(Math.max(8 + vy, r.bottom + 6 + vy), vy + vh - bh - 8);
    box.style.transform = `translate(${x}px, ${y}px)`;
  }

  // Theme
  const modeBtn = document.getElementById("modeBtn");
  const THEMES = ["auto","light","dark"]; const THEME_KEY="vl.theme";
  const mq = window.matchMedia?.("(prefers-color-scheme: dark)");
  const eff = m => m==="auto" ? (mq?.matches ? "dark":"light") : m;
  function applyTheme(mode){
    const e = eff(mode);
    html.setAttribute("data-theme", e);
    html.style.colorScheme = e;
    if (modeBtn){
      modeBtn.textContent = mode==="auto" ? "Thème: Auto" : (e==="dark"?"Thème: Sombre":"Thème: Clair");
      modeBtn.setAttribute("aria-pressed", mode!=="auto");
    }
    storage.set(THEME_KEY, mode);
  }
  applyTheme(storage.get(THEME_KEY,"auto"));
  modeBtn?.addEventListener("click", ()=>{
    const cur = storage.get(THEME_KEY,"auto");
    applyTheme(THEMES[(THEMES.indexOf(cur)+1)%THEMES.length]);
  });
  mq?.addEventListener?.("change", ()=>{ if(storage.get(THEME_KEY,"auto")==="auto") applyTheme("auto") });

  // Zoom / print
  const ZOOM_KEY="vl.zoom", zoomIn=document.getElementById("zoomIn"), zoomOut=document.getElementById("zoomOut"), printBtn=document.getElementById("printBtn");
  const getZoom = ()=> storage.get(ZOOM_KEY,16);
  const setZoom = px => { const z=Math.max(12,Math.min(22,px|0)); html.style.fontSize=z+"px"; storage.set(ZOOM_KEY,z); };
  setZoom(getZoom());
  zoomIn?.addEventListener("click", ()=> setZoom(getZoom()+1));
  zoomOut?.addEventListener("click", ()=> setZoom(getZoom()-1));
  printBtn?.addEventListener("click", ()=> window.print());

  // Toolbar height
  function updateToolbarH(){ if(!toolbar) return; html.style.setProperty("--tbH", toolbar.getBoundingClientRect().height+"px"); }
  const onResize = ()=> (window.requestAnimationFrame||setTimeout)(updateToolbarH,16);
  updateToolbarH(); addEventListener("resize", onResize);

  // TOC FAB
  function thresholdY(){ const s = sommaire ? (sommaire.offsetTop+sommaire.offsetHeight) : 0; return Math.min(innerHeight, s||innerHeight); }
  function onScroll(){ const y = scrollY || document.documentElement.scrollTop || 0; fabToc?.toggleAttribute("hidden", !(y>thresholdY())); }
  onScroll(); addEventListener("scroll", onScroll, {passive:true}); addEventListener("resize", onScroll);
  fabToc?.addEventListener("click", ()=> (sommaire||document.body).scrollIntoView({behavior:scrollBehavior,block:"start"}));

  // Shortcuts
  addEventListener("keydown", (e)=>{
    const ctrl = e.ctrlKey || e.metaKey;
    if (ctrl && e.key.toLowerCase()==="k"){ e.preventDefault(); q?.focus(); q?.select(); }
    if (ctrl && (e.key==="="||e.key==="+")){ e.preventDefault(); setZoom(getZoom()+1); }
    if (ctrl && e.key==="-" ){ e.preventDefault(); setZoom(getZoom()-1); }
    if (ctrl && e.key==="0" ){ e.preventDefault(); setZoom(16); }
    if (e.key==="F3"){ e.preventDefault(); e.shiftKey ? gotoPrev() : gotoNext(); }
  });

  /* ========== HELPERS ==========\ */
  const norm = s => (s||"").toLowerCase().normalize("NFD").replace(/[\u0300-\u036f]/g,"");
  const rxToken = /[a-z0-9_]+/gi;

  function siteBase(){
    const parts = location.pathname.split("/").filter(Boolean);
    const baseFolder = parts.length ? parts[0] : "";
    return location.origin + "/" + (baseFolder ? baseFolder + "/" : "");
  }
  const SITE_BASE   = siteBase();
  const SITEMAP_URL = SITE_BASE + "sitemap.xml";
  const basePath    = new URL(SITE_BASE).pathname;
  const relPath = url => new URL(url,SITE_BASE).pathname.replace(basePath,"").replace(/^\/+/,"") || "index";
  function canonical(url){
    try{ const u=new URL(url,SITE_BASE); u.hash=""; if(u.pathname.endsWith("/index.html")) u.pathname=u.pathname.replace(/\/index\.html$/,"/"); return u.href; }catch{ return null; }
  }
  function isInternal(url){ try{ return new URL(url,SITE_BASE).href.startsWith(SITE_BASE) }catch{ return false } }
  function isHtmlLike(url){ return !/\.(png|jpe?g|gif|svg|webp|ico|pdf|zip|gz|bz2|7z|rar|mp[34]|wav|ogg|webm|woff2?|ttf|otf|css|json|map)$/i.test(url) }
  async function fetchText(url){ const res=await fetch(url,{credentials:"same-origin"}); if(!res.ok) throw new Error("HTTP "+res.status); return res.text(); }

  async function pagesFromSitemap(){
    try{
      const xml = await fetchText(SITEMAP_URL);
      const doc = new DOMParser().parseFromString(xml,"application/xml");
      const locs = [...doc.querySelectorAll("url > loc")].map(n=>n.textContent.trim()).filter(Boolean);
      const urls = locs.filter(isInternal).filter(isHtmlLike).map(canonical);
      return Array.from(new Set(urls));
    }catch{ return null; }
  }
  async function pagesFromInlineConfig(){
    try{
      const fg=document.getElementById("file-groups"); if(!fg) return null;
      const parsed=JSON.parse(fg.textContent||"{}");
      if (parsed && Array.isArray(parsed.pages) && parsed.pages.length){
        const urls=parsed.pages.filter(isInternal).filter(isHtmlLike).map(canonical);
        return Array.from(new Set(urls));
      }
    }catch{}
    return null;
  }
  async function crawlSite(seed){
    const start=canonical(seed||SITE_BASE);
    const queue=[start]; const seen=new Set(queue); const out=[];
    async function worker(){
      while(queue.length){
        const url=queue.shift(); out.push(url);
        if(out.length>=CRAWL_LIMIT) break;
        let html=""; try{ html=await fetchText(url) }catch{ continue }
        const doc=new DOMParser().parseFromString(html,"text/html");
        const links=[...doc.querySelectorAll("a[href]")].map(a=>a.getAttribute("href")).filter(Boolean);
        for(const href of links){
          const abs=canonical(new URL(href,url).href); if(!abs) continue;
          if(!isInternal(abs)||!isHtmlLike(abs)) continue;
          if(!seen.has(abs) && seen.size<CRAWL_LIMIT*2){ seen.add(abs); queue.push(abs); }
        }
      }
    }
    await Promise.all(Array.from({length:FETCH_CONCURRENCY}, worker));
    return out.slice(0,CRAWL_LIMIT);
  }

  /* ========== INDEXING ==========\ */
  function extractNodesFromDoc(doc, pageUrl){
    const scope = doc.querySelector("#content, main, article") || doc.body || doc;
    const nodes=[]; const sectionTitles=new Map();
    scope.querySelectorAll("section").forEach(sec=>{
      const h2=sec.querySelector("h2"); const h3=sec.querySelector("h3");
      sectionTitles.set(sec,[h2?.textContent||"", h3?.textContent||""].filter(Boolean).join(" › "));
    });
    const push=(el,type,fam,weight,title,anchor)=>nodes.push({
      id:-1,type,fam,weight,title,file:relPath(pageUrl),url:canonical(pageUrl),anchor,text:(el.textContent||"").trim()
    });
    scope.querySelectorAll("h1,h2,h3").forEach(el=>{
      const level=el.tagName==="H1"?1:(el.tagName==="H2"?2:3);
      const fam  =level===1?"Title":"Subtitle";
      push(el,"heading",fam, level===1?4:(level===2?3:2), (el.textContent||"").trim(), el.id||"");
    });
    scope.querySelectorAll("p, li").forEach(el=>{
      const sec=el.closest("section"); const title=sectionTitles.get(sec)||""; const anchor=sec?.id||"";
      push(el,"text","Content",1,title,anchor);
    });
    scope.querySelectorAll("pre code").forEach(el=>{
      const sec=el.closest("section"); const title=sectionTitles.get(sec)||"Code block"; const anchor=sec?.id||"";
      push(el,"code-block","Code Block",2,title,anchor);
    });
    scope.querySelectorAll("code:not(pre code)").forEach(el=>{
      const sec=el.closest("section"); const title=sectionTitles.get(sec)||"Code"; const anchor=sec?.id||"";
      push(el,"code-inline","Inline Code",1.5,title,anchor);
    });
    return nodes;
  }

  function buildIndex(pageBundles){
    const docs=[]; pageBundles.forEach(pb=>pb.nodes.forEach(n=>docs.push(n)));
    docs.forEach((d,i)=> d.id=i);

    const postings=new Map();
    docs.forEach(d=>{
      const textNorm=norm(d.text); d.textNorm=textNorm;
      const tokens=(textNorm.match(rxToken)||[]).filter(t=>!STOP.has(t));
      const tf=new Map(); tokens.forEach(t=> tf.set(t,(tf.get(t)||0)+1));
      tf.forEach((f,t)=>{
        if(!postings.has(t)) postings.set(t,new Map());
        postings.get(t).set(d.id,(postings.get(t).get(d.id)||0) + f*(d.weight||1));
      });
      d.tokenCount=tokens.length;
    });

    const N=docs.length;
    const df=new Map(); postings.forEach((map,t)=> df.set(t,map.size));
    const globalTF=new Map(); postings.forEach((map,t)=>{ let s=0; map.forEach(v=>s+=v); globalTF.set(t,s); });

    const titleBonus=new Map();
    docs.filter(d=>d.type==="heading").forEach(d=>{
      (d.textNorm.match(rxToken)||[]).forEach(tok=>{
        if(STOP.has(tok)) return;
        titleBonus.set(tok,(titleBonus.get(tok)||0)+1.5);
      });
    });

    const keyScores=[];
    df.forEach((dF,tok)=>{
      if(tok.length<3) return;
      const score=(globalTF.get(tok)||0)*Math.log(1+N/(1+dF)) + (titleBonus.get(tok)||0);
      keyScores.push([tok,score]);
    });
    keyScores.sort((a,b)=> b[1]-a[1]);
    const TOP_KEYWORDS=keyScores.slice(0,50).map(([t])=>t);
    const freqList=Array.from(globalTF.entries()).sort((a,b)=> b[1]-a[1]).slice(0,50).map(([t])=>t);

    return { docs, postings, TOP_KEYWORDS, freqList };
  }

  function serializeIndex(index, meta, pages){
    const postings={}; index.postings.forEach((map,tok)=> postings[tok]=Array.from(map.entries()));
    const docs=index.docs.map(d=>({
      id:d.id,type:d.type,fam:d.fam,weight:d.weight,title:d.title,
      file:d.file,url:d.url,anchor:d.anchor,text:d.text,textNorm:d.textNorm,tokenCount:d.tokenCount
    }));
    return { meta, docs, postings, top:index.TOP_KEYWORDS, freq:index.freqList, pages };
  }
  function deserializeIndex(saved){
    const postings=new Map();
    for(const [tok,arr] of Object.entries(saved.postings||{})) postings.set(tok,new Map(arr));
    return { docs:saved.docs||[], postings, TOP_KEYWORDS:saved.top||[], freqList:saved.freq||[], meta:saved.meta||{}, pages:saved.pages||[] };
  }

  let SITE_INDEX=null;
  let FILES=[{id:"all",label:"Toutes les pages"}];

  async function makeIndex(progressCb){
    let pages = await pagesFromInlineConfig() || await pagesFromSitemap() || await crawlSite(SITE_BASE);
    const here = canonical(location.href);
    if (here && !pages.includes(here)) pages.unshift(here);

    const bundles=[]; let done=0;
    async function fetchOne(url){
      let html=""; try{ html=await fetchText(url) }catch{ done++; progressCb?.(done,pages.length); return; }
      const doc=new DOMParser().parseFromString(html,"text/html");
      const title=(doc.querySelector("title")?.textContent||"").trim()||url;
      const nodes=extractNodesFromDoc(doc,url);
      bundles.push({ url:canonical(url), title, nodes });
      done++; progressCb?.(done,pages.length);
    }
    let i=0;
    await Promise.all(Array.from({length:FETCH_CONCURRENCY}, async()=>{ while(true){ const idx=i++; if(idx>=pages.length) break; await fetchOne(pages[idx]); } }));

    const index=buildIndex(bundles);
    const meta={ ts:Date.now(), base:SITE_BASE, version:3, pagesCount:bundles.length };
    const pageList=bundles.map(b=>({ url:b.url, title:b.title, id:relPath(b.url) }));
    return { index, meta, pages:pageList };
  }

  function buildFilesFromIndex(pagesList){
    const labels=new Map();
    (pagesList||SITE_INDEX?.pages||[]).forEach(p=>{
      try{ const id=p.id||relPath(p.url); labels.set(id, (p.title||id).replace(/\s*\|\s*.+$/,"")); }catch{}
    });
    if(!labels.size && SITE_INDEX){ SITE_INDEX.docs.forEach(d=> labels.set(d.file, labels.get(d.file)||d.file)); }
    FILES=[{id:"all",label:"Toutes les pages"}, ...Array.from(labels.entries()).slice(0,100).map(([id,label])=>({id,label}))];
  }

  function setProgress(d,t){ hitsEl && (hitsEl.textContent=`Indexation du site… ${d}/${t}`); }
  async function ensureIndex(force){
    if(!force){
      const saved=storage.get(INDEX_KEY);
      if(saved && saved.meta && saved.meta.base===SITE_BASE && (Date.now()-(saved.meta.ts||0) < INDEX_TTL_MS)){
        const d=deserializeIndex(saved);
        SITE_INDEX=d; buildFilesFromIndex(d.pages);
        hitsEl && (hitsEl.textContent=`Index chargé (${d.meta.pagesCount || d.pages.length} pages)`); return;
      }
    }
    hitsEl && (hitsEl.textContent="Indexation du site…");
    const { index, meta, pages } = await makeIndex(setProgress);
    SITE_INDEX={...index, meta, pages}; buildFilesFromIndex(pages);
    try{ storage.set(INDEX_KEY, serializeIndex(index, meta, pages)); }catch{}
    hitsEl && (hitsEl.textContent=`Index prêt • ${pages.length} pages`);
  }

  /* ========== PARSE / MATCH ==========\ */
  function parseQuery(qStr){
    const raw = qStr ?? "";
    const filters = { file:null, type:null, url:null };
    const filterRx = /\b(file|type|url):([^\s]+)/gi;

    raw.replace(filterRx,(_,k,v)=>{ const key=(k||"").toLowerCase(); const val=(v||"").toLowerCase(); if(key==="url") filters.url=val; if(key==="file") filters.file=val; if(key==="type") filters.type=val; return ""; });

    const nQ = norm(raw.replace(filterRx," ")).trim();
    const phrases=[]; let rest = nQ.replace(/"([^"]+)"/g,(_,p)=>{ if(p.trim()) phrases.push(p.trim()); return " "; });
    const parts = rest.split(/\s+/).map(s=>s.trim()).filter(Boolean);

    const pos=[],neg=[],wild=[],chars=[];
    for(let t of parts){
      let isNeg=t.startsWith("-"); if(isNeg) t=t.slice(1);
      let isWild=t.endsWith("*");  if(isWild) t=t.slice(0,-1);
      if(!t) continue;
      if(t.length===1 && !STOP.has(t)){ if(isNeg) neg.push(t); else chars.push(t); continue; }
      if(STOP.has(t)) continue;
      if(isNeg) neg.push(t); else if(isWild) wild.push(t); else pos.push(t);
    }
    return { phrases,pos,neg,wild,chars,filters,raw };
  }

  function matchAndScore(query){
    if(!SITE_INDEX) return { heading:[], code:[], text:[], keyword:[], word:[], char:[], summary:{heading:0,code:0,text:0,total:0}, TOP_KEYWORDS:[], freqList:[] };
    const { docs, postings, TOP_KEYWORDS, freqList } = SITE_INDEX;
    const { phrases,pos,neg,wild,chars,filters } = parseQuery(query ?? "");
    const groups = { heading:[], code:[], text:[], keyword:[], word:[], char:[] };

    if(!(query ?? "").trim()){
      groups.keyword = TOP_KEYWORDS.map(k=>({label:k,fam:"Keyword"}));
      groups.word    = freqList.map(k=>({label:k,fam:"Word"}));
    }else{
      pos.forEach(w => groups.word.push({label:w,fam:"Word"}));
      wild.forEach(w=> groups.word.push({label:w+"*",fam:"Word"}));
      phrases.forEach(p=> groups.keyword.push({label:`"${p}"`,fam:"Keyword"}));
      chars.forEach(c  => groups.char.push({label:c,fam:"Character"}));
    }

    const candSet=new Set();
    const addCands = tok => { const m=postings.get(tok); if(!m) return; m.forEach((_,id)=>candSet.add(id)); };
    pos.forEach(addCands);
    wild.forEach(w => postings.forEach((map,tok)=>{ if(tok.startsWith(w)) addCands(tok); }));

    if(!pos.length && !wild.length && (phrases.length||chars.length)) docs.forEach(d=>candSet.add(d.id));
    if(!phrases.length && !pos.length && !wild.length && !chars.length) docs.forEach(d=>candSet.add(d.id));

    const typeFilter =
        (filters.type && /head|title/.test(filters.type)) ? "heading" :
        (filters.type && /code/.test(filters.type))       ? "code"    :
        (filters.type && /text|content/.test(filters.type))? "text"   : null;

    candSet.forEach(id=>{
      const d=docs[id]; if(!d) return;
      if(filters.file && String(d.file||"").toLowerCase()!==filters.file) return;
      if(filters.url  && !String(d.url ||"").toLowerCase().includes(filters.url)) return;
      if(typeFilter){
        if(typeFilter==="heading" && d.type!=="heading") return;
        if(typeFilter==="code"    && !String(d.type).startsWith("code")) return;
        if(typeFilter==="text"    && d.type!=="text") return;
      }
      const tnorm = d.textNorm || norm(d.text||"");
      for(const n of neg){ if(tnorm.includes(n)) return; }
      for(const p of phrases){ if(!tnorm.includes(p)) return; }

      let score=0;
      pos.forEach(t => { const base=(postings.get(t)?.get(d.id))||0; score += Math.log(1+base)*2; });
      wild.forEach(w=> { let sum=0; postings.forEach((map,tok)=>{ if(tok.startsWith(w)) sum += (map.get(d.id)||0); }); score += Math.log(1+sum)*1.5; });
      phrases.forEach(p=>{ if(tnorm.includes(p)) score += 3; });
      chars.forEach(c=>{ const cnt=(tnorm.split(c).length-1)||0; score += Math.min(2, cnt*0.2); });
      if(d.type==="heading") score += 2;
      if(String(d.type).startsWith("code")) score += 0.5;

      const item={ id, score, d };
      if(d.type==="heading") groups.heading.push(item);
      else if(String(d.type).startsWith("code")) groups.code.push(item);
      else groups.text.push(item);
    });

    groups.heading.sort((a,b)=> b.score - a.score);
    groups.code.sort((a,b)=> b.score - a.score);
    groups.text.sort((a,b)=> b.score - a.score);

    const summary = { heading:groups.heading.length, code:groups.code.length, text:groups.text.length, total:groups.heading.length+groups.text.length+groups.code.length };
    return { ...groups, summary, TOP_KEYWORDS, freqList };
  }

  /* ========== HIGHLIGHT ==========\ */
  let marks=[], curMark=-1;
  function clearMarks(){ marks.forEach(m=>{ const t=document.createTextNode(m.textContent); m.replaceWith(t); }); marks=[]; curMark=-1; }
  function markAll(needles){
    if(!needles.length || !content) return 0;
    const targets=content.querySelectorAll("section, p, li, pre, code, h1, h2, h3");
    const needlesN=needles.map(n=>({raw:n, n:norm(n)})).filter(n=>n.n);
    const findOne=(srcLower,from)=>{
      let best={i:-1,t:null};
      for(const nd of needlesN){
        const idx=srcLower.indexOf(nd.n, from);
        if(idx!==-1 && (best.i===-1 || idx<best.i)) best={i:idx,t:nd};
      }
      return best;
    };
    targets.forEach(el=>{
      const tw=document.createTreeWalker(el, NodeFilter.SHOW_TEXT, null);
      const local=[];
      while(tw.nextNode()){
        const node=tw.currentNode; const src=node.nodeValue; const low=norm(src);
        let from=0; if(!low) continue;
        const out=[];
        while(true){
          const f=findOne(low, from);
          if(f.i===-1) break;
          const realStart=f.i + (src.slice(0,f.i).length - low.slice(0,f.i).length);
          if(realStart>from) out.push(document.createTextNode(src.slice(from,realStart)));
          const marked=document.createElement("mark"); marked.setAttribute("data-hl","");
          const rawSlice=src.slice(realStart, realStart+(f.t.raw.length||f.t.n.length));
          marked.textContent = rawSlice || src.slice(realStart, realStart+f.t.n.length);
          out.push(marked); local.push(marked);
          from = realStart + (rawSlice || f.t.n).length;
          if(from>=src.length) break;
        }
        if(from<src.length) out.push(document.createTextNode(src.slice(from)));
        if(out.length){ const frag=document.createDocumentFragment(); out.forEach(n=>frag.appendChild(n)); node.parentNode.replaceChild(frag,node); }
      }
      marks.push(...local);
    });
    return marks.length;
  }

  // NAVIGATION: index du 1er match visible + focus amélioré
  function firstMatchIndexBelowViewport(){
    if(!marks.length) return -1;
    const tb = toolbar?.getBoundingClientRect().height || 0;
    const topLimit = tb + 8;
    for(let i=0;i<marks.length;i++){
      const r = marks[i].getBoundingClientRect();
      if(r.top > topLimit) return i;
    }
    return 0;
  }
  function focusCur(){
    marks.forEach(m=> m.removeAttribute("data-cur"));
    if(curMark>=0 && marks[curMark]){
      const m = marks[curMark];
      m.setAttribute("data-cur","");
      const y = m.getBoundingClientRect().top + (window.scrollY || document.documentElement.scrollTop || 0);
      const tb = toolbar?.getBoundingClientRect().height || 0;
      window.scrollTo({ top: Math.max(0, y - (innerHeight/2) - tb), behavior: scrollBehavior });
      hitsEl && (hitsEl.textContent = `${curMark+1}/${marks.length} correspondances`);
    }else{
      hitsEl && (hitsEl.textContent = "Aucun résultat");
    }
  }
  function gotoNext(){ if(!marks.length) return; curMark=(curMark+1)%marks.length; focusCur(); }
  function gotoPrev(){ if(!marks.length) return; curMark=(curMark-1+marks.length)%marks.length; focusCur(); }
  window.gotoNext=gotoNext; window.gotoPrev=gotoPrev;

  // Boutons Préc./Suiv. dans la toolbar
  (function ensureFindNav(){
    if(!toolbar) return;
    if(toolbar.querySelector(".find-nav")) return;
    const box = document.createElement("div");
    box.className = "find-nav";
    Object.assign(box.style,{display:"flex",gap:"6px",alignItems:"center",marginLeft:"8px"});
    const prev = Object.assign(document.createElement("button"),{type:"button",textContent:"Préc."});
    const next = Object.assign(document.createElement("button"),{type:"button",textContent:"Suiv."});
    prev.addEventListener("click", ()=> gotoPrev());
    next.addEventListener("click", ()=> gotoNext());
    box.appendChild(prev); box.appendChild(next);
    toolbar.appendChild(box);
  })();

  /* ========== SUGGESTIONS UI ==========\ */
  let selIndex=-1;
  function hideSugg(){
    const box = document.getElementById("suggestions");
    if(!box) return;
    box.setAttribute("hidden","");
    box.innerHTML="";
    box.style.transform = "translate(-9999px,-9999px)";
    srchRoot?.setAttribute("aria-expanded","false");
    selIndex=-1;
  }
  function famTag(text, emph=false){ const s=document.createElement("span"); s.className="tag"+(emph?" emph":""); s.textContent=text; return s; }
  function pill(label, pressed, cb){ const p=document.createElement("button"); p.type="button"; p.className="pill"; p.textContent=label; p.setAttribute("aria-pressed", pressed?"true":"false"); p.addEventListener("click", cb); return p; }
  function addGroupHeader(label, addons){
    const h=document.createElement("div"); h.className="group-h"; h.textContent=label;
    if(addons){ const box=document.createElement("div"); box.className="filters"; addons.forEach(x=>box.appendChild(x)); h.appendChild(box); }
    ensureSuggBox().appendChild(h);
  }

  function addItemToSugg(item){
    const { d } = item;
    const btn=document.createElement("button"); btn.type="button"; btn.className="sugg-item";
    btn.setAttribute("role","option"); btn.setAttribute("aria-selected","false");
    const left=document.createElement("div"); left.className="sugg-left";
    const right=document.createElement("div"); right.className="sugg-right";
    const path=d.title || (d.type==="heading"?"Titre": String(d.type).startsWith("code")?"Code":"Contenu");
    const snippet=(d.text||"").replace(/\s+/g," ").trim().slice(0,160);
    const anchor=d.anchor ? "#"+d.anchor : "";
    left.innerHTML = `<strong>${path}</strong><small>${new URL(d.url).pathname} — … ${snippet} …</small>`;
    right.appendChild(famTag(d.fam||"Content",true));
    right.appendChild(famTag(`file: ${d.file}`));
    btn.appendChild(left); btn.appendChild(right);
    btn.addEventListener("focus", ()=> btn.setAttribute("aria-selected","true"));
    btn.addEventListener("blur",  ()=> btn.setAttribute("aria-selected","false"));
    btn.addEventListener("click", ()=>{
      hideSugg(); clearMarks();
      const here=canonical(location.href), there=canonical(d.url);
      if(here===there){
        const target=d.anchor ? document.getElementById(d.anchor) : content;
        (target||content)?.scrollIntoView({behavior:scrollBehavior,block:"start"});
        const parsed=parseQuery(q?.value||"");
        const needles=[...parsed.phrases, ...parsed.pos, ...parsed.wild.map(w=>w), ...parsed.chars];
        if(needles.length){
          const total=markAll(needles);
          if(total){
            const idx = firstMatchIndexBelowViewport();
            curMark = idx >= 0 ? idx : 0;
            focusCur();
          }
        }
      }else{
        location.href = d.url + anchor;
      }
    });
    ensureSuggBox().appendChild(btn);
  }

  function renderSuggestions(query){
    const box = ensureSuggBox();
    box.removeAttribute("hidden");
    srchRoot?.setAttribute("aria-expanded","true");
    const res=matchAndScore(query ?? "");
    box.innerHTML="";

    const filePills=[pill("Toutes les pages", true, ()=>{
      if(!q) return; q.value=q.value.replace(/\bfile:[^\s]+/i,"").trim(); renderSuggestions(q.value);
    })];
    FILES.slice(1).forEach(f=>{
      filePills.push(pill(f.label,false,()=>{
        if(!q) return;
        if(!/\bfile:/i.test(q.value)) q.value+=" ";
        q.value = q.value.replace(/\bfile:[^\s]+/i,"").trim() + " file:" + f.id;
        renderSuggestions(q.value);
      }));
    });
    const typePills=[
      pill("Tous types", !/\btype:/i.test(q?.value||""), ()=>{ if(!q) return; q.value=q.value.replace(/\btype:[^\s]+/i,"").trim(); renderSuggestions(q.value); }),
      pill("Titres", /head|title/i.test(q?.value||""), ()=>{ if(!q) return; q.value=q.value.replace(/\btype:[^\s]+/i,"").trim()+" type:heading"; renderSuggestions(q.value); }),
      pill("Code", /type:code/i.test(q?.value||""), ()=>{ if(!q) return; q.value=q.value.replace(/\btype:[^\s]+/i,"").trim()+" type:code"; renderSuggestions(q.value); }),
      pill("Texte", /type:(text|content)/i.test(q?.value||""), ()=>{ if(!q) return; q.value=q.value.replace(/\btype:[^\s]+/i,"").trim()+" type:text"; renderSuggestions(q.value); }),
    ];
    const refreshBtn=pill("↻ Réindexer", false, async()=>{
      storage.del(INDEX_KEY);
      hitsEl && (hitsEl.textContent="Réindexation…");
      await ensureIndex(true);
      renderSuggestions(q?.value||"");
    });
    addGroupHeader("Filtres", [...filePills, ...typePills, refreshBtn]);

    if(res.keyword.length || res.word.length || res.char.length){
      addGroupHeader("Suggestions de requêtes");
      res.keyword.slice(0,6).forEach(tok=>{
        const b=document.createElement("button"); b.type="button"; b.className="sugg-item";
        b.innerHTML=`<div class="sugg-left"><strong>${tok.label}</strong><small>Mot-clé</small></div><div class="sugg-right">${famTag("Keyword",true).outerHTML}</div>`;
        b.addEventListener("click", ()=>{ if(!q) return; q.value=tok.label.replace(/^"|"$/g,""); doSearch(q.value); });
        box.appendChild(b);
      });
      res.word.slice(0,6).forEach(tok=>{
        const b=document.createElement("button"); b.type="button"; b.className="sugg-item";
        b.innerHTML=`<div class="sugg-left"><strong>${tok.label}</strong><small>Terme</small></div><div class="sugg-right">${famTag("Word",true).outerHTML}</div>`;
        b.addEventListener("click", ()=>{ if(!q) return; q.value=tok.label; doSearch(q.value); });
        box.appendChild(b);
      });
    }else if(!(query ?? "").trim() && SITE_INDEX){
      addGroupHeader("Mots-clés fréquents");
      const bar=document.createElement("div"); bar.className="chipbar";
      (SITE_INDEX.TOP_KEYWORDS||[]).slice(0,12).forEach(k=>{
        const c=document.createElement("button"); c.type="button"; c.className="chip"; c.textContent=k;
        c.addEventListener("click", ()=>{ if(!q) return; q.value=k; doSearch(k); });
        bar.appendChild(c);
      });
      box.appendChild(bar);
    }

    if(res.heading.length){ addGroupHeader("Titres & sous-titres"); res.heading.slice(0,8).forEach(it=>addItemToSugg(it)); }
    if(res.text.length){ addGroupHeader("Contenus"); res.text.slice(0,8).forEach(it=>addItemToSugg(it)); }
    if(res.code.length){ addGroupHeader("Blocs de code"); res.code.slice(0,8).forEach(it=>addItemToSugg(it)); }

    if(!res.summary.total && (query ?? "").trim()){
      const e=document.createElement("div"); e.className="sugg-item"; e.innerHTML="<small>Aucun résultat</small>"; box.appendChild(e);
    }

    updateToolbarH();
    requestAnimationFrame(positionSuggBox);
  }

  /* ========== SEARCH FLOW ==========\ */
  function doSearch(term){
    hideSugg(); clearMarks();
    const parsed  = parseQuery(term ?? "");
    const res     = matchAndScore(term ?? "");
    const needles = [...parsed.phrases, ...parsed.pos, ...parsed.wild.map(w=>w), ...parsed.chars];
    if(needles.length){
      const total=markAll(needles);
      if(total){
        const idx = firstMatchIndexBelowViewport();
        curMark = idx >= 0 ? idx : 0;
        focusCur();
      }
    }
    const parts=[];
    if(res.summary.heading) parts.push(`Titres: ${res.summary.heading}`);
    if(res.summary.text)    parts.push(`Texte: ${res.summary.text}`);
    if(res.summary.code)    parts.push(`Code: ${res.summary.code}`);
    hitsEl && (hitsEl.textContent = parts.length ? `${parts.join(" • ")} • Total: ${res.summary.total}` : ((term ?? "").trim() ? "Aucun résultat" : hitsEl?.textContent));
    renderSuggestions(term ?? "");
  }

  /* ========== EVENTS & INIT ==========\ */
  if(q){
    q.addEventListener("input", ()=>{ if(q._t) clearTimeout(q._t); q._t=setTimeout(()=>renderSuggestions(q.value||""),140); });
    q.addEventListener("keydown", (e)=>{
      const box = document.getElementById("suggestions");
      const items=[...(box?.querySelectorAll(".sugg-item")||[])];
      if(e.key==="Enter"){
        if(items.length && selIndex>=0){ e.preventDefault(); items[selIndex]?.click(); return; }
        e.preventDefault(); doSearch(q.value||""); return;
      }
      if(e.key==="ArrowDown" || e.key==="ArrowUp"){
        if(!items.length) return;
        e.preventDefault();
        selIndex = e.key==="ArrowDown" ? (selIndex+1)%items.length : (selIndex-1+items.length)%items.length;
        items.forEach((el,i)=> el.setAttribute("aria-selected", i===selIndex ? "true":"false"));
        items[selIndex]?.scrollIntoView({block:"nearest"});
        return;
      }
      if(e.key==="Escape"){ hideSugg(); }
    });
    q.addEventListener("focus", positionSuggBox, {passive:true});
  }
  searchBtn?.addEventListener("click", ()=> doSearch(q?.value || ""));
  document.addEventListener("click", (e)=>{
    const box = document.getElementById("suggestions");
    if(!srchRoot) return;
    if(!(srchRoot.contains(e.target) || box?.contains(e.target))) hideSugg();
  }, {capture:true});

  addEventListener("resize", positionSuggBox, { passive:true });
  addEventListener("scroll", positionSuggBox, { passive:true });
  addEventListener("orientationchange", ()=>{ updateToolbarH(); positionSuggBox(); }, { passive:true });

  if (window.visualViewport){
    visualViewport.addEventListener("resize", positionSuggBox, {passive:true});
    visualViewport.addEventListener("scroll", positionSuggBox, {passive:true});
  }

  if (toolbar && "MutationObserver" in window){
    const mo=new MutationObserver(()=> onResize());
    mo.observe(document.body, {childList:true, subtree:true});
  }

  // ouverture directe via ?q= ou #q=
  (function autoQueryFromURL(){
    const qs = new URLSearchParams(location.search);
    const fromSearch = (qs.get("q")||"").trim();
    const fromHash   = ((location.hash||"").match(/(^|[?#&])q=([^&]+)/)?.[2]||"").trim();
    const term = decodeURIComponent(fromSearch || fromHash || "");
    if(term){
      q && (q.value = term);
      doSearch(term);
    }
  })();

  (async function init(){
    try{ await ensureIndex(false); }catch{ await ensureIndex(true); }
    renderSuggestions("");
  })();

})();

