const $=id=>document.getElementById(id);

function tick(){
  const n=new Date();
  $('clock').innerHTML=
    n.toLocaleTimeString('en-US',{hour:'numeric',minute:'2-digit',hour12:true})+
    '<br>'+n.toLocaleDateString('en-US',{weekday:'short',month:'short',day:'numeric'});
}
setInterval(tick,1000);tick();

function togglePanel(){
  const p=$('panel'),t=$('tray'),hide=!p.classList.contains('hide');
  p.classList.toggle('hide',hide);t.classList.toggle('open',!hide);
}

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
      pfPendingUrl=null;
      if(pfOrigUrl){
        $('pfDzThumb').style.backgroundImage=`url(${pfOrigUrl})`;
        $('pfDzEmpty').style.display='none';$('pfDzImg').style.display='';$('pfReuploadBtn').style.display='';
      } else {
        $('pfDzThumb').style.backgroundImage='';
        $('pfDzEmpty').style.display='';$('pfDzImg').style.display='none';$('pfReuploadBtn').style.display='none';
      }
      pfChanged=false;$('pfSaveBtn').setAttribute('disabled','');back();
    };
    showModal('m-discard');return;
  }
  if(top==='s-timeOff'&&timeOffDirty){_discardAction=back;showModal('m-discard');return;}
  if(top==='s-calendar'&&calPending!==calSrc){_discardAction=discardCalSource;showModal('m-discard');return;}
  if(top==='s-ancs'&&ancsPending!==ancsLevel){_discardAction=discardAncs;showModal('m-discard');return;}
  if(top==='s-clock'&&clockPending!==clockFace){_discardAction=discardClock;showModal('m-discard');return;}
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
function setConn(s){
  const dot=$('hDot'),td=$('tdot'),name=$('hName'),state=$('hState');
  const connSections=$('connRequiredSections'),toDivider=$('mainTimeOffDivider'),fwIco=$('fwIco');
  dot.className='h-dot '+s;td.className='tdot '+s;
  if(s==='on'){
    name.textContent='Ori-XT-9F';state.textContent='Connected';
    connSections.style.display='';toDivider.style.display='none';
    fwIco.style.display=fwAvail?'':'none';
    readSlotsFromDevice(); // simulate Orion reading Device Settings from Ori on connect
  } else if(s==='rec'){
    name.textContent='Ori-XT-9F';state.textContent='Syncing…';
    connSections.style.display='none';toDivider.style.display='';
    fwIco.style.display='none';
  } else {
    name.textContent='Ori-XT-9F';state.textContent='Disconnected';
    connSections.style.display='none';toDivider.style.display='';
    fwIco.style.display='none';
  }
  back();
}

