const $=id=>document.getElementById(id);

// Tauri v2 global bridge (tauri.conf.json has withGlobalTauri:true, so this
// is available on window without an ES module import — app.js is loaded as
// a classic script).
const invoke=(...a)=>window.__TAURI__.core.invoke(...a);
const listen=(...a)=>window.__TAURI__.event.listen(...a);

// TEMPORARY DEBUG: mirror a log line to BOTH the webview console and the
// backend terminal (via the debug_log command) so a single terminal capture
// shows the Rust BLE path and this JS ANCS path interleaved. Remove with the
// debug_log command once the ANCS-list issue is resolved.
function dlog(msg){ console.log(msg); try{ invoke('debug_log',{msg}); }catch(_){} }

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
document.addEventListener('keydown',e=>{
  if(e.key!=='Escape') return;
  if(closeTopOverlay()) return;
  backWithCheck();
});

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

// Used only by setConn() — force-closes a currently-open subscreen when a
// background connection-state change makes it stale, but ONLY for the
// screens that are actually gated behind #connRequiredSections' own
// visibility (Notification Filter, Clock Face, Time Format, Quick Actions;
// pc-app.md/screen-layout.md). Their entry row on the main screen is hidden
// during Connecting/Syncing ONLY (not Disconnected, since 2026-07-11 — see
// setConn()'s own comment) — so setConn() only calls this when transitioning
// INTO one of those two states; leaving one open across THOSE would strand
// the user on a screen they can no longer navigate back into via the row
// that opened it. A transition into Disconnected does the opposite of
// stranding: the row stays reachable, and the whole point of that state now
// is to let the user keep editing right where they are (queued locally,
// synced on reconnect — save*() functions' pending_* handling).
//
// Settings, Profile, Time Off, and Calendar Source are NOT connection-gated
// (pc-app.md: "Profile and Time Off subscreens ... are accessible regardless
// of connection state"; Settings never was either) and must NEVER be
// force-closed here — conn-state cycles through connecting/rec/on on every
// ordinary background BLE reconnect (completely normal for a wireless
// link), and closing whatever the user happens to have open because of that
// blip (previously: any screen at all, via an unconditional back()
// fallthrough) is its own bad surprise, independent of whether anything was
// actually unsaved.
//
// Each of the four gated screens still keeps its own unsaved-edit guard —
// mirrors backWithCheck()'s dirty checks, minus the confirmation modal,
// since popping up "Discard changes?" out of nowhere because of a
// connectivity blip would be its own bad surprise.
//
// s-setup does NOT go through this function at all: openSetupWizard() now
// unwinds the whole stack itself before showing it, and suFinishSetup()
// explicitly pops it back off once setup completes — see their own
// comments. (Previously this function's generic "close whatever's on top"
// fallback was what made both of those work, and only one stack level deep;
// now that the fallback is gone, they own that job directly.)
function dismissTransientScreen(){
  // Tear down an armed-but-not-yet-recorded shortcut recorder unconditionally,
  // same as backWithCheck() does for its 's-shortcuts' case — otherwise a
  // reconnect blip while the recorder is armed (but before any key is
  // pressed) leaves the capturing document keydown listener orphaned, and
  // the user's next keystroke anywhere in the app gets silently swallowed.
  if(_kbdRecSlot) stopKbdRecord();
  const top=stack[stack.length-1];
  const gatedAndClean=
    (top==='s-ancs'&&ancsPending===ancsLevel)||
    (top==='s-clock'&&clockPending===clockFace)||
    (top==='s-timeformat'&&timeFormatPending===timeFormat)||
    (top==='s-shortcuts'&&!_isSlotsDirty());
  if(gatedAndClean) back();
}

function showModal(id){$(id).classList.add('show');}
function hideModal(id){$(id).classList.remove('show');}
// Instantly hides a modal — no opacity fade. Kept as a building block for
// switchModal() below; see that function for why a plain fade (even a
// single one-directional fade-in) still isn't enough on its own.
function hideModalInstant(id){
  const el=$(id);
  el.style.transition='none';
  el.classList.remove('show');
  void el.offsetHeight; // flush layout so transition:none takes effect before it's restored below
  el.style.transition='';
}
// Instantly shows a modal — no opacity fade. Counterpart to hideModalInstant,
// used when something else is being closed instantly in the same call (e.g.
// callTakeOverScreen interrupting an already-open modal for an incoming
// call) — pairs with an instant hide so nothing crossfades or reveals an
// unblurred frame in between.
function showModalInstant(id){
  const el=$(id);
  el.style.transition='none';
  el.classList.add('show');
  void el.offsetHeight;
  el.style.transition='';
}
// Switches from one open modal directly to another with ZERO animation on
// BOTH sides — for modal-to-modal navigation only (iPhone Info -> ANCS
// list, list <-> detail, list -> iPhone Info, iPhone Info -> Unpair, any
// open modal -> incoming-call takeover, setup's Pair -> Pair-failed). The
// very first modal open (e.g. tapping the header phone icon from the bare
// main screen) is untouched — plain showModal() there keeps its normal
// fade-in, which was never reported as a problem.
//
// Two things were tried and both still showed the main screen, unblurred,
// for a frame before the destination modal's own blur caught up:
//   1. A plain hideModal()+showModal() crossfades two independently semi-
//      transparent + backdrop-blurred .modal-bg layers at once — since
//      that doesn't blend additively, the combined coverage of whatever's
//      behind both actually DIPS below either endpoint mid-transition
//      (50% opaque * 50% opaque = only 25% combined coverage).
//   2. Killing only the OUTGOING modal's transition (hideModalInstant) and
//      leaving the incoming modal's normal fade-in removes that dip, but
//      still starts the incoming modal at a hard opacity:0 — a real,
//      user-visible frame of the fully-unblurred main screen before its
//      backdrop-filter blur (re-)promotes and ramps in over the fade.
// Disabling the CSS transition on BOTH elements for one atomic DOM update
// removes every intermediate animation frame entirely: the browser has
// nothing to paint except "old modal gone, new modal fully there, already
// blurred" in a single step, so there's nothing left to flicker.
function switchModal(fromId, toId){
  const from=$(fromId), to=$(toId);
  from.style.transition='none';
  to.style.transition='none';
  from.classList.remove('show');
  to.classList.add('show');
  void to.offsetHeight; // flush so the fully-switched state is what actually paints
  from.style.transition='';
  to.style.transition='';
}
// The id of whichever .modal-bg is currently open, or null — at most one is
// ever open at a time in normal use. Lets a destination modal that's reached
// from more than one origin (e.g. the ANCS list, opened either straight from
// iPhone Info or via Detail's "back") switch instantly from WHICHEVER one is
// actually showing, without each call site having to know or hardcode it.
function currentOpenModal(){
  const el=document.querySelector('.modal-bg.show');
  return el?el.id:null;
}
// Opens `toId`: instantly (switchModal, no animation) if another modal is
// currently open, or with the normal fade-in if nothing was (a bare main
// screen -> first modal open, which was never reported as flickering and
// keeps its established look).
function openModalFrom(toId){
  const from=currentOpenModal();
  if(from && from!==toId) switchModal(from,toId);
  else showModal(toId);
}

// Esc always closes whatever's on top the SAME way its own Close/Cancel/Back
// button would — never a bare hideModal() — so dirty-state guards (discard-
// changes prompt) and in-flight-transfer locks are respected identically to
// a mouse click. Returns whether it handled anything, so the document
// listener below falls through to backWithCheck() (screen-stack Back) only
// when no modal/overlay was open. m-crop is checked first since it isn't a
// .modal-bg (crop-ovl has its own layer) and can sit on top of one.
function closeTopOverlay(){
  if($('m-crop').classList.contains('show')){ cancelCrop(); return true; }
  const id=currentOpenModal();
  if(!id) return false;
  switch(id){
    case 'm-ori-info': closeOriInfoModal(); break;
    case 'm-iphone-info': hideModal('m-iphone-info'); break;
    case 'm-ancs-list': ancsListBack(); break;
    case 'm-ancs-detail': ancsBackToList($('ancsDetailCard').dataset.bucket); break;
    case 'm-discard': hideModal('m-discard'); break; // "Keep editing" — Esc must never fire the destructive Discard action
    case 'm-reset': hideModal('m-reset'); break;
    case 'm-unpair-phone': hideModal('m-unpair-phone'); break;
    case 'm-pairfail': suPairFailClose(); break;
    case 'm-pair':
      // Only phase 1 (Enter Passkey) has a Cancel button — phases 2/3
      // (Connecting/Syncing) are non-dismissable, same as they render with
      // no Cancel button at all.
      if($('sp1').style.display!=='none') suCancelPairing();
      break;
    case 'm-fw':
      // fw-i (installing) is non-dismissable once accepted (ota.md) — no
      // Cancel button renders there either.
      if($('fw-i').style.display==='none') hideModal('m-fw');
      break;
    case 'm-orion-update':
      // ou-i (installing) is non-dismissable; ou-c (Cancel) and ou-d
      // (Later) both just close the modal.
      if($('ou-i').style.display==='none') hideModal('m-orion-update');
      break;
    case 'm-incoming-call': hideModal('m-incoming-call'); break;
    default: hideModal(id);
  }
  return true;
}

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
    // Orion app self-update modal's version line — current→new, same pairing
    // settingsAppVer/orionUpdateSub above already use (orionAppVersion is
    // this app's own current version; orionUpdateVersion is the new one from
    // the 'orion-update-available' event). NOT orionFwVersion — that variable
    // holds Ori's (the device's) new firmware version from the unrelated
    // 'fw-update-available' event and belongs on the Ori firmware modal
    // (m-fw's oufVerLine, see clickFw()), not here.
    $('ouVerLine').textContent=orionAppVersion+' → '+orionUpdateVersion;
  }
}
let connState='on';
function setConn(s){
  connState=s;
  // Reveal the main screen the first time it has anything authoritative to
  // show (see styles.css's #s-main.pending-init comment) — covers both the
  // bootstrap's "already paired" launch path and first-time setup finishing
  // (suFinishSetup's setConn('on'), which never touched #s-main before). A
  // no-op everywhere else, since the class is already gone by then.
  $('s-main').classList.remove('pending-init');
  invoke('set_tray_status',{state:s}).catch(()=>{});
  const dot=$('hDot'),state=$('hState');
  const connSections=$('connRequiredSections'),toDivider=$('mainTimeOffDivider'),fwIco=$('fwIco');
  const t=I18N[appLang].main;
  dot.className='h-dot '+s;
  // Manual reconnect stays visible for as long as Orion ISN'T connected.
  // Its spin/click-guard (.is-retrying) is driven separately, by
  // setReconnectBusy() off the "reconnect-attempt" event — NOT by this
  // state — because an attempt can be quietly running (scanning for Ori)
  // for a while before this ever leaves "off" (ble-protocol.md's Connecting
  // state only starts once Ori is actually found). Keying the button off
  // conn-state alone left it looking idle/clickable for most of a typical
  // attempt, which is exactly what prompted this split.
  $('reconnectIco').style.display=s==='on'?'none':'';
  if(s==='on'){
    state.textContent=t.connected;
    connSections.style.display='';toDivider.style.display='none';
    fwIco.style.display=fwAvail?'':'none';
    readSlotsFromDevice(); // Orion reads Device Settings from Ori on connect (ble-protocol.md §6.4)
    // Re-derive the phone icon from the cached status: the backend's
    // phone_bond_watcher is spawned BEFORE the supervisor emits "on", so its
    // initial read can race ahead of this event — arriving while connState
    // was still 'rec', where setPhoneBondStatus cached it but kept the icon
    // hidden. Without this re-apply, a lost race leaves the icon hidden
    // until the next iPhone state-change notify (potentially hours). In the
    // normal ordering the cache is still blank here and this is a no-op.
    setPhoneBondStatus(lastPhoneBondStatus);
  } else if(s==='connecting'){
    // Ori found, BLE link + encryption being established — before the real
    // sync (`run_sync`) starts. Same not-yet-synced treatment as "rec".
    state.textContent=t.connecting;
    connSections.style.display='none';toDivider.style.display='';
    fwIco.style.display='none';
    setPhoneBondStatus({b:false,c:false,n:''});
  } else if(s==='rec'){
    state.textContent=t.syncing;
    connSections.style.display='none';toDivider.style.display='';
    fwIco.style.display='none';
    setPhoneBondStatus({b:false,c:false,n:''});
  } else {
    // Disconnected. Unlike Connecting/Syncing, the Notification Filter /
    // Clock Face / Time Format / Quick Actions rows stay VISIBLE and
    // editable here (2026-07-11) — the whole point of this state is that
    // the user can change them with no live link at all: each save*()
    // function's save_device_settings call persists the edit into Orion's
    // own local store (SavedState::pending_clock_face/pending_time_format/
    // pending_ancs_filter) and run_sync flushes it to Ori on the next
    // successful reconnect (central.rs). No readSlotsFromDevice() call
    // here — there's no link to read from, and the rows already show
    // whatever's currently in clockFace/timeFormat/ancsLevel/slotCommitted,
    // which is exactly right whether that's the last value Ori confirmed or
    // an offline edit still waiting to be delivered.
    state.textContent=t.disconnected;
    connSections.style.display='';toDivider.style.display='none';
    fwIco.style.display='none';
    setPhoneBondStatus({b:false,c:false,n:''});
    // Ori link is broken — close every ANCS/call surface, since their content
    // is relayed from Ori and can no longer be verified or acted on (request:
    // "Close all modals related to ANCS notification / incoming / ongoing call
    // if the Ori connection is broken"). Reconnect's resync repopulates a
    // still-live call/notification, so this is not done on connecting/rec.
    closeAncsSurfacesOnDrop();
  }
  // Only Connecting/Syncing actually hide connRequiredSections (above) — a
  // gated subscreen left open across those needs closing (dismissTransientScreen's
  // own doc comment); Disconnected keeps the row reachable, so there's
  // nothing to strand.
  if(s==='connecting'||s==='rec') dismissTransientScreen();
  // Keep an already-open Ori Info modal's dot/state (and signal bars, which
  // zero out once disconnected) in step with every connection-state change
  // instead of waiting for the next ORI_INFO_POLL_MS tick.
  if($('m-ori-info').classList.contains('show')) refreshOriInfoModal();
}

// Header's manual reconnect button — nudges the backend past its current
// exponential backoff wait instead of the user having to wait up to 30s for
// the next automatic attempt. No optimistic UI change here beyond what
// setReconnectBusy already does on its own — the real `reconnect-attempt`/
// `conn-state` events (already listened for) drive the header once the
// nudged attempt actually starts/succeeds. Guarded against firing while a
// reconnect is already in flight — belt-and-suspenders alongside the icon's
// own .is-retrying (pointer-events:none): the backend's `force_reconnect` is
// already a safe no-op in that state, but skipping the invoke entirely
// avoids relying on CSS alone to prevent a double-trigger, and avoids
// banking a stray notify permit that would otherwise fire early on some
// later, unrelated backoff sleep.
function doForceReconnect(){
  if(reconnectBusy) return;
  invoke('force_reconnect').catch(()=>{});
}

