const fs=require('fs'),vm=require('vm'),assert=require('assert');
const src=fs.readFileSync('frontend/src/app.js','utf8');
const nodes=new Map();function node(){return {hidden:false,textContent:'',disabled:false,children:[],events:{},append(...x){this.children.push(...x)},addEventListener(k,f){this.events[k]=f}}}
function $(id){if(!nodes.has(id))nodes.set(id,node());return nodes.get(id)}
let status={ok:true,active:true,package_id:'sp-test',filename:'test.ffpfsc',state:'downloading',downloaded_bytes:200,total_bytes:1000,speed_bytes:100,percent:20};let fail=false;const posts=[];
const ctx={console,Map,JSON,Date,Math,Number,Boolean,String,Error,$,state:{selected:null},installInFlight:false,
 localStorage:{getItem(){return '[]'},setItem(){}},document:{createElement:node},
 formatSize:x=>String(x),formatDuration:x=>Math.round(x)+' sn',showToast(){},installIdentity:r=>({game:{title:'PKG'},pkg:{}}),
 fetch:async(url,options)=>{if(options&&options.method==='POST'){posts.push(url);return {ok:!fail,json:async()=>({ok:!fail,error:'rejected'})}}return {ok:true,json:async()=>({...status})}}};
vm.createContext(ctx);vm.runInContext(src.slice(src.indexOf('  const downloadHistory ='),src.indexOf('  window.setInterval(() => { if (!document.hidden) pollSpectrum();')),ctx);
(async()=>{
 await ctx.pollSpectrum();assert.equal($('#spectrum-download-eta').textContent,'Tahmini kalan süre: 8 sn');assert.equal($('#spectrum-pause').disabled,false);assert.equal($('#spectrum-resume').disabled,true);
 status={...status,state:'cancelled',active:false,speed_bytes:0};await ctx.pollSpectrum();assert.equal($('#spectrum-resume').disabled,false);assert.equal($('#spectrum-pause').disabled,true);
 await ctx.downloadControl('resume');assert.equal(posts.at(-1),'/api/store/spectrum/resume');
 status={...status,native_gdrive:true,state:'paused',active:true};await ctx.pollSpectrum();assert.equal($('#spectrum-resume').disabled,false);
 await ctx.downloadControl('cancel');assert.equal(posts.at(-1),'/api/store/spectrum/cancel');
 status={...status,state:'completed',active:false};await ctx.pollSpectrum();assert.equal($('#spectrum-download').hidden,true);assert($('#downloads-history').children.some(x=>x.children.some(c=>c.textContent==='İndirme tamamlandı')));
 const html=fs.readFileSync('frontend/index.html','utf8');const downloads=html.slice(html.indexOf('<section id="downloads-page"'),html.indexOf('<section id="settings-page"'));assert(downloads.includes('id="spectrum-download"'));assert(downloads.includes('id="active-download"'));assert(html.indexOf('data-page="downloads"')>html.indexOf('data-page="settings"'));
 console.log('DOWNLOAD_UI_PASS eta, pause/resume, cancel endpoint, completed history, tabs');
})().catch(e=>{console.error(e);process.exit(1)});