let pfChanged=false;
let pfCommitted={name:'',title:'',email:'',phone:''};
function openProfileScreen(){
  pfOrigUrl=null;pfPendingUrl=null;
  const savedBg=$('mainProfPhoto').style.backgroundImage;
  const hasSaved=savedBg&&savedBg!=='none'&&savedBg!=='';
  if(hasSaved){
    const url=savedBg.slice(4,-1).replace(/['"]/g,'');
    $('pfDzThumb').style.backgroundImage=savedBg;
    $('pfDzEmpty').style.display='none';$('pfDzImg').style.display='';$('pfReuploadBtn').style.display='';
    pfOrigUrl=url;
  } else {
    $('pfDzThumb').style.backgroundImage='';
    $('pfDzEmpty').style.display='';$('pfDzImg').style.display='none';$('pfReuploadBtn').style.display='none';
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
  pfChanged=textChanged||pfPendingUrl!==null;
  const valid=$('nmInp').value.trim().length>0&&$('tlInp').value.trim().length>0;
  if(pfChanged&&valid)$('pfSaveBtn').removeAttribute('disabled');else $('pfSaveBtn').setAttribute('disabled','');
}
function saveProfile(){
  if(!pfChanged) return;
  if(!$('nmInp').value.trim()||!$('tlInp').value.trim()) return;
  const name=$('nmInp').value||'—';
  $('mainName').textContent=name;
  if(pfPendingUrl){
    const photo=$('mainProfPhoto');
    photo.style.backgroundImage=`url(${pfPendingUrl})`;
    photo.style.backgroundSize='cover';photo.style.backgroundPosition='center';
    $('mainProfInitials').style.display='none';pfPendingUrl=null;
  }
  const photo=$('mainProfPhoto');
  if(!photo.style.backgroundImage){
    const parts=name.trim().split(' ');
    $('mainProfInitials').textContent=(parts[0][0]+(parts[1]?parts[1][0]:'')).toUpperCase();
  }
  pfChanged=false;$('pfSaveBtn').setAttribute('disabled','');back();
}
cc('nmInp','nmCnt',32);cc('tlInp','tlCnt',32);cc('emInp','emCnt',32);cc('phInp','phCnt',16);

let timeOffActive=false,timeOffCustomPhoto=false,timeOffDirty=false;
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
  if(toCommittedPhotoUrl){
    timeOffOrigUrl=toCommittedPhotoUrl;timeOffPendingUrl=null;
    $('timeOffDzThumb').style.backgroundImage=`url(${toCommittedPhotoUrl})`;
    $('timeOffDzEmpty').style.display='none';$('timeOffDzImg').style.display='';$('toReuploadBtn').style.display='';
  } else {
    timeOffOrigUrl=null;timeOffPendingUrl=null;
    $('timeOffDzThumb').style.backgroundImage='';
    $('timeOffDzEmpty').style.display='';$('timeOffDzImg').style.display='none';$('toReuploadBtn').style.display='none';
  }
  updatePeriodDisplay();
  timeOffDirty=false;cc('timeOffDt','dtCnt',48);updateToSaveState();show('s-timeOff');
}
function exitTimeOff(){
  timeOffDirty=false;timeOffActive=false;
  toCommittedStart=null;toCommittedEnd=null;toCommittedDest='';toCommittedPhotoUrl=null;
  $('mainTimeOffBanner').style.backgroundImage='';
  $('timeOffToggle').classList.remove('on');setTimeOffState(false);back();
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
    const f=v=>v.toLocaleDateString('en-US',{month:'short',day:'numeric'});
    const range=f(selStart)+' – '+f(selEnd);
    $('mainTimeOffDates').textContent=range;$('mainTimeOffTextDates').textContent=range;
  }
}
function saveTimeOff(){
  const dest=$('timeOffDt').value.trim();
  if(!dest||[...dest].length>48||!selStart||!selEnd) return;
  updateTimeOff();
  if(timeOffPendingUrl){
    const b=$('mainTimeOffBanner');
    b.style.backgroundImage=`url(${timeOffPendingUrl})`;
    b.style.backgroundSize='cover';b.style.backgroundPosition='center';b.style.backgroundRepeat='no-repeat';
    toCommittedPhotoUrl=timeOffPendingUrl;
  }
  toCommittedStart=selStart;toCommittedEnd=selEnd;toCommittedDest=dest;
  timeOffDirty=false;timeOffActive=true;
  $('timeOffToggle').classList.add('on');setTimeOffState(true);back();
}
function hideToErr(id){$(id).style.display='none';}

let calYear,calMonth,selStart=null,selEnd=null,selPhase=0,calHover=null;
const MONTHS=['January','February','March','April','May','June','July','August','September','October','November','December'];
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
  $('pcalMonth').textContent=MONTHS[calMonth]+' '+calYear;
  document.querySelector('.pcal-nav-prev').classList.toggle('dis',calYear===today.getFullYear()&&calMonth===today.getMonth());
  $('pcalHint').textContent=selPhase===0?'Select start date':selPhase===1?'Now select end date':'';
  const grid=$('pcalGrid');grid.innerHTML='';
  ['Mo','Tu','We','Th','Fr','Sa','Su'].forEach(h=>{
    const el=document.createElement('div');el.className='pcal-dh';el.textContent=h;grid.appendChild(el);
  });
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
  const txt=$('periodText');
  if(!selStart){txt.textContent='Select dates…';txt.style.color='var(--t3)';}
  else if(!selEnd){
    txt.textContent=selStart.toLocaleDateString('en-US',{month:'short',day:'numeric'})+' → …';txt.style.color='var(--t2)';
  } else {
    const sameY=selStart.getFullYear()===selEnd.getFullYear();
    const d1=selStart.toLocaleDateString('en-US',{month:'short',day:'numeric',...(sameY?{}:{year:'numeric'})});
    const d2=selEnd.toLocaleDateString('en-US',{month:'short',day:'numeric',year:'numeric'});
    txt.textContent=d1+' – '+d2;txt.style.color='var(--t1)';
  }
}