// Tracks whether a reconnect attempt is actually running right now —
// scanning included, not just the found-and-connecting sub-phase `setConn`
// covers (see its comment). This — not conn-state — is what the button's
// spin + click-guard key off, because scanning for Ori (the bulk of a
// typical attempt when it's genuinely unreachable) is silent on conn-state
// by design. Backend emits this from `supervise_connection_loop`, bracketing
// the whole `ble::reconnect` call — so it covers both a user-clicked nudge
// and Ori's own periodic background scan attempts equally.
let reconnectBusy=false;
function setReconnectBusy(busy){
  reconnectBusy=busy;
  $('reconnectIco').classList.toggle('is-retrying',busy);
  // The header label should read "Connecting…" for the whole time an
  // attempt is actually running — including the scan itself, which
  // conn-state stays silent through by design (setConn's "connecting" only
  // fires once Ori is actually found). Only ever touches the label, never
  // `connState` — connSections/toDivider/fwIco/phone-status visibility all
  // still key off the real conn-state, not this flag.
  if(busy){
    $('hState').textContent=I18N[appLang].main.connecting;
  } else if(connState==='off'){
    // Attempt just ended without ever moving past "off" (not found, or
    // failed) — conn-state's own "off" only re-fires once per settle
    // sequence (supervise_connection_loop's settled_off guard), so a later
    // retry in the same sequence would otherwise leave this stuck reading
    // "Connecting…" once its busy window ends with nothing to revert it.
    $('hState').textContent=I18N[appLang].main.disconnected;
  }
  // else: connState is already 'connecting'/'rec'/'on' — setConn() just set
  // the right text for that transition; nothing to do here.
}

// Orion just observes Ori's iPhone/ANCS bond (char 000F, ble-protocol.md
// §3/§11) — read once on connect, then updated on every notify. Read-only:
// Orion has no phone-pairing action of its own to offer (that's done on
// Ori's own screen). Hidden whenever Orion isn't connected to Ori (nothing
// live to report) or no iPhone has ever been bonded (b:false — nothing to
// show). Disconnected state is a diagonal slash (.phone-slash in the SVG),
// not a colour change, mirroring Ori's own status-bar phone icon.
let lastPhoneBondStatus={b:false,c:false,n:'',m:0,u:0,t:0,s:0};
function setPhoneBondStatus(status){
  lastPhoneBondStatus=status;
  const ico=$('phoneIco');
  if(connState!=='on'||!status.b){
    ico.style.display='none';
  } else {
    ico.style.display='';
    const t=I18N[appLang].main;
    if(status.c){
      ico.classList.remove('phone-disconnected');
      ico.title=t.phoneConnectedTitle.replace('{name}',status.n||t.phoneUnknownName);
    } else {
      ico.classList.add('phone-disconnected');
      ico.title=t.phoneDisconnectedTitle;
    }
  }
  // Keep an already-open iPhone Info modal live — Ori pushes a fresh
  // PhoneBondStatus (char 000F) on every queue/filter change AND on every
  // RSSI bucket change (~5 s poll while connected), but previously only the
  // header icon above reacted to it; the modal's own dot/state/signal-bars/
  // badges were rendered once at open and then sat stale for as long as it
  // stayed open. openIphoneInfoModal() already does exactly the full
  // re-render this needs (name/dot/state/sigbars/badges + tile onclicks) and
  // is safe to call again while already open — same idempotent re-render
  // pattern already used elsewhere for a language switch, see the
  // 'm-iphone-info' checks near the listen() handlers below.
  if($('m-iphone-info').classList.contains('show')) openIphoneInfoModal();
  // Same treatment for the Ori Info modal's iPhone row — it shows the same
  // bond name/paired state, just one line instead of the full stats card.
  if($('m-ori-info').classList.contains('show')) refreshOriInfoModal();
}

// Lights up the first `level` (0-4) bars of a .sig-bars element — shared by
// the Ori info modal and the iPhone info modal.
function renderSigBars(id,level){
  const el=$(id); if(!el) return;
  [...el.children].forEach((b,i)=>b.classList.toggle('active',i<level));
}

// iPhone info / stats — tapping the header phone icon (while an iPhone is
// bonded) opens this read-only snapshot instead of jumping straight to the
// Unpair confirm. Counts + signal come straight from Ori's ANCS client,
// relayed over the Phone Bond Status characteristic (char 000F) into
// lastPhoneBondStatus's m/u/t/s fields — real data, not a mock. The Unpair
// button routes to the existing confirm modal (m-unpair-phone) so the
// destructive action still takes a deliberate second tap.
function openIphoneInfoModal(){
  const t=I18N[appLang].iphoneInfoModal;
  const s=lastPhoneBondStatus;
  $('ipInfoTitle').textContent=s.n||I18N[appLang].unpairPhoneModal.fallbackName;
  $('ipInfoState').textContent=s.c?I18N[appLang].main.connected:I18N[appLang].main.disconnected;
  $('ipInfoDot').className='p-dot '+(s.c?'available':'offline');
  renderSigBars('ipInfoSigBars',s.c?s.s:0);
  $('ipInfoSigBars').title=t.sigLbl;

  // Counts + badges are only meaningful while the iPhone link is actually up.
  // Disconnected: icons stay visible but dimmed (.zero) and badges hidden —
  // not replaced with a text hint (matches the final Ori_UI_Prototype.js design).
  const setStat=(icoId,badgeId,tileId,count,label)=>{
    const shown=s.c?count:0;
    $(icoId).classList.toggle('zero',shown===0);
    const badge=$(badgeId);
    badge.style.display=(s.c&&shown>0)?'':'none';
    badge.textContent=shown>99?'99+':shown;
    $(tileId).title=label;
  };
  setStat('ipInfoMissedIco','ipInfoMissedBadge','ipInfoMissedTile',s.m,t.missedLbl);
  setStat('ipInfoUnreadIco','ipInfoUnreadBadge','ipInfoUnreadTile',s.u,t.unreadLbl);
  setStat('ipInfoNotifIco','ipInfoNotifBadge','ipInfoNotifTile',s.t,t.notifLbl);
  // Tap-to-drill-down (ble-protocol.md §13) — clickable whenever connected,
  // even at zero count (opens the empty state rather than being inert — a
  // dimmed icon means "nothing here right now," not "you can't check"; same
  // policy as Ori's own on-device modal_iphone_info.cpp). Gated on
  // PhoneBondStatus's own live connection flag (char 000F), not
  // ANCS_STORE.size — that's the authoritative link state even if Orion's
  // local mirror is momentarily behind.
  $('ipInfoMissedTile').onclick=s.c?()=>openAncsListModal('missed'):null;
  $('ipInfoUnreadTile').onclick=s.c?()=>openAncsListModal('unread'):null;
  $('ipInfoNotifTile').onclick=s.c?()=>openAncsListModal('other'):null;
  $('ipInfoMissedTile').style.cursor=$('ipInfoMissedTile').onclick?'pointer':'';
  $('ipInfoUnreadTile').style.cursor=$('ipInfoUnreadTile').onclick?'pointer':'';
  $('ipInfoNotifTile').style.cursor=$('ipInfoNotifTile').onclick?'pointer':'';
  $('ipInfoCancelBtn').textContent=I18N[appLang].common.cancel;
  $('ipInfoUnpairBtn').textContent=I18N[appLang].unpairPhoneModal.unpair;
  openModalFrom('m-iphone-info');
}
// Unpair from the info modal → hand off to the existing confirm modal.
function ipInfoToUnpair(){
  openUnpairPhoneModal();
}

// Tapping Unpair on the iPhone info modal offers to unpair the bonded
// iPhone — the one phone-pairing action Orion can actually take (re-pairing
// still only happens on Ori's own screen). Available whether or not the
// phone link is currently connected: the bond lives in Ori's NVS either way,
// and Unpair Phone (Device Command char 0008, ble-protocol.md §3) only needs
// the Orion<->Ori link up, not the iPhone one.
function openUnpairPhoneModal(){
  const t=I18N[appLang].unpairPhoneModal;
  const name=lastPhoneBondStatus.n||t.fallbackName;
  $('unpairPhoneBody').textContent=t.body.replace('{name}',name);
  openModalFrom('m-unpair-phone');
}
function doUnpairPhone(){
  hideModal('m-unpair-phone');
  invoke('unpair_phone').catch(()=>{});
  // Ori notifies Phone Bond Status back to {b:false,c:false} once it
  // processes this; the phone-bond-status listener below will pick that up
  // and hide the icon. No optimistic local update — wait for the real event.
}

