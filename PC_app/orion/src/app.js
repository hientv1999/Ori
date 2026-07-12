const $=id=>document.getElementById(id);

// Tauri v2 global bridge (tauri.conf.json has withGlobalTauri:true, so this
// is available on window without an ES module import — app.js is loaded as
// a classic script).
const invoke=(...a)=>window.__TAURI__.core.invoke(...a);
const listen=(...a)=>window.__TAURI__.event.listen(...a);

// Declared up front (not with the rest of the I18N block below) because
// code below reads them before the script has run far enough to reach the
// I18N block — a `let`/`const` referenced before its own declaration line
// executes throws, even though the binding is hoisted.
let appLang='en';
const LOCALE_MAP={en:'en-US',vi:'vi-VN',es:'es-ES',fr:'fr-FR'};

// The panel IS the whole app window (tauri.conf.json) — there's no in-page
// taskbar/tray to render (that was prototype-only dev chrome simulating the
// real OS taskbar). Minimize hides the window the same way clicking the
// real tray icon does (src-tauri/src/lib.rs toggle_panel).
function minimizePanel(){invoke('hide_panel');}

// Window is frameless (decorations:false) — .app-titlebar is the only drag
// handle. data-tauri-drag-region alone was unreliable in testing, so drive
// it explicitly via the documented startDragging() API instead.
document.addEventListener('DOMContentLoaded',()=>{
  const titlebar=document.querySelector('.app-titlebar');
  if(!titlebar) return;
  titlebar.addEventListener('mousedown',e=>{
    if(e.target.closest('.h-ico')) return; // let the minimize button handle its own click
    if(e.buttons===1) window.__TAURI__.window.getCurrentWindow().startDragging();
  });
});

const stack=[];
function show(id){
  const el=$(id);if(!el) return;
  if(stack.length>0){const prev=$(stack[stack.length-1]);if(prev)prev.classList.add('behind');}
  stack.push(id);el.style.zIndex=stack.length+1;
  requestAnimationFrame(()=>el.classList.add('show'));
}
function back(){
  if(!stack.length) return;
  const id=stack.pop(),el=$(id);
  if(el){el.classList.remove('show');el.style.zIndex='';}
  if(stack.length){const prev=$(stack[stack.length-1]);if(prev)prev.classList.remove('behind');}
}
document.addEventListener('keydown',e=>{if(e.key==='Escape') backWithCheck();});

let _discardAction=null;
function backWithCheck(){
  const top=stack[stack.length-1];
  if(top==='s-setup') return;
  if(top==='s-profile'&&pfChanged){
    _discardAction=()=>{
      $('nmInp').value=pfCommitted.name;$('tlInp').value=pfCommitted.title;
      $('emInp').value=pfCommitted.email;$('phInp').value=pfCommitted.phone;
      cc('nmInp','nmCnt',32);cc('tlInp','tlCnt',32);cc('emInp','emCnt',32);cc('phInp','phCnt',16);
      pfPendingUrl=null;pfRemoved=false;
      if(pfOrigUrl){
        $('pfDzThumb').style.backgroundImage=`url(${pfOrigUrl})`;
        $('pfDzEmpty').style.display='none';$('pfDzImg').style.display='';$('pfReuploadBtn').style.display='';$('pfRemoveBtn').style.display='';
      } else {
        $('pfDzThumb').style.backgroundImage='';
        $('pfDzEmpty').style.display='';$('pfDzImg').style.display='none';$('pfReuploadBtn').style.display='none';$('pfRemoveBtn').style.display='none';
      }
      pfChanged=false;$('pfSaveBtn').setAttribute('disabled','');back();
    };
    showModal('m-discard');return;
  }
  if(top==='s-timeOff'&&timeOffDirty){_discardAction=back;showModal('m-discard');return;}
  if(top==='s-calendar'&&calPending!==calSrc){_discardAction=discardCalSource;showModal('m-discard');return;}
  if(top==='s-ancs'&&ancsPending!==ancsLevel){_discardAction=discardAncs;showModal('m-discard');return;}
  if(top==='s-clock'&&clockPending!==clockFace){_discardAction=discardClock;showModal('m-discard');return;}
  if(top==='s-timeformat'&&timeFormatPending!==timeFormat){_discardAction=discardTimeFormat;showModal('m-discard');return;}
  if(top==='s-shortcuts'){
    if(_kbdRecSlot) stopKbdRecord();
    if(_isSlotsDirty()){_discardAction=discardShortcuts;showModal('m-discard');return;}
    back();return;
  }
  back();
}
function confirmDiscard(){
  hideModal('m-discard');
  if(_discardAction){_discardAction();_discardAction=null;}
}

function showModal(id){$(id).classList.add('show');}
function hideModal(id){$(id).classList.remove('show');}

let fwAvail=false;
let orionFwVersion='';
let orionAppVersion='1.0.0';
let orionUpdateAvail=false;
let orionUpdateVersion='1.1.0';
function updateOrionUpdateRow(){
  const t=I18N[appLang].orionUpdate;
  $('settingsAppVer').textContent=orionAppVersion;
  $('orionUpdateRow').style.display=orionUpdateAvail?'':'none';
  if(orionUpdateAvail){
    $('orionUpdateMain').textContent=t.rowMain;
    $('orionUpdateSub').textContent=t.rowSub.replace('{v}',orionUpdateVersion);
  }
}
let connState='on';
function setConn(s){
  connState=s;
  const dot=$('hDot'),state=$('hState');
  const connSections=$('connRequiredSections'),toDivider=$('mainTimeOffDivider'),fwIco=$('fwIco');
  const t=I18N[appLang].main;
  dot.className='h-dot '+s;
  if(s==='on'){
    state.textContent=t.connected;
    connSections.style.display='';toDivider.style.display='none';
    fwIco.style.display=fwAvail?'':'none';
    readSlotsFromDevice(); // Orion reads Device Settings from Ori on connect (ble-protocol.md §6.4)
  } else if(s==='rec'){
    state.textContent=t.syncing;
    connSections.style.display='none';toDivider.style.display='';
    fwIco.style.display='none';
  } else {
    state.textContent=t.disconnected;
    connSections.style.display='none';toDivider.style.display='';
    fwIco.style.display='none';
  }
  back();
}