let timeOffOrigUrl=null,timeOffPendingUrl=null;
function timeOffPickPhoto(){$('timeOffPhoInp').click();}
function openCropExisting(){if(timeOffOrigUrl) openCrop(timeOffOrigUrl,applyTimeOffCrop);}
function applyTimeOffCrop(url){
  timeOffDirty=true;timeOffCustomPhoto=true;timeOffPendingUrl=url;
  $('timeOffDzThumb').style.backgroundImage=`url(${url})`;
  $('timeOffDzEmpty').style.display='none';$('timeOffDzImg').style.display='';$('toReuploadBtn').style.display='';
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
  pfPendingUrl=url;
  $('pfDzThumb').style.backgroundImage=`url(${url})`;
  $('pfDzEmpty').style.display='none';$('pfDzImg').style.display='';$('pfReuploadBtn').style.display='';
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
const calInfo={ms:{name:'Microsoft Teams',ok:true},gg:{name:'Google Calendar',ok:true}};
function _renderCalOpts(s){
  ['ms','gg'].forEach(k=>$('co-'+k).classList.toggle('sel',k===s));
  $('ggSignInRow').style.display=s==='ms'?'none':'';
}
function _updateCalSave(){
  const btn=$('calSaveBtn');if(!btn) return;
  if(calPending!==calSrc)btn.removeAttribute('disabled');else btn.setAttribute('disabled','');
}
function openCalendarSource(){
  calPending=calSrc;_renderCalOpts(calPending);_updateCalSave();show('s-calendar');
}
function setCalPending(s){calPending=s;_renderCalOpts(s);_updateCalSave();}
function saveCalSource(){
  calSrc=calPending;
  const info=calInfo[calSrc];
  $('mainCalSub').textContent=info.name+(info.ok?' · Connected':' · Not connected');
  back();
}
function discardCalSource(){calPending=calSrc;_renderCalOpts(calPending);back();}
function signGoogle(){
  $('ggSt').textContent='Signing in…';$('ggSignBtn').disabled=true;
  setTimeout(()=>{
    calInfo.gg.ok=true;$('ggSt').textContent='Connected';
    $('ggSignBtn').disabled=false;$('ggSignBtn').textContent='Sign out from Google';setCalPending('gg');
  },1400);
}
$('ggSignInRow').style.display='none';
initCal();

const siImgMap={
  'vol-mute':'../firmware/img/shortcut_icons/vol-mute.png',
  'mic-mute':'../firmware/img/shortcut_icons/mic-mute.png',
  'screenshot':'../firmware/img/shortcut_icons/screenshot.png',
  'lock-screen':'../firmware/img/shortcut_icons/lock-screen.png',
  'favorite':'../firmware/img/shortcut_icons/favorite.png'
};
function applySlot(n){
  const v=$('ss'+n).value;
  $('si'+n).src=siImgMap[v]||'';
  const favEl=$('fav'+n);
  favEl.style.display=v==='favorite'?'block':'none';
  if(v==='favorite') renderKbdCombo(n);
  _updateSlotSave();
}
// Simulates Orion reading Device Settings (incl. shortcut slots) from Ori on
// (re)connect. The select values represent Ori's NVS-persisted state.
function readSlotsFromDevice(){
  [1,2,3].forEach(n=>{
    slotCommitted[n-1]=$('ss'+n).value;
    kbdCommitted[n-1]=[];_kbdCombos[n-1]=[];
    applySlot(n);
    $('ms'+n).src=siImgMap[$('ss'+n).value]||'';
  });
}

// ── Keyboard shortcut recorder ──────────────────────────────────────────────
let _kbdRecSlot=0;
const _kbdCombos=[[],[],[]];
// Committed (saved) state for subscreens with Save buttons
let ancsLevel=3;let ancsPending=3;
let clockFace='digital';let clockPending='digital';
const slotCommitted=['vol-mute','mic-mute','screenshot'];
const kbdCommitted=[[],[],[]];

function renderKbdCombo(n){
  const parts=_kbdCombos[n-1];
  const disp=$('kbdDisp'+n),hint=$('kbdHint'+n);
  if(!parts.length){
    disp.innerHTML='<span class="kbd-unset">Not set</span>';
    hint.textContent='Click to set';
  } else {
    disp.innerHTML=parts.map(p=>`<kbd class="kc">${p}</kbd>`).join('<span class="kbd-sep"> + </span>');
    hint.textContent='Click to change';
  }
}

function startKbdRecord(n){
  if(_kbdRecSlot) stopKbdRecord();
  _kbdRecSlot=n;
  $('kbdRec'+n).classList.add('recording');
  $('kbdDisp'+n).innerHTML='<span class="kbd-recording-text">Press shortcut…</span>';
  $('kbdHint'+n).textContent='Esc to cancel';
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
function openShortcuts(){
  [1,2,3].forEach(n=>{$('ss'+n).value=slotCommitted[n-1];_kbdCombos[n-1]=[...kbdCommitted[n-1]];applySlot(n);});
  _updateSlotSave();show('s-shortcuts');
}

// ── Subscreen option selection ───────────────────────────────────────────────
function setClock(f){clockPending=f;_renderClock(f);_updateClockSave();}
function setAncs(level){ancsPending=level;_renderAncs(level);_updateAncsSave();}

// ── Save handlers ────────────────────────────────────────────────────────────
function saveAncs(){
  ancsLevel=ancsPending;
  [0,1,2,3].forEach(i=>$('an-ico-'+i).style.display=i===ancsLevel?'block':'none');
  back();
}
function saveClock(){
  clockFace=clockPending;
  $('mcDig').style.display=clockFace==='digital'?'flex':'none';$('mcAna').style.display=clockFace==='analog'?'block':'none';
  back();
}
function saveSlots(){
  if(_kbdRecSlot) stopKbdRecord();
  // Commit pending state
  [1,2,3].forEach(n=>{slotCommitted[n-1]=$('ss'+n).value;kbdCommitted[n-1]=[..._kbdCombos[n-1]];});
  [1,2,3].forEach(i=>$('ms'+i).src=siImgMap[$('ss'+i).value]||'');
  back();
}

// ── Discard handlers ─────────────────────────────────────────────────────────
function discardAncs(){ancsPending=ancsLevel;_renderAncs(ancsPending);back();}
function discardClock(){clockPending=clockFace;_renderClock(clockPending);back();}
function discardShortcuts(){
  if(_kbdRecSlot) stopKbdRecord();
  [1,2,3].forEach(n=>{$('ss'+n).value=slotCommitted[n-1];_kbdCombos[n-1]=[...kbdCommitted[n-1]];applySlot(n);});
  back();
}

// Device-only reset: Ori wipes its own NVS + bonds and reboots into setup,
// but Orion's local profile cache is untouched (ble-protocol.md §7.2) — so
// only the connection state drops, the cached profile stays on screen.
function doFactoryReset(){hideModal('m-reset');setConn('off');}
// Clear All: also wipes Orion's local cache (profile, calendar sign-in,
// shortcuts), then walks back through first-run setup since there's now
// neither a local profile nor a paired device.
function doClearAll(){
  hideModal('m-reset');
  calSrc='ms';calPending='ms';_renderCalOpts('ms');
  $('mainCalSub').textContent=calInfo.ms.name+(calInfo.ms.ok?' · Connected':' · Not connected');
  $('nmInp').value='';$('tlInp').value='';$('emInp').value='';$('phInp').value='';
  pfOrigUrl=null;pfPendingUrl=null;
  $('pfDzThumb').style.backgroundImage='';$('pfDzEmpty').style.display='';$('pfDzImg').style.display='none';
  $('mainProfPhoto').style.backgroundImage='';
  $('mainProfInitials').style.display='';$('mainProfInitials').textContent='?';
  $('mainName').textContent='Your Name';setConn('off');
  openSetupWizard();
}

let _fwInstallTimer=null;
function clickFw(){
  if(fwAvail){showModal('m-fw');}
  else{const ico=$('fwIco');ico.style.transition='transform .15s';ico.style.transform='rotate(20deg)';setTimeout(()=>ico.style.transform='',300);}
}
function simFwUpdate(){
  fwAvail=true;const ico=$('fwIco');
  ico.style.display='';ico.classList.add('fw-on');ico.title='Firmware update available';back();
}
function startFwInstall(){
  $('fw-c').style.display='none';$('fw-i').style.display='';
  let p=0;
  const ring=$('fwRing'),pct=$('fwPct'),lbl=$('fwLbl'),title=$('fwMTitle');
  clearInterval(_fwInstallTimer);
  _fwInstallTimer=setInterval(()=>{
    p+=1.6;if(p>100) p=100;
    ring.style.strokeDashoffset=358-(358*p/100);pct.textContent=Math.round(p)+'%';
    if(p<55) lbl.textContent='Keep Ori plugged in';
    else if(p<80){lbl.textContent='Verifying…';title.textContent='Verifying…';}
    else if(p<100){lbl.textContent='Updating firmware…';title.textContent='Updating firmware…';}
    else{
      clearInterval(_fwInstallTimer);pct.textContent='✓';lbl.textContent='Now running v1.1.0';title.textContent='Done';
      setTimeout(()=>{
        hideModal('m-fw');fwAvail=false;
        const d=$('fwIco');d.classList.remove('fw-on');d.title='Firmware up to date';d.style.display='none';
        $('fw-c').style.display='';$('fw-i').style.display='none';
        ring.style.strokeDashoffset=358;pct.textContent='0%';title.textContent='Updating firmware…';
      },2000);
    }
  },55);
}

// ── First-run setup wizard ───────────────────────────────────────────────────
let suPhotoUrl=null,suSelectedDevice=null,_suScanTimer=null;

function openSetupWizard(){
  clearTimeout(_suScanTimer);
  $('suNmInp').value='';$('suTlInp').value='';$('suEmInp').value='';$('suPhInp').value='';
  cc('suNmInp','suNmCnt',32);cc('suTlInp','suTlCnt',32);cc('suEmInp','suEmCnt',32);cc('suPhInp','suPhCnt',16);
  $('suProfileNext').setAttribute('disabled','');
  suPhotoUrl=null;
  $('suDzThumb').style.backgroundImage='';
  $('suDzEmpty').style.display='';$('suDzImg').style.display='none';$('suReuploadBtn').style.display='none';
  suSelectedDevice=null;
  suResetPasskeyInputs();
  suShowStep('welcome');
  show('s-setup');
}
function suGoToProfile(){suShowStep('profile');}
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
  $('suDzEmpty').style.display='none';$('suDzImg').style.display='';$('suReuploadBtn').style.display='';
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
function suBackToProfile(){clearTimeout(_suScanTimer);suShowStep('profile');}

function suStartScan(){
  clearTimeout(_suScanTimer);
  $('suScanning').style.display='';$('suDevList').style.display='none';$('suDevList').innerHTML='';
  _suScanTimer=setTimeout(()=>{
    const devices=[{name:'Ori-XT-9F',sig:'Strong signal'},{name:'Ori-4C-12',sig:'Weak signal'}];
    $('suDevList').innerHTML=devices.map(d=>
      '<div class="su-dev" onclick="suSelectDevice(\''+d.name+'\')">'+
        '<div class="su-dev-ico"><div class="wm" style="font-size:12px;"><span class="o">o</span><span class="r">r</span><span class="i">i</span></div></div>'+
        '<div class="su-dev-info"><div class="su-dev-name">'+d.name+'</div><div class="su-dev-sig">'+d.sig+'</div></div>'+
        '<div class="chev">›</div>'+
      '</div>'
    ).join('')+'<div class="su-rescan" onclick="suStartScan()">⟳ Scan again</div>';
    $('suScanning').style.display='none';$('suDevList').style.display='';
  },1500);
}
function suSelectDevice(name){
  suSelectedDevice=name;$('suPasskeyDevName').textContent=name;
  suResetPasskeyInputs();
  suShowPairPhase(1);
}

// ── Passkey entry (6 separate digit boxes) ──────────────────────────────────
function suResetPasskeyInputs(){
  for(let i=0;i<6;i++){$('suPk'+i).value='';}
}
function suPkInput(e,idx){
  const el=e.target;
  el.value=el.value.replace(/[^0-9]/g,'').slice(-1);
  if(el.value&&idx<5) $('suPk'+(idx+1)).focus();
}
function suPkKeydown(e,idx){
  if(e.key==='Backspace'&&!e.target.value&&idx>0) $('suPk'+(idx-1)).focus();
}
function suPkPaste(e){
  const text=e.clipboardData.getData('text').replace(/[^0-9]/g,'').slice(0,6);
  if(!text.length) return;
  e.preventDefault();
  [...text].forEach((ch,i)=>{if($('suPk'+i)) $('suPk'+i).value=ch;});
  $('suPk'+Math.min(text.length,5)).focus();
}

let _suSyncTimer=null,_suConnectTimer=null;
function suSubmitPasskey(){
  const entered=[0,1,2,3,4,5].map(i=>$('suPk'+i).value).join('');
  if(entered.length<6) return;
  suShowPairPhase(2);
  clearTimeout(_suConnectTimer);
  _suConnectTimer=setTimeout(suRunSync,1400);
}
function suRunSync(){
  suShowPairPhase(3);let p=0;
  const ring=$('suRing'),pct=$('suPct'),lbl=$('suLbl');
  lbl.textContent='A busy day ahead…';
  clearInterval(_suSyncTimer);
  _suSyncTimer=setInterval(()=>{
    p+=2.2;if(p>100) p=100;
    ring.style.strokeDashoffset=358-(358*p/100);pct.textContent=Math.round(p)+'%';
    if(p>=100){
      clearInterval(_suSyncTimer);pct.textContent='✓';lbl.textContent='Ori is set up!';
      setTimeout(suFinishSetup,1200);
    }
  },65);
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

// ── Prototype nav — jump directly to any page/state for design review ───────
function _navReset(){
  document.querySelectorAll('.modal-bg.show').forEach(m=>m.classList.remove('show'));
  $('m-crop').classList.remove('show');
  $('fw-c').style.display='';$('fw-i').style.display='none'; // reset fw modal to its default (pre-install) sub-view
  $('sp1').style.display='';$('sp2').style.display='none';$('sp3').style.display='none'; // reset pairing modal to its default (Enter Passkey) sub-view
  stack.length=0;
  document.querySelectorAll('.screen.show,.screen.behind').forEach(el=>{
    el.classList.remove('show','behind');el.style.zIndex='';
  });
  if(_kbdRecSlot) stopKbdRecord();
  clearTimeout(_suScanTimer);clearTimeout(_suConnectTimer);clearInterval(_suSyncTimer);clearInterval(_fwInstallTimer);
  $('panel').classList.remove('hide');$('tray').classList.add('open');
}

const NAV_PAGES={
  'main-connected':()=>{setConn('on');},
  'main-syncing':()=>{setConn('rec');},
  'main-disconnected':()=>{setConn('off');},
  'setup-welcome':()=>{openSetupWizard();},
  'setup-profile':()=>{openSetupWizard();suShowStep('profile');},
  'setup-discover':()=>{openSetupWizard();suShowStep('discover');suStartScan();},
  'setup-passkey':()=>{openSetupWizard();suShowStep('discover');suSelectDevice('Ori-XT-9F');},
  'setup-connecting':()=>{openSetupWizard();suShowStep('discover');suSelectDevice('Ori-XT-9F');suShowPairPhase(2);},
  'setup-syncing':()=>{
    openSetupWizard();suShowStep('discover');suSelectDevice('Ori-XT-9F');suShowPairPhase(3);
    $('suRing').style.strokeDashoffset=358-(358*60/100);$('suPct').textContent='60%';$('suLbl').textContent='A busy day ahead…';
  },
  'settings':()=>{setConn('on');show('s-settings');},
  'profile-editor':()=>{setConn('on');openProfileScreen();},
  'timeoff-editor':()=>{setConn('on');openTimeOffScreen();},
  'calendar-source':()=>{setConn('on');openCalendarSource();},
  'quick-actions':()=>{setConn('on');openShortcuts();},
  'clock-face':()=>{setConn('on');openClock();},
  'notification-filter':()=>{setConn('on');openAncs();},
  'modal-reset':()=>{setConn('on');show('s-settings');showModal('m-reset');},
  'modal-pairing-fail':()=>{openSetupWizard();suShowStep('discover');suSelectDevice('Ori-XT-9F');suShowPairPhase(2);suShowPairFail();},
  'modal-fw-available':()=>{setConn('on');simFwUpdate();showModal('m-fw');},
  'modal-fw-installing':()=>{
    setConn('on');simFwUpdate();showModal('m-fw');
    $('fw-c').style.display='none';$('fw-i').style.display='';
    $('fwRing').style.strokeDashoffset=358-(358*45/100);$('fwPct').textContent='45%';
    $('fwLbl').textContent='Keep Ori plugged in';$('fwMTitle').textContent='Updating firmware…';
  },
};

function navTo(id){
  const fn=NAV_PAGES[id];if(!fn) return;
  _navReset();
  fn();
  document.querySelectorAll('.nav button').forEach(b=>b.classList.toggle('active',b.dataset.page===id));
}

navTo('main-connected');