// ── ANCS drill-down — tap a call/message/notifications icon in the iPhone
// Info modal to see the underlying notifications for that one category, then
// tap a row for its detail (or, for a still-ringing call, the incoming-call
// takeover instead). Orion-only: Ori's own iPhone Info icons are
// informational, this drill-down doesn't exist on the device screen.
//
// ANCS_STORE is a live mirror fed entirely by the 'ancs-notification' event
// (char 0010, ble-protocol.md §13) — there is no local mock data here, only
// a place to hold whatever Ori has actually relayed. Bucketed exactly like
// PhoneBondStatus's own m/u/t aggregate counts (char 000F): category 2
// (MissedCall) -> missed, category 4 (Social) -> unread, everything else ->
// other. Calls (category 1 IncomingCall, category 12 ActiveCall) never
// arrive via ancs-notification at all — they're relayed exclusively via
// 'ancs-call-state' (see CallSession below), so they never appear as a row
// in any bucket/list.
const ANCS_STORE={missed:new Map(),unread:new Map(),other:new Map()};
function ancsBucketFor(category){
  return category===2?'missed':category===4?'unread':'other';
}
// Escapes untrusted external text (notification title/body/app/action
// labels all originate from arbitrary phone apps, unlike the rest of this
// file's mostly-static UI strings) before it's interpolated into an
// innerHTML template — same concern suRenderDevices() addresses for BLE
// device names, just via escaping here instead of textContent since these
// templates are built as strings, not DOM nodes, to keep this a faithful
// structural port of the prototype's own ancs*() functions.
function escapeHtml(s){
  return String(s==null?'':s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}
// "add" covers both a genuinely-new notification and an ANCS Modified event
// — Orion replaces its stored copy in place, keyed by uid (ble-protocol.md
// §13). Removes any stale copy first in case a Modified event ever changed
// a notification's category (not expected in practice, but keying strictly
// by uid keeps this correct either way). Returns the bucket it landed in.
function ancsUpsert(n){
  ancsRemove(n.u);
  const bucket=ancsBucketFor(n.c);
  ANCS_STORE[bucket].set(n.u,{
    uid:n.u,app:n.a||'',title:n.t||'',body:n.b||'',recvEpoch:n.e||0,
    pos_label:n.p||'',neg_label:n.n||'',has_neg_action:!!n.g,silent:!!n.s,
    icon_token:n.k||'',
  });
  return bucket;
}
// Deletes uid from whichever bucket actually has it (a "remove" for a uid
// Orion never received — e.g. one the filter was already excluding — is a
// harmless no-op, same as on Ori itself). Returns the bucket it was removed
// from, or null.
function ancsRemove(uid){
  for(const bucket of ['missed','unread','other']){
    if(ANCS_STORE[bucket].delete(uid)) return bucket;
  }
  return null;
}
function ancsClearAll(){
  ANCS_STORE.missed.clear();ANCS_STORE.unread.clear();ANCS_STORE.other.clear();
}
// Newest-first, derived from recv_epoch (not array/insertion order — a
// real Map has no guaranteed relayed-order under Modified-event replacement)
// with uid as a stable tiebreaker for same-second arrivals.
function ancsBucketItems(bucket){
  return [...ANCS_STORE[bucket].values()].sort((a,b)=>(b.recvEpoch-a.recvEpoch)||(b.uid-a.uid));
}

function ancsTimeAgo(epoch){
  const t=I18N[appLang].ancsList;
  if(!epoch) return '';
  const diffS=Math.max(0,Math.floor(Date.now()/1000)-epoch);
  if(diffS<60) return t.justNow;
  const mins=Math.floor(diffS/60);
  if(mins<60) return t.minAgo.replace('{n}',mins);
  const hours=Math.floor(mins/60);
  if(hours<24) return t.hourAgo.replace('{n}',hours);
  return t.dayAgo.replace('{n}',Math.floor(hours/24));
}

const ANCS_ICON_PATHS={
  call:'<path d="M6.62 10.79c1.44 2.83 3.76 5.14 6.59 6.59l2.2-2.2c.27-.27.67-.36 1.02-.24 1.12.37 2.33.57 3.57.57.55 0 1 .45 1 1V20c0 .55-.45 1-1 1-9.39 0-17-7.61-17-17 0-.55.45-1 1-1h3.5c.55 0 1 .45 1 1 0 1.25.2 2.45.57 3.57.11.35.03.74-.25 1.02l-2.2 2.2z" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round" stroke-linecap="round"/>',
  message:'<path d="M6 4 H18 A3 3 0 0 1 21 7 V14 A3 3 0 0 1 18 17 L13 17 8 21 8 17 6 17 A3 3 0 0 1 3 14 V7 A3 3 0 0 1 6 4 Z" stroke="currentColor" stroke-width="1.7" stroke-linejoin="round" stroke-linecap="round"/>',
  bell:'<path d="M18 8a6 6 0 1 0-12 0c0 7-3 9-3 9h18s-3-2-3-9Z" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/><path d="M13.73 21a2 2 0 0 1-3.46 0" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>',
  close:'<path d="M5 5 L19 19 M19 5 L5 19" stroke="currentColor" stroke-width="2" stroke-linecap="round"/>',
  'bell-off':'<path d="M18 8a6 6 0 1 0-12 0c0 7-3 9-3 9h18s-3-2-3-9Z" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/><path d="M13.73 21a2 2 0 0 1-3.46 0" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/><path d="M3 3 L21 21" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"/>',
};
function ancsIconSvg(kind){
  return '<svg viewBox="0 0 24 24" fill="none">'+ANCS_ICON_PATHS[kind]+'</svg>';
}
// Category fallback icon is always a circle (background-color bubble), never
// a rounded square — reserved for tokens with no brand asset. A real brand
// icon (ANCS_APP_ICON_MAP below) renders as a rounded square instead, see
// ancsIconMarkup().
function ancsCategoryClass(bucket){
  return bucket==='missed'?'cat-missed':bucket==='unread'?'cat-unread':'cat-other';
}
function ancsIconKind(bucket){
  return bucket==='missed'?'call':bucket==='unread'?'message':'bell';
}

// Real per-app brand icons, keyed by the same icon_token vocabulary Ori's own
// on-device status bar uses (firmware/img/ANCS_icons/, ble-protocol.md §13's
// "Icon tokens" — 48 brand apps + twitter). Ported directly from the same
// 60×60 source images the firmware compiles into flash (`convert_icons.py`'s
// `cropped/` output), so Orion's icons are pixel-identical to Ori's.
const ANCS_APP_ICON_MAP={
  gmail:'assets/ancs_icons/gmail.png', messenger:'assets/ancs_icons/messenger.png',
  instagram:'assets/ancs_icons/instagram.png', facebook:'assets/ancs_icons/facebook.png',
  whatsapp:'assets/ancs_icons/whatsapp.png', slack:'assets/ancs_icons/slack.png',
  twitter:'assets/ancs_icons/twitter.png', teams:'assets/ancs_icons/teams.png',
  sms:'assets/ancs_icons/sms.png', phone:'assets/ancs_icons/phone.png',
  discord:'assets/ancs_icons/discord.png', telegram:'assets/ancs_icons/telegram.png',
  youtube:'assets/ancs_icons/youtube.png', youtube_music:'assets/ancs_icons/youtube_music.png',
  tiktok:'assets/ancs_icons/tiktok.png', spotify:'assets/ancs_icons/spotify.png',
  wechat:'assets/ancs_icons/wechat.png', line:'assets/ancs_icons/line.png',
  zoom:'assets/ancs_icons/zoom.png', outlook:'assets/ancs_icons/outlook.png',
  snapchat:'assets/ancs_icons/snapchat.png', google_meet:'assets/ancs_icons/google_meet.png',
  facetime:'assets/ancs_icons/facetime.png', linkedin:'assets/ancs_icons/linkedin.png',
  reddit:'assets/ancs_icons/reddit.png', threads:'assets/ancs_icons/threads.png',
  twitch:'assets/ancs_icons/twitch.png', uber:'assets/ancs_icons/uber.png',
  apple_music:'assets/ancs_icons/apple_music.png', amazon:'assets/ancs_icons/amazon.png',
  viber:'assets/ancs_icons/viber.png', claude:'assets/ancs_icons/claude.png',
  chatgpt:'assets/ancs_icons/chatgpt.png', google_map:'assets/ancs_icons/google_map.png',
  google_photos:'assets/ancs_icons/google_photos.png', health:'assets/ancs_icons/health.png',
  apple_calendar:'assets/ancs_icons/apple_calendar.png', apple_findmy:'assets/ancs_icons/apple_findmy.png',
  apple_mail:'assets/ancs_icons/apple_mail.png', apple_maps:'assets/ancs_icons/apple_maps.png',
  apple_reminders:'assets/ancs_icons/apple_reminders.png', apple_wallet:'assets/ancs_icons/apple_wallet.png',
  github:'assets/ancs_icons/github.png', google_authenticator:'assets/ancs_icons/google_authenticator.png',
  microsoft_authenticator:'assets/ancs_icons/microsoft_authenticator.png', notion:'assets/ancs_icons/notion.png',
  venmo:'assets/ancs_icons/venmo.png', skype:'assets/ancs_icons/skype.png',
  paypal:'assets/ancs_icons/paypal.png',
};
// Shared icon-tile inner markup for the list row and the detail header. A
// recognised icon_token renders the real brand icon; "" or an unrecognised
// token (Orion/firmware drifted out of sync on the supported set) falls back
// to the category glyph — mirrors Ori's own ancs_icons::image()/
// category_image() fallback chain (ble-protocol.md §13).
function ancsIconMarkup(bucket,iconToken){
  const img=ANCS_APP_ICON_MAP[iconToken];
  return img?'<img class="ancs-app-icon-img" src="'+img+'" alt="">':ancsIconSvg(ancsIconKind(bucket));
}
function ancsIconTileClass(baseClass,bucket,iconToken){
  return baseClass+(ANCS_APP_ICON_MAP[iconToken]?' has-img':' '+ancsCategoryClass(bucket));
}

const ANCS_LIST_TITLES=()=>I18N[appLang].ancsList.titles;
const ANCS_LIST_EMPTY=()=>I18N[appLang].ancsList.empty;

// Groups raw notifications by (app, title) — same rule Ori's own
// app_state::ancs_collect_same_title uses for the status-bar tiles and the
// stacked-message overlay. ancsBucketItems() is already newest-first, so the
// first item seen for a key is the group's reference (most recent).
function ancsGroupItems(bucket){
  const items=ancsBucketItems(bucket);
  const order=[];
  const byKey={};
  items.forEach(it=>{
    const key=it.app+'|'+it.title;
    if(!byKey[key]){
      byKey[key]={uids:[],items:[]};
      order.push(byKey[key]);
    }
    byKey[key].uids.push(it.uid);
    byKey[key].items.push(it);
  });
  return order.map(g=>({uids:g.uids,items:g.items,ref:g.items[0],count:g.items.length}));
}
function ancsFindGroup(bucket,uid){
  return ancsGroupItems(bucket).find(g=>g.uids.indexOf(uid)!==-1);
}

// Row shows icon + title + latest preview only — no timestamp (shown in the
// detail modal once opened) and one row per GROUP, not per raw notification.
function openAncsListModal(bucket){
  const t=I18N[appLang].ancsList;
  $('ancsListTitle').textContent=ANCS_LIST_TITLES()[bucket];
  const groups=ancsGroupItems(bucket);
  dlog('[ORION-DEBUG] openAncsListModal bucket='+bucket+': '+ANCS_STORE[bucket].size+' raw item(s), '+groups.length+' group(s)');
  const bodyEl=$('ancsListBody');
  bodyEl.dataset.bucket=bucket;
  bodyEl.innerHTML=groups.length===0?'<div class="ancs-list-empty">'+ANCS_LIST_EMPTY()[bucket]+'</div>':
    groups.map(g=>{
      const it=g.ref;
      const stacked=g.count>1;
      // Both badges overlap the icon's top-right corner, so they're
      // mutually exclusive — count wins when a stacked group's reference
      // happens to also be silent, same priority as Ori's own status-bar
      // tile (stacked-count is the more load-bearing signal of the two).
      const countBadge=stacked?'<div class="ancs-row-count">'+(g.count>9?'9+':g.count)+'</div>':'';
      const silentBadge=(!stacked && it.silent)?
        '<div class="ancs-row-silent-badge" title="'+t.silentTitle+'">'+ancsIconSvg('bell-off')+'</div>':'';
      // Swiping is only wired when there's actually a negative action to
      // send (mirrors the detail modal's danger/Read-all button — same
      // has_neg_action field) — a row with nothing to dismiss doesn't swipe.
      return '<div class="ancs-row-wrap">'+
        '<div class="ancs-row" data-uid="'+it.uid+'" data-bucket="'+bucket+'" data-has-del="'+(it.has_neg_action?'1':'')+'">'+
          '<div class="ancs-row-icon-wrap">'+
            '<div class="'+ancsIconTileClass('ancs-row-icon',bucket,it.icon_token)+'">'+ancsIconMarkup(bucket,it.icon_token)+'</div>'+
            countBadge+
            silentBadge+
          '</div>'+
          '<div class="ancs-row-text">'+
            '<div class="ancs-row-title">'+escapeHtml(it.title)+'</div>'+
            '<div class="ancs-row-preview">'+escapeHtml(it.body)+'</div>'+
          '</div>'+
        '</div>'+
      '</div>';
    }).join('');
  ancsInitRowGestures(bodyEl);
  openModalFrom('m-ancs-list');
}

// Swipe-left-to-delete — dragging a row left past ANCS_SWIPE_COMMIT of its
// own width and releasing dismisses it (or its whole stacked group); the row
// itself slides with the drag and fades proportionally (further dragged =
// fainter) — no revealed background/icon underneath. Pointer events cover
// mouse drag and trackpad/touch alike. Only one row can be mid-drag at a
// time (ancsSwipeState).
const ANCS_SWIPE_COMMIT = 0.35; // fraction of row width that commits the delete
let ancsSwipeState = null;

function ancsInitRowGestures(container){
  container.querySelectorAll('.ancs-row').forEach(row=>{
    row.addEventListener('pointerdown', ancsSwipeStart);
    row.addEventListener('click', ancsRowClick);
  });
}

function ancsRowClick(e){
  const row=e.currentTarget;
  // A completed drag (in either direction) shouldn't also open the detail —
  // pointerup fires just before click. The flag is cleared here, one-shot.
  if(row.dataset.suppressClick){ delete row.dataset.suppressClick; return; }
  openAncsDetail(Number(row.dataset.uid), row.dataset.bucket);
}

function ancsSwipeStart(e){
  if(e.button!==undefined && e.button!==0) return; // primary mouse button / touch / pen only
  const row=e.currentTarget;
  if(!row.dataset.hasDel) return; // nothing to swipe to
  const rect=row.getBoundingClientRect();
  ancsSwipeState={row,startX:e.clientX,dx:0,width:rect.width,dragging:false,pointerId:e.pointerId};
  row.addEventListener('pointermove', ancsSwipeMove);
  row.addEventListener('pointerup', ancsSwipeEnd);
  row.addEventListener('pointercancel', ancsSwipeEnd);
}

function ancsSwipeMove(e){
  const s=ancsSwipeState;
  if(!s || e.pointerId!==s.pointerId) return;
  const rawDx=e.clientX-s.startX;
  if(!s.dragging){
    if(Math.abs(rawDx)<6) return; // ignore tiny jitter so plain clicks stay clicks
    s.dragging=true;
    s.row.style.transition='none';
    s.row.setPointerCapture(s.pointerId);
  }
  s.dx=Math.max(-s.width,Math.min(0,rawDx)); // left only, clamped to the row's own width
  s.row.style.transform='translateX('+s.dx+'px)';
  // Fade proportionally to how far it's been dragged — at dx=0 fully
  // opaque, at dx=-width fully transparent. This IS the delete affordance;
  // there's no separate revealed panel to look at instead.
  s.row.style.opacity=String(Math.max(0,1-Math.abs(s.dx)/s.width));
}

function ancsSwipeEnd(e){
  const s=ancsSwipeState;
  if(!s || e.pointerId!==s.pointerId) return;
  s.row.removeEventListener('pointermove', ancsSwipeMove);
  s.row.removeEventListener('pointerup', ancsSwipeEnd);
  s.row.removeEventListener('pointercancel', ancsSwipeEnd);
  const committed=s.dragging && Math.abs(s.dx)>s.width*ANCS_SWIPE_COMMIT;
  const uid=Number(s.row.dataset.uid), bucket=s.row.dataset.bucket;
  s.row.style.transition='transform .18s ease-out, opacity .18s ease-out';
  if(committed){
    s.row.style.transform='translateX(-100%)';
    s.row.style.opacity='0';
    // No local mutation, no re-render here — the row already reads as
    // "gone" visually; the actual removal from ANCS_STORE (and thus the
    // list, on its next render) only happens once the real
    // AncsNotification{op:"remove"} notify confirms Ori actually cleared
    // it (ble-protocol.md §13's "never optimistic" rule).
    setTimeout(()=>{ ancsRowQuickDismiss(uid,bucket); },160);
  } else {
    s.row.style.transform='translateX(0)';
    s.row.style.opacity='1';
  }
  if(s.dragging) s.row.dataset.suppressClick='1';
  ancsSwipeState=null;
}

// Button set/labels are derived from the notification's own fields — same
// rule Ori's modal_ancs_notification.cpp uses: positive action (if any) as
// the accent button, negative action (if any) as the danger button, and a
// plain Close only when NEITHER is offered. Never a hardcoded "Answer" /
// "Decline" / "Dismiss" string — whatever pos_label/neg_label says is what
// renders. (Stacked groups skip this — single "Read all" instead, below.)
function ancsActionsHTML(it,uid,bucket){
  const t=I18N[appLang].ancsList;
  const hasPos=!!it.pos_label;
  const hasNeg=!!it.has_neg_action;
  let html='';
  if(hasPos) html+='<button class="btn btn-primary btn-full" onclick="ancsPositiveAction('+uid+',\''+bucket+'\')">'+escapeHtml(it.pos_label)+'</button>';
  if(hasNeg) html+='<button class="btn btn-dng btn-full" onclick="ancsNegativeAction('+uid+',\''+bucket+'\')">'+escapeHtml(it.neg_label||t.dismissFallback)+'</button>';
  if(!hasPos&&!hasNeg) html+='<button class="btn btn-sec btn-full" onclick="ancsBackToList(\''+bucket+'\')">'+t.closeFallback+'</button>';
  return html;
}

// Layout mirrors Ori's own ANCS overlay: silent badge top-left, close-X
// top-right (both corners; the X dismisses unconditionally, same as Ori's
// ui::add_close_x, regardless of which action buttons are below), icon,
// title, body/time, actions. No app-name line — the icon + title already
// identify the source. Stacked groups (count > 1) replace the single
// body/time/action-buttons with every message (oldest first, divider-split)
// and one "Read all" button. Stashes which bucket/uids are on screen
// (dataset) so a later 'remove'/'clear' event can tell whether THIS detail
// just went stale (ble-protocol.md §13).
function openAncsDetail(uid,bucket){
  const t=I18N[appLang].ancsList;
  const g=ancsFindGroup(bucket,uid);
  if(!g) return;
  const it=g.ref;
  const stacked=g.count>1;
  const iconCls=ancsIconTileClass('ad-app-icon',bucket,it.icon_token);
  const silentBadge=it.silent?
    '<div class="ad-silent-badge">'+ancsIconSvg('bell-off')+t.silentBadge+'</div>':'';

  let bodyHtml;
  if(stacked){
    const oldestFirst=g.items.slice().reverse();
    bodyHtml='<div class="ad-count">'+t.messagesCount.replace('{n}',g.count)+'</div>'+
      oldestFirst.map((m,i)=>(i>0?'<div class="ad-divider"></div>':'')+
        '<div class="ad-body">'+escapeHtml(m.body)+'</div>'+
        '<div class="ad-time">'+ancsTimeAgo(m.recvEpoch)+'</div>'
      ).join('');
  } else {
    bodyHtml=(it.body?'<div class="ad-body">'+escapeHtml(it.body)+'</div>':'')+
      '<div class="ad-time">'+ancsTimeAgo(it.recvEpoch)+'</div>';
  }
  const actions=stacked?
    '<button class="btn btn-dng btn-full" onclick="ancsReadAllGroup('+it.uid+',\''+bucket+'\')">'+t.readAll+'</button>'
    :ancsActionsHTML(it,it.uid,bucket);

  const card=$('ancsDetailCard');
  card.dataset.bucket=bucket;
  card.dataset.uids=g.uids.join(',');
  card.innerHTML=
    '<div class="ancs-detail'+(it.silent?' has-silent':'')+'">'+
      silentBadge+
      '<div class="ad-close-x" onclick="ancsBackToList(\''+bucket+'\')">'+ancsIconSvg('close')+'</div>'+
      '<div class="'+iconCls+'">'+ancsIconMarkup(bucket,it.icon_token)+'</div>'+
      '<div class="ad-title">'+escapeHtml(it.title)+'</div>'+
      bodyHtml+
      '<div class="ad-actions">'+actions+'</div>'+
    '</div>';
  openModalFrom('m-ancs-detail');
}

// Dismissing a detail — by action, Read all, or the corner X — always
// returns to the list it was opened from, not the screen behind everything.
// The user drilled in one level at a time, so they back out the same way.
function ancsBackToList(bucket){
  openAncsListModal(bucket);
}

// Action taps write AncsNotificationAction (char 0012) and stop there — no
// local mutation, no navigation. ble-protocol.md §13: "Orion does not
// update its own UI optimistically on write: it waits for the resulting
// AncsNotification{op:"remove"} ... the same as every other state change in
// this protocol." The stale-detail handling wired into the
// 'ancs-notification' listener below is what actually closes this modal and
// returns to the list, once Ori confirms the notification is really gone.
function ancsPositiveAction(uid,bucket){
  invoke('ancs_notification_action',{u:uid,a:0}).catch(e=>console.error('ancs_notification_action failed:',e));
}
function ancsNegativeAction(uid,bucket){
  invoke('ancs_notification_action',{u:uid,a:1}).catch(e=>console.error('ancs_notification_action failed:',e));
}
// "Read all" on a stacked group — one write per uid (Negative/dismiss —
// same action a lone row's own Dismiss button sends), mirroring the loop in
// modal_ancs_notification.cpp's on_read().
function ancsReadAllGroup(refUid,bucket){
  const g=ancsFindGroup(bucket,refUid);
  if(!g) return;
  g.uids.forEach(uid=>invoke('ancs_notification_action',{u:uid,a:1}).catch(e=>console.error('ancs_notification_action failed:',e)));
}
// Row's own swipe-to-delete commit — clears the whole group (if stacked),
// same as Read all above.
function ancsRowQuickDismiss(uid,bucket){
  const g=ancsFindGroup(bucket,uid);
  if(!g) return;
  g.uids.forEach(u=>invoke('ancs_notification_action',{u,a:1}).catch(e=>console.error('ancs_notification_action failed:',e)));
}
// Back from the list → iPhone Info (re-render so any change above shows).
function ancsListBack(){
  openIphoneInfoModal();
}

// Re-renders the drill-down list in place if it's currently open and showing
// this bucket — called after every ANCS_STORE mutation (ble-protocol.md §13:
// "if the drill-down list modal is currently open, re-render it in place").
function ancsRefreshOpenList(bucket){
  const bodyEl=$('ancsListBody');
  if($('m-ancs-list').classList.contains('show') && bodyEl.dataset.bucket===bucket){
    openAncsListModal(bucket);
  }
}
// Closes the detail modal and returns to its list if it's currently showing
// the uid that was just removed — "never leaves a detail open for content
// that no longer exists on the phone" (ble-protocol.md §13, pc-app.md).
function ancsCloseStaleDetail(uid,bucket){
  const card=$('ancsDetailCard');
  if(!$('m-ancs-detail').classList.contains('show')) return false;
  if(card.dataset.bucket!==bucket) return false;
  const shown=(card.dataset.uids||'').split(',').map(Number);
  if(shown.indexOf(uid)===-1) return false;
  ancsBackToList(bucket); // closes detail, re-renders the (now-updated) list
  return true;
}
function ancsRefreshIphoneInfoIfOpen(){
  if($('m-iphone-info').classList.contains('show')) openIphoneInfoModal();
}

// ── Incoming call — auto-appears over whatever Orion is currently showing
// the instant AncsCallState{st:1} arrives (char 0011, ble-protocol.md §13),
// mirroring Ori's own modal_incoming_call.cpp: a ringing banner with
// Answer/Decline, then (once answered) an in-call view with a live mm:ss
// duration timer that keeps running independent of the dialog's visibility
// — reopening it shows the running time, not a reset one.
const CallSession={running:false,uid:0,elapsedS:0,timerId:null,label:null};

function callSessionStart(uid,seedElapsedS){
  if(CallSession.running && CallSession.uid===uid) return; // already running — never reset an in-progress call
  callSessionStop();
  CallSession.running=true;
  CallSession.uid=uid;
  CallSession.elapsedS=seedElapsedS||0; // seeded from AncsCallState's "e" on a reconnect mid-call, not restarted at zero
  CallSession.timerId=setInterval(()=>{
    CallSession.elapsedS++;
    if(CallSession.label) CallSession.label.textContent=callFmtDuration(CallSession.elapsedS);
  },1000);
}
function callSessionStop(){
  if(CallSession.timerId) clearInterval(CallSession.timerId);
  CallSession.running=false;CallSession.uid=0;CallSession.elapsedS=0;CallSession.timerId=null;CallSession.label=null;
}
function callFmtDuration(s){
  const hh=Math.floor(s/3600),mm=Math.floor((s%3600)/60),ss=s%60;
  const pad=n=>String(n).padStart(2,'0');
  return hh>0?hh+':'+pad(mm)+':'+pad(ss):pad(mm)+':'+pad(ss);
}
// Every other modal is hidden first so the call takes over the whole panel
// cleanly, regardless of what was open when it arrived — matches Ori
// creating its scrim on lv_screen_active(), which always ends up topmost no
// matter what screen/modal was showing. Instant (hideModalInstant), not a
// plain classList.remove — showModal('m-incoming-call') follows in the same
// call, so a normal fade-out here would still show a frame of unblurred main
// screen before the incoming-call view's own blur ramps in (switchModal's
// comment). Returns whether anything was actually interrupted, so the caller
// can show m-incoming-call instantly too in that case — nothing was
// interrupted → the call is arriving fresh over a bare main screen, where
// the normal fade-in was never reported as a problem.
function callTakeOverScreen(){
  const open=[...document.querySelectorAll('.modal-bg.show')];
  open.forEach(m=>hideModalInstant(m.id));
  return open.length>0;
}

// Field-richness note: AncsCallState (char 0011) carries the full caller
// context — st/u/e plus a (app), t (title), p/n (action labels), g
// (has_neg_action), and k (icon token) — decoded by the Rust backend
// (src-tauri/src/ble/cbor.rs) and relayed here via 'ancs-call-state'. Calls
// never arrive via 'ancs-notification' (ble-protocol.md §13: no separate
// char-0010 payload to look the call up in), so this event is the ONLY source
// for the title/app/icon/action-labels this view and the header chip need.
// Every text field is still read defensively (missing → empty), so a bare
// st:2 (reconnect mid-call, or a follow-up tick with no metadata) falls back
// to the last cached meta and then to generic ANCS Answer/Decline copy —
// CBOR's own "unknown/absent keys" tolerance (ble-protocol.md §4/§9).
let lastCallMeta=null; // {uid,title,app,icon} — cached whenever a richer payload supplies it, so a bare st:2 (e.g. reconnect mid-call, or a follow-up tick with no metadata) can still show a title/icon
function callMetaFor(c){
  if(c.t||c.a||c.k) lastCallMeta={uid:c.u,title:c.t||'',app:c.a||'',icon:c.k||''};
  const cached=lastCallMeta&&lastCallMeta.uid===c.u?lastCallMeta:null;
  return {
    title:c.t||(cached&&cached.title)||'',
    app:c.a||(cached&&cached.app)||'',
    icon:c.k||(cached&&cached.icon)||'', // calling app's icon token (AncsCallState "k") — shows the real brand icon, falling back to the call glyph
    pos_label:c.p||'',
    neg_label:c.n||'',
    has_neg_action:c.g!==undefined?!!c.g:true, // a ringing/active call is always declinable/endable unless told otherwise
  };
}

// Call app-icon markup — the real brand icon (viber/phone/…) when Ori relayed
// a recognised icon token on AncsCallState ("k"), else the generic ringing
// call glyph. Mirrors ancsIconMarkup()/ancsIconTileClass() for notifications,
// but with the call glyph (not a category bell) as the fallback. Used by both
// the incoming/active call modal and the header call chip below.
function callIconInner(iconToken){
  const img=ANCS_APP_ICON_MAP[iconToken];
  return img?'<img class="ancs-app-icon-img" src="'+img+'" alt="">':ancsIconSvg('call');
}
function callIconTileClass(base,iconToken){
  return base+(ANCS_APP_ICON_MAP[iconToken]?' has-img':' cat-ringing');
}

// Latest call state (the raw AncsCallState payload), so the header call chip
// can reopen the correct view on click and pick the right app icon. null
// whenever there's no live call (st:0, or the Ori link dropped).
let currentCall=null;

// Header call chip — a quick-access icon shown to the LEFT of the phone icon
// whenever a call is ringing/active, so the user can reopen the call view
// (caller identity + running duration) after dismissing the full modal with
// its close button. Shows the calling app's own icon; clicking it reopens the
// ringing/in-call modal per the live state. Hidden whenever there's no call.
// Ring color: solid yellow while ringing (.ringing), solid red once answered/
// active (.on-call) — mirrors Ori's own status-bar call tile ring exactly
// (widget_status_bar.cpp's make_ancs_tile, styles.css's --call-ring-* vars).
function updateCallChip(){
  const chip=$('callIco');
  if(!chip) return;
  if(currentCall&&(currentCall.st===1||currentCall.st===2)){
    const m=callMetaFor(currentCall);
    const t=I18N[appLang].incomingCall;
    chip.innerHTML=callIconInner(m.icon);
    chip.classList.toggle('has-img',!!ANCS_APP_ICON_MAP[m.icon]);
    chip.classList.toggle('ringing',currentCall.st===1);
    chip.classList.toggle('on-call',currentCall.st===2);
    chip.title=currentCall.st===1?t.incoming:t.onCall;
    chip.style.display='';
  }else{
    chip.style.display='none';
    chip.classList.remove('has-img','ringing','on-call');
    chip.innerHTML='';
  }
}
// Click the header call chip → reopen the current call view exactly as it
// appeared. Reopening an active call never resets its timer (callSessionStart
// no-ops for an already-running uid). No-op if the call ended meanwhile (the
// chip would already be hidden).
function openCallFromChip(){
  if(!currentCall) return;
  if(currentCall.st===1) showIncomingCall(currentCall);
  else if(currentCall.st===2) showActiveCall(currentCall);
}

// Ori link broke (conn-state "off") — every ANCS-derived surface (drill-down
// list, its detail, the ringing/in-call view, and the iPhone Info hub that
// opens them) is now showing data Orion can no longer verify or act on (an
// action write would just fail), so tear them all down. The call session
// timer stops and currentCall clears too; if the call is still live when Ori
// reconnects, resync_orion_call_state replays it (ble-protocol.md §13) and the
// view + chip come back, its timer reseeded from the relayed elapsed seconds.
// Only fired on a sustained disconnect, NOT on the transient connecting/rec
// reconnect phases — those are reconciled by the resync itself (clear+re-add
// for notifications, a fresh st for calls), so closing there would just be a
// flicker before the same content returns.
function closeAncsSurfacesOnDrop(){
  hideModal('m-ancs-detail');
  hideModal('m-ancs-list');
  hideModal('m-incoming-call');
  hideModal('m-iphone-info');
  callSessionStop();
  currentCall=null;
  updateCallChip();
}

function showIncomingCall(c){
  const t=I18N[appLang].incomingCall;
  const m=callMetaFor(c);
  const interrupted=callTakeOverScreen();
  $('callCard').innerHTML=
    '<div class="ancs-detail">'+
      '<div class="ad-close-x" title="'+t.dismissRingingTitle+'" onclick="hideModal(\'m-incoming-call\')">'+ancsIconSvg('close')+'</div>'+
      '<div class="'+callIconTileClass('ad-app-icon',m.icon)+'">'+callIconInner(m.icon)+'</div>'+
      '<div class="ad-eyebrow">'+t.incoming+'</div>'+
      '<div class="ad-title">'+escapeHtml(m.title||t.unknownCaller)+'</div>'+
      (m.app?'<div class="ad-time" style="text-align:center;">'+escapeHtml(m.app)+'</div>':'')+
      '<div class="ad-actions">'+
        '<button class="btn btn-primary btn-full" onclick="callAnswer('+c.u+')">'+escapeHtml(m.pos_label||t.answerFallback)+'</button>'+
        (m.has_neg_action?'<button class="btn btn-dng btn-full" onclick="callDecline('+c.u+')">'+escapeHtml(m.neg_label||t.declineFallback)+'</button>':'')+
      '</div>'+
    '</div>';
  (interrupted?showModalInstant:showModal)('m-incoming-call');
}
// No optimistic switch to the active-call view — wait for the real
// AncsCallState{st:2} the answer produces (same "never optimistic" rule as
// the ANCS actions above).
function callAnswer(uid){
  invoke('ancs_notification_action',{u:uid,a:0}).catch(e=>console.error('ancs_notification_action failed:',e));
}
// ANCS Negative on a ringing call = Decline — same action code the general
// dismiss path uses, just reached from a different screen. Wait for
// AncsCallState{st:0} to actually close the ringing view.
function callDecline(uid){
  invoke('ancs_notification_action',{u:uid,a:1}).catch(e=>console.error('ancs_notification_action failed:',e));
}
function showActiveCall(c){
  const t=I18N[appLang].incomingCall;
  const m=callMetaFor(c);
  callSessionStart(c.u,c.e);
  const interrupted=callTakeOverScreen();
  $('callCard').innerHTML=
    '<div class="ancs-detail">'+
      '<div class="ad-close-x" title="'+t.hideActiveTitle+'" onclick="hideModal(\'m-incoming-call\')">'+ancsIconSvg('close')+'</div>'+
      '<div class="'+callIconTileClass('ad-app-icon',m.icon)+'">'+callIconInner(m.icon)+'</div>'+
      '<div class="ad-eyebrow">'+t.onCall+'</div>'+
      '<div class="ad-title">'+escapeHtml(m.title||t.unknownCaller)+'</div>'+
      '<div class="ad-call-timer" id="callTimerLabel">'+callFmtDuration(CallSession.elapsedS)+'</div>'+
      '<div class="ad-actions">'+
        '<button class="btn btn-dng btn-full" onclick="callEnd('+c.u+')">'+t.endCall+'</button>'+
      '</div>'+
    '</div>';
  CallSession.label=$('callTimerLabel');
  (interrupted?showModalInstant:showModal)('m-incoming-call');
}
// Same ANCS Negative action as Decline (hanging up = declining, from ANCS's
// point of view) — "End call" is a fixed literal for the same reason the
// eyebrow text is (mirrors Ori's own on_end_call(), which hardcodes it too,
// independent of neg_label, since by now the ringing-era label no longer
// fits). Wait for AncsCallState{st:0} to close the view + stop the timer.
function callEnd(uid){
  invoke('ancs_notification_action',{u:uid,a:1}).catch(e=>console.error('ancs_notification_action failed:',e));
}

// Ori device info / stats — tapping the header's device name + connection
// state opens a read-only snapshot of everything Orion knows about this
// specific Ori: identity (name, firmware, address, serial number,
// manufacture date), its signal, and its other bond (iPhone/ANCS).
//
// Live while open (mirrors the iPhone Info modal's own "live while open"
// treatment, pc-app.md): a poll timer re-fetches every ORI_INFO_POLL_MS
// while the modal is visible, plus push hooks off `setConn()`/
// `setPhoneBondStatus()` (search their bodies for 'm-ori-info') so the
// connection dot/state and iPhone row react immediately on a change instead
// of waiting for the next tick.
//
// Two data sources, merged:
//   - get_ori_info (Rust): no live BLE read. name/firmware/last-synced are
//     session-only (repopulate within moments of the next connect
//     regardless). address/serial_number/manufacture_date are DISK-
//     persisted (store::SavedState, Rust side) — they never change for a
//     given bond, so once learned they survive a disconnect AND an app
//     restart, not just this session (see its own Rust doc comment).
//   - read_device_settings (Rust, char 000E live read): serial_number ("s")
//     and manufacture_date ("b") come from Ori's write-once "factory" NVS
//     partition — Rust write-through caches these into the same persisted
//     store the moment they're first read, so there's no JS-side caching
//     needed here either, just "prefer the live value when we have one,
//     else the persisted one." signal_bars ("r") is Ori's own live RSSI to
//     Orion, sampled fresh on every read and bucketed 0-4 — the reverse of
//     the iPhone Info modal's signal bars (there Ori is central and reads
//     live; here Orion is central, and Windows' btleplug can't read RSSI of
//     an already-connected peripheral — only from advertising, which stops
//     once connected — so Ori reports its own reading back instead,
//     ble-protocol.md §4/§6.4). Signal bars are NOT persisted anywhere —
//     0 bars while disconnected, same "don't show what you can't verify"
//     policy Ori itself applies to presence/weather.
const ORI_INFO_POLL_MS=3000;
let oriInfoPollTimer=null;

async function refreshOriInfoModal(){
  const t=I18N[appLang].oriInfoModal;
  const [info,settings]=await Promise.all([
    invoke('get_ori_info').catch(()=>null),
    invoke('read_device_settings').catch(()=>null),
  ]);
  if(!info) return false;

  $('oriInfoTitle').textContent=info.name||$('hName').textContent;
  // Dot/state/signal-bars are driven off ONE signal — whether this
  // refresh's own live char-000E read just succeeded — not off `connState`
  // (app.js's app-wide connection state machine, `setConn()`). connState is
  // deliberately debounced (supervise_connection_loop's `settled_off` gate,
  // commands.rs) so a brief drop doesn't flash the whole UI; this modal's
  // live-read poll has no such debounce and can catch a dead link several
  // seconds before connState does. Driving all three off `connState` alone
  // used to show 0 bars (live, correct) next to "Connected" (stale) during
  // that gap — mixing a live signal with a debounced one always risks that
  // kind of contradiction, so all three now come from the same read.
  const live = connState==='on' && settings!=null;
  $('oriInfoDot').className='p-dot '+(live?'available':connState==='off'?'offline':'away');
  $('oriInfoState').textContent = live ? I18N[appLang].main.connected
    : connState==='connecting' ? I18N[appLang].main.connecting
    : connState==='rec' ? I18N[appLang].main.syncing
    : I18N[appLang].main.disconnected;
  renderSigBars('oriInfoSigBars',live?(settings.r||0):0);
  $('oriInfoSigBars').title=t.sigLbl;

  // Firmware version is intentionally NOT persisted (unlike address/serial/
  // manufacture date below) — it can change across an OTA update, so Rust's
  // session cache is a "last known, only trustworthy while connected" value,
  // not a permanent identity fact. Gate the display on `live` too: without
  // this, a disconnect would keep showing the last-read version as if it
  // were still confirmed current, the same stale-data problem the dot/state/
  // bars fix above addressed.
  $('oriInfoFwLbl').textContent=t.fwLbl;$('oriInfoFw').textContent=live?(info.firmware_version||t.unknown):t.unknown;
  $('oriInfoAddrLbl').textContent=t.addrLbl;$('oriInfoAddr').textContent=info.address||t.unknown;

  // Prefer a fresh live read (settings.s/.b) when connected, otherwise fall
  // back to get_ori_info's disk-persisted copy (store::SavedState — Rust
  // side, survives an app restart) rather than a client-side JS cache: the
  // two never actually disagree except on the very first read of a newly
  // provisioned unit, and Rust is now the one source of truth for "the last
  // known value," not this modal's own session.
  $('oriInfoSnLbl').textContent=t.snLbl;$('oriInfoSn').textContent=(settings&&settings.s)||info.serial_number||t.unknown;
  $('oriInfoMfgLbl').textContent=t.mfgLbl;$('oriInfoMfg').textContent=(settings&&settings.b)||info.manufacture_date||t.unknown;

  $('oriInfoPhoneLbl').textContent=t.phoneLbl;
  // Connection state, not identity — the phone's actual name already has a
  // home (header icon tooltip, iPhone Info modal title, Unpair modal). This
  // row only answers "is there a bond, and is it live right now."
  //
  // Gated on `live` (Ori itself reachable), NOT just `lastPhoneBondStatus.b`:
  // setConn() force-resets the cache to {b:false,...} on every non-'on'
  // conn-state (main.js's own "don't show what can't be verified" fallback,
  // same as the header phone icon hiding). Reading that reset `b:false` as
  // "no iPhone ever bonded" once Ori disconnects would misreport a real,
  // just-unverifiable bond as never having been set up. Only trust the cache
  // to distinguish Not Setup / Connected / Disconnected while Ori itself is
  // live; otherwise the honest answer is "don't know," same as every other
  // field here that depends on Ori actually being reachable right now.
  $('oriInfoPhone').textContent = !live ? t.unknown
    : !lastPhoneBondStatus.b ? t.notSetup
    : lastPhoneBondStatus.c ? I18N[appLang].main.connected : I18N[appLang].main.disconnected;
  $('oriInfoSyncLbl').textContent=t.syncLbl;
  const secs=info.last_synced_secs_ago;
  $('oriInfoSync').textContent = secs==null ? t.unknown : secs<60 ? t.justNow : t.minAgo.replace('{n}',Math.round(secs/60));

  $('oriInfoCloseBtn').textContent=I18N[appLang].pairfail.close;
  return true;
}

async function openOriInfoModal(){
  if(!(await refreshOriInfoModal())) return;
  showModal('m-ori-info');
  // Guarded so a stray second open() call (shouldn't happen — the header
  // trigger is a single click target) never stacks a duplicate interval.
  if(!oriInfoPollTimer) oriInfoPollTimer=setInterval(refreshOriInfoModal,ORI_INFO_POLL_MS);
}

function closeOriInfoModal(){
  if(oriInfoPollTimer){clearInterval(oriInfoPollTimer);oriInfoPollTimer=null;}
  hideModal('m-ori-info');
}

let pfChanged=false,pfRemoved=false;
let pfCommitted={name:'',title:'',email:'',phone:''};
// Populates the profile card + the editor's underlying inputs from the
// backend's cached profile (`get_initial_state`'s `profile` field) — the
// one thing that's normally missing on every app launch that ISN'T the
// tail end of first-time setup (`suFinishSetup` is the only other place
// that ever touches these elements), which is why the panel used to show a
// blank profile card after a plain app restart / reconnect. No-op when
// there's no real cached name yet (fresh install) — leaves the HTML's own
// placeholder markup alone rather than computing bogus initials from an
// empty string.
function hydrateProfileCard(profile){
  if(!profile||!profile.name||!profile.name.trim()) return;
  $('nmInp').value=profile.name;
  $('tlInp').value=profile.title||'';
  $('emInp').value=profile.email||'';
  $('phInp').value=profile.phone||'';
  $('mainName').textContent=profile.name;
  const photo=$('mainProfPhoto');
  if(profile.photoDataUrl){
    photo.style.backgroundImage=`url(${profile.photoDataUrl})`;
    photo.style.backgroundSize='cover';photo.style.backgroundPosition='center';
    $('mainProfInitials').style.display='none';
  } else {
    photo.style.backgroundImage='';
    $('mainProfInitials').style.display='';
    const parts=profile.name.trim().split(' ');
    $('mainProfInitials').textContent=(parts[0][0]+(parts[1]?parts[1][0]:'')).toUpperCase();
  }
}
// Same fix as hydrateProfileCard, just never extended to Time Off originally
// — get_initial_state's `time_off` field is the backend's persisted entry,
// which otherwise only ever reached the UI right after saveTimeOff() ran in
// this same session. Without this, a plain relaunch showed "no Time Off"
// even though the store and Ori both still had a real one, and saving a new
// entry in that state would have silently overwritten the still-valid one.
function hydrateTimeOffCard(timeOff){
  if(!timeOff||!timeOff.start||!timeOff.end||!timeOff.destination) return;
  // Wire fields are epoch SECONDS, and "end" is exclusive (midnight the day
  // after the last selected day — see saveTimeOff's own comment on why);
  // convert back to the inclusive last-day Date toCommittedEnd otherwise
  // always holds.
  const start=new Date(timeOff.start*1000);
  const exclusiveEnd=new Date(timeOff.end*1000);
  const end=new Date(exclusiveEnd.getFullYear(),exclusiveEnd.getMonth(),exclusiveEnd.getDate()-1);
  toCommittedStart=start;toCommittedEnd=end;toCommittedDest=timeOff.destination;
  toCommittedPhotoUrl=timeOff.photoDataUrl||null;
  timeOffActive=true;
  $('timeOffToggle').classList.add('on');
  $('mainTimeOffDest').textContent=toCommittedDest;$('mainTimeOffTextDest').textContent=toCommittedDest;
  const f=v=>v.toLocaleDateString(LOCALE_MAP[appLang],{month:'short',day:'numeric'});
  const range=f(start)+' – '+f(end);
  $('mainTimeOffDates').textContent=range;$('mainTimeOffTextDates').textContent=range;
  if(toCommittedPhotoUrl){
    const b=$('mainTimeOffBanner');
    b.style.backgroundImage=`url(${toCommittedPhotoUrl})`;
    b.style.backgroundSize='cover';b.style.backgroundPosition='center';b.style.backgroundRepeat='no-repeat';
  }
  setTimeOffState(true);
}
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
  // Can now genuinely reject (e.g. a disk-full/permissions failure writing
  // the local store) instead of always silently succeeding — surfaced to
  // the console rather than left as an unhandled promise rejection; the UI
  // already optimistically applied the edit above, so there's no pending
  // state to roll back here.
  invoke('save_profile',{input:{name,title:$('tlInp').value,email:$('emInp').value,phone:$('phInp').value,photoDataUrl,photoRemoved}})
    .catch(e=>console.error('save_profile failed:',e));
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
  // Can now genuinely reject — see save_profile's comment on why.
  invoke('clear_timeoff').catch(e=>console.error('clear_timeoff failed:',e));
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
  // Wire fields are epoch SECONDS (ble-protocol.md §4), not JS milliseconds
  // — and `selEnd` is local midnight at the *start* of the last selected
  // day, so the end sent to Ori is midnight at the start of the day after,
  // covering that whole last day rather than excluding almost all of it.
  const endExclusive=new Date(selEnd.getFullYear(),selEnd.getMonth(),selEnd.getDate()+1);
  // Can now genuinely reject — see save_profile's comment on why.
  invoke('save_timeoff',{input:{start:Math.floor(selStart.getTime()/1000),end:Math.floor(endExclusive.getTime()/1000),destination:dest,photoDataUrl,photoRemoved}})
    .catch(e=>console.error('save_timeoff failed:',e));
  timeOffPhotoRemoved=false;
  timeOffDirty=false;timeOffActive=true;
  $('timeOffToggle').classList.add('on');setTimeOffState(true);back();
}
let calYear,calMonth,selStart=null,selEnd=null,selPhase=0,calHover=null;
function initCal(){const t=new Date();calYear=t.getFullYear();calMonth=t.getMonth();renderCal();}
function togglePeriodCal(){
  const cal=$('periodCal'),disp=$('periodDisplay'),open=cal.style.display!=='none';
  if(open){cal.style.display='none';disp.classList.remove('open');}
  else{cal.style.display='';disp.classList.add('open');renderCal();}
}
function calNav(dir){
  let y=calYear,m=calMonth+dir;
  if(m<0){m=11;y--;}if(m>11){m=0;y++;}
  // Never navigate before the current real month — mirrors the same
  // year/month comparison renderCal() uses to disable .pcal-nav-prev, as a
  // belt-and-suspenders guard in case a caller bypasses the disabled button
  // (e.g. a future programmatic call).
  const today=new Date();
  if(dir<0&&(y<today.getFullYear()||(y===today.getFullYear()&&m<today.getMonth()))) return;
  calYear=y;calMonth=m;
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
    const cd=new Date(calYear,calMonth,d),past=cd<today;
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
// Hover-preview restyle only — used instead of a full renderCal() on every
// mousemove, which used to clear #pcalGrid's innerHTML and recreate every
// weekday-header + day-cell element (plus re-attach each day's onclick) on
// every hovered-cell change. None of that structure actually changes while
// hovering (same month, same today/past/dis classification) — only which
// cells count as sel-start/sel-end/in-range shifts as calHover moves. This
// walks the grid's already-built day cells (stable data-d, same recipe
// renderCal() used) and just retoggles those three classes.
function updateCalRangeClasses(){
  let hEnd=null;
  if(selPhase===1&&calHover!==null){const hd=new Date(calYear,calMonth,calHover);if(hd>selStart)hEnd=hd;}
  const rangeEnd=selEnd||hEnd;
  document.querySelectorAll('#pcalGrid .pcal-d[data-d]').forEach(el=>{
    const cd=new Date(calYear,calMonth,parseInt(el.dataset.d));
    const isSS=selStart&&cd.getTime()===selStart.getTime();
    const isSE=selEnd&&cd.getTime()===selEnd.getTime();
    const inRange=selStart&&rangeEnd&&cd>selStart&&cd<rangeEnd;
    el.classList.toggle('sel-start',!!isSS);
    el.classList.toggle('sel-end',!!isSE);
    el.classList.toggle('in-range',!!inRange);
  });
}
function onCalMove(e){
  if(selPhase!==1) return;
  const cell=e.target.closest('.pcal-d[data-d]:not(.dis)');
  const d=cell?parseInt(cell.dataset.d):null;
  if(d!==calHover){calHover=d;updateCalRangeClasses();}
}
function onCalLeave(){if(calHover!==null){calHover=null;updateCalRangeClasses();}}
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
  // Can now genuinely reject — see save_profile's comment on why.
  invoke('set_calendar_source',{source:calSrc}).catch(e=>console.error('set_calendar_source failed:',e));
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
  'favorite-1':'assets/shortcut_icons/favorite-1.png',
  'favorite-2':'assets/shortcut_icons/favorite-2.png',
  'favorite-3':'assets/shortcut_icons/favorite-3.png',
  'calculator':'assets/shortcut_icons/calculator.png',
  'copy':'assets/shortcut_icons/copy.png',
  'cut':'assets/shortcut_icons/cut.png',
  'paste':'assets/shortcut_icons/paste.png',
  'undo':'assets/shortcut_icons/undo.png',
  'redo':'assets/shortcut_icons/redo.png',
  'save':'assets/shortcut_icons/save.png'
};
function applySlot(n){
  const v=$('ss'+n).value;
  $('si'+n).src=siImgMap[v]||'';
  const favEl=$('fav'+n);
  const isFav=v.startsWith('favorite');
  favEl.style.display=isFav?'block':'none';
  if(isFav) renderKbdCombo(n);
  _updateSlotSave();
}
// Orion reads Device Settings from Ori on every (re)connect to recover the
// NVS-persisted fields (clock_face, time_format, ancs_filter, shortcut slot
// tokens) — ble-protocol.md §6.4. Presence/weather are ephemeral and not
// returned here. The Favorite key combos, in contrast, never live on Ori
// (host-side action mapping is Orion-local — pc-app.md), so they're fetched
// separately from Orion's own store; without this a combo recorded in a
// previous session would still fire but show as "Not set" here after a restart.
function readSlotsFromDevice(){
  Promise.all([
    invoke('read_device_settings'),
    invoke('get_shortcut_combos').catch(()=>[[],[],[]]),
  ]).then(([s,combos])=>{
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
      const combo=(combos&&combos[n-1])||[];
      kbdCommitted[n-1]=[...combo];_kbdCombos[n-1]=[...combo];
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
  // Built via DOM APIs, not innerHTML — `parts` holds raw `e.key` values
  // (_onKbdKey below), which for a symbol key produced with Shift (e.g.
  // Shift+, -> "<") is exactly that literal character. Interpolating it
  // into an HTML string let a recorded "<" (or similar) silently vanish
  // from the display instead of showing it, because the parser read it as
  // markup rather than text — same class of bug, and same fix
  // (createElement + textContent), as suRenderDevices()'s BLE device names.
  disp.innerHTML='';
  if(!parts.length){
    const span=document.createElement('span');
    span.className='kbd-unset';span.textContent=t.notSet;
    disp.appendChild(span);
    hint.textContent=t.clickToSet;
  } else {
    parts.forEach((p,i)=>{
      if(i>0){
        const sep=document.createElement('span');
        sep.className='kbd-sep';sep.textContent=' + ';
        disp.appendChild(sep);
      }
      const kbd=document.createElement('kbd');
      kbd.className='kc';kbd.textContent=p;
      disp.appendChild(kbd);
    });
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
  return [1,2,3].some(n=>$('ss'+n).value.startsWith('favorite')&&_kbdCombos[n-1].length===0);
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
  // Can now genuinely reject — see save_profile's comment on why.
  invoke('save_device_settings',{settings:{f:ancsLevel}})
    .catch(e=>console.error('save_device_settings (notification filter) failed:',e));
  back();
}
function saveClock(){
  clockFace=clockPending;
  $('mcDig').style.display=clockFace==='digital'?'flex':'none';$('mcAna').style.display=clockFace==='analog'?'block':'none';
  // Can now genuinely reject — see save_profile's comment on why.
  invoke('save_device_settings',{settings:{c:clockFace==='analog'?1:0}})
    .catch(e=>console.error('save_device_settings (clock face) failed:',e));
  back();
}
function saveTimeFormat(){
  timeFormat=timeFormatPending;
  _renderMainTimeFormatPreview();
  // Can now genuinely reject — see save_profile's comment on why.
  invoke('save_device_settings',{settings:{h:timeFormat==='12'?1:0}})
    .catch(e=>console.error('save_device_settings (time format) failed:',e));
  back();
}
function saveSlots(){
  if(_kbdRecSlot) stopKbdRecord();
  // Commit pending state
  [1,2,3].forEach(n=>{slotCommitted[n-1]=$('ss'+n).value;kbdCommitted[n-1]=[..._kbdCombos[n-1]];});
  [1,2,3].forEach(i=>$('ms'+i).src=siImgMap[$('ss'+i).value]||'');
  // Icon tokens go to Ori over BLE; Favorite key combos are a local Orion
  // setting only (pc-app.md — "host-side action mapping is local to Orion").
  // Can now genuinely reject — see save_profile's comment on why.
  invoke('save_shortcuts',{slots:slotCommitted,combos:kbdCommitted})
    .catch(e=>console.error('save_shortcuts failed:',e));
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
  // Can now genuinely reject — see save_profile's comment on why.
  invoke('clear_all').catch(e=>console.error('clear_all failed:',e));
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

async function clickFw(){
  if(fwAvail){
    // oufVerLine's current→new version line: current comes from Ori's cached
    // Firmware Revision String (get_ori_info, same source openOriInfoModal
    // reads); new is orionFwVersion, set from the 'fw-update-available'
    // event payload (see the listener below) — that's Ori's device firmware,
    // not Orion's own app version, so it belongs on this modal only.
    const info=await invoke('get_ori_info').catch(()=>null);
    $('oufVerLine').textContent=(info&&info.firmware_version||I18N[appLang].oriInfoModal.unknown)+' → '+orionFwVersion;
    showModal('m-fw');
  }
  else{const ico=$('fwIco');ico.style.transition='transform .15s';ico.style.transform='rotate(20deg)';setTimeout(()=>ico.style.transform='',300);}
}
function startFwInstall(){
  $('fw-c').style.display='none';$('fw-i').style.display='';
  // Can now genuinely reject — see save_profile's comment on why.
  invoke('firmware_install').catch(e=>console.error('firmware_install failed:',e));
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
  // Can now genuinely reject — see save_profile's comment on why.
  invoke('orion_update_install').catch(e=>console.error('orion_update_install failed:',e));
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
  // Can now genuinely reject — see save_profile's comment on why.
  invoke('orion_restart').catch(e=>console.error('orion_restart failed:',e));
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
    passkey:{title:'Enter passkey',bodyPre:'Enter the 6-digit code shown on ',bodySuf:'.',cancel:'Cancel',pair:'Pair'},
    connecting:{title:'Pairing with Ori…',sub:'Waiting for Ori to confirm…'},
    syncing:{title:'Setting up Ori…',progressLabel:'A busy day ahead…',doneLabel:'Ori is set up!'},
    pairfail:{title:'Couldn’t pair with Ori',body:'The passkey didn’t match, or the request timed out on Ori. Pick a device to try again.',close:'Close'},
    oriInfoModal:{fwLbl:'Firmware',addrLbl:'Address',snLbl:'Serial Number',mfgLbl:'Manufactured',sigLbl:'Signal',phoneLbl:'iPhone',syncLbl:'Synced',
      notSetup:'Not setup',justNow:'Just now',minAgo:'{n} min ago',unknown:'Unknown'},
    iphoneInfoModal:{missedLbl:'Missed Calls',unreadLbl:'Unread Messages',sigLbl:'Signal',notifLbl:'Notifications'},
    ancsList:{titles:{missed:'Missed Calls',unread:'Unread Messages',other:'Notifications'},
      empty:{missed:'No missed calls',unread:'No unread messages',other:'No notifications'},
      silentBadge:'Silent',silentTitle:'Delivered silently',readAll:'Read all',
      dismissFallback:'Dismiss',closeFallback:'Close',messagesCount:'{n} messages',
      justNow:'Just now',minAgo:'{n} min ago',hourAgo:'{n}h ago',dayAgo:'{n}d ago'},
    incomingCall:{incoming:'Incoming call',onCall:'On call',endCall:'End call',
      answerFallback:'Answer',declineFallback:'Decline',unknownCaller:'Unknown caller',
      dismissRingingTitle:'Dismiss — call keeps ringing',hideActiveTitle:'Hide — call keeps going'},
    settings:{title:'Settings',general:'General',auto:'Run automatically',autoSub:'Launch at Windows startup',
      calendar:'Calendar Source',language:'Language',about:'About',app:'Software Version',reset:'Reset'},
    main:{connected:'Connected',connecting:'Connecting…',syncing:'Syncing…',disconnected:'Disconnected',timeOff:'Time Off',
      noTimeOffPlanned:'No Time Off planned',tapToSet:'Tap to set',notifFilterRow:'Notification Filter',
      clockFaceRow:'Clock Face',timeFormatRow:'Time Format',quickActionsRow:'Quick Actions',presenceAvailable:'Available',
      settingsIcoTitle:'Settings',minimizeIcoTitle:'Minimize',fwUpToDate:'Firmware up to date',fwAvailable:'Firmware update available',
      phoneConnectedTitle:'{name} — Connected',phoneDisconnectedTitle:'iPhone disconnected',phoneUnknownName:'iPhone',
      reconnectTitle:'Reconnect'},
    profileEditor:{title:'Profile'},
    timeOffEditor:{periodLbl:'Period',selectDates:'Select dates…',selectStartDate:'Select start date',
      selectEndDate:'Now select end date',destinationLbl:'Destination',destinationPh:'City, Country',
      destinationPhotoLbl:'Destination photo'},
    calendarSource:{teamsStatus:'Signed in · Exchange Online',notSignedIn:'Not signed in',signingIn:'Signing in…',
      notConnected:'Not connected',signInGoogle:'Sign in with Google',signOutGoogle:'Sign out from Google',
      signInMicrosoft:'Sign in with Microsoft',signOutMicrosoft:'Sign out from Microsoft'},
    quickActions:{slotPrefix:'Slot',notSet:'Not set',clickToSet:'Click to set',clickToChange:'Click to change',
      pressShortcut:'Press shortcut…',escToCancel:'Esc to cancel',
      actionLabels:{'vol-mute':'Volume Mute','mic-mute':'Mic Mute',screenshot:'Snip Tools','lock-screen':'Lock Screen','favorite-1':'Favorite 1','favorite-2':'Favorite 2','favorite-3':'Favorite 3',calculator:'Calculator',copy:'Copy',cut:'Cut',paste:'Paste',undo:'Undo',redo:'Redo',save:'Save'}},
    clockFace:{digitalLbl:'Digital',digitalSub:'Large digits',analogLbl:'Analog',analogSub:'Tick dial with hands'},
    timeFormat:{h24Lbl:'24-hour',h24Sub:'e.g. 14:30',h12Lbl:'12-hour',h12Sub:'e.g. 2:30 PM'},
    notifFilter:{disabledLbl:'Disabled',disabledSub:'No notifications shown on Ori',callOnlyLbl:'Call Only',
      callOnlySub:'Incoming calls only',importantLbl:'Important',importantSub:'Calls and high-priority alerts',
      allLbl:'All',allSub:'Every notification (default)'},
    discardModal:{title:'Discard changes?',body:'Unsaved changes will be lost.',keepEditing:'Keep editing',discard:'Discard'},
    resetModal:{title:'Reset',
      body:'This will erase your profile, settings, and factory reset Ori.',
      reset:'Reset'},
    unpairPhoneModal:{title:'Unpair iPhone',
      body:'Ori will no longer show notifications from {name}.',
      fallbackName:'The paired iPhone',unpair:'Unpair'},
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
    passkey:{title:'Nhập mã ghép nối',bodyPre:'Nhập mã 6 số hiển thị trên ',bodySuf:'.',cancel:'Hủy',pair:'Ghép nối'},
    connecting:{title:'Đang ghép nối với Ori…',sub:'Đang chờ Ori xác nhận…'},
    syncing:{title:'Đang thiết lập Ori…',progressLabel:'Một ngày bận rộn đang chờ…',doneLabel:'Ori đã thiết lập xong!'},
    pairfail:{title:'Không thể ghép nối với Ori',body:'Mã ghép nối không khớp, hoặc yêu cầu đã hết thời gian chờ trên Ori. Hãy chọn một thiết bị để thử lại.',close:'Đóng'},
    oriInfoModal:{fwLbl:'Firmware',addrLbl:'Địa chỉ',snLbl:'Số sê-ri',mfgLbl:'Ngày sản xuất',sigLbl:'Tín hiệu',phoneLbl:'iPhone',syncLbl:'Đồng bộ',
      notSetup:'Chưa thiết lập',justNow:'Vừa xong',minAgo:'{n} phút trước',unknown:'Chưa rõ'},
    iphoneInfoModal:{missedLbl:'Cuộc gọi nhỡ',unreadLbl:'Tin nhắn chưa đọc',sigLbl:'Tín hiệu',notifLbl:'Thông báo'},
    ancsList:{titles:{missed:'Cuộc gọi nhỡ',unread:'Tin nhắn chưa đọc',other:'Thông báo'},
      empty:{missed:'Không có cuộc gọi nhỡ',unread:'Không có tin nhắn chưa đọc',other:'Không có thông báo'},
      silentBadge:'Im lặng',silentTitle:'Gửi ở chế độ im lặng',readAll:'Đọc tất cả',
      dismissFallback:'Bỏ qua',closeFallback:'Đóng',messagesCount:'{n} tin nhắn',
      justNow:'Vừa xong',minAgo:'{n} phút trước',hourAgo:'{n} giờ trước',dayAgo:'{n} ngày trước'},
    incomingCall:{incoming:'Cuộc gọi đến',onCall:'Đang gọi',endCall:'Kết thúc cuộc gọi',
      answerFallback:'Trả lời',declineFallback:'Từ chối',unknownCaller:'Không rõ số',
      dismissRingingTitle:'Bỏ qua — cuộc gọi vẫn đổ chuông',hideActiveTitle:'Ẩn — cuộc gọi vẫn tiếp tục'},
    settings:{title:'Cài đặt',general:'Chung',auto:'Tự động chạy',autoSub:'Khởi động cùng Windows',
      calendar:'Nguồn lịch',language:'Ngôn ngữ',about:'Giới thiệu',app:'Phiên bản phần mềm',reset:'Đặt lại'},
    main:{connected:'Đã kết nối',connecting:'Đang kết nối…',syncing:'Đang đồng bộ…',disconnected:'Đã ngắt kết nối',timeOff:'Nghỉ phép',
      noTimeOffPlanned:'Chưa có lịch nghỉ phép',tapToSet:'Nhấn để đặt',notifFilterRow:'Bộ lọc thông báo',
      clockFaceRow:'Mặt đồng hồ',timeFormatRow:'Định dạng giờ',quickActionsRow:'Thao tác nhanh',presenceAvailable:'Đang hoạt động',
      settingsIcoTitle:'Cài đặt',minimizeIcoTitle:'Thu nhỏ',fwUpToDate:'Firmware đã mới nhất',fwAvailable:'Có bản cập nhật firmware',
      phoneConnectedTitle:'{name} — Đã kết nối',phoneDisconnectedTitle:'iPhone đã ngắt kết nối',phoneUnknownName:'iPhone',
      reconnectTitle:'Kết nối lại'},
    profileEditor:{title:'Hồ sơ'},
    timeOffEditor:{periodLbl:'Khoảng thời gian',selectDates:'Chọn ngày…',selectStartDate:'Chọn ngày bắt đầu',
      selectEndDate:'Bây giờ chọn ngày kết thúc',destinationLbl:'Điểm đến',destinationPh:'Thành phố, Quốc gia',
      destinationPhotoLbl:'Ảnh điểm đến'},
    calendarSource:{teamsStatus:'Đã đăng nhập · Exchange Online',notSignedIn:'Chưa đăng nhập',signingIn:'Đang đăng nhập…',
      notConnected:'Chưa kết nối',signInGoogle:'Đăng nhập bằng Google',signOutGoogle:'Đăng xuất khỏi Google',
      signInMicrosoft:'Đăng nhập bằng Microsoft',signOutMicrosoft:'Đăng xuất khỏi Microsoft'},
    quickActions:{slotPrefix:'Khe',notSet:'Chưa đặt',clickToSet:'Nhấn để đặt',clickToChange:'Nhấn để đổi',
      pressShortcut:'Nhấn tổ hợp phím…',escToCancel:'Nhấn Esc để hủy',
      actionLabels:{'vol-mute':'Tắt âm lượng','mic-mute':'Tắt mic',screenshot:'Công cụ cắt','lock-screen':'Khóa màn hình','favorite-1':'Yêu thích 1','favorite-2':'Yêu thích 2','favorite-3':'Yêu thích 3',calculator:'Máy tính',copy:'Sao chép',cut:'Cắt',paste:'Dán',undo:'Hoàn tác',redo:'Làm lại',save:'Lưu'}},
    clockFace:{digitalLbl:'Số',digitalSub:'Chữ số lớn',analogLbl:'Kim',analogSub:'Mặt số kim chỉ giờ'},
    timeFormat:{h24Lbl:'24 giờ',h24Sub:'VD: 14:30',h12Lbl:'12 giờ',h12Sub:'VD: 2:30 CH'},
    notifFilter:{disabledLbl:'Tắt',disabledSub:'Không hiển thị thông báo trên Ori',callOnlyLbl:'Chỉ cuộc gọi',
      callOnlySub:'Chỉ cuộc gọi đến',importantLbl:'Quan trọng',importantSub:'Cuộc gọi và thông báo quan trọng',
      allLbl:'Tất cả',allSub:'Mọi thông báo (mặc định)'},
    discardModal:{title:'Hủy các thay đổi?',body:'Các thay đổi chưa lưu sẽ bị mất.',keepEditing:'Tiếp tục chỉnh sửa',discard:'Hủy bỏ'},
    resetModal:{title:'Đặt lại',
      body:'Việc này sẽ xóa hồ sơ, cài đặt của bạn, và đặt lại Ori về mặc định gốc.',
      reset:'Đặt lại'},
    unpairPhoneModal:{title:'Hủy ghép nối iPhone',
      body:'Ori sẽ không còn hiển thị thông báo từ {name} nữa.',
      fallbackName:'iPhone đã ghép nối',unpair:'Hủy ghép nối'},
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
    passkey:{title:'Introduce el código',bodyPre:'Introduce el código de 6 dígitos que aparece en ',bodySuf:'.',cancel:'Cancelar',pair:'Emparejar'},
    connecting:{title:'Emparejando con Ori…',sub:'Esperando confirmación de Ori…'},
    syncing:{title:'Configurando Ori…',progressLabel:'Un día ocupado por delante…',doneLabel:'¡Ori está listo!'},
    pairfail:{title:'No se pudo emparejar con Ori',body:'El código no coincidió, o la solicitud caducó en Ori. Elige un dispositivo para intentarlo de nuevo.',close:'Cerrar'},
    oriInfoModal:{fwLbl:'Firmware',addrLbl:'Dirección',snLbl:'Número de serie',mfgLbl:'Fabricado',sigLbl:'Señal',phoneLbl:'iPhone',syncLbl:'Sincronizado',
      notSetup:'Sin configurar',justNow:'Ahora mismo',minAgo:'Hace {n} min',unknown:'Desconocido'},
    iphoneInfoModal:{missedLbl:'Llamadas perdidas',unreadLbl:'Mensajes sin leer',sigLbl:'Señal',notifLbl:'Notificaciones'},
    ancsList:{titles:{missed:'Llamadas perdidas',unread:'Mensajes sin leer',other:'Notificaciones'},
      empty:{missed:'Sin llamadas perdidas',unread:'Sin mensajes sin leer',other:'Sin notificaciones'},
      silentBadge:'Silencio',silentTitle:'Entregada en silencio',readAll:'Leer todo',
      dismissFallback:'Descartar',closeFallback:'Cerrar',messagesCount:'{n} mensajes',
      justNow:'Ahora mismo',minAgo:'Hace {n} min',hourAgo:'Hace {n} h',dayAgo:'Hace {n} d'},
    incomingCall:{incoming:'Llamada entrante',onCall:'En llamada',endCall:'Finalizar llamada',
      answerFallback:'Responder',declineFallback:'Rechazar',unknownCaller:'Número desconocido',
      dismissRingingTitle:'Descartar — la llamada sigue sonando',hideActiveTitle:'Ocultar — la llamada sigue en curso'},
    settings:{title:'Configuración',general:'General',auto:'Iniciar automáticamente',autoSub:'Abrir al iniciar Windows',
      calendar:'Fuente de calendario',language:'Idioma',about:'Acerca de',app:'Versión del software',reset:'Restablecer'},
    main:{connected:'Conectado',connecting:'Conectando…',syncing:'Sincronizando…',disconnected:'Desconectado',timeOff:'Tiempo libre',
      noTimeOffPlanned:'Sin tiempo libre planeado',tapToSet:'Toca para configurar',notifFilterRow:'Filtro de notificaciones',
      clockFaceRow:'Esfera del reloj',timeFormatRow:'Formato de hora',quickActionsRow:'Acciones rápidas',presenceAvailable:'Disponible',
      settingsIcoTitle:'Configuración',minimizeIcoTitle:'Minimizar',fwUpToDate:'Firmware actualizado',fwAvailable:'Actualización de firmware disponible',
      phoneConnectedTitle:'{name} — Conectado',phoneDisconnectedTitle:'iPhone desconectado',phoneUnknownName:'iPhone',
      reconnectTitle:'Reconectar'},
    profileEditor:{title:'Perfil'},
    timeOffEditor:{periodLbl:'Periodo',selectDates:'Selecciona fechas…',selectStartDate:'Selecciona la fecha de inicio',
      selectEndDate:'Ahora selecciona la fecha de fin',destinationLbl:'Destino',destinationPh:'Ciudad, país',
      destinationPhotoLbl:'Foto del destino'},
    calendarSource:{teamsStatus:'Sesión iniciada · Exchange Online',notSignedIn:'No has iniciado sesión',signingIn:'Iniciando sesión…',
      notConnected:'No conectado',signInGoogle:'Iniciar sesión con Google',signOutGoogle:'Cerrar sesión de Google',
      signInMicrosoft:'Iniciar sesión con Microsoft',signOutMicrosoft:'Cerrar sesión de Microsoft'},
    quickActions:{slotPrefix:'Ranura',notSet:'Sin definir',clickToSet:'Haz clic para definir',clickToChange:'Haz clic para cambiar',
      pressShortcut:'Presiona el atajo…',escToCancel:'Esc para cancelar',
      actionLabels:{'vol-mute':'Silenciar volumen','mic-mute':'Silenciar micrófono',screenshot:'Herramienta de recorte','lock-screen':'Bloquear pantalla','favorite-1':'Favorito 1','favorite-2':'Favorito 2','favorite-3':'Favorito 3',calculator:'Calculadora',copy:'Copiar',cut:'Cortar',paste:'Pegar',undo:'Deshacer',redo:'Rehacer',save:'Guardar'}},
    clockFace:{digitalLbl:'Digital',digitalSub:'Dígitos grandes',analogLbl:'Analógico',analogSub:'Esfera con agujas'},
    timeFormat:{h24Lbl:'24 horas',h24Sub:'p. ej. 14:30',h12Lbl:'12 horas',h12Sub:'p. ej. 2:30 p.m.'},
    notifFilter:{disabledLbl:'Desactivado',disabledSub:'No se muestran notificaciones en Ori',callOnlyLbl:'Solo llamadas',
      callOnlySub:'Solo llamadas entrantes',importantLbl:'Importante',importantSub:'Llamadas y alertas de alta prioridad',
      allLbl:'Todas',allSub:'Todas las notificaciones (predeterminado)'},
    discardModal:{title:'¿Descartar los cambios?',body:'Los cambios no guardados se perderán.',keepEditing:'Seguir editando',discard:'Descartar'},
    resetModal:{title:'Restablecer',
      body:'Esto eliminará tu perfil, tus ajustes, y restablecerá Ori a los valores de fábrica.',
      reset:'Restablecer'},
    unpairPhoneModal:{title:'Desvincular iPhone',
      body:'Ori dejará de mostrar notificaciones de {name}.',
      fallbackName:'El iPhone vinculado',unpair:'Desvincular'},
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
    passkey:{title:'Saisir le code',bodyPre:'Saisissez le code à 6 chiffres affiché sur ',bodySuf:'.',cancel:'Annuler',pair:'Appairer'},
    connecting:{title:'Appairage avec Ori…',sub:"En attente de confirmation d'Ori…"},
    syncing:{title:"Configuration d'Ori…",progressLabel:'Une journée bien remplie vous attend…',doneLabel:'Ori est configuré !'},
    pairfail:{title:"Impossible d'appairer avec Ori",body:'Le code ne correspondait pas, ou la demande a expiré sur Ori. Choisissez un appareil pour réessayer.',close:'Fermer'},
    oriInfoModal:{fwLbl:'Firmware',addrLbl:'Adresse',snLbl:'Numéro de série',mfgLbl:'Fabriqué',sigLbl:'Signal',phoneLbl:'iPhone',syncLbl:'Synchronisé',
      notSetup:'Non configuré',justNow:"À l'instant",minAgo:'Il y a {n} min',unknown:'Inconnu'},
    iphoneInfoModal:{missedLbl:'Appels manqués',unreadLbl:'Messages non lus',sigLbl:'Signal',notifLbl:'Notifications'},
    ancsList:{titles:{missed:'Appels manqués',unread:'Messages non lus',other:'Notifications'},
      empty:{missed:'Aucun appel manqué',unread:'Aucun message non lu',other:'Aucune notification'},
      silentBadge:'Silencieux',silentTitle:'Livrée en mode silencieux',readAll:'Tout lire',
      dismissFallback:'Ignorer',closeFallback:'Fermer',messagesCount:'{n} messages',
      justNow:"À l'instant",minAgo:'Il y a {n} min',hourAgo:'Il y a {n} h',dayAgo:'Il y a {n} j'},
    incomingCall:{incoming:'Appel entrant',onCall:'En appel',endCall:"Terminer l'appel",
      answerFallback:'Répondre',declineFallback:'Refuser',unknownCaller:'Numéro inconnu',
      dismissRingingTitle:"Ignorer — l'appel continue de sonner",hideActiveTitle:"Masquer — l'appel continue"},
    settings:{title:'Paramètres',general:'Général',auto:'Démarrage automatique',autoSub:'Lancer au démarrage de Windows',
      calendar:'Source de calendrier',language:'Langue',about:'À propos',app:'Version du logiciel',reset:'Réinitialiser'},
    main:{connected:'Connecté',connecting:'Connexion…',syncing:'Synchronisation…',disconnected:'Déconnecté',timeOff:'Congé',
      noTimeOffPlanned:'Aucun congé prévu',tapToSet:'Toucher pour définir',notifFilterRow:'Filtre de notifications',
      clockFaceRow:"Cadran de l'horloge",timeFormatRow:"Format de l'heure",quickActionsRow:'Actions rapides',presenceAvailable:'Disponible',
      settingsIcoTitle:'Paramètres',minimizeIcoTitle:'Réduire',fwUpToDate:'Firmware à jour',fwAvailable:'Mise à jour du firmware disponible',
      phoneConnectedTitle:'{name} — Connecté',phoneDisconnectedTitle:'iPhone déconnecté',phoneUnknownName:'iPhone',
      reconnectTitle:'Reconnecter'},
    profileEditor:{title:'Profil'},
    timeOffEditor:{periodLbl:'Période',selectDates:'Sélectionner les dates…',selectStartDate:'Sélectionner la date de début',
      selectEndDate:'Sélectionnez maintenant la date de fin',destinationLbl:'Destination',destinationPh:'Ville, pays',
      destinationPhotoLbl:'Photo de la destination'},
    calendarSource:{teamsStatus:'Connecté · Exchange Online',notSignedIn:'Non connecté',signingIn:'Connexion en cours…',
      notConnected:'Non connecté',signInGoogle:'Se connecter avec Google',signOutGoogle:'Se déconnecter de Google',
      signInMicrosoft:'Se connecter avec Microsoft',signOutMicrosoft:'Se déconnecter de Microsoft'},
    quickActions:{slotPrefix:'Emplacement',notSet:'Non défini',clickToSet:'Cliquer pour définir',clickToChange:'Cliquer pour modifier',
      pressShortcut:'Appuyez sur le raccourci…',escToCancel:'Échap pour annuler',
      actionLabels:{'vol-mute':'Muet volume','mic-mute':'Muet micro',screenshot:'Outil de capture','lock-screen':"Verrouiller l'écran",'favorite-1':'Favori 1','favorite-2':'Favori 2','favorite-3':'Favori 3',calculator:'Calculatrice',copy:'Copier',cut:'Couper',paste:'Coller',undo:'Annuler',redo:'Rétablir',save:'Enregistrer'}},
    clockFace:{digitalLbl:'Numérique',digitalSub:'Grands chiffres',analogLbl:'Analogique',analogSub:'Cadran à aiguilles'},
    timeFormat:{h24Lbl:'24 heures',h24Sub:'ex. 14:30',h12Lbl:'12 heures',h12Sub:'ex. 2:30 PM'},
    notifFilter:{disabledLbl:'Désactivé',disabledSub:'Aucune notification affichée sur Ori',callOnlyLbl:'Appels uniquement',
      callOnlySub:'Appels entrants uniquement',importantLbl:'Important',importantSub:'Appels et alertes prioritaires',
      allLbl:'Toutes',allSub:'Toutes les notifications (par défaut)'},
    discardModal:{title:'Abandonner les modifications ?',body:'Les modifications non enregistrées seront perdues.',keepEditing:'Continuer la modification',discard:'Abandonner'},
    resetModal:{title:"Réinitialiser",
      body:"Cela supprimera votre profil, vos réglages, et réinitialisera Ori aux paramètres d'usine.",
      reset:'Réinitialiser'},
    unpairPhoneModal:{title:"Dissocier l'iPhone",
      body:"Ori n'affichera plus les notifications de {name}.",
      fallbackName:"L'iPhone associé",unpair:'Dissocier'},
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
  // Fire-and-forget — nothing here needs to block on the write landing, and
  // a failure just means the next launch falls back to the last-persisted
  // language (or 'en') rather than this one, same "not a big deal" severity
  // as every other local-only preference in store::SavedState.
  invoke('set_language',{code}).catch(e=>console.error('set_language failed:',e));
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
  $('suPkPairBtn').textContent=t.passkey.pair;
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
  $('reconnectIco').title=t.main.reconnectTitle;
  $('fwIco').title=fwAvail?t.main.fwAvailable:t.main.fwUpToDate;
  // reconnectBusy (scanning, not yet found) overrides connState's own text
  // the same way setReconnectBusy does — otherwise switching language
  // mid-attempt would revert "Connecting…" back to "Disconnected" here.
  $('hState').textContent=reconnectBusy?t.main.connecting:
    {on:t.main.connected,connecting:t.main.connecting,rec:t.main.syncing,off:t.main.disconnected}[connState];
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
  $('unpairPhoneTitle').textContent=t.unpairPhoneModal.title;
  $('unpairPhoneBtn').textContent=t.unpairPhoneModal.unpair;
  $('unpairPhoneCancelBtn').textContent=t.common.cancel;
  $('ancsListBackBtn').textContent=t.common.back;
  // ANCS list/detail and the incoming-call view are built entirely at
  // open-time (openAncsListModal/openAncsDetail/showIncomingCall/
  // showActiveCall all read I18N[appLang] fresh on every render) — nothing
  // else here is static markup that needs a language-switch refresh.
  // Re-derive rather than translate the last-set string in place: the icon's
  // tooltip embeds the phone's name and connected/disconnected wording, so a
  // language switch needs the same status→string logic setPhoneBondStatus
  // already has, not a copy of it. No-op while nothing has ever been bonded.
  if(lastPhoneBondStatus.b) setPhoneBondStatus(lastPhoneBondStatus);
  // Ori Info modal's row labels (fw/address/serial/manufactured/phone/synced)
  // are set imperatively in refreshOriInfoModal(), not static markup, so a
  // language switch needs the same re-render while it's open.
  if($('m-ori-info').classList.contains('show')) refreshOriInfoModal();
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
  // Entering first-run setup (fresh boot, factory reset, or a needs-repair
  // route-back — see the listeners at the bottom of this file) always takes
  // over the whole panel, so unwind whatever subscreen(s) are currently open
  // first — Settings -> Calendar Source can nest two deep — rather than
  // leaving them buried underneath s-setup in the stack. This used to be
  // handled implicitly (and only one stack level deep) by
  // dismissTransientScreen()'s old unconditional back() fallthrough; now
  // that it's scoped to only the connection-gated screens (see its own
  // comment), this is s-setup's own cleanup to do.
  while(stack.length) back();
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
  // Phase 1 (Enter Passkey) — let the user start typing the code shown on
  // Ori's screen immediately, without having to click into the first box.
  if(n===1) $('suPk0').focus();
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
// Built via DOM APIs, not innerHTML — d.name comes straight off an
// unauthenticated BLE advertisement (central.rs only checks it starts with
// "Ori-"; anything can broadcast that). Concatenating it into an HTML string
// (previously also embedded inside an inline onclick="...") let a
// maliciously-named nearby device execute script in this webview — which
// has withGlobalTauri + no CSP, i.e. direct invoke() access to every Tauri
// command. textContent + addEventListener never parse the name as markup.
function suRenderDevices(devices){
  const t=I18N[appLang].discover;
  const list=$('suDevList');
  list.innerHTML='';
  devices.forEach(d=>{
    const row=document.createElement('div');
    row.className='su-dev';
    row.addEventListener('click',()=>suSelectDevice(d.name));

    const ico=document.createElement('div');
    ico.className='su-dev-ico';
    const wm=document.createElement('div');
    wm.className='wm';wm.style.fontSize='12px';
    ['o','r','i'].forEach(ch=>{
      const span=document.createElement('span');
      span.className=ch;span.textContent=ch;
      wm.appendChild(span);
    });
    ico.appendChild(wm);

    const info=document.createElement('div');
    info.className='su-dev-info';
    const nameEl=document.createElement('div');
    nameEl.className='su-dev-name';nameEl.textContent=d.name;
    const sigEl=document.createElement('div');
    sigEl.className='su-dev-sig';sigEl.textContent=d.strong?t.strongSignal:t.weakSignal;
    info.appendChild(nameEl);info.appendChild(sigEl);

    const chev=document.createElement('div');
    chev.className='chev';chev.textContent='›';

    row.appendChild(ico);row.appendChild(info);row.appendChild(chev);
    list.appendChild(row);
  });
  const rescan=document.createElement('div');
  rescan.className='su-rescan';
  rescan.textContent='⟳ '+t.rescan;
  rescan.addEventListener('click',suStartScan);
  list.appendChild(rescan);
  $('suScanning').style.display='none';$('suDevList').style.display='';
}
function suSelectDevice(name){
  suSelectedDevice=name;$('suPasskeyDevName').textContent=name;
  suResetPasskeyInputs();
  suShowPairPhase(1);
  // Connecting and starting the WinRT pairing ceremony here — not on
  // submit — is what actually makes Ori generate and show its 6-digit
  // code; the user needs something to read before they can type it.
  invoke('ble_start_pairing',{name}).catch(()=>suShowPairFail());
}
// Orion is the entry side — Ori displays the 6-digit code on its own screen,
// the user types it here. On Windows this drives WinRT's
// DeviceInformationCustomPairing (PairingRequested → ProvidePin), which lets
// an app own the passkey UI instead of the system flyout. macOS's
// CoreBluetooth has no equivalent hook (hard platform wall, not yet solved
// for macOS — that build hasn't started) — deferred until that work begins.
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
  // Enter submits from any digit box — suSubmitPasskey() already no-ops if
  // fewer than 6 digits have been entered, so this is safe even mid-typing.
  if(e.key==='Enter') suSubmitPasskey();
}
function suPkPaste(e){
  const text=e.clipboardData.getData('text').replace(/[^0-9]/g,'').slice(0,6);
  if(!text.length) return;
  e.preventDefault();
  [...text].forEach((ch,i)=>{if($('suPk'+i)) $('suPk'+i).value=ch;});
  $('suPk'+Math.min(text.length,5)).focus();
}
function suSubmitPasskey(){
  const entered=[0,1,2,3,4,5].map(i=>$('suPk'+i).value).join('');
  if(entered.length<6) return;
  suShowPairPhase(2);
  invoke('ble_submit_passkey',{passkey:entered,profile:{
    name:$('suNmInp').value,title:$('suTlInp').value,email:$('suEmInp').value,phone:$('suPhInp').value,
    photoDataUrl:suPhotoUrl
  }}).catch(()=>suShowPairFail());
}
function suUpdateSyncProgress({pct,label,done}){
  suShowPairPhase(3);
  const ring=$('suRing'),pctEl=$('suPct'),lbl=$('suLbl');
  ring.style.strokeDashoffset=358-(358*Math.min(pct,100)/100);
  pctEl.textContent=done?'✓':Math.round(pct)+'%';
  lbl.textContent=label||I18N[appLang].syncing.progressLabel;
  if(done) setTimeout(suFinishSetup,1200);
}
// Backing out of the passkey screen before submitting a code — tells the
// backend to drop the in-flight WinRT pairing ceremony (ble::cancel_pairing)
// instead of just hiding the modal, which used to leave a blocking-pool
// thread parked and a polling task running until Ori's own ~30s pairing
// timeout cleaned it up on its own.
function suCancelPairing(){
  hideModal('m-pair');
  invoke('ble_cancel_pairing');
}
function suShowPairFail(){openModalFrom('m-pairfail');}
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
  // Pop s-setup off the stack ourselves — dismissTransientScreen() (run
  // inside setConn() above) no longer does this generically, since its
  // fallthrough is now scoped to only the connection-gated screens (see its
  // comment). openSetupWizard() guarantees s-setup is the sole stack entry
  // at this point (nothing else is pushed during the wizard flow — every
  // step/phase change within it uses suShowStep()/suShowPairPhase(), which
  // toggle sub-panels in place rather than touching `stack`), so this is
  // always safe, but the top-of-stack check is a defensive no-op guard in
  // case that invariant ever changes.
  if(stack[stack.length-1]==='s-setup') back();
  $('hName').textContent=suSelectedDevice||'Ori-XT-9F';
  $('suRing').style.strokeDashoffset=358;$('suPct').textContent='0%';
}

// ── Run at login (Settings > General) ───────────────────────────────────────
// Purely a local OS-level setting (tauri-plugin-autostart, pc-app.md) — no
// BLE/Ori dependency, so unlike the connection-gated settings (clock face /
// ANCS filter / shortcuts, hydrated only on a live connect via
// readSlotsFromDevice) this is fetched once, eagerly, at app bootstrap,
// alongside get_initial_state below — Settings itself isn't connection-gated
// either (pc-app.md), so its toggle shouldn't have to wait for one.
let autostartEnabled=false;
function setAutostartToggle(enabled){
  autostartEnabled=enabled;
  $('autoToggle').classList.toggle('on',enabled);
}
// Optimistic flip + revert-on-failure, same pattern as the Save handlers
// above (e.g. saveProfile's invoke(...).catch(...)) — the toggle isn't a
// staged Save/Discard subscreen, so there's no pending state to hold; on
// rejection we just revert the class and log, mirroring how those handlers
// treat a backend failure as "surface it, don't block the optimistic UI".
async function toggleAutostart(){
  const next=!autostartEnabled;
  setAutostartToggle(next);
  try{
    await invoke('set_autostart_enabled',{enabled:next});
  }catch(e){
    console.error('set_autostart_enabled failed:',e);
    setAutostartToggle(!next);
  }
}

// ── App bootstrap — real backend wiring (replaces the prototype's fake
// setTimeout-driven state machine with Tauri invoke/listen) ─────────────────
initCal(); // after I18N (renderCal reads it for locale-aware month/weekday names)
applyI18n();

listen('conn-state',e=>setConn(e.payload));
listen('reconnect-attempt',e=>setReconnectBusy(e.payload));
listen('scan-result',e=>suRenderDevices(e.payload));
listen('sync-progress',e=>suUpdateSyncProgress(e.payload));
listen('pairing-failed',()=>suShowPairFail());
// Backend detected Ori's bond is gone — either it advertised SETUP (a
// factory reset, ble-protocol.md §7.1) or repeated reconnect attempts kept
// failing past discovery (the encryption-failure fallback, same section).
// Either way Orion has already cleared its own local pairing record; route
// straight back to the setup wizard the same way doClearAll() does.
listen('needs-repair',()=>{setConn('off');openSetupWizard();});
listen('phone-bond-status',e=>{
  setPhoneBondStatus(e.payload);
  // Ori pushes a fresh notify on every ANCS queue change (missed/unread/
  // notification counts), not just on connect/disconnect — if the iPhone
  // Info modal is open when one arrives, refresh it in place so the badges
  // update live instead of only reflecting the snapshot from when it opened.
  if($('m-iphone-info').classList.contains('show')) openIphoneInfoModal();
});
listen('fw-update-available',e=>{fwAvail=true;orionFwVersion=e.payload.version;
  const ico=$('fwIco');ico.style.display='';ico.classList.add('fw-on');ico.title=I18N[appLang].main.fwAvailable;});
listen('fw-progress',e=>fwApplyProgress(e.payload));
// ANCS relay (chars 0010/0011, ble-protocol.md §13) — feeds ANCS_STORE and
// the incoming-call takeover. See the ANCS drill-down / incoming-call
// sections above for the store, rendering, and action-write functions this
// drives.
listen('ancs-notification',e=>{
  const n=e.payload;
  dlog('[ORION-DEBUG] ancs-notification event: '+JSON.stringify(n));
  if(n.o==='add'){
    const bucket=ancsUpsert(n);
    dlog('[ORION-DEBUG]   add -> bucket='+bucket+', store now: missed='+ANCS_STORE.missed.size+' unread='+ANCS_STORE.unread.size+' other='+ANCS_STORE.other.size);
    ancsRefreshOpenList(bucket);
    ancsRefreshIphoneInfoIfOpen();
    return;
  }
  if(n.o==='remove'){
    const bucket=ancsRemove(n.u);
    dlog('[ORION-DEBUG]   remove u='+n.u+' -> bucket='+bucket);
    if(!bucket) return; // never relayed to Orion in the first place — harmless no-op, same as on Ori itself
    if(!ancsCloseStaleDetail(n.u,bucket)) ancsRefreshOpenList(bucket);
    ancsRefreshIphoneInfoIfOpen();
    return;
  }
  if(n.o==='clear'){
    dlog('[ORION-DEBUG]   clear -> wiping ANCS_STORE');
    // Full clear-and-repopulate (an ANCS filter change, ble-protocol.md
    // §13) — every currently-shown uid in any open detail is gone, and any
    // open list is now empty, regardless of which bucket either was showing.
    const detailBucket=$('ancsDetailCard').dataset.bucket;
    const detailWasOpen=$('m-ancs-detail').classList.contains('show');
    ancsClearAll();
    if(detailWasOpen) ancsBackToList(detailBucket); // closes detail, re-renders its (now-empty) list
    else if($('m-ancs-list').classList.contains('show')) openAncsListModal($('ancsListBody').dataset.bucket);
    ancsRefreshIphoneInfoIfOpen();
  }
});
listen('ancs-call-state',e=>{
  const c=e.payload;
  // Remember the live call so the header chip can reopen it and knows which
  // app icon to draw; cleared on st:0. Applies equally to a call relayed by
  // resync_orion_call_state the instant Orion (re)connects — that's what makes
  // an already-ringing/ongoing call show up immediately (ble-protocol.md §13).
  currentCall=(c.st===1||c.st===2)?c:null;
  if(c.st===1) showIncomingCall(c);
  else if(c.st===2) showActiveCall(c);
  else { callSessionStop(); hideModal('m-incoming-call'); }
  updateCallChip();
});
listen('orion-update-available',e=>{orionUpdateAvail=true;orionUpdateVersion=e.payload.version;updateOrionUpdateRow();});
listen('orion-update-progress',e=>ouApplyProgress(e.payload));

invoke('get_initial_state').then(state=>{
  // Applied directly (not via setAppLang, which also persists) — the value
  // just came FROM disk, so writing it straight back on every single launch
  // would be a pointless disk write. Guarded on I18N[...] existing in case
  // a future build ever drops a locale that an older state.json still names.
  if(state.language&&I18N[state.language]){appLang=state.language;applyI18n();}
  hydrateProfileCard(state.profile);
  hydrateTimeOffCard(state.time_off);
  // setConn() reveals #s-main (removes .pending-init) — see its own
  // comment. The unpaired/error paths below leave #s-main hidden;
  // openSetupWizard()'s #s-setup renders fine as its own overlay
  // regardless of #s-main's visibility.
  if(state.paired) setConn(state.connection);
  else openSetupWizard();
}).catch(()=>openSetupWizard());

// Independent of the pairing bootstrap above — a failure here must never
// route into openSetupWizard() (this has nothing to do with pairing state).
invoke('get_autostart_enabled').then(setAutostartToggle)
  .catch(e=>console.error('get_autostart_enabled failed:',e));