let pfChanged=false,pfRemoved=false;
let pfCommitted={name:'',title:'',email:'',phone:''};
function openProfileScreen(){
  pfOrigUrl=null;pfPendingUrl=null;pfRemoved=false;
  const savedBg=$('mainProfPhoto').style.backgroundImage;
  const hasSaved=savedBg&&savedBg!=='none'&&savedBg!=='';
  if(hasSaved){
    const url=savedBg.slice(4,-1).replace(/['"]/g,'');
    $('pfDzThumb').style.backgroundImage=savedBg;
    $('pfDzEmpty').style.display='none';$('pfDzImg').style.display='';$('pfReuploadBtn').style.display='';$('pfRemoveBtn').style.display='';
    pfOrigUrl=url;
  } else {
    $('pfDzThumb').style.backgroundImage='';
    $('pfDzEmpty').style.display='';$('pfDzImg').style.display='none';$('pfReuploadBtn').style.display='none';$('pfRemoveBtn').style.display='none';
  }
  pfCommitted={name:$('nmInp').value,title:$('tlInp').value,email:$('emInp').value,phone:$('phInp').value};
  pfChanged=false;$('pfSaveBtn').setAttribute('disabled','');show('s-profile');
}
function cc(iId,cId,mx){
  const l=$(iId).value.length,el=$(cId);
  el.textContent=l+' / '+mx;el.className='fcnt'+(l>=mx?' warn':'');
}
function pfDirty(){
  cc('nmInp','nmCnt',32);cc('tlInp','tlCnt',32);cc('emInp','emCnt',32);cc('phInp','phCnt',16);
  const textChanged=$('nmInp').value!==pfCommitted.name||$('tlInp').value!==pfCommitted.title||
    $('emInp').value!==pfCommitted.email||$('phInp').value!==pfCommitted.phone;
  pfChanged=textChanged||pfPendingUrl!==null||pfRemoved;
  const valid=$('nmInp').value.trim().length>0&&$('tlInp').value.trim().length>0;
  if(pfChanged&&valid)$('pfSaveBtn').removeAttribute('disabled');else $('pfSaveBtn').setAttribute('disabled','');
}
function pfRemovePhoto(){
  pfPendingUrl=null;pfRemoved=true;
  $('pfDzThumb').style.backgroundImage='';
  $('pfDzEmpty').style.display='';$('pfDzImg').style.display='none';
  $('pfReuploadBtn').style.display='none';$('pfRemoveBtn').style.display='none';
  pfDirty();
}
function saveProfile(){
  if(!pfChanged) return;
  if(!$('nmInp').value.trim()||!$('tlInp').value.trim()) return;
  const name=$('nmInp').value||'—';
  const photoDataUrl=pfPendingUrl,photoRemoved=pfRemoved;
  $('mainName').textContent=name;
  const photo=$('mainProfPhoto');
  if(pfPendingUrl){
    photo.style.backgroundImage=`url(${pfPendingUrl})`;
    photo.style.backgroundSize='cover';photo.style.backgroundPosition='center';
    $('mainProfInitials').style.display='none';pfPendingUrl=null;
  } else if(pfRemoved){
    photo.style.backgroundImage='';
    $('mainProfInitials').style.display='';
  }
  if(!photo.style.backgroundImage){
    const parts=name.trim().split(' ');
    $('mainProfInitials').textContent=(parts[0][0]+(parts[1]?parts[1][0]:'')).toUpperCase();
  }
  invoke('save_profile',{name,title:$('tlInp').value,email:$('emInp').value,phone:$('phInp').value,photoDataUrl,photoRemoved});
  pfRemoved=false;
  pfChanged=false;$('pfSaveBtn').setAttribute('disabled','');back();
}
cc('nmInp','nmCnt',32);cc('tlInp','tlCnt',32);cc('emInp','emCnt',32);cc('phInp','phCnt',16);

let timeOffActive=false,timeOffCustomPhoto=false,timeOffDirty=false,timeOffPhotoRemoved=false;
let toCommittedStart=null,toCommittedEnd=null,toCommittedDest='',toCommittedPhotoUrl=null;
function updateToSaveState(){
  const dest=$('timeOffDt').value.trim();
  const ok=timeOffDirty&&selStart&&selEnd&&dest.length>0&&[...dest].length<=48;
  const btn=$('toSaveBtn');if(!btn) return;
  if(ok) btn.removeAttribute('disabled');else btn.setAttribute('disabled','');
}
function toggleTimeOff(){
  if(timeOffActive){exitTimeOff();return;}
  if(toCommittedStart&&toCommittedEnd&&toCommittedDest.length>0&&[...toCommittedDest].length<=48){
    timeOffActive=true;$('timeOffToggle').classList.add('on');setTimeOffState(true);
  } else {
    openTimeOffScreen();
  }
}
function openTimeOffScreen(){
  // Always reinit from committed state — discards any stale unsaved edits
  selStart=toCommittedStart;selEnd=toCommittedEnd;
  selPhase=selStart?(selEnd?2:1):0;calHover=null;
  $('timeOffDt').value=toCommittedDest;
  timeOffPhotoRemoved=false;
  if(toCommittedPhotoUrl){
    timeOffOrigUrl=toCommittedPhotoUrl;timeOffPendingUrl=null;
    $('timeOffDzThumb').style.backgroundImage=`url(${toCommittedPhotoUrl})`;
    $('timeOffDzEmpty').style.display='none';$('timeOffDzImg').style.display='';$('toReuploadBtn').style.display='';$('toRemoveBtn').style.display='';
  } else {
    timeOffOrigUrl=null;timeOffPendingUrl=null;
    $('timeOffDzThumb').style.backgroundImage='';
    $('timeOffDzEmpty').style.display='';$('timeOffDzImg').style.display='none';$('toReuploadBtn').style.display='none';$('toRemoveBtn').style.display='none';
  }
  updatePeriodDisplay();
  timeOffDirty=false;cc('timeOffDt','dtCnt',48);updateToSaveState();show('s-timeOff');
}
function timeOffRemovePhoto(){
  timeOffPendingUrl=null;timeOffPhotoRemoved=true;timeOffDirty=true;
  $('timeOffDzThumb').style.backgroundImage='';
  $('timeOffDzEmpty').style.display='';$('timeOffDzImg').style.display='none';
  $('toReuploadBtn').style.display='none';$('toRemoveBtn').style.display='none';
  updateToSaveState();
}
function exitTimeOff(){
  timeOffDirty=false;timeOffActive=false;
  toCommittedStart=null;toCommittedEnd=null;toCommittedDest='';toCommittedPhotoUrl=null;
  $('mainTimeOffBanner').style.backgroundImage='';
  $('timeOffToggle').classList.remove('on');setTimeOffState(false);back();
  invoke('clear_timeoff');
}
function setTimeOffState(active){
  const card=$('mainTimeOffCard');
  if(active){card.style.maxHeight='400px';card.classList.remove('collapsed');}
  else{card.style.maxHeight=card.offsetHeight+'px';requestAnimationFrame(()=>card.classList.add('collapsed'));}
  const hasPhoto=active&&!!toCommittedPhotoUrl;
  $('mainTimeOffBanner').style.display=!active||hasPhoto?'':'none';
  $('mainTimeOffText').style.display=active&&!hasPhoto?'':'none';
  $('mainTimeOffOverlay').style.display=hasPhoto?'':'none';
  $('mainTimeOffEmpty').style.display=active?'none':'';
}
function updateTimeOff(){
  const d=$('timeOffDt').value.trim();
  if(d){$('mainTimeOffDest').textContent=d;$('mainTimeOffTextDest').textContent=d;}
  if(selStart&&selEnd){
    const f=v=>v.toLocaleDateString(LOCALE_MAP[appLang],{month:'short',day:'numeric'});
    const range=f(selStart)+' – '+f(selEnd);
    $('mainTimeOffDates').textContent=range;$('mainTimeOffTextDates').textContent=range;
  }
}
function saveTimeOff(){
  const dest=$('timeOffDt').value.trim();
  if(!dest||[...dest].length>48||!selStart||!selEnd) return;
  updateTimeOff();
  const photoDataUrl=timeOffPendingUrl,photoRemoved=timeOffPhotoRemoved;
  if(timeOffPendingUrl){
    const b=$('mainTimeOffBanner');
    b.style.backgroundImage=`url(${timeOffPendingUrl})`;
    b.style.backgroundSize='cover';b.style.backgroundPosition='center';b.style.backgroundRepeat='no-repeat';
    toCommittedPhotoUrl=timeOffPendingUrl;
  } else if(timeOffPhotoRemoved){
    $('mainTimeOffBanner').style.backgroundImage='';
    toCommittedPhotoUrl=null;
  }
  toCommittedStart=selStart;toCommittedEnd=selEnd;toCommittedDest=dest;
  invoke('save_timeoff',{start:selStart.getTime(),end:selEnd.getTime(),destination:dest,photoDataUrl,photoRemoved});
  timeOffPhotoRemoved=false;
  timeOffDirty=false;timeOffActive=true;
  $('timeOffToggle').classList.add('on');setTimeOffState(true);back();
}
function hideToErr(id){$(id).style.display='none';}

let calYear,calMonth,selStart=null,selEnd=null,selPhase=0,calHover=null;
function initCal(){const t=new Date();calYear=t.getFullYear();calMonth=t.getMonth();renderCal();}
function togglePeriodCal(){
  const cal=$('periodCal'),disp=$('periodDisplay'),open=cal.style.display!=='none';
  if(open){cal.style.display='none';disp.classList.remove('open');}
  else{cal.style.display='';disp.classList.add('open');renderCal();}
}
function calNav(dir){
  calMonth+=dir;
  if(calMonth<0){calMonth=11;calYear--;}if(calMonth>11){calMonth=0;calYear++;}
  renderCal();
}
function renderCal(){
  const today=new Date();today.setHours(0,0,0,0);
  const loc=LOCALE_MAP[appLang];
  const monthName=new Date(calYear,calMonth,1).toLocaleDateString(loc,{month:'long'});
  $('pcalMonth').textContent=monthName+' '+calYear;
  document.querySelector('.pcal-nav-prev').classList.toggle('dis',calYear===today.getFullYear()&&calMonth===today.getMonth());
  const ti=I18N[appLang].timeOffEditor;
  $('pcalHint').textContent=selPhase===0?ti.selectStartDate:selPhase===1?ti.selectEndDate:'';
  const grid=$('pcalGrid');grid.innerHTML='';
  // 2024-01-01 is a Monday, so i=0..6 walks Mon→Sun to match the grid's week start
  for(let i=0;i<7;i++){
    const wd=new Date(2024,0,1+i).toLocaleDateString(loc,{weekday:'short'});
    const el=document.createElement('div');el.className='pcal-dh';el.textContent=wd;grid.appendChild(el);
  }
  const firstDow=new Date(calYear,calMonth,1).getDay();
  const offset=firstDow===0?6:firstDow-1;
  const daysInMonth=new Date(calYear,calMonth+1,0).getDate();
  for(let i=0;i<offset;i++){const el=document.createElement('div');el.className='pcal-d empty';grid.appendChild(el);}
  let hEnd=null;
  if(selPhase===1&&calHover!==null){const hd=new Date(calYear,calMonth,calHover);if(hd>selStart)hEnd=hd;}
  const rangeEnd=selEnd||hEnd;
  for(let d=1;d<=daysInMonth;d++){
    const cd=new Date(calYear,calMonth,d),past=cd<=today;
    const isSS=selStart&&cd.getTime()===selStart.getTime();
    const isSE=selEnd&&cd.getTime()===selEnd.getTime();
    const inRange=selStart&&rangeEnd&&cd>selStart&&cd<rangeEnd;
    const cls=['pcal-d',past?'dis':'',cd.getTime()===today.getTime()?'today':'',
      isSS?'sel-start':'',isSE?'sel-end':'',inRange?'in-range':''].filter(Boolean).join(' ');
    const el=document.createElement('div');
    el.className=cls;el.textContent=d;el.dataset.d=d;
    if(!past) el.onclick=()=>selectDay(d);
    grid.appendChild(el);
  }
}
function onCalMove(e){
  if(selPhase!==1) return;
  const cell=e.target.closest('.pcal-d[data-d]:not(.dis)');
  const d=cell?parseInt(cell.dataset.d):null;
  if(d!==calHover){calHover=d;renderCal();}
}
function onCalLeave(){if(calHover!==null){calHover=null;renderCal();}}
function selectDay(d){
  const date=new Date(calYear,calMonth,d);
  if(selPhase===0||selPhase===2){selStart=date;selEnd=null;selPhase=1;}
  else{
    if(date<selStart){selStart=date;selPhase=1;}
    else if(date.getTime()===selStart.getTime()){selStart=null;selPhase=0;}
    else{selEnd=date;selPhase=2;calHover=null;
      setTimeout(()=>{$('periodCal').style.display='none';$('periodDisplay').classList.remove('open');},180);
    }
  }
  timeOffDirty=true;updatePeriodDisplay();updateToSaveState();renderCal();
}
function updatePeriodDisplay(){
  const txt=$('periodText'),loc=LOCALE_MAP[appLang];
  if(!selStart){txt.textContent=I18N[appLang].timeOffEditor.selectDates;txt.style.color='var(--t3)';}
  else if(!selEnd){
    txt.textContent=selStart.toLocaleDateString(loc,{month:'short',day:'numeric'})+' → …';txt.style.color='var(--t2)';
  } else {
    const sameY=selStart.getFullYear()===selEnd.getFullYear();
    const d1=selStart.toLocaleDateString(loc,{month:'short',day:'numeric',...(sameY?{}:{year:'numeric'})});
    const d2=selEnd.toLocaleDateString(loc,{month:'short',day:'numeric',year:'numeric'});
    txt.textContent=d1+' – '+d2;txt.style.color='var(--t1)';
  }
}

let timeOffOrigUrl=null,timeOffPendingUrl=null;
function timeOffPickPhoto(){$('timeOffPhoInp').click();}
function openCropExisting(){if(timeOffOrigUrl) openCrop(timeOffOrigUrl,applyTimeOffCrop);}
function applyTimeOffCrop(url){
  timeOffDirty=true;timeOffCustomPhoto=true;timeOffPendingUrl=url;timeOffPhotoRemoved=false;
  $('timeOffDzThumb').style.backgroundImage=`url(${url})`;
  $('timeOffDzEmpty').style.display='none';$('timeOffDzImg').style.display='';$('toReuploadBtn').style.display='';$('toRemoveBtn').style.display='';
  updateToSaveState();
}
function loadTimeOffPhoto(inp){
  const file=inp.files[0];if(!file) return;inp.value='';
  const reader=new FileReader();
  reader.onload=e=>{timeOffOrigUrl=e.target.result;openCrop(timeOffOrigUrl,applyTimeOffCrop);};
  reader.readAsDataURL(file);
}

let pfOrigUrl=null,pfPendingUrl=null;
function pfPickPhoto(){$('pfPhotoInp').click();}
function openPfCropExisting(){if(pfOrigUrl) openCrop(pfOrigUrl,applyPfCrop,1,228,228,true);}
function applyPfCrop(url){
  pfPendingUrl=url;pfRemoved=false;
  $('pfDzThumb').style.backgroundImage=`url(${url})`;
  $('pfDzEmpty').style.display='none';$('pfDzImg').style.display='';$('pfReuploadBtn').style.display='';$('pfRemoveBtn').style.display='';
  pfDirty();
}
function loadProfilePhoto(inp){
  const file=inp.files[0];if(!file) return;inp.value='';
  const reader=new FileReader();
  reader.onload=e=>{pfOrigUrl=e.target.result;openCrop(pfOrigUrl,applyPfCrop,1,228,228,true);};
  reader.readAsDataURL(file);
}

let _cropR=528/396,_cropOutW=528,_cropOutH=396,_cropCircle=false;
let _cropImg=null,_cropCb=null,_cScale=1,_cBox={x:0,y:0,w:0,h:0},_cDrag=null;
let _zoom=1,_imgX=0,_imgY=0,_lastPinchDist=null;
const CHIT=16;

function openCrop(url,cb,ratio=528/396,outW=528,outH=396,circle=false){
  _cropR=ratio;_cropOutW=outW;_cropOutH=outH;_cropCircle=circle;_cropCb=cb;
  const img=new Image();
  img.onload=()=>{_cropImg=img;$('m-crop').classList.add('show');setTimeout(initCropCanvas,60);};
  img.src=url;
}
function initCropCanvas(){
  const cv=$('cropCanvas'),area=$('cropArea');
  const mw=area.clientWidth,mh=area.clientHeight;
  _zoom=1;_imgX=0;_imgY=0;
  _cScale=Math.min(mw/_cropImg.width,mh/_cropImg.height,1);
  const iw=Math.round(_cropImg.width*_cScale),ih=Math.round(_cropImg.height*_cScale);
  cv.width=mw;cv.height=mh;
  _imgX=Math.round((mw-iw)/2);_imgY=Math.round((mh-ih)/2);
  let bw,bh;
  if(iw/ih>=_cropR){bh=ih;bw=bh*_cropR;}else{bw=iw;bh=bw/_cropR;}
  _cBox={x:Math.round(_imgX+(iw-bw)/2),y:Math.round(_imgY+(ih-bh)/2),w:Math.round(bw),h:Math.round(bh)};
  drawCrop();
}
function drawCrop(){
  const cv=$('cropCanvas'),ctx=cv.getContext('2d');
  const imgW=Math.round(_cropImg.width*_cScale*_zoom),imgH=Math.round(_cropImg.height*_cScale*_zoom);
  const {x,y,w,h}=_cBox,cx=x+w/2,cy=y+h/2,r=w/2;
  ctx.clearRect(0,0,cv.width,cv.height);
  ctx.globalAlpha=0.35;ctx.drawImage(_cropImg,_imgX,_imgY,imgW,imgH);ctx.globalAlpha=1;
  ctx.save();ctx.beginPath();
  if(_cropCircle) ctx.arc(cx,cy,r,0,Math.PI*2);else ctx.rect(x,y,w,h);
  ctx.clip();ctx.drawImage(_cropImg,_imgX,_imgY,imgW,imgH);ctx.restore();
  if(_cropCircle){
    ctx.strokeStyle='rgba(255,255,255,.9)';ctx.lineWidth=1.5;
    ctx.beginPath();ctx.arc(cx,cy,r,0,Math.PI*2);ctx.stroke();
    ctx.fillStyle='#fff';
    [[cx,y],[x+w,cy],[cx,y+h],[x,cy]].forEach(([hx,hy])=>{ctx.beginPath();ctx.arc(hx,hy,4.5,0,Math.PI*2);ctx.fill();});
  } else {
    ctx.strokeStyle='rgba(255,255,255,.9)';ctx.lineWidth=1.5;ctx.strokeRect(x+.75,y+.75,w-1.5,h-1.5);
    ctx.strokeStyle='rgba(255,255,255,.18)';ctx.lineWidth=.7;ctx.beginPath();
    [1,2].forEach(i=>{ctx.moveTo(x+w*i/3,y);ctx.lineTo(x+w*i/3,y+h);ctx.moveTo(x,y+h*i/3);ctx.lineTo(x+w,y+h*i/3);});
    ctx.stroke();
    ctx.fillStyle='#fff';
    [[x,y],[x+w,y],[x,y+h],[x+w,y+h]].forEach(([hx,hy])=>{ctx.beginPath();ctx.arc(hx,hy,4.5,0,Math.PI*2);ctx.fill();});
  }
}
function cropZone(mx,my){
  const {x,y,w,h}=_cBox,d=(ax,ay)=>Math.hypot(mx-ax,my-ay);
  if(_cropCircle){
    const cx=x+w/2,cy=y+h/2;
    if(d(cx,y)<=CHIT) return 'nw';if(d(x+w,cy)<=CHIT) return 'ne';
    if(d(cx,y+h)<=CHIT) return 'se';if(d(x,cy)<=CHIT) return 'sw';
    if(d(cx,cy)<=w/2) return 'move';return null;
  }
  if(d(x,y)<=CHIT) return 'nw';if(d(x+w,y)<=CHIT) return 'ne';
  if(d(x,y+h)<=CHIT) return 'sw';if(d(x+w,y+h)<=CHIT) return 'se';
  if(mx>=x&&mx<=x+w&&my>=y&&my<=y+h) return 'move';return null;
}
function onCropDown(e){
  const cv=$('cropCanvas'),r=cv.getBoundingClientRect();
  const mx=(e.clientX-r.left)*(cv.width/r.width),my=(e.clientY-r.top)*(cv.height/r.height);
  const z=cropZone(mx,my);
  if(!z){
    const imgW=_cropImg.width*_cScale*_zoom,imgH=_cropImg.height*_cScale*_zoom;
    if(mx>=_imgX&&mx<=_imgX+imgW&&my>=_imgY&&my<=_imgY+imgH){
      _cDrag={z:'pan',sx:mx,sy:my,ix:_imgX,iy:_imgY};
      cv.setPointerCapture(e.pointerId);cv.style.cursor='grabbing';e.preventDefault();
    }
    return;
  }
  _cDrag={z,sx:mx,sy:my,box:{..._cBox}};
  cv.setPointerCapture(e.pointerId);
  if(z==='move') cv.style.cursor='grabbing';
  e.preventDefault();
}
function onCropMove(e){
  const cv=$('cropCanvas'),r=cv.getBoundingClientRect();
  const mx=Math.max(0,Math.min(cv.width,(e.clientX-r.left)*(cv.width/r.width)));
  const my=Math.max(0,Math.min(cv.height,(e.clientY-r.top)*(cv.height/r.height)));
  const imgW=_cropImg.width*_cScale*_zoom,imgH=_cropImg.height*_cScale*_zoom;
  const ir=_imgX+imgW,ib=_imgY+imgH;
  if(!_cDrag){
    const z=cropZone(mx,my),c={nw:'nw-resize',ne:'ne-resize',sw:'sw-resize',se:'se-resize',move:'grab'};
    cv.style.cursor=c[z]||(mx>=_imgX&&mx<=ir&&my>=_imgY&&my<=ib?'grab':'default');return;
  }
  if(_cDrag.z==='pan'){
    _imgX=_cDrag.ix+(mx-_cDrag.sx);_imgY=_cDrag.iy+(my-_cDrag.sy);
    clampImageToCrop();drawCrop();return;
  }
  // Visible region = intersection of image and canvas (crop box must stay inside)
  const visX0=Math.max(_imgX,0),visX1=Math.min(ir,cv.width);
  const visY0=Math.max(_imgY,0),visY1=Math.min(ib,cv.height);
  const MIN=50,ob=_cDrag.box;let nb={...ob};
  if(_cDrag.z==='move'){
    nb.x=Math.max(visX0,Math.min(visX1-ob.w,ob.x+(mx-_cDrag.sx)));
    nb.y=Math.max(visY0,Math.min(visY1-ob.h,ob.y+(my-_cDrag.sy)));
  } else {
    const isRight=_cDrag.z==='se'||_cDrag.z==='ne',isBottom=_cDrag.z==='se'||_cDrag.z==='sw';
    const fx=isRight?ob.x:ob.x+ob.w,fy=isBottom?ob.y:ob.y+ob.h;
    let rw=Math.abs(mx-fx),rh=Math.abs(my-fy),w,h;
    if(rw/_cropR>=rh){w=Math.max(MIN,rw);h=w/_cropR;}else{h=Math.max(MIN/_cropR,rh);w=h*_cropR;}
    nb.x=isRight?fx:fx-w;nb.y=isBottom?fy:fy-h;nb.w=w;nb.h=h;
    nb.x=Math.max(visX0,nb.x);nb.y=Math.max(visY0,nb.y);
    if(nb.x+nb.w>visX1){nb.w=visX1-nb.x;nb.h=nb.w/_cropR;}
    if(nb.y+nb.h>visY1){nb.h=visY1-nb.y;nb.w=nb.h*_cropR;}
    nb.x=Math.max(visX0,nb.x);
  }
  _cBox=nb;drawCrop();
}
function onCropUp(){
  if(_cDrag&&(_cDrag.z==='move'||_cDrag.z==='pan')) $('cropCanvas').style.cursor='grab';
  _cDrag=null;
}
function applyCrop(){
  const {x,y,w,h}=_cBox,es=_cScale*_zoom,out=document.createElement('canvas');
  out.width=_cropOutW;out.height=_cropOutH;
  out.getContext('2d').drawImage(_cropImg,(x-_imgX)/es,(y-_imgY)/es,w/es,h/es,0,0,_cropOutW,_cropOutH);
  if(_cropCb) _cropCb(out.toDataURL('image/jpeg',0.92));
  $('m-crop').classList.remove('show');
}
function cancelCrop(){$('m-crop').classList.remove('show');}
function clampImageToCrop(){
  // Clamp image to canvas bounds — prevents off-screen drift.
  // Canvas-bounds clamp also guarantees crop-box coverage because the crop
  // box is always within [0, cv.width] × [0, cv.height].
  const cv=$('cropCanvas');
  const imgW=_cropImg.width*_cScale*_zoom,imgH=_cropImg.height*_cScale*_zoom;
  if(imgW>cv.width) _imgX=Math.max(cv.width-imgW,Math.min(0,_imgX));
  else _imgX=Math.round((cv.width-imgW)/2);
  if(imgH>cv.height) _imgY=Math.max(cv.height-imgH,Math.min(0,_imgY));
  else _imgY=Math.round((cv.height-imgH)/2);
}
function onCropWheel(e){
  e.preventDefault();
  const cv=$('cropCanvas'),r=cv.getBoundingClientRect();
  const mx=(e.clientX-r.left)*(cv.width/r.width),my=(e.clientY-r.top)*(cv.height/r.height);
  // Scale proportionally to deltaY so each physical scroll notch (≈100 units)
  // gives ~10% zoom instead of a flat 10% per fired event (which fires many
  // times per gesture on trackpads, causing runaway zoom).
  const factor=Math.pow(0.999,e.deltaY);
  const minZ=Math.max(_cBox.w/(_cropImg.width*_cScale),_cBox.h/(_cropImg.height*_cScale));
  const newZoom=Math.max(minZ,Math.min(5,_zoom*factor));
  const zf=newZoom/_zoom;
  _imgX=mx+(_imgX-mx)*zf;_imgY=my+(_imgY-my)*zf;_zoom=newZoom;
  clampImageToCrop();drawCrop();
}
function onCropTouchStart(e){
  if(e.touches.length===2){e.preventDefault();
    _lastPinchDist=Math.hypot(e.touches[0].clientX-e.touches[1].clientX,e.touches[0].clientY-e.touches[1].clientY);
    _cDrag=null;
  }
}
function onCropTouchMove(e){
  if(e.touches.length===2&&_lastPinchDist!==null){e.preventDefault();
    const dist=Math.hypot(e.touches[0].clientX-e.touches[1].clientX,e.touches[0].clientY-e.touches[1].clientY);
    const factor=dist/_lastPinchDist;_lastPinchDist=dist;
    const cv=$('cropCanvas'),r=cv.getBoundingClientRect();
    const px=(e.touches[0].clientX+e.touches[1].clientX)/2,py=(e.touches[0].clientY+e.touches[1].clientY)/2;
    const cx=(px-r.left)*(cv.width/r.width),cy=(py-r.top)*(cv.height/r.height);
    const minZ=Math.max(_cBox.w/(_cropImg.width*_cScale),_cBox.h/(_cropImg.height*_cScale));
    const newZoom=Math.max(minZ,Math.min(5,_zoom*factor));
    const zf=newZoom/_zoom;
    _imgX=cx+(_imgX-cx)*zf;_imgY=cy+(_imgY-cy)*zf;_zoom=newZoom;
    clampImageToCrop();drawCrop();
  }
}
function onCropTouchEnd(e){if(e.touches.length<2) _lastPinchDist=null;}

let calSrc='ms',calPending='ms';
const calInfo={ms:{name:'Microsoft Teams',ok:true},gg:{name:'Google Calendar',ok:false}};
function _renderCalOpts(s){
  ['ms','gg'].forEach(k=>$('co-'+k).classList.toggle('sel',k===s));
  $('msSignInRow').style.display=s==='ms'?'':'none';
  $('ggSignInRow').style.display=s==='gg'?'':'none';
}
function _updateCalSave(){
  const btn=$('calSaveBtn');if(!btn) return;
  if(calPending!==calSrc)btn.removeAttribute('disabled');else btn.setAttribute('disabled','');
}
function openCalendarSource(){
  calPending=calSrc;_renderCalOpts(calPending);_updateCalSave();show('s-calendar');
}
function setCalPending(s){calPending=s;_renderCalOpts(s);_updateCalSave();}
function _calStatusSuffix(info){
  const t=I18N[appLang];
  return ' · '+(info.ok?t.main.connected:t.calendarSource.notConnected);
}
function saveCalSource(){
  calSrc=calPending;
  const info=calInfo[calSrc];
  $('mainCalSub').textContent=info.name+_calStatusSuffix(info);
  invoke('set_calendar_source',{source:calSrc});
  back();
}
function discardCalSource(){calPending=calSrc;_renderCalOpts(calPending);back();}
let ggSigningIn=false;
function _renderGgStatus(){
  const t=I18N[appLang].calendarSource;
  $('ggSt').textContent=ggSigningIn?t.signingIn:(calInfo.gg.ok?I18N[appLang].main.connected:t.notSignedIn);
}
function _renderGgSignBtn(){
  const t=I18N[appLang].calendarSource;
  $('ggSignBtn').textContent=calInfo.gg.ok?t.signOutGoogle:t.signInGoogle;
}
function signGoogle(){
  if(calInfo.gg.ok){
    invoke('oauth_signout',{provider:'google'});
    calInfo.gg.ok=false;_renderGgStatus();_renderGgSignBtn();
    return;
  }
  ggSigningIn=true;_renderGgStatus();$('ggSignBtn').disabled=true;
  invoke('oauth_google').then(()=>{
    ggSigningIn=false;calInfo.gg.ok=true;_renderGgStatus();
    $('ggSignBtn').disabled=false;_renderGgSignBtn();setCalPending('gg');
  }).catch(()=>{ggSigningIn=false;_renderGgStatus();$('ggSignBtn').disabled=false;});
}
let msSigningIn=false;
function _renderMsStatus(){
  const t=I18N[appLang].calendarSource;
  $('teamsStatusTxt').textContent=msSigningIn?t.signingIn:(calInfo.ms.ok?t.teamsStatus:t.notSignedIn);
}
function _renderMsSignBtn(){
  const t=I18N[appLang].calendarSource;
  $('msSignBtn').textContent=calInfo.ms.ok?t.signOutMicrosoft:t.signInMicrosoft;
}
function signMicrosoft(){
  if(calInfo.ms.ok){
    invoke('oauth_signout',{provider:'microsoft'});
    calInfo.ms.ok=false;_renderMsStatus();_renderMsSignBtn();
    return;
  }
  msSigningIn=true;_renderMsStatus();$('msSignBtn').disabled=true;
  invoke('oauth_microsoft').then(()=>{
    msSigningIn=false;calInfo.ms.ok=true;_renderMsStatus();
    $('msSignBtn').disabled=false;_renderMsSignBtn();setCalPending('ms');
  }).catch(()=>{msSigningIn=false;_renderMsStatus();$('msSignBtn').disabled=false;});
}

const siImgMap={
  'vol-mute':'assets/shortcut_icons/vol-mute.png',
  'mic-mute':'assets/shortcut_icons/mic-mute.png',
  'screenshot':'assets/shortcut_icons/screenshot.png',
  'lock-screen':'assets/shortcut_icons/lock-screen.png',
  'favorite':'assets/shortcut_icons/favorite.png',
  'calculator':'assets/shortcut_icons/calculator.png'
};
function applySlot(n){
  const v=$('ss'+n).value;
  $('si'+n).src=siImgMap[v]||'';
  const favEl=$('fav'+n);
  favEl.style.display=v==='favorite'?'block':'none';
  if(v==='favorite') renderKbdCombo(n);
  _updateSlotSave();
}
// Orion reads Device Settings from Ori on every (re)connect to recover the
// NVS-persisted fields (clock_face, time_format, ancs_filter, shortcut slot
// tokens) — ble-protocol.md §6.4. Presence/weather are ephemeral and not
// returned here.
function readSlotsFromDevice(){
  invoke('read_device_settings').then(s=>{
    if(s.c!==undefined){
      clockFace=s.c===1?'analog':'digital';
      $('mcDig').style.display=clockFace==='digital'?'flex':'none';$('mcAna').style.display=clockFace==='analog'?'block':'none';
    }
    if(s.h!==undefined){timeFormat=s.h===1?'12':'24';_renderMainTimeFormatPreview();}
    if(s.f!==undefined){
      ancsLevel=s.f;
      [0,1,2,3].forEach(i=>$('an-ico-'+i).style.display=i===ancsLevel?'block':'none');
    }
    [1,2,3].forEach(n=>{
      const tok=s[String(n)];
      if(tok){slotCommitted[n-1]=tok;$('ss'+n).value=tok;}
      kbdCommitted[n-1]=[];_kbdCombos[n-1]=[];
      applySlot(n);
      $('ms'+n).src=siImgMap[$('ss'+n).value]||'';
    });
  });
}

// ── Keyboard shortcut recorder ──────────────────────────────────────────────
let _kbdRecSlot=0;
const _kbdCombos=[[],[],[]];
// Committed (saved) state for subscreens with Save buttons
let ancsLevel=3;let ancsPending=3;
let clockFace='digital';let clockPending='digital';
let timeFormat='24';let timeFormatPending='24';
const slotCommitted=['vol-mute','mic-mute','screenshot'];
const kbdCommitted=[[],[],[]];

function renderKbdCombo(n){
  const parts=_kbdCombos[n-1];
  const disp=$('kbdDisp'+n),hint=$('kbdHint'+n);
  const t=I18N[appLang].quickActions;
  if(!parts.length){
    disp.innerHTML=`<span class="kbd-unset">${t.notSet}</span>`;
    hint.textContent=t.clickToSet;
  } else {
    disp.innerHTML=parts.map(p=>`<kbd class="kc">${p}</kbd>`).join('<span class="kbd-sep"> + </span>');
    hint.textContent=t.clickToChange;
  }
}

function startKbdRecord(n){
  if(_kbdRecSlot) stopKbdRecord();
  _kbdRecSlot=n;
  const t=I18N[appLang].quickActions;
  $('kbdRec'+n).classList.add('recording');
  $('kbdDisp'+n).innerHTML=`<span class="kbd-recording-text">${t.pressShortcut}</span>`;
  $('kbdHint'+n).textContent=t.escToCancel;
  document.addEventListener('keydown',_onKbdKey,true);
}

function stopKbdRecord(){
  const n=_kbdRecSlot; if(!n) return;
  _kbdRecSlot=0;
  document.removeEventListener('keydown',_onKbdKey,true);
  $('kbdRec'+n).classList.remove('recording');
  renderKbdCombo(n);
  _updateSlotSave();
}

function _onKbdKey(e){
  e.preventDefault(); e.stopPropagation();
  const n=_kbdRecSlot; if(!n) return;
  if(e.key==='Escape'){ stopKbdRecord(); return; }
  if(['Control','Alt','Shift','Meta'].includes(e.key)) return;
  const parts=[];
  if(e.ctrlKey)  parts.push('Ctrl');
  if(e.altKey)   parts.push('Alt');
  if(e.shiftKey) parts.push('Shift');
  if(e.metaKey)  parts.push('Win');
  let k=e.key;
  if(k.length===1)       k=k.toUpperCase();
  else if(k===' ')       k='Space';
  else if(k==='Backspace')k='⌫';
  else if(k==='Delete')  k='Del';
  else if(k==='Enter')   k='↵';
  else if(k==='Tab')     k='Tab';
  else if(k==='ArrowUp') k='↑';
  else if(k==='ArrowDown')k='↓';
  else if(k==='ArrowLeft')k='←';
  else if(k==='ArrowRight')k='→';
  else if(k==='Home')    k='Home';
  else if(k==='End')     k='End';
  else if(k==='PageUp')  k='PgUp';
  else if(k==='PageDown')k='PgDn';
  else if(k==='Insert')  k='Ins';
  parts.push(k);
  _kbdCombos[n-1]=parts;
  stopKbdRecord();
}

// ── Subscreen pending-state helpers ─────────────────────────────────────────
function _renderClock(f){
  $('co-dig').classList.toggle('sel',f==='digital');$('co-ana').classList.toggle('sel',f==='analog');
}
function _renderAncs(l){
  [0,1,2,3].forEach(i=>$('an-'+i).classList.toggle('sel',i===l));
}
function _updateClockSave(){
  const d=clockPending!==clockFace;
  if(d)$('clockSaveBtn').removeAttribute('disabled');else $('clockSaveBtn').setAttribute('disabled','');
}
function _renderTimeFormat(f){
  $('tf-24').classList.toggle('sel',f==='24');$('tf-12').classList.toggle('sel',f==='12');
}
function _updateTimeFormatSave(){
  const d=timeFormatPending!==timeFormat;
  if(d)$('tfSaveBtn').removeAttribute('disabled');else $('tfSaveBtn').setAttribute('disabled','');
}
// Renders the main-row preview using a fixed sample time (2:30 PM) so it
// reflects both the chosen format and the active app language's locale.
function _renderMainTimeFormatPreview(){
  const sample=new Date(2024,0,1,14,30);
  $('mainTimeFormatPreview').textContent=
    sample.toLocaleTimeString(LOCALE_MAP[appLang],{hour:'numeric',minute:'2-digit',hour12:timeFormat==='12'});
}
function _updateAncsSave(){
  const d=ancsPending!==ancsLevel;
  if(d)$('ancsSaveBtn').removeAttribute('disabled');else $('ancsSaveBtn').setAttribute('disabled','');
}
function _isSlotsDirty(){
  return [1,2,3].some(n=>{
    if($('ss'+n).value!==slotCommitted[n-1]) return true;
    const p=_kbdCombos[n-1],c=kbdCommitted[n-1];
    return p.length!==c.length||p.some((v,i)=>v!==c[i]);
  });
}
function _hasIncompleteFavorite(){
  return [1,2,3].some(n=>$('ss'+n).value==='favorite'&&_kbdCombos[n-1].length===0);
}
function _updateSlotSave(){
  const btn=$('slotsSaveBtn'); if(!btn) return;
  if(_isSlotsDirty()&&!_hasIncompleteFavorite())btn.removeAttribute('disabled');else btn.setAttribute('disabled','');
}

// ── Subscreen open (initialises pending from committed) ──────────────────────
function openAncs(){
  ancsPending=ancsLevel;_renderAncs(ancsPending);_updateAncsSave();show('s-ancs');
}
function openClock(){
  clockPending=clockFace;_renderClock(clockPending);_updateClockSave();show('s-clock');
}
function openTimeFormat(){
  timeFormatPending=timeFormat;_renderTimeFormat(timeFormatPending);_updateTimeFormatSave();show('s-timeformat');
}
function openShortcuts(){
  [1,2,3].forEach(n=>{$('ss'+n).value=slotCommitted[n-1];_kbdCombos[n-1]=[...kbdCommitted[n-1]];applySlot(n);});
  _updateSlotSave();show('s-shortcuts');
}

// ── Subscreen option selection ───────────────────────────────────────────────
function setClock(f){clockPending=f;_renderClock(f);_updateClockSave();}
function setAncs(level){ancsPending=level;_renderAncs(level);_updateAncsSave();}
function setTimeFormat(f){timeFormatPending=f;_renderTimeFormat(f);_updateTimeFormatSave();}

// ── Save handlers ────────────────────────────────────────────────────────────
function saveAncs(){
  ancsLevel=ancsPending;
  [0,1,2,3].forEach(i=>$('an-ico-'+i).style.display=i===ancsLevel?'block':'none');
  invoke('save_device_settings',{f:ancsLevel});
  back();
}
function saveClock(){
  clockFace=clockPending;
  $('mcDig').style.display=clockFace==='digital'?'flex':'none';$('mcAna').style.display=clockFace==='analog'?'block':'none';
  invoke('save_device_settings',{c:clockFace==='analog'?1:0});
  back();
}
function saveTimeFormat(){
  timeFormat=timeFormatPending;
  _renderMainTimeFormatPreview();
  invoke('save_device_settings',{h:timeFormat==='12'?1:0});
  back();
}
function saveSlots(){
  if(_kbdRecSlot) stopKbdRecord();
  // Commit pending state
  [1,2,3].forEach(n=>{slotCommitted[n-1]=$('ss'+n).value;kbdCommitted[n-1]=[..._kbdCombos[n-1]];});
  [1,2,3].forEach(i=>$('ms'+i).src=siImgMap[$('ss'+i).value]||'');
  // Icon tokens go to Ori over BLE; Favorite key combos are a local Orion
  // setting only (pc-app.md — "host-side action mapping is local to Orion").
  invoke('save_shortcuts',{slots:slotCommitted,combos:kbdCommitted});
  back();
}

// ── Discard handlers ─────────────────────────────────────────────────────────
function discardAncs(){ancsPending=ancsLevel;_renderAncs(ancsPending);back();}
function discardClock(){clockPending=clockFace;_renderClock(clockPending);back();}
function discardTimeFormat(){timeFormatPending=timeFormat;_renderTimeFormat(timeFormatPending);back();}
function discardShortcuts(){
  if(_kbdRecSlot) stopKbdRecord();
  [1,2,3].forEach(n=>{$('ss'+n).value=slotCommitted[n-1];_kbdCombos[n-1]=[...kbdCommitted[n-1]];applySlot(n);});
  back();
}

// Reset: wipes Ori's own NVS + bonds (factory reset, ble-protocol.md §7.2)
// AND Orion's local cache (profile, calendar sign-in, shortcuts) — there's
// no longer a lesser "device-only" reset option, so this always walks back
// through first-run setup since there's now neither a local profile nor a
// paired device.
function doReset(){
  hideModal('m-reset');
  invoke('clear_all');
  calSrc='ms';calPending='ms';_renderCalOpts('ms');
  $('mainCalSub').textContent=calInfo.ms.name+_calStatusSuffix(calInfo.ms);
  $('nmInp').value='';$('tlInp').value='';$('emInp').value='';$('phInp').value='';
  pfOrigUrl=null;pfPendingUrl=null;
  $('pfDzThumb').style.backgroundImage='';$('pfDzEmpty').style.display='';$('pfDzImg').style.display='none';
  $('mainProfPhoto').style.backgroundImage='';
  $('mainProfInitials').style.display='';$('mainProfInitials').textContent='?';
  $('mainName').textContent='Your Name';

  // Time Off — was previously left committed, so it reappeared on the next
  // setup pass even though the rest of the profile was wiped.
  timeOffDirty=false;timeOffActive=false;timeOffCustomPhoto=false;timeOffPhotoRemoved=false;
  toCommittedStart=null;toCommittedEnd=null;toCommittedDest='';toCommittedPhotoUrl=null;
  timeOffOrigUrl=null;timeOffPendingUrl=null;
  $('mainTimeOffBanner').style.backgroundImage='';
  $('timeOffToggle').classList.remove('on');setTimeOffState(false);

  // Quick Actions shortcuts — back to the factory default tokens/combos.
  const defaultSlots=['vol-mute','mic-mute','screenshot'];
  [1,2,3].forEach(n=>{
    slotCommitted[n-1]=defaultSlots[n-1];kbdCommitted[n-1]=[];
    $('ss'+n).value=defaultSlots[n-1];applySlot(n);
    $('ms'+n).src=siImgMap[defaultSlots[n-1]]||'';
  });

  // Clock face / time format / notification filter — back to Ori's defaults.
  clockFace='digital';clockPending='digital';
  $('mcDig').style.display='flex';$('mcAna').style.display='none';
  timeFormat='24';timeFormatPending='24';_renderMainTimeFormatPreview();
  ancsLevel=3;ancsPending=3;
  [0,1,2,3].forEach(i=>$('an-ico-'+i).style.display=i===3?'block':'none');

  setConn('off');
  openSetupWizard();
}

function clickFw(){
  if(fwAvail){showModal('m-fw');}
  else{const ico=$('fwIco');ico.style.transition='transform .15s';ico.style.transform='rotate(20deg)';setTimeout(()=>ico.style.transform='',300);}
}
function startFwInstall(){
  $('fw-c').style.display='none';$('fw-i').style.display='';
  invoke('firmware_install');
}
// Driven by 'fw-progress' events from the USB CDC OTA sender (ota.md) — phase
// is one of "downloading"/"verifying"/"installing"/"done".
function fwApplyProgress({pct,phase,version}){
  const t=I18N[appLang].fwModal;
  const ring=$('fwRing'),pctEl=$('fwPct'),lbl=$('fwLbl'),title=$('fwMTitle');
  if(phase==='done'){
    ring.style.strokeDashoffset=0;pctEl.textContent='✓';
    lbl.textContent=t.nowRunning.replace('{v}',version||'');title.textContent=t.done;
    setTimeout(()=>{
      hideModal('m-fw');fwAvail=false;
      const d=$('fwIco');d.classList.remove('fw-on');d.title=I18N[appLang].main.fwUpToDate;d.style.display='none';
      $('fw-c').style.display='';$('fw-i').style.display='none';
      ring.style.strokeDashoffset=358;pctEl.textContent='0%';title.textContent=t.updatingFirmware;
    },2000);
    return;
  }
  ring.style.strokeDashoffset=358-(358*Math.min(pct,100)/100);pctEl.textContent=Math.round(pct)+'%';
  if(phase==='verifying'){lbl.textContent=t.verifying;title.textContent=t.verifying;}
  else if(phase==='installing'){lbl.textContent=t.updatingFirmware;title.textContent=t.updatingFirmware;}
  else{lbl.textContent=t.keepPluggedIn;}
}

// ── Orion app self-update (separate from Ori firmware update above) ─────────
// Orion checks its own release channel independent of any Ori connection —
// this never touches BLE/USB CDC. Since Orion has no window to swap code
// under while running, the flow ends in a restart prompt rather than an
// automatic relaunch.
function startOrionInstall(){
  $('ou-c').style.display='none';$('ou-i').style.display='';
  invoke('orion_update_install');
}
// Driven by 'orion-update-progress' events; phase is
// "downloading"/"installing"/"ready".
function ouApplyProgress({pct,phase}){
  const t=I18N[appLang].orionUpdate;
  if(phase==='ready'){
    $('ou-i').style.display='none';$('ou-d').style.display='';
    $('ouDoneTitle').textContent=t.doneTitle;
    $('ouDoneBody').textContent=t.doneBody.replace('{v}',orionUpdateVersion);
    return;
  }
  const ring=$('ouRing'),pctEl=$('ouPct'),lbl=$('ouLbl'),title=$('ouInstallTitle');
  ring.style.strokeDashoffset=358-(358*Math.min(pct,100)/100);pctEl.textContent=Math.round(pct)+'%';
  const label=phase==='installing'?t.installing:t.downloading;
  lbl.textContent=label;title.textContent=label;
}
function restartOrion(){
  hideModal('m-orion-update');
  orionAppVersion=orionUpdateVersion;
  orionUpdateAvail=false;
  updateOrionUpdateRow();
  $('ou-c').style.display='';$('ou-i').style.display='none';$('ou-d').style.display='none';
  $('ouRing').style.strokeDashoffset=358;$('ouPct').textContent='0%';
  invoke('orion_restart');
}

// ── Language (single, union setting for Orion's own UI) ─────────────────────
// One app-wide language setting — no separate Ori-firmware language track.
// Starting point for the picker is the Welcome screen; covers the whole
// first-run wizard so switching there isn't a half-translated dead end.
// More surfaces (e.g. Settings) adopt this same `appLang` state as they're
// wired up. `appLang`/`LOCALE_MAP` themselves are declared at the top of the
// file — see the comment there.
const I18N={
  en:{
    name:'English',
    common:{cancel:'Cancel',save:'Save',back:'Back'},
    welcome:{title:'Welcome to Orion',
      desc:'Orion keeps your Ori in sync — profile, calendar, and quick controls, all handled automatically from this PC.',
      start:'Start'},
    profile:{changeLang:'Back',title:'Set up your profile',sub:"This is what appears on Ori's display.",
      photoLbl:'Profile Photo',choosePhoto:'Choose photo',recrop:'Recrop',reupload:'Reupload',remove:'Remove',
      nameLbl:'Name',namePh:'Full name',jobLbl:'Job title',jobPh:'e.g. Senior Product Manager',
      emailLbl:'Email',emailPh:'name@company.com',phoneLbl:'Phone',phonePh:'+1 555 000 0000',next:'Next'},
    discover:{editProfile:'Edit profile',title:'Select your Ori',sub:'Make sure Ori is powered on and in pairing mode.',
      scanning:'Looking for nearby Ori devices…',rescan:'Scan again',strongSignal:'Strong signal',weakSignal:'Weak signal'},
    passkey:{title:'Confirm pairing',bodyPre:'Windows will ask you to confirm a pairing code — check it matches the 6-digit code shown on ',bodySuf:', then accept it there.',cancel:'Cancel'},
    connecting:{title:'Pairing with Ori…',sub:'Waiting for Ori to confirm…'},
    syncing:{title:'Setting up Ori…',progressLabel:'A busy day ahead…',doneLabel:'Ori is set up!'},
    pairfail:{title:'Couldn’t pair with Ori',body:'The passkey didn’t match, or the request timed out on Ori. Pick a device to try again.',close:'Close'},
    settings:{title:'Settings',general:'General',auto:'Run automatically',autoSub:'Launch at Windows startup',
      calendar:'Calendar Source',language:'Language',about:'About',app:'Version',reset:'Reset'},
    main:{connected:'Connected',syncing:'Syncing…',disconnected:'Disconnected',timeOff:'Time Off',
      noTimeOffPlanned:'No Time Off planned',tapToSet:'Tap to set',notifFilterRow:'Notification Filter',
      clockFaceRow:'Clock Face',timeFormatRow:'Time Format',quickActionsRow:'Quick Actions',presenceAvailable:'Available',
      settingsIcoTitle:'Settings',minimizeIcoTitle:'Minimize',fwUpToDate:'Firmware up to date',fwAvailable:'Firmware update available'},
    profileEditor:{title:'Profile'},
    timeOffEditor:{periodLbl:'Period',selectDates:'Select dates…',selectStartDate:'Select start date',
      selectEndDate:'Now select end date',destinationLbl:'Destination',destinationPh:'City, Country',
      destinationPhotoLbl:'Destination photo'},
    calendarSource:{teamsStatus:'Signed in · Exchange Online',notSignedIn:'Not signed in',signingIn:'Signing in…',
      notConnected:'Not connected',signInGoogle:'Sign in with Google',signOutGoogle:'Sign out from Google',
      signInMicrosoft:'Sign in with Microsoft',signOutMicrosoft:'Sign out from Microsoft'},
    quickActions:{slotPrefix:'Slot',notSet:'Not set',clickToSet:'Click to set',clickToChange:'Click to change',
      pressShortcut:'Press shortcut…',escToCancel:'Esc to cancel',
      actionLabels:{'vol-mute':'Volume Mute','mic-mute':'Mic Mute',screenshot:'Screenshot','lock-screen':'Lock Screen',favorite:'Favorite',calculator:'Calculator'}},
    clockFace:{digitalLbl:'Digital',digitalSub:'Large digits',analogLbl:'Analog',analogSub:'Tick dial with hands'},
    timeFormat:{h24Lbl:'24-hour',h24Sub:'e.g. 14:30',h12Lbl:'12-hour',h12Sub:'e.g. 2:30 PM'},
    notifFilter:{disabledLbl:'Disabled',disabledSub:'No notifications shown on Ori',callOnlyLbl:'Call Only',
      callOnlySub:'Incoming calls only',importantLbl:'Important',importantSub:'Calls and high-priority alerts',
      allLbl:'All',allSub:'Every notification (default)'},
    discardModal:{title:'Discard changes?',body:'Unsaved changes will be lost.',keepEditing:'Keep editing',discard:'Discard'},
    resetModal:{title:'Reset',
      body:'This will erase your profile, settings, and factory reset Ori.',
      reset:'Reset'},
    fwModal:{title:'Ori Update Available',update:'Update',
      updatingFirmware:'Updating Ori…',keepPluggedIn:'Keep Ori plugged in',verifying:'Verifying…',
      nowRunning:'Now running {v}',done:'Done',
      changelog:['Added Vietnamese, Spanish, and French language support','Faster reconnect after Ori goes to sleep','Fixed a rare crash when removing a Time Off photo']},
    orionUpdate:{title:'Orion Update Available',update:'Update',
      downloading:'Updating Orion…',installing:'Installing…',
      doneTitle:'Orion Update Ready',doneBody:'Restart Orion to finish updating to {v}.',
      restartNow:'Restart Now',later:'Later',
      rowMain:'Update available',rowSub:'Version {v} · Tap to install',
      changelog:['Improved reconnect reliability after sleep/wake','Added French language support','Fixed tray icon flicker on Windows 11']},
    cropOverlay:{title:'Crop Photo',apply:'Apply',
      hint:'Drag crop box to reposition · Drag corners to resize · Scroll or pinch to zoom · Drag outside box to pan'}
  },
  vi:{
    name:'Tiếng Việt',
    common:{cancel:'Hủy',save:'Lưu',back:'Quay lại'},
    welcome:{title:'Chào mừng đến với Orion',
      desc:'Orion giữ Ori của bạn luôn đồng bộ — hồ sơ, lịch và các điều khiển nhanh, tất cả được xử lý tự động từ máy tính này.',
      start:'Bắt đầu'},
    profile:{changeLang:'Quay lại',title:'Thiết lập hồ sơ của bạn',sub:'Đây là thông tin hiển thị trên màn hình Ori.',
      photoLbl:'Ảnh hồ sơ',choosePhoto:'Chọn ảnh',recrop:'Cắt lại',reupload:'Tải lại ảnh',remove:'Xóa ảnh',
      nameLbl:'Họ tên',namePh:'Họ và tên',jobLbl:'Chức danh',jobPh:'VD: Trưởng phòng Sản phẩm',
      emailLbl:'Email',emailPh:'ten@congty.com',phoneLbl:'Số điện thoại',phonePh:'+84 90 000 0000',next:'Tiếp theo'},
    discover:{editProfile:'Sửa hồ sơ',title:'Chọn Ori của bạn',sub:'Đảm bảo Ori đã mở và đang ở chế độ ghép nối.',
      scanning:'Đang tìm thiết bị Ori gần đây…',rescan:'Quét lại',strongSignal:'Tín hiệu mạnh',weakSignal:'Tín hiệu yếu'},
    passkey:{title:'Xác nhận ghép nối',bodyPre:'Windows sẽ yêu cầu bạn xác nhận mã ghép nối — kiểm tra mã đó khớp với mã 6 số hiển thị trên ',bodySuf:', sau đó chấp nhận ở đó.',cancel:'Hủy'},
    connecting:{title:'Đang ghép nối với Ori…',sub:'Đang chờ Ori xác nhận…'},
    syncing:{title:'Đang thiết lập Ori…',progressLabel:'Một ngày bận rộn đang chờ…',doneLabel:'Ori đã thiết lập xong!'},
    pairfail:{title:'Không thể ghép nối với Ori',body:'Mã ghép nối không khớp, hoặc yêu cầu đã hết thời gian chờ trên Ori. Hãy chọn một thiết bị để thử lại.',close:'Đóng'},
    settings:{title:'Cài đặt',general:'Chung',auto:'Tự động chạy',autoSub:'Khởi động cùng Windows',
      calendar:'Nguồn lịch',language:'Ngôn ngữ',about:'Giới thiệu',app:'Phiên bản',reset:'Đặt lại'},
    main:{connected:'Đã kết nối',syncing:'Đang đồng bộ…',disconnected:'Đã ngắt kết nối',timeOff:'Nghỉ phép',
      noTimeOffPlanned:'Chưa có lịch nghỉ phép',tapToSet:'Nhấn để đặt',notifFilterRow:'Bộ lọc thông báo',
      clockFaceRow:'Mặt đồng hồ',timeFormatRow:'Định dạng giờ',quickActionsRow:'Thao tác nhanh',presenceAvailable:'Đang hoạt động',
      settingsIcoTitle:'Cài đặt',minimizeIcoTitle:'Thu nhỏ',fwUpToDate:'Firmware đã mới nhất',fwAvailable:'Có bản cập nhật firmware'},
    profileEditor:{title:'Hồ sơ'},
    timeOffEditor:{periodLbl:'Khoảng thời gian',selectDates:'Chọn ngày…',selectStartDate:'Chọn ngày bắt đầu',
      selectEndDate:'Bây giờ chọn ngày kết thúc',destinationLbl:'Điểm đến',destinationPh:'Thành phố, Quốc gia',
      destinationPhotoLbl:'Ảnh điểm đến'},
    calendarSource:{teamsStatus:'Đã đăng nhập · Exchange Online',notSignedIn:'Chưa đăng nhập',signingIn:'Đang đăng nhập…',
      notConnected:'Chưa kết nối',signInGoogle:'Đăng nhập bằng Google',signOutGoogle:'Đăng xuất khỏi Google',
      signInMicrosoft:'Đăng nhập bằng Microsoft',signOutMicrosoft:'Đăng xuất khỏi Microsoft'},
    quickActions:{slotPrefix:'Khe',notSet:'Chưa đặt',clickToSet:'Nhấn để đặt',clickToChange:'Nhấn để đổi',
      pressShortcut:'Nhấn tổ hợp phím…',escToCancel:'Nhấn Esc để hủy',
      actionLabels:{'vol-mute':'Tắt âm lượng','mic-mute':'Tắt mic',screenshot:'Chụp màn hình','lock-screen':'Khóa màn hình',favorite:'Yêu thích',calculator:'Máy tính'}},
    clockFace:{digitalLbl:'Số',digitalSub:'Chữ số lớn',analogLbl:'Kim',analogSub:'Mặt số kim chỉ giờ'},
    timeFormat:{h24Lbl:'24 giờ',h24Sub:'VD: 14:30',h12Lbl:'12 giờ',h12Sub:'VD: 2:30 CH'},
    notifFilter:{disabledLbl:'Tắt',disabledSub:'Không hiển thị thông báo trên Ori',callOnlyLbl:'Chỉ cuộc gọi',
      callOnlySub:'Chỉ cuộc gọi đến',importantLbl:'Quan trọng',importantSub:'Cuộc gọi và thông báo quan trọng',
      allLbl:'Tất cả',allSub:'Mọi thông báo (mặc định)'},
    discardModal:{title:'Hủy các thay đổi?',body:'Các thay đổi chưa lưu sẽ bị mất.',keepEditing:'Tiếp tục chỉnh sửa',discard:'Hủy bỏ'},
    resetModal:{title:'Đặt lại',
      body:'Việc này sẽ xóa hồ sơ, cài đặt của bạn, và đặt lại Ori về mặc định gốc.',
      reset:'Đặt lại'},
    fwModal:{title:'Có bản cập nhật Ori',update:'Cập nhật',
      updatingFirmware:'Đang cập nhật Ori…',keepPluggedIn:'Giữ Ori được cắm điện',verifying:'Đang xác minh…',
      nowRunning:'Đang chạy {v}',done:'Hoàn tất',
      changelog:['Đã thêm hỗ trợ tiếng Việt, tiếng Tây Ban Nha và tiếng Pháp','Kết nối lại nhanh hơn sau khi Ori vào chế độ ngủ','Đã sửa lỗi hiếm gặp gây treo khi xóa ảnh Nghỉ phép']},
    orionUpdate:{title:'Có bản cập nhật Orion',update:'Cập nhật',
      downloading:'Đang cập nhật Orion…',installing:'Đang cài đặt…',
      doneTitle:'Orion đã sẵn sàng cập nhật',doneBody:'Khởi động lại Orion để hoàn tất cập nhật lên {v}.',
      restartNow:'Khởi động lại ngay',later:'Để sau',
      rowMain:'Có bản cập nhật',rowSub:'Phiên bản {v} · Nhấn để cài đặt',
      changelog:['Cải thiện độ ổn định kết nối lại sau khi ngủ/thức','Đã thêm hỗ trợ tiếng Pháp','Đã sửa lỗi biểu tượng khay nhấp nháy trên Windows 11']},
    cropOverlay:{title:'Cắt ảnh',apply:'Áp dụng',
      hint:'Kéo khung cắt để di chuyển · Kéo góc để đổi kích thước · Cuộn hoặc chụm để thu phóng · Kéo ngoài khung để di chuyển ảnh'}
  },
  es:{
    name:'Español',
    common:{cancel:'Cancelar',save:'Guardar',back:'Atrás'},
    welcome:{title:'Bienvenido a Orion',
      desc:'Orion mantiene tu Ori sincronizado — perfil, calendario y controles rápidos, todo gestionado automáticamente desde este PC.',
      start:'Comenzar'},
    profile:{changeLang:'Atrás',title:'Configura tu perfil',sub:'Esto es lo que aparece en la pantalla de Ori.',
      photoLbl:'Foto de perfil',choosePhoto:'Elegir foto',recrop:'Recortar de nuevo',reupload:'Subir otra vez',remove:'Quitar',
      nameLbl:'Nombre',namePh:'Nombre completo',jobLbl:'Cargo',jobPh:'p. ej. Gerente de Producto',
      emailLbl:'Correo',emailPh:'nombre@empresa.com',phoneLbl:'Teléfono',phonePh:'+34 600 000 000',next:'Siguiente'},
    discover:{editProfile:'Editar perfil',title:'Selecciona tu Ori',sub:'Asegúrate de que Ori esté encendido y en modo de emparejamiento.',
      scanning:'Buscando dispositivos Ori cercanos…',rescan:'Buscar de nuevo',strongSignal:'Señal fuerte',weakSignal:'Señal débil'},
    passkey:{title:'Confirmar emparejamiento',bodyPre:'Windows te pedirá confirmar un código de emparejamiento — comprueba que coincide con el código de 6 dígitos que aparece en ',bodySuf:', y acéptalo allí.',cancel:'Cancelar'},
    connecting:{title:'Emparejando con Ori…',sub:'Esperando confirmación de Ori…'},
    syncing:{title:'Configurando Ori…',progressLabel:'Un día ocupado por delante…',doneLabel:'¡Ori está listo!'},
    pairfail:{title:'No se pudo emparejar con Ori',body:'El código no coincidió, o la solicitud caducó en Ori. Elige un dispositivo para intentarlo de nuevo.',close:'Cerrar'},
    settings:{title:'Configuración',general:'General',auto:'Iniciar automáticamente',autoSub:'Abrir al iniciar Windows',
      calendar:'Fuente de calendario',language:'Idioma',about:'Acerca de',app:'Versión',reset:'Restablecer'},
    main:{connected:'Conectado',syncing:'Sincronizando…',disconnected:'Desconectado',timeOff:'Tiempo libre',
      noTimeOffPlanned:'Sin tiempo libre planeado',tapToSet:'Toca para configurar',notifFilterRow:'Filtro de notificaciones',
      clockFaceRow:'Esfera del reloj',timeFormatRow:'Formato de hora',quickActionsRow:'Acciones rápidas',presenceAvailable:'Disponible',
      settingsIcoTitle:'Configuración',minimizeIcoTitle:'Minimizar',fwUpToDate:'Firmware actualizado',fwAvailable:'Actualización de firmware disponible'},
    profileEditor:{title:'Perfil'},
    timeOffEditor:{periodLbl:'Periodo',selectDates:'Selecciona fechas…',selectStartDate:'Selecciona la fecha de inicio',
      selectEndDate:'Ahora selecciona la fecha de fin',destinationLbl:'Destino',destinationPh:'Ciudad, país',
      destinationPhotoLbl:'Foto del destino'},
    calendarSource:{teamsStatus:'Sesión iniciada · Exchange Online',notSignedIn:'No has iniciado sesión',signingIn:'Iniciando sesión…',
      notConnected:'No conectado',signInGoogle:'Iniciar sesión con Google',signOutGoogle:'Cerrar sesión de Google',
      signInMicrosoft:'Iniciar sesión con Microsoft',signOutMicrosoft:'Cerrar sesión de Microsoft'},
    quickActions:{slotPrefix:'Ranura',notSet:'Sin definir',clickToSet:'Haz clic para definir',clickToChange:'Haz clic para cambiar',
      pressShortcut:'Presiona el atajo…',escToCancel:'Esc para cancelar',
      actionLabels:{'vol-mute':'Silenciar volumen','mic-mute':'Silenciar micrófono',screenshot:'Captura de pantalla','lock-screen':'Bloquear pantalla',favorite:'Favorito',calculator:'Calculadora'}},
    clockFace:{digitalLbl:'Digital',digitalSub:'Dígitos grandes',analogLbl:'Analógico',analogSub:'Esfera con agujas'},
    timeFormat:{h24Lbl:'24 horas',h24Sub:'p. ej. 14:30',h12Lbl:'12 horas',h12Sub:'p. ej. 2:30 p.m.'},
    notifFilter:{disabledLbl:'Desactivado',disabledSub:'No se muestran notificaciones en Ori',callOnlyLbl:'Solo llamadas',
      callOnlySub:'Solo llamadas entrantes',importantLbl:'Importante',importantSub:'Llamadas y alertas de alta prioridad',
      allLbl:'Todas',allSub:'Todas las notificaciones (predeterminado)'},
    discardModal:{title:'¿Descartar los cambios?',body:'Los cambios no guardados se perderán.',keepEditing:'Seguir editando',discard:'Descartar'},
    resetModal:{title:'Restablecer',
      body:'Esto eliminará tu perfil, tus ajustes, y restablecerá Ori a los valores de fábrica.',
      reset:'Restablecer'},
    fwModal:{title:'Actualización de Ori disponible',update:'Actualizar',
      updatingFirmware:'Actualizando Ori…',keepPluggedIn:'Mantén Ori conectado',verifying:'Verificando…',
      nowRunning:'Ahora ejecutando {v}',done:'Listo',
      changelog:['Se agregó soporte para vietnamita, español y francés','Reconexión más rápida cuando Ori sale del reposo','Se corrigió un error poco frecuente al eliminar una foto de Tiempo libre']},
    orionUpdate:{title:'Actualización de Orion disponible',update:'Actualizar',
      downloading:'Actualizando Orion…',installing:'Instalando…',
      doneTitle:'Actualización de Orion lista',doneBody:'Reinicia Orion para terminar de actualizar a {v}.',
      restartNow:'Reiniciar ahora',later:'Más tarde',
      rowMain:'Actualización disponible',rowSub:'Versión {v} · Toca para instalar',
      changelog:['Mejor fiabilidad de reconexión tras suspender/reanudar','Se agregó soporte para francés','Se corrigió el parpadeo del icono en la bandeja en Windows 11']},
    cropOverlay:{title:'Recortar foto',apply:'Aplicar',
      hint:'Arrastra el recuadro para reposicionar · Arrastra las esquinas para redimensionar · Desplázate o pellizca para hacer zoom · Arrastra fuera del recuadro para desplazar'}
  },
  fr:{
    name:'Français',
    common:{cancel:'Annuler',save:'Enregistrer',back:'Retour'},
    welcome:{title:'Bienvenue sur Orion',
      desc:'Orion synchronise votre Ori — profil, calendrier et raccourcis, tout est géré automatiquement depuis ce PC.',
      start:'Commencer'},
    profile:{changeLang:'Retour',title:'Configurez votre profil',sub:"C'est ce qui apparaît sur l'écran d'Ori.",
      photoLbl:'Photo de profil',choosePhoto:'Choisir une photo',recrop:'Recadrer',reupload:'Réimporter',remove:'Supprimer',
      nameLbl:'Nom',namePh:'Nom complet',jobLbl:'Poste',jobPh:'p. ex. Chef de produit senior',
      emailLbl:'E-mail',emailPh:'nom@entreprise.com',phoneLbl:'Téléphone',phonePh:'+33 6 00 00 00 00',next:'Suivant'},
    discover:{editProfile:'Modifier le profil',title:'Sélectionnez votre Ori',sub:"Assurez-vous qu'Ori est allumé et en mode d'appairage.",
      scanning:"Recherche d'appareils Ori à proximité…",rescan:'Rescanner',strongSignal:'Signal fort',weakSignal:'Signal faible'},
    passkey:{title:'Confirmer le jumelage',bodyPre:'Windows va vous demander de confirmer un code de jumelage — vérifiez qu\'il correspond au code à 6 chiffres affiché sur ',bodySuf:', puis acceptez-le.',cancel:'Annuler'},
    connecting:{title:'Appairage avec Ori…',sub:"En attente de confirmation d'Ori…"},
    syncing:{title:"Configuration d'Ori…",progressLabel:'Une journée bien remplie vous attend…',doneLabel:'Ori est configuré !'},
    pairfail:{title:"Impossible d'appairer avec Ori",body:'Le code ne correspondait pas, ou la demande a expiré sur Ori. Choisissez un appareil pour réessayer.',close:'Fermer'},
    settings:{title:'Paramètres',general:'Général',auto:'Démarrage automatique',autoSub:'Lancer au démarrage de Windows',
      calendar:'Source de calendrier',language:'Langue',about:'À propos',app:'Version',reset:'Réinitialiser'},
    main:{connected:'Connecté',syncing:'Synchronisation…',disconnected:'Déconnecté',timeOff:'Congé',
      noTimeOffPlanned:'Aucun congé prévu',tapToSet:'Toucher pour définir',notifFilterRow:'Filtre de notifications',
      clockFaceRow:"Cadran de l'horloge",timeFormatRow:"Format de l'heure",quickActionsRow:'Actions rapides',presenceAvailable:'Disponible',
      settingsIcoTitle:'Paramètres',minimizeIcoTitle:'Réduire',fwUpToDate:'Firmware à jour',fwAvailable:'Mise à jour du firmware disponible'},
    profileEditor:{title:'Profil'},
    timeOffEditor:{periodLbl:'Période',selectDates:'Sélectionner les dates…',selectStartDate:'Sélectionner la date de début',
      selectEndDate:'Sélectionnez maintenant la date de fin',destinationLbl:'Destination',destinationPh:'Ville, pays',
      destinationPhotoLbl:'Photo de la destination'},
    calendarSource:{teamsStatus:'Connecté · Exchange Online',notSignedIn:'Non connecté',signingIn:'Connexion en cours…',
      notConnected:'Non connecté',signInGoogle:'Se connecter avec Google',signOutGoogle:'Se déconnecter de Google',
      signInMicrosoft:'Se connecter avec Microsoft',signOutMicrosoft:'Se déconnecter de Microsoft'},
    quickActions:{slotPrefix:'Emplacement',notSet:'Non défini',clickToSet:'Cliquer pour définir',clickToChange:'Cliquer pour modifier',
      pressShortcut:'Appuyez sur le raccourci…',escToCancel:'Échap pour annuler',
      actionLabels:{'vol-mute':'Muet volume','mic-mute':'Muet micro',screenshot:"Capture d'écran",'lock-screen':"Verrouiller l'écran",favorite:'Favori',calculator:'Calculatrice'}},
    clockFace:{digitalLbl:'Numérique',digitalSub:'Grands chiffres',analogLbl:'Analogique',analogSub:'Cadran à aiguilles'},
    timeFormat:{h24Lbl:'24 heures',h24Sub:'ex. 14:30',h12Lbl:'12 heures',h12Sub:'ex. 2:30 PM'},
    notifFilter:{disabledLbl:'Désactivé',disabledSub:'Aucune notification affichée sur Ori',callOnlyLbl:'Appels uniquement',
      callOnlySub:'Appels entrants uniquement',importantLbl:'Important',importantSub:'Appels et alertes prioritaires',
      allLbl:'Toutes',allSub:'Toutes les notifications (par défaut)'},
    discardModal:{title:'Abandonner les modifications ?',body:'Les modifications non enregistrées seront perdues.',keepEditing:'Continuer la modification',discard:'Abandonner'},
    resetModal:{title:"Réinitialiser",
      body:"Cela supprimera votre profil, vos réglages, et réinitialisera Ori aux paramètres d'usine.",
      reset:'Réinitialiser'},
    fwModal:{title:"Mise à jour d'Ori disponible",update:'Mettre à jour',
      updatingFirmware:"Mise à jour d'Ori…",keepPluggedIn:'Gardez Ori branché',verifying:'Vérification…',
      nowRunning:'Exécute maintenant {v}',done:'Terminé',
      changelog:["Ajout de la prise en charge du vietnamien, de l'espagnol et du français","Reconnexion plus rapide après la mise en veille d'Ori","Correction d'un rare plantage lors de la suppression d'une photo de congé"]},
    orionUpdate:{title:"Mise à jour d'Orion disponible",update:'Mettre à jour',
      downloading:"Mise à jour d'Orion…",installing:'Installation…',
      doneTitle:"Mise à jour d'Orion prête",doneBody:"Redémarrez Orion pour terminer la mise à jour vers {v}.",
      restartNow:'Redémarrer maintenant',later:'Plus tard',
      rowMain:'Mise à jour disponible',rowSub:'Version {v} · Toucher pour installer',
      changelog:["Fiabilité de reconnexion améliorée après veille/réveil","Ajout de la prise en charge du français","Correction du scintillement de l'icône de la zone de notification sous Windows 11"]},
    cropOverlay:{title:'Recadrer la photo',apply:'Appliquer',
      hint:'Faites glisser le cadre pour le repositionner · Faites glisser les coins pour redimensionner · Faites défiler ou pincez pour zoomer · Faites glisser hors du cadre pour déplacer'}
  }
};
function setAppLang(code){
  if(!I18N[code]) return;
  appLang=code;
  applyI18n();
}
function applyI18n(){
  const t=I18N[appLang];
  $('welcomeLangSel').value=appLang;
  $('suWelcomeTitle').textContent=t.welcome.title;
  $('suWelcomeDesc').textContent=t.welcome.desc;
  $('suStartBtn').textContent=t.welcome.start;
  $('suChangeLangTxt').textContent=t.profile.changeLang;
  $('suProfileTitle').textContent=t.profile.title;
  $('suProfileSub').textContent=t.profile.sub;
  $('suPhotoLbl').textContent=t.profile.photoLbl;
  $('suChoosePhotoTxt').textContent=t.profile.choosePhoto;
  $('suRecropTxt').textContent=t.profile.recrop;
  $('suReuploadTxt').textContent=t.profile.reupload;
  $('suRemoveTxt').textContent=t.profile.remove;
  $('suNameLblTxt').textContent=t.profile.nameLbl;
  $('suNmInp').placeholder=t.profile.namePh;
  $('suJobLblTxt').textContent=t.profile.jobLbl;
  $('suTlInp').placeholder=t.profile.jobPh;
  $('suEmailLblTxt').textContent=t.profile.emailLbl;
  $('suEmInp').placeholder=t.profile.emailPh;
  $('suPhoneLblTxt').textContent=t.profile.phoneLbl;
  $('suPhInp').placeholder=t.profile.phonePh;
  $('suProfileNext').textContent=t.profile.next;
  $('suEditProfileTxt').textContent=t.discover.editProfile;
  $('suDiscoverTitle').textContent=t.discover.title;
  $('suDiscoverSub').textContent=t.discover.sub;
  $('suScanningTxt').textContent=t.discover.scanning;
  $('suPkTitle').textContent=t.passkey.title;
  $('suPkBodyPre').textContent=t.passkey.bodyPre;
  $('suPkBodySuf').textContent=t.passkey.bodySuf;
  $('suPkCancelBtn').textContent=t.passkey.cancel;
  $('suConnTitle').textContent=t.connecting.title;
  $('suConnSub').textContent=t.connecting.sub;
  $('suSyncTitle').textContent=t.syncing.title;
  $('suFailTitle').textContent=t.pairfail.title;
  $('suFailBody').textContent=t.pairfail.body;
  $('suFailCloseBtn').textContent=t.pairfail.close;
  $('settingsLangSel').value=appLang;
  $('settingsTitle').textContent=t.settings.title;
  $('settingsGeneralLbl').textContent=t.settings.general;
  $('settingsAutoMain').textContent=t.settings.auto;
  $('settingsAutoSub').textContent=t.settings.autoSub;
  $('settingsCalLbl').textContent=t.settings.calendar;
  $('settingsLangLbl').textContent=t.settings.language;
  $('settingsAboutLbl').textContent=t.settings.about;
  $('settingsAppLbl').textContent=t.settings.app;
  $('settingsResetBtn').textContent=t.settings.reset;
  $('settingsBackTxt').textContent=t.common.back;
  // main screen
  $('mainTimeOffLabel').textContent=t.main.timeOff;
  $('mainTimeOffEmptyTxt').textContent=t.main.noTimeOffPlanned;
  $('mainTimeOffEmptySub').textContent=t.main.tapToSet;
  $('mainAncsLabel').textContent=t.main.notifFilterRow;
  $('mainClockLabel').textContent=t.main.clockFaceRow;
  $('mainTimeFormatLabel').textContent=t.main.timeFormatRow;
  $('mainQaLabel').textContent=t.main.quickActionsRow;
  $('mainPText').textContent=t.main.presenceAvailable;
  $('gearIco').title=t.main.settingsIcoTitle;
  $('appMinimizeIco').title=t.main.minimizeIcoTitle;
  $('fwIco').title=fwAvail?t.main.fwAvailable:t.main.fwUpToDate;
  $('hState').textContent={on:t.main.connected,rec:t.main.syncing,off:t.main.disconnected}[connState];
  // profile editor
  $('pfEditTitle').textContent=t.profileEditor.title;
  $('pfPhotoLbl2').textContent=t.profile.photoLbl;
  $('pfChoosePhotoTxt').textContent=t.profile.choosePhoto;
  $('pfRecropTxt').textContent=t.profile.recrop;
  $('pfReuploadTxt').textContent=t.profile.reupload;
  $('pfRemoveTxt').textContent=t.profile.remove;
  $('pfNameLblTxt').textContent=t.profile.nameLbl;
  $('pfJobLblTxt').textContent=t.profile.jobLbl;
  $('pfEmailLblTxt').textContent=t.profile.emailLbl;
  $('emInp').placeholder=t.profile.emailPh;
  $('pfPhoneLblTxt').textContent=t.profile.phoneLbl;
  $('phInp').placeholder=t.profile.phonePh;
  $('pfCancelBtn').textContent=t.common.cancel;
  $('pfSaveBtn').textContent=t.common.save;
  // time off editor
  $('toEditTitle').textContent=t.main.timeOff;
  $('toPeriodLblTxt').textContent=t.timeOffEditor.periodLbl;
  $('toDestLblTxt').textContent=t.timeOffEditor.destinationLbl;
  $('timeOffDt').placeholder=t.timeOffEditor.destinationPh;
  $('toPhotoLbl').textContent=t.timeOffEditor.destinationPhotoLbl;
  $('toChoosePhotoTxt').textContent=t.profile.choosePhoto;
  $('toRecropTxt').textContent=t.profile.recrop;
  $('toReuploadTxt').textContent=t.profile.reupload;
  $('toRemoveTxt').textContent=t.profile.remove;
  $('toCancelBtn').textContent=t.common.cancel;
  $('toSaveBtn').textContent=t.common.save;
  updatePeriodDisplay();
  renderCal();
  // calendar source
  $('calEditTitle').textContent=t.settings.calendar;
  _renderMsStatus();
  _renderMsSignBtn();
  _renderGgStatus();
  _renderGgSignBtn();
  $('calCancelBtn').textContent=t.common.cancel;
  $('calSaveBtn').textContent=t.common.save;
  // quick actions
  $('qaEditTitle').textContent=t.main.quickActionsRow;
  $('qaSlotLbl1').textContent=t.quickActions.slotPrefix+' 1';
  $('qaSlotLbl2').textContent=t.quickActions.slotPrefix+' 2';
  $('qaSlotLbl3').textContent=t.quickActions.slotPrefix+' 3';
  document.querySelectorAll('#ss1 option,#ss2 option,#ss3 option').forEach(o=>{
    if(t.quickActions.actionLabels[o.value]) o.textContent=t.quickActions.actionLabels[o.value];
  });
  [1,2,3].forEach(renderKbdCombo);
  $('qaCancelBtn').textContent=t.common.cancel;
  $('slotsSaveBtn').textContent=t.common.save;
  // clock face
  $('clockEditTitle').textContent=t.main.clockFaceRow;
  $('clockDigitalLbl').textContent=t.clockFace.digitalLbl;
  $('clockDigitalSub').textContent=t.clockFace.digitalSub;
  $('clockAnalogLbl').textContent=t.clockFace.analogLbl;
  $('clockAnalogSub').textContent=t.clockFace.analogSub;
  $('clockCancelBtn').textContent=t.common.cancel;
  $('clockSaveBtn').textContent=t.common.save;
  // time format
  $('tfEditTitle').textContent=t.main.timeFormatRow;
  $('tf24Lbl').textContent=t.timeFormat.h24Lbl;
  $('tf24Sub').textContent=t.timeFormat.h24Sub;
  $('tf12Lbl').textContent=t.timeFormat.h12Lbl;
  $('tf12Sub').textContent=t.timeFormat.h12Sub;
  $('tfCancelBtn').textContent=t.common.cancel;
  $('tfSaveBtn').textContent=t.common.save;
  _renderMainTimeFormatPreview();
  // notification filter
  $('ancsEditTitle').textContent=t.main.notifFilterRow;
  $('ancsDisabledLbl').textContent=t.notifFilter.disabledLbl;
  $('ancsDisabledSub').textContent=t.notifFilter.disabledSub;
  $('ancsCallOnlyLbl').textContent=t.notifFilter.callOnlyLbl;
  $('ancsCallOnlySub').textContent=t.notifFilter.callOnlySub;
  $('ancsImportantLbl').textContent=t.notifFilter.importantLbl;
  $('ancsImportantSub').textContent=t.notifFilter.importantSub;
  $('ancsAllLbl').textContent=t.notifFilter.allLbl;
  $('ancsAllSub').textContent=t.notifFilter.allSub;
  $('ancsCancelBtn').textContent=t.common.cancel;
  $('ancsSaveBtn').textContent=t.common.save;
  // modals
  $('discardTitle').textContent=t.discardModal.title;
  $('discardBody').textContent=t.discardModal.body;
  $('discardKeepBtn').textContent=t.discardModal.keepEditing;
  $('discardBtn').textContent=t.discardModal.discard;
  $('resetTitle').textContent=t.resetModal.title;
  $('resetBody').textContent=t.resetModal.body;
  $('resetBtn').textContent=t.resetModal.reset;
  $('resetCancelBtn').textContent=t.common.cancel;
  $('fwTitle').textContent=t.fwModal.title;
  $('fwChangelog').innerHTML=t.fwModal.changelog.map(item=>`<li>${item}</li>`).join('');
  $('fwCancelBtn').textContent=t.common.cancel;
  $('fwUpdateBtn').textContent=t.fwModal.update;
  $('ouTitle').textContent=t.orionUpdate.title;
  $('ouChangelog').innerHTML=t.orionUpdate.changelog.map(item=>`<li>${item}</li>`).join('');
  $('ouCancelBtn').textContent=t.common.cancel;
  $('ouUpdateBtn').textContent=t.orionUpdate.update;
  $('ouLaterBtn').textContent=t.orionUpdate.later;
  $('ouRestartBtn').textContent=t.orionUpdate.restartNow;
  updateOrionUpdateRow();
  $('cropBackTxt').textContent=t.common.back;
  $('cropTitleTxt').textContent=t.cropOverlay.title;
  $('cropApplyTxt').textContent=t.cropOverlay.apply;
  $('cropHintTxt').textContent=t.cropOverlay.hint;
}

// ── First-run setup wizard ───────────────────────────────────────────────────
let suPhotoUrl=null,suSelectedDevice=null;

function openSetupWizard(){
  $('suNmInp').value='';$('suTlInp').value='';$('suEmInp').value='';$('suPhInp').value='';
  cc('suNmInp','suNmCnt',32);cc('suTlInp','suTlCnt',32);cc('suEmInp','suEmCnt',32);cc('suPhInp','suPhCnt',16);
  $('suProfileNext').setAttribute('disabled','');
  suPhotoUrl=null;
  $('suDzThumb').style.backgroundImage='';
  $('suDzEmpty').style.display='';$('suDzImg').style.display='none';$('suReuploadBtn').style.display='none';$('suRemoveBtn').style.display='none';
  suSelectedDevice=null;
  applyI18n();
  suShowStep('welcome');
  show('s-setup');
}
function suGoToProfile(){suShowStep('profile');}
// Lets the user change language after starting profile setup, without
// losing what they've already entered — just switches the visible step,
// doesn't reset via openSetupWizard().
function suBackToWelcome(){suShowStep('welcome');}
function suDirty(){
  cc('suNmInp','suNmCnt',32);cc('suTlInp','suTlCnt',32);cc('suEmInp','suEmCnt',32);cc('suPhInp','suPhCnt',16);
  const ok=$('suNmInp').value.trim().length>0&&$('suTlInp').value.trim().length>0;
  if(ok)$('suProfileNext').removeAttribute('disabled');else $('suProfileNext').setAttribute('disabled','');
}
function suPickPhoto(){$('suPhotoInp').click();}
function suOpenCropExisting(){if(suPhotoUrl) openCrop(suPhotoUrl,suApplyCrop,1,228,228,true);}
function suApplyCrop(url){
  suPhotoUrl=url;
  $('suDzThumb').style.backgroundImage=`url(${url})`;
  $('suDzEmpty').style.display='none';$('suDzImg').style.display='';$('suReuploadBtn').style.display='';$('suRemoveBtn').style.display='';
}
function suRemovePhoto(){
  suPhotoUrl=null;
  $('suDzThumb').style.backgroundImage='';
  $('suDzEmpty').style.display='';$('suDzImg').style.display='none';
  $('suReuploadBtn').style.display='none';$('suRemoveBtn').style.display='none';
}
function suLoadPhoto(inp){
  const file=inp.files[0];if(!file) return;inp.value='';
  const reader=new FileReader();
  reader.onload=e=>{suPhotoUrl=e.target.result;openCrop(suPhotoUrl,suApplyCrop,1,228,228,true);};
  reader.readAsDataURL(file);
}

function suShowStep(name){
  ['welcome','profile','discover'].forEach(s=>$('su-step-'+s).classList.toggle('show',s===name));
}
// Pairing modal phases (n: 1=Enter Passkey, 2=Connecting, 3=Syncing) — this is
// where Enter Passkey/Connecting/Syncing live now, instead of as wizard steps.
function suShowPairPhase(n){
  [1,2,3].forEach(i=>$('sp'+i).style.display=i===n?'':'none');
  showModal('m-pair');
}
function suGoDiscover(){
  if($('suProfileNext').hasAttribute('disabled')) return;
  suShowStep('discover');suStartScan();
}
function suBackToProfile(){suShowStep('profile');}

function suStartScan(){
  $('suScanning').style.display='';$('suDevList').style.display='none';$('suDevList').innerHTML='';
  invoke('ble_scan');
}
function suRenderDevices(devices){
  const t=I18N[appLang].discover;
  $('suDevList').innerHTML=devices.map(d=>
    '<div class="su-dev" onclick="suSelectDevice(\''+d.name+'\')">'+
      '<div class="su-dev-ico"><div class="wm" style="font-size:12px;"><span class="o">o</span><span class="r">r</span><span class="i">i</span></div></div>'+
      '<div class="su-dev-info"><div class="su-dev-name">'+d.name+'</div><div class="su-dev-sig">'+(d.strong?t.strongSignal:t.weakSignal)+'</div></div>'+
      '<div class="chev">›</div>'+
    '</div>'
  ).join('')+'<div class="su-rescan" onclick="suStartScan()">⟳ '+t.rescan+'</div>';
  $('suScanning').style.display='none';$('suDevList').style.display='';
}
function suSelectDevice(name){
  suSelectedDevice=name;$('suPasskeyDevName').textContent=name;
  suShowPairPhase(1);
  invoke('ble_pair',{name,profile:{
    name:$('suNmInp').value,title:$('suTlInp').value,email:$('suEmInp').value,phone:$('suPhInp').value
  }}).catch(()=>suShowPairFail());
}
// Pairing itself is OS-native (Windows/macOS Bluetooth prompt) — Orion never
// sees or handles the 6-digit code (setup-flow.md, ble-protocol.md §6.1).
// sp1 above just tells the user what to expect; the backend drives us
// through sp2 ('pairing-connecting') then sp3 ('sync-progress') once the OS
// bond completes and the real BEGIN/END sync starts.
listen('pairing-connecting',()=>suShowPairPhase(2));
function suUpdateSyncProgress({pct,label,done}){
  suShowPairPhase(3);
  const ring=$('suRing'),pctEl=$('suPct'),lbl=$('suLbl');
  ring.style.strokeDashoffset=358-(358*Math.min(pct,100)/100);
  pctEl.textContent=done?'✓':Math.round(pct)+'%';
  lbl.textContent=label||I18N[appLang].syncing.progressLabel;
  if(done) setTimeout(suFinishSetup,1200);
}
function suShowPairFail(){hideModal('m-pair');showModal('m-pairfail');}
function suPairFailClose(){
  hideModal('m-pairfail');
  suShowStep('discover');
  suStartScan();
}
function suFinishSetup(){
  const name=$('suNmInp').value.trim()||'—';
  $('nmInp').value=$('suNmInp').value;$('tlInp').value=$('suTlInp').value;
  $('emInp').value=$('suEmInp').value;$('phInp').value=$('suPhInp').value;
  cc('nmInp','nmCnt',32);cc('tlInp','tlCnt',32);cc('emInp','emCnt',32);cc('phInp','phCnt',16);
  $('mainName').textContent=name;
  const photo=$('mainProfPhoto');
  if(suPhotoUrl){
    photo.style.backgroundImage=`url(${suPhotoUrl})`;
    photo.style.backgroundSize='cover';photo.style.backgroundPosition='center';
    $('mainProfInitials').style.display='none';
  } else {
    photo.style.backgroundImage='';$('mainProfInitials').style.display='';
    const parts=name.trim().split(' ');
    $('mainProfInitials').textContent=(parts[0][0]+(parts[1]?parts[1][0]:'')).toUpperCase();
  }
  hideModal('m-pair');
  setConn('on');
  $('hName').textContent=suSelectedDevice||'Ori-XT-9F';
  $('suRing').style.strokeDashoffset=358;$('suPct').textContent='0%';
}

// ── App bootstrap — real backend wiring (replaces the prototype's fake
// setTimeout-driven state machine with Tauri invoke/listen) ─────────────────
initCal(); // after I18N (renderCal reads it for locale-aware month/weekday names)
applyI18n();

listen('conn-state',e=>setConn(e.payload));
listen('scan-result',e=>suRenderDevices(e.payload));
listen('sync-progress',e=>suUpdateSyncProgress(e.payload));
listen('pairing-failed',()=>suShowPairFail());
listen('fw-update-available',e=>{fwAvail=true;orionFwVersion=e.payload.version;
  const ico=$('fwIco');ico.style.display='';ico.classList.add('fw-on');ico.title=I18N[appLang].main.fwAvailable;});
listen('fw-progress',e=>fwApplyProgress(e.payload));
listen('orion-update-available',e=>{orionUpdateAvail=true;orionUpdateVersion=e.payload.version;updateOrionUpdateRow();});
listen('orion-update-progress',e=>ouApplyProgress(e.payload));

invoke('get_initial_state').then(state=>{
  if(state.paired) setConn(state.connection);
  else openSetupWizard();
}).catch(()=>openSetupWizard());
