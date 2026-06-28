/* Ori UI Prototype — behavior script (v9)
   - PTO screen: image 75% height / info bar 25% (label + destination + dates)
   - Meeting rows: title and location single-line with ellipsis; tap row to expand detail overlay
   - Re-pair iPhone hides status bar so its layout matches Step 4 exactly */

// Long mock name exercises the 2-line wrap path now that the name font is
// 30px (matching status-bar time + clock date). See `.profile-name` CSS.
const PROFILE = { name: 'Everstorm Dominion', title: 'Founder, Ori', email: 'everstorm@ori.app', phone: '+1 (415) 555 0192' };
const BLE_NAME = 'Ori-XT-9F';
const PASSKEY = '476 218';
const FW_VERSION = '1.0.1'; // firmware version shown on the post-update ack screen

// Mock ANCS notification data per app. In firmware, these fields come from
// the ANCS Notification Attribute commands: Title, Subtitle, Message, Date,
// and DisplayName (human-readable app name from AppIdentifier lookup).
const ANCS_NOTIFICATIONS = {
  gmail: {
    displayName: 'Gmail',
    title: 'Priya Nair',
    body: 'Hey, have you had a chance to review the firmware PR? The team is waiting on approval before they can merge and kick off the build.',
    time: '2 min ago',
  },
  messenger: {
    displayName: 'Messenger',
    title: 'Marcus Lee',
    body: "Studio is booked for the design review. I'll send the invite now — can you confirm you're free at 10:30? Also, Hannah said she needs at least 20 minutes at the end to walk through the chassis tolerances with the vendor rep, so I've blocked the room until 12:00 just in case. Let me know if you want me to pull in the DFM report beforehand so we're not scrambling for numbers during the meeting.",
    time: '5 min ago',
  },
  instagram: {
    displayName: 'Instagram',
    title: 'New activity',
    body: 'oridevice and 47 others liked your photo.',
    time: '18 min ago',
    silent: true,   // EventFlags SILENT bit — badge shown in the overlay
  },
  facebook: {
    displayName: 'Facebook',
    title: 'Hannah Kim commented',
    body: "Great progress on the display calibration — the green channel fix looks solid. Let's review the pin map again before M3.",
    time: '1 hr ago',
  },
};

function initialsOf(fullName) {
  const parts = fullName.trim().split(/\s+/);
  if (parts.length === 1) return parts[0].slice(0, 2).toUpperCase();
  return (parts[0][0] + parts[parts.length - 1][0]).toUpperCase();
}

function applyProfile() {
  document.getElementById('profile-photo').textContent = initialsOf(PROFILE.name);
  document.getElementById('profile-name').textContent = PROFILE.name;
  document.getElementById('profile-title').textContent = PROFILE.title;
}

// Mock "now" is 14:30. The 14:00-15:00 row is marked inProgress so the
// device highlights the meeting the user is currently supposed to be in.
const TODAY_MEETINGS = [
  { start: '09:30', end: '10:00', title: 'Daily standup', loc: 'Conf Room A', org: 'Priya N.' },
  { start: '10:30', end: '11:30', title: 'Industrial design review', loc: 'Studio', org: 'Marcus L.' },
  { start: '13:00', end: '13:45', title: 'Orion app sync', loc: 'Zoom', org: 'Hannah K.' },
  { start: '14:00', end: '15:00', title: 'Investor update prep', loc: 'Office', org: 'Xander T.', inProgress: true },
  { start: '15:30', end: '16:00', title: 'Quick sync with marketing', loc: 'Studio', org: 'Sam R.' },
  { start: '16:30', end: '17:00', title: 'Q4 budget review', loc: 'Virtual — Microsoft Teams Conference Room B', org: 'Dr. Christopher Vandenbrook' },
];
const OVERLAP_MEETINGS = [
  { start: '10:00', end: '11:00', title: 'Firmware architecture review', loc: 'Conf Room A', org: 'Marcus L.', overlap: true },
  { start: '10:00', end: '10:30', title: 'Quick chat with Priya', loc: 'Coffee bar', org: 'Priya N.', overlap: true },
  { start: '10:30', end: '11:30', title: 'Vendor call — display panels', loc: 'Zoom', org: 'Hannah K.', overlap: true },
];
const LONG_TITLE_MEETINGS = [
  { start: '09:00', end: '10:00', title: 'Q3 roadmap planning across firmware, Orion, mobile, and operations', loc: 'Conf Room A', org: 'Priya N.' },
  { start: '11:00', end: '12:00', title: 'Industrial design review — chassis, materials, and tooling decisions for the pilot run', loc: 'Studio', org: 'Marcus L.' },
  { start: '14:00', end: '15:30', title: 'Investor preview', loc: 'Office', org: 'Xander T.', inProgress: true },
];
const OVERLAP_LONG_MEETINGS = [
  { start: '10:00', end: '11:00', title: 'Strategic planning session for Q4 — deep dive into roadmap and prioritisation', loc: 'Studio', org: 'Marcus L.', overlap: true },
  { start: '10:00', end: '10:45', title: 'Manufacturing partner kickoff with the Shenzhen vendor', loc: 'Zoom', org: 'Hannah K.', overlap: true },
  { start: '11:00', end: '12:00', title: 'Standup', loc: 'Studio', org: 'Priya N.' },
];
const LONG_LIST_MEETINGS = [
  { start: '09:00', end: '09:30', title: 'Daily standup', loc: 'Conf Room A', org: 'Priya N.' },
  { start: '09:30', end: '10:30', title: 'Firmware architecture review with hardware team', loc: 'Conf Room A', org: 'Marcus L.' },
  { start: '10:30', end: '11:00', title: 'Orion app sync', loc: 'Zoom', org: 'Hannah K.' },
  { start: '11:00', end: '12:00', title: 'Industrial design review', loc: 'Studio', org: 'Marcus L.' },
  { start: '13:00', end: '13:45', title: 'Vendor call — display panels and cover glass options', loc: 'Zoom', org: 'Hannah K.' },
  { start: '14:00', end: '15:00', title: 'One-on-one with Priya', loc: 'Coffee bar', org: 'Priya N.', inProgress: true },
  { start: '15:00', end: '15:30', title: 'Investor update prep', loc: 'Office', org: 'Xander T.' },
  { start: '15:30', end: '16:00', title: 'Quick sync with marketing', loc: 'Studio', org: 'Sam R.' },
  { start: '16:00', end: '17:00', title: 'Weekly retro', loc: 'Conf Room B', org: 'Hannah K.' },
];

const SCREENS = {
  'meeting-list': {
    label: 'Primary state', title: 'Meeting list — work hours',
    desc: 'Default left-panel view. Title and location are single-line (ellipsis on overflow). Tap any meeting row to expand — shows full title, location, and time.',
    statusBar: { ancsApps: ['gmail', 'messenger', 'instagram'], phoneConnected: true },
    leftRender: () => meetingListHTML(TODAY_MEETINGS),
  },
  'no-meetings': {
    label: 'Primary state', title: 'No meetings today',
    desc: 'Shown during work hours when the cached meeting list is empty.',
    statusBar: { ancsApps: ['gmail'], phoneConnected: true },
    leftRender: () => '<div class="empty"><svg class="glyph"><use href="#i-cal"/></svg><div class="headline">No meetings today</div><div class="sub">Enjoy the focus time</div></div>',
  },
  'clock': {
    label: 'Primary state', title: 'Digital clock',
    desc: 'Entered by tapping the time in the status bar. Status-bar date/time is hidden since the clock face IS the time. Mode-toggle (calendar icon) returns to the previous mode.',
    statusBar: { ancsApps: ['gmail', 'facebook'], phoneConnected: true, hideDateTime: true },
    mode: 'clock',
    leftRender: () => clockHTML(),
  },
  'calendar': {
    label: 'Primary state', title: 'Calendar (month view)',
    desc: 'Entered by long-pressing the time in the status bar for 1s (1.2s here). View-only month grid with today highlighted; left/right arrows navigate months. Mode-toggle (calendar-return icon) returns to the previous mode. Status-bar date/time is hidden since this view IS a calendar.',
    statusBar: { ancsApps: ['gmail', 'facebook'], phoneConnected: true, hideDateTime: true },
    mode: 'clock',
    leftRender: () => calendarHTML(),
  },
  'pto': {
    label: 'Primary state', title: 'PTO active',
    desc: 'Destination name is single-line (ellipsis on overflow). Tap the card to open the full PTO detail overlay.',
    statusBar: { ancsApps: [], phoneConnected: true },
    leftRender: () => ptoHTML('Lisbon, Portugal', 'May 13 – May 21'),
  },
  'pto-long-dest': {
    label: 'Edge case', title: 'PTO — long destination name',
    desc: 'Destination overflows the card width and is clipped with ellipsis. Tap the card to see the full name.',
    statusBar: { ancsApps: [], phoneConnected: true },
    leftRender: () => ptoHTML('São Paulo, State of São Paulo, Brazil', 'Jun 2 – Jun 14'),
  },
  'pto-detail': {
    label: 'Modal popup', title: 'PTO detail overlay',
    desc: 'Opened by tapping the destination card on the PTO screen. Shows the full destination and date range. Dismissed via the Close button.',
    statusBar: { ancsApps: [], phoneConnected: true },
    leftRender: () => ptoHTML('São Paulo, State of São Paulo, Brazil', 'Jun 2 – Jun 14'),
    modal: () => ptoDetailHTML('São Paulo, State of São Paulo, Brazil', 'Jun 2 – Jun 14'),
  },
  'ancs-notification': {
    label: 'Modal popup', title: 'ANCS notification detail',
    desc: 'Tap any ANCS icon in the status bar to open. Shows app name, notification title, message preview (up to 3 lines), and timestamp — all fields available from the iOS ANCS protocol. "Read" dismisses the notification from the status bar entirely (firmware triggers ANCS PositiveAction, which tells iOS to mark it read — the device then receives an ANCS Removed event and hides the icon). "Close" dismisses the overlay only.',
    statusBar: { ancsApps: ['gmail', 'messenger', 'instagram'], phoneConnected: true },
    leftRender: () => meetingListHTML(TODAY_MEETINGS),
    modal: () => ancsDetailHTML('gmail'),
  },
  'ancs-notification-silent': {
    label: 'Modal popup', title: 'ANCS silent notification detail',
    desc: 'When the notification\'s ANCS EventFlags SILENT bit is set (iOS delivered it without sound/vibration), the overlay shows a small pill badge in the top-left corner. Whether the notification appears at all depends on the Orion "show silent notifications" toggle. This demo uses Instagram — marked silent in the mock data — to show the badge design.',
    statusBar: { ancsApps: ['gmail', 'messenger', 'instagram'], phoneConnected: true },
    leftRender: () => meetingListHTML(TODAY_MEETINGS),
    modal: () => ancsDetailHTML('instagram'),
  },
  'profile-detail': {
    label: 'Modal popup', title: 'Profile detail overlay',
    desc: 'Tap the profile photo (short tap) to open. Shows the full name, job title, email, and phone number from the cached profile. Closed only via the Close button — tapping the scrim does not dismiss.',
    statusBar: { ancsApps: ['gmail', 'messenger'], phoneConnected: true },
    leftRender: () => meetingListHTML(TODAY_MEETINGS),
    modal: () => profileDetailHTML(),
  },
  'kbd-mode': {
    label: 'Primary state', title: 'Media mode (BLE bridge)',
    desc: 'Touch surface acts as a secondary controller for the paired PC — large album art (tap = play/pause, swipe ↔ = prev/next, swipe ↕ = volume with momentary HUD), now-playing title + artist, three user-assignable shortcut buttons (default mock: mute audio, mute mic, screen capture). All commands travel as custom BLE messages to Orion which bridges to OS APIs. Tap the toggle in the status bar to switch back to calendar mode.',
    statusBar: { ancsApps: ['gmail', 'messenger', 'instagram'], phoneConnected: true },
    mode: 'media',
    leftRender: () => mediaModeHTML(),
  },
  'countdown': {
    label: 'Modal popup', title: '5-minute pre-meeting alert',
    desc: 'Dismissed via the Close button only.',
    statusBar: { ancsApps: ['gmail', 'messenger'], phoneConnected: true },
    leftRender: () => meetingListHTML(TODAY_MEETINGS),
    modal: () => countdownHTML('Industrial design review', 'Starts at 10:30 · Studio'),
  },
  'factory-reset': {
    label: 'Modal popup', title: 'Factory reset confirmation',
    desc: 'Triggered by long-pressing the profile photo for 3s (1.2s here).',
    statusBar: { ancsApps: ['gmail', 'messenger'], phoneConnected: true },
    leftRender: () => meetingListHTML(TODAY_MEETINGS),
    modal: () => factoryResetHTML(),
  },
  'meeting-detail': {
    label: 'Modal popup', title: 'Meeting detail overlay',
    desc: 'Tap any meeting row in the list to open this overlay. Shows the full title, location, and time. Tap anywhere on the scrim to dismiss.',
    statusBar: { ancsApps: ['gmail', 'messenger'], phoneConnected: true },
    leftRender: () => meetingListHTML(LONG_TITLE_MEETINGS),
    modal: () => meetingDetailHTML(LONG_TITLE_MEETINGS[0]),
  },
  'setup-welcome': {
    label: 'Setup flow', title: 'Welcome aboard',
    desc: 'All four step dots inactive (before Step 1). Dots fixed at y≈440.',
    hideStatusBar: true,
    setup: () => setupWelcomeHTML(),
  },
  'setup-install': {
    label: 'Setup flow', title: 'Step 1 — Install Orion',
    desc: 'Visit ori.app/orion on a PC. Next button advances.',
    hideStatusBar: true,
    setup: () => setupInstallHTML(),
  },
  'setup-link-orion': {
    label: 'Setup flow', title: 'Step 2 — Link Orion',
    desc: 'Waiting for Orion to connect. Shows the BLE device name so the user picks the right Ori.',
    hideStatusBar: true,
    setup: () => setupLinkOrionHTML(),
  },
  'setup-passkey': {
    label: 'Setup flow', title: 'Step 2 — Passkey',
    desc: '6-digit passkey modal for secure BLE bonding — overlaid on the Link Orion base screen.',
    hideStatusBar: true,
    setup: () => setupLinkOrionHTML(),
    modal: () => passkeyHTML(PASSKEY),
  },
  'setup-orioning': {
    label: 'Setup flow', title: 'Step 2 — Orioning',
    desc: 'First sync from Orion (profile, calendar, PTO, time) — overlaid on the Link Orion base screen.',
    hideStatusBar: true,
    setup: () => setupLinkOrionHTML(),
    modal: () => orioningModalHTML(67),
  },
  'setup-phone': {
    label: 'Setup flow', title: 'Step 3 — Pair iPhone ',
    desc: 'Optional. Skip; can re-pair later by long-press on phone-disconnect icon.',
    hideStatusBar: true,
    setup: () => setupPhoneHTML({ allowSkip: true }),
  },
  'setup-done': {
    label: 'Setup flow', title: 'Setup complete',
    desc: 'Brief acknowledgement before transitioning to normal state.',
    hideStatusBar: true,
    setup: () => setupDoneHTML(),
  },
  'reconnect-syncing': {
    label: 'Runtime', title: 'Reconnect-Syncing overlay',
    desc: 'Triggered when Ori reconnects to Orion after being offline. Overlays the left panel only — status bar and profile card remain visible. Auto-dismisses when RUNTIME_READY arrives (~300 ms when nothing changed; longer when calendar data changed). Touch on the overlay is inert.',
    statusBar: { ancsApps: ['gmail', 'messenger'], phoneConnected: true },
    leftRender: () => reconnectSyncingHTML(),
  },
  'ota-updating': {
    label: 'OTA update', title: '1 · Downloading firmware',
    desc: 'Full-screen takeover while Orion streams the firmware over USB CDC into a PSRAM staging buffer. No flash is written yet, so the LCD keeps refreshing and the progress ring is LIVE (0→100%). Status bar, profile card, and left panel hidden; touch inert. Sweeps clockwise; % centred in the ring.',
    hideStatusBar: true,
    setup: () => otaDownloadingHTML(62),
  },
  'ota-ready': {
    label: 'OTA update', title: '2 · Ready to install',
    desc: 'Shown once the image has fully downloaded into PSRAM and its SHA-256 verifies. Explains that the screen goes dark during install. The primary "Update now" button starts the PSRAM→flash copy — this is the user-driven gate before any flash is written. (A download-phase failure instead resumes runtime with the screen still live — no reboot.)',
    hideStatusBar: true,
    setup: () => otaReadyHTML(),
  },
  'ota-installing': {
    label: 'OTA update', title: '3 · Installing (screen dark)',
    desc: 'After tapping "Update now": the firmware halts the LCD and copies the staged image from PSRAM to the inactive flash slot. The panel is physically DARK here (the RGB panel\'s PSRAM-framebuffer DMA cannot run while flash is written — shared MSPI bus). Lasts a few seconds; install→reboot is atomic, so it goes straight to the post-reboot "Updated" ack (no separate "Update complete / Restart" step).',
    hideStatusBar: true,
    setup: () => otaInstallingHTML(),
  },
  'ota-updated-ack': {
    label: 'OTA update', title: '4 · Updated (boot ack)',
    desc: 'The FIRST screen shown after the post-update reboot: confirms the update succeeded and shows the new firmware version, with a "Close" button. The firmware persists an "update acknowledged" flag in NVS — so if the user restarts again without tapping Close, this screen reappears on every boot until acknowledged.',
    hideStatusBar: true,
    setup: () => otaUpdatedAckHTML(),
  },
  'ota-error': {
    label: 'OTA update', title: '⚠ Update failed',
    desc: 'Shown when the OTA cannot complete — corrupted download (hash/truncated), lost connection (timeout), no room (too large / out of memory), or a flash write error. The firmware maps each failure code to one plain-language reason. Close dismisses back to runtime (download-phase failures keep the display alive; a commit-phase failure reboots first). Edit otaErrorHTML(msg) to preview other messages.',
    hideStatusBar: true,
    setup: () => otaErrorHTML(),
  },
  'repair-phone': {
    label: 'Runtime', title: 'Re-pair iPhone',
    desc: 'Reached by long-pressing the phone-disconnect icon when no iPhone is bonded (or after unpairing). Status bar hidden so the layout matches Step 4 exactly. Cancel returns to the main screen.',
    hideStatusBar: true,
    setup: () => repairPhoneHTML(),
  },
  'unpair-phone': {
    label: 'Runtime', title: 'Unpair iPhone?',
    desc: 'Shown when the user long-presses the phone-disconnect icon and an iPhone bond already exists. "Unpair" clears the bond and proceeds to the re-pair screen. "Cancel" returns to the previous screen.',
    hideStatusBar: true,
    setup: () => unpairPhoneHTML(),
  },
  'phone-disconnected': {
    label: 'Edge case', title: 'iPhone disconnected',
    desc: 'Phone outline + diagonal slash. Long-press → unpair confirmation (iPhone is bonded but offline). To demo the "no iPhone bonded" path use the sidebar button for Re-pair iPhone directly.',
    statusBar: { ancsApps: [], phoneConnected: false, phonePaired: true },
    leftRender: () => meetingListHTML(TODAY_MEETINGS),
  },
  'overlapping': {
    label: 'Edge case', title: 'Overlapping meetings',
    desc: 'Times accent-colored to flag overlap.',
    statusBar: { ancsApps: ['gmail', 'messenger'], phoneConnected: true },
    leftRender: () => meetingListHTML(OVERLAP_MEETINGS),
  },
  'long-title': {
    label: 'Edge case', title: 'Long title (single-line, tap to expand)',
    desc: 'Title and location single-line with ellipsis. Tap any meeting row to see the full title, location, and time in the detail overlay.',
    statusBar: { ancsApps: ['gmail'], phoneConnected: true },
    leftRender: () => meetingListHTML(LONG_TITLE_MEETINGS),
  },
  'overlap-and-long': {
    label: 'Edge case', title: 'Overlap + long title',
    desc: 'Overlap in the time column; titles single-line with ellipsis. Tap to expand.',
    statusBar: { ancsApps: ['gmail'], phoneConnected: true },
    leftRender: () => meetingListHTML(OVERLAP_LONG_MEETINGS),
  },
  'long-list': {
    label: 'Edge case', title: 'Scrollable list',
    desc: '9 meetings — visible scrollbar indicates length and position.',
    statusBar: { ancsApps: ['gmail', 'messenger'], phoneConnected: true },
    leftRender: () => meetingListHTML(LONG_LIST_MEETINGS),
  },
  'cached': {
    label: 'Edge case', title: 'Orion offline — using cached list',
    desc: 'No BLE link to Orion. Cached meetings still render with a SYNCED pill. Note: the Media mode-toggle button is hidden from the status bar — Media mode is useless without Orion bridging commands to the OS. The profile-photo border also auto-falls to dark grey (presence-offline) because Ori can no longer verify the user\'s real Teams status.',
    statusBar: { ancsApps: ['gmail', 'messenger'], phoneConnected: true, pcConnected: false },
    leftRender: () => meetingListHTML(TODAY_MEETINGS, true),
  },

  'presence-available': {
    label: 'Teams presence', title: 'Available — green border',
    desc: 'Default Teams "Available" state. Profile-photo border is green (--presence-available). Orion pushed PresenceStatus = 0x00 to Ori via the BLE Presence Status characteristic.',
    statusBar: { ancsApps: ['gmail', 'messenger', 'instagram'], phoneConnected: true },
    presence: 'available',
    leftRender: () => meetingListHTML(TODAY_MEETINGS),
  },
  'presence-busy': {
    label: 'Teams presence', title: 'Busy / DND — red border',
    desc: 'Maps from Teams "Busy", "Do Not Disturb", "In a call", "In a meeting", or "Presenting" — Orion collapses all of these into PresenceStatus = 0x01 (Busy). Profile-photo border is red.',
    statusBar: { ancsApps: ['gmail', 'messenger'], phoneConnected: true },
    presence: 'busy',
    leftRender: () => meetingListHTML(TODAY_MEETINGS),
  },
  'presence-away': {
    label: 'Teams presence', title: 'Be right back / Away — yellow border',
    desc: 'Maps from Teams "Be Right Back" or "Appear Away" — PresenceStatus = 0x02 (Away). Profile-photo border is yellow.',
    statusBar: { ancsApps: ['gmail', 'messenger'], phoneConnected: true },
    presence: 'away',
    leftRender: () => meetingListHTML(TODAY_MEETINGS),
  },
  'presence-offline': {
    label: 'Teams presence', title: 'Appear offline — grey border',
    desc: 'User chose Teams "Appear Offline" — PresenceStatus = 0x03 (Offline). Profile-photo border is dark grey. This is also the same border colour the device falls back to whenever Orion is BLE-disconnected (see the "Orion offline" edge case for that fallback path).',
    statusBar: { ancsApps: ['gmail', 'messenger'], phoneConnected: true },
    presence: 'offline',
    leftRender: () => meetingListHTML(TODAY_MEETINGS),
  },
};

let _meetingData = [];

function meetingListHTML(items, cached) {
  _meetingData = items;
  let html = '<div class="meeting-list">';
  if (cached) html += '<div class="synced-pill">SYNCED · 12 min ago</div>';
  for (let i = 0; i < items.length; i++) {
    const m = items[i];
    // in-progress beats overlap — "you should be in this room right now"
    // is more actionable than "this overlaps with another item".
    const cls =
      'meeting' +
      (m.inProgress ? ' in-progress' :
        m.overlap ? ' overlap' : '');
    html += '<div class="' + cls + '" onclick="showMeetingDetail(' + i + ')" style="cursor:pointer">' +
      '<div class="time-block"><div class="start">' + m.start +
      '</div><div class="end">— ' + m.end + '</div></div>' +
      '<div class="content">' +
      '<div class="title">' + escapeHtml(m.title) + '</div>' +
      '<div class="meta"><span class="loc">' + escapeHtml(m.loc) +
      '</span><span class="dot"></span><span class="org">' + escapeHtml(m.org) +
      '</span></div></div></div>';
  }
  return html + '</div>';
}

function showMeetingDetail(idx) {
  const m = _meetingData[idx];
  if (!m) return;
  const modalLayer = document.getElementById('modal-layer');
  modalLayer.innerHTML = '<div class="modal-scrim">' +
    meetingDetailHTML(m) + '</div>';
}

function closeMeetingDetail() {
  const ml = document.getElementById('modal-layer');
  if (ml) ml.innerHTML = '';
}

function meetingDetailHTML(m) {
  // Red for in-progress, accent-gold for overlap, white for normal —
  // applied to both the title and the time to match the meeting-list rule.
  const stateColor = m.inProgress ? 'var(--danger)' : m.overlap ? 'var(--accent)' : 'var(--text-1)';
  return '<div class="meeting-detail">' +
    '<div class="md-title" style="color:' + stateColor + '">' + escapeHtml(m.title) + '</div>' +
    '<div class="md-loc">' + escapeHtml(m.loc) + '</div>' +
    '<div class="md-org">' + escapeHtml(m.org) + '</div>' +
    '<div class="md-time" style="color:' + stateColor + '">' + escapeHtml(m.start) + ' – ' + escapeHtml(m.end) + '</div>' +
    '<div class="profile-close-row"><button class="btn btn-tertiary" onclick="closeMeetingDetail()">Close</button></div>' +
    '</div>';
}

function clockHTML() {
  const d = new Date();
  let h = d.getHours();
  const m = d.getMinutes().toString().padStart(2, '0');
  const ampm = h >= 12 ? 'PM' : 'AM';
  h = h % 12 || 12;
  const dateStr = d.toLocaleDateString(undefined, { weekday: 'long', month: 'long', day: 'numeric' });
  return '<div class="clock-face"><div class="clock-time"><span>' + h +
    '</span><span class="colon">:</span><span>' + m + '</span></div>' +
    '<div class="clock-date">' + dateStr.toUpperCase() + ' · ' + ampm + '</div></div>';
}

let _calViewDate = null; // first-of-month being viewed; null = current month

function calendarHTML() {
  const today = new Date();
  const view = _calViewDate || new Date(today.getFullYear(), today.getMonth(), 1);
  const year = view.getFullYear();
  const month = view.getMonth();
  const monthLabel = view.toLocaleDateString(undefined, { month: 'long', year: 'numeric' });
  const weekdays = ['M', 'T', 'W', 'T', 'F', 'S', 'S'];
  // getDay() is 0=Sun..6=Sat; shift so Monday is column 0.
  const firstDow = (new Date(year, month, 1).getDay() + 6) % 7;
  const daysInMonth = new Date(year, month + 1, 0).getDate();
  const daysInPrevMonth = new Date(year, month, 0).getDate();

  const cells = [];
  for (let i = 0; i < firstDow; i++) {
    cells.push({ day: daysInPrevMonth - firstDow + 1 + i, outside: true });
  }
  for (let d = 1; d <= daysInMonth; d++) {
    const isToday = d === today.getDate() && month === today.getMonth() && year === today.getFullYear();
    cells.push({ day: d, outside: false, today: isToday });
  }
  let nextDay = 1;
  while (cells.length % 7 !== 0) {
    cells.push({ day: nextDay++, outside: true });
  }

  return '<div class="calendar-view">' +
    '<div class="cal-header">' +
    '<div class="cal-month">' + monthLabel + '</div>' +
    '<div class="cal-nav">' +
    '<div class="cal-nav-btn" id="cal-prev" title="Previous month"><svg viewBox="0 0 24 24"><use href="#i-chev-left"/></svg></div>' +
    '<div class="cal-nav-btn" id="cal-next" title="Next month"><svg viewBox="0 0 24 24"><use href="#i-chev-right"/></svg></div>' +
    '</div></div>' +
    '<div class="cal-weekdays">' +
    weekdays.map(w => '<div class="cal-weekday">' + w + '</div>').join('') +
    '</div>' +
    '<div class="cal-grid">' +
    cells.map(c => '<div class="cal-day' + (c.outside ? ' outside' : '') + (c.today ? ' today' : '') + '">' +
      '<div class="cal-day-num">' + c.day + '</div></div>').join('') +
    '</div></div>';
}

function renderCalendar() {
  document.getElementById('left-panel').innerHTML = calendarHTML();
  bindCalendarNav();
}

function bindCalendarNav() {
  const today = new Date();
  const base = _calViewDate || new Date(today.getFullYear(), today.getMonth(), 1);
  document.getElementById('cal-prev').onclick = () => {
    _calViewDate = new Date(base.getFullYear(), base.getMonth() - 1, 1);
    renderCalendar();
  };
  document.getElementById('cal-next').onclick = () => {
    _calViewDate = new Date(base.getFullYear(), base.getMonth() + 1, 1);
    renderCalendar();
  };
}

let _ptoDestination = '';
let _ptoDates = '';

function ptoHTML(destination, dates) {
  _ptoDestination = destination;
  _ptoDates = dates;
  return '<div class="pto">' +
    '<div class="pto-scene"></div><div class="pto-sun"></div>' +
    '<svg class="pto-mountains" viewBox="0 0 528 130" preserveAspectRatio="none">' +
    '<path d="M0 130 L0 80 L80 30 L160 70 L240 20 L340 65 L420 35 L528 75 L528 130 Z" fill="#3a4a60" opacity="0.9"/>' +
    '<path d="M0 130 L0 100 L70 65 L150 95 L230 60 L320 90 L400 70 L528 100 L528 130 Z" fill="#243348" opacity="0.85"/>' +
    '</svg><div class="pto-water"></div><div class="pto-overlay"></div>' +
    '<div class="pto-text"><div class="pto-text-card" onclick="showPtoDetail()">' +
    '<div class="label">On PTO</div>' +
    '<div class="destination">' + escapeHtml(destination) + '</div>' +
    '<div class="dates">' + escapeHtml(dates) + '</div>' +
    '</div></div>' +
    '</div>';
}

function showPtoDetail() {
  const modalLayer = document.getElementById('modal-layer');
  modalLayer.innerHTML = '<div class="modal-scrim">' +
    ptoDetailHTML(_ptoDestination, _ptoDates) + '</div>';
}

function closePtoDetail() {
  const ml = document.getElementById('modal-layer');
  if (ml) ml.innerHTML = '';
}

function ptoDetailHTML(destination, dates) {
  return '<div class="pto-detail">' +
    '<div class="pd-scroll">' +
    '<div class="pd-label">On PTO</div>' +
    '<div class="pd-destination">' + escapeHtml(destination) + '</div>' +
    '<div class="pd-dates">' + escapeHtml(dates) + '</div>' +
    '</div>' +
    '<div class="profile-close-row"><button class="btn btn-tertiary" onclick="closePtoDetail()">Close</button></div>' +
    '</div>';
}

function countdownHTML(meetingName, when) {
  const R = 100, C = 2 * Math.PI * R, remain = C * 0.62;
  return '<div class="countdown"><div class="ring"><svg viewBox="0 0 230 230">' +
    '<circle class="track" cx="115" cy="115" r="' + R + '" fill="none" stroke-width="8"/>' +
    '<circle class="progress" cx="115" cy="115" r="' + R + '" fill="none" stroke-width="8" stroke-dasharray="' + C.toFixed(1) + '" stroke-dashoffset="' + (C - remain).toFixed(1) + '"/>' +
    '</svg><div class="label"><div class="big">3:07</div><div class="small">until start</div></div></div>' +
    '<div class="meeting-name">' + escapeHtml(meetingName) + '</div>' +
    '<div class="meeting-when">' + escapeHtml(when) + '</div>' +
    '<div class="profile-close-row"><button class="btn btn-tertiary" onclick="closeModal()">Close</button></div></div>';
}

function factoryResetHTML() {
  return '<div class="alert-card reset"><div class="icon-circle"><svg width="36" height="36"><use href="#i-warn"/></svg></div>' +
    '<h3>Factory reset Ori?</h3><p>All data and paired devices will be removed</p>' +
    '<div class="actions"><button class="btn btn-danger" onclick="closeModal()">Reset</button>' +
    '<button class="btn btn-tertiary" onclick="closeModal()">Cancel</button></div></div>';
}

function passkeyHTML(passkey) {
  return '<div class="passkey-card">' +
    '<h3>Confirm on Orion</h3>' +
    '<div class="passkey-digits">' + passkey + '</div>' +
    '</div>';
}

function orioningModalHTML(pct) {
  const R = 90, C = 2 * Math.PI * R, off = C * (1 - pct / 100);
  return '<div class="passkey-card">' +
    '<h3>A busy day ahead…</h3>' +
    '<div class="orioning-ring" style="width:140px;height:140px;margin:24px auto 0">' +
    '<svg viewBox="0 0 200 200">' +
    '<circle class="track" cx="100" cy="100" r="' + R + '" fill="none" stroke-width="7"/>' +
    '<circle class="progress" cx="100" cy="100" r="' + R + '" fill="none" stroke-width="7" stroke-dasharray="' + C.toFixed(1) + '" stroke-dashoffset="' + off.toFixed(1) + '"/>' +
    '</svg>' +
    '<div class="pct-label" style="font-size:30px">' + pct + '%</div>' +
    '</div>' +
    '</div>';
}

function setupShell(stepIndex, body, extraStyle) {
  var styleAttr = extraStyle ? ' style="' + extraStyle + '"' : '';
  if (stepIndex === 'hide') return '<div class="setup"' + styleAttr + '>' + body + '</div>';
  let dots = '';
  for (let i = 0; i < 3; i++) {
    let cls = '';
    if (typeof stepIndex === 'number') {
      if (i === stepIndex) cls = 'active';
      else if (i < stepIndex) cls = 'done';
    }
    dots += '<div class="step-dot ' + cls + '"></div>';
  }
  return '<div class="setup"' + styleAttr + '>' + body + '<div class="steps">' + dots + '</div></div>';
}

function brandMarkHTML(size, mtop) {
  if (mtop === undefined) mtop = 8;
  return '<div class="brand-mark" style="margin-top:' + mtop + 'px">' +
    '<div class="bm-namerow">' +
    '<div class="bm-line"></div>' +
    '<div class="word">o<span class="dot">r</span>i</div>' +
    '<div class="bm-line bm-line-r"></div>' +
    '</div>' +
    '</div>';
}

function setupWelcomeHTML() {
  return setupShell('pre',
    brandMarkHTML(132, -2) +
    '<div style="flex:1;display:flex;flex-direction:column;align-items:center;justify-content:center;width:100%">' +
    '<p style="font-size:42px;color:var(--text-1);letter-spacing:0.3px;">A display for your day</p>' +
    '<p style="margin-top:10px;font-size:26px;color:var(--text-2);">Your desk deserves better</p>' +
    '</div>' +
    '<button class="btn btn-primary" onclick="setScreen(\'setup-install\')">Start</button>',
    'padding-bottom:80px'
  );
}

function setupInstallHTML() {
  return setupShell(0,
    brandMarkHTML(132, -2) +
    '<div style="flex:1;display:flex;flex-direction:column;align-items:center;justify-content:center;width:100%">' +
    '<p style="font-size:42px;color:var(--text-1);">Download Orion at <span style="color:var(--accent)">ori.app/orion</span></p>' +
    '<p style="margin-top:10px;font-size:26px;color:var(--text-2);">Available on Windows and macOS</p>' +
    '</div>' +
    '<button class="btn btn-primary" onclick="setScreen(\'setup-link-orion\')">Next</button>',
    'padding-bottom:80px'
  );
}

function setupLinkOrionHTML() {
  return setupShell(1,
    brandMarkHTML(132, -2) +
    '<div style="flex:1;display:flex;flex-direction:column;align-items:center;justify-content:center;width:100%">' +
    '<p style="font-size:42px;color:var(--text-1);">Connect on Orion</p>' +
    '<div class="ble-name" style="margin-top:10px">' + escapeHtml(BLE_NAME) + '</div>' +
    '<div class="pairing-anim" style="margin-top:24px"></div>' +
    '</div>'
  );
}

function setupPhoneHTML(opts) {
  return setupShell(2,
    brandMarkHTML(132, -2) +
    '<div style="flex:1;display:flex;flex-direction:column;align-items:center;justify-content:center;width:100%">' +
    '<p style="font-size:42px;color:var(--text-1);">Connect on iPhone</p>' +
    '<div class="ble-name" style="margin-top:10px">' + escapeHtml(BLE_NAME) + '</div>' +
    '<div class="pairing-anim" style="margin-top:24px"></div>' +
    '</div>' +
    '<button class="btn btn-tertiary" onclick="setScreen(\'setup-done\')">Skip</button>',
    'padding-bottom:80px'
  );
}

function setupDoneHTML() {
  const firstName = escapeHtml(PROFILE.name.split(' ')[0]);
  clearTimeout(_setupDoneTimer);
  _setupDoneTimer = setTimeout(() => {
    if (currentScreenId === 'setup-done') setScreen('meeting-list');
  }, 5000);
  return '<div class="setup" style="cursor:pointer" onclick="skipSetupDone()">' +
    brandMarkHTML(132, -2) +
    '<div style="flex:1;display:flex;flex-direction:column;align-items:center;justify-content:center;width:100%">' +
    '<p style="font-size:42px;color:var(--text-1);">Welcome, ' + firstName + '</p>' +
    '<p style="margin-top:10px;font-size:26px;color:var(--text-2);">Let\'s get to work</p>' +
    '<div class="ok-check" style="margin-top:28px"><svg width="130" height="130" viewBox="0 0 100 100">' +
    '<circle cx="50" cy="50" r="44" fill="none" stroke="rgba(127,180,138,0.15)" stroke-width="2.5"/>' +
    '<circle cx="50" cy="50" r="44" fill="none" stroke="#7FB48A" stroke-width="2.5" stroke-linecap="round" class="ok-ring" transform="rotate(-90 50 50)"/>' +
    '<path d="M29 51 L43 65 L71 35" fill="none" stroke="#7FB48A" stroke-width="5" stroke-linecap="round" stroke-linejoin="round" class="ok-tick"/>' +
    '</svg></div>' +
    '</div>' +
    '<div class="setup-done-bar"></div>' +
    '</div>';
}

function skipSetupDone() {
  clearTimeout(_setupDoneTimer);
  setScreen('meeting-list');
}

// Reconnect-Syncing overlay — replaces the left panel while Ori runs the
// hash-manifest delta sync after reconnecting to Orion. Status bar and
// profile card stay visible. Non-dismissable; auto-clears when sync ends.
function reconnectSyncingHTML() {
  return '<div class="reconnect-overlay">' +
    '<div class="reconnect-ring"></div>' +
    '<p style="margin-top:22px;font-size:24px;color:var(--text-3)">Refreshing your day…</p>' +
    '</div>';
}

// ── OTA firmware update flow (PSRAM-staging model) ─────────────────────────
//
// Sequence on the device:
//   1. ota-downloading  — image streams into PSRAM; progress bar is LIVE (no
//                         flash written yet, so the screen keeps refreshing).
//   2. ota-ready        — download done + verified; instruction that the screen
//                         goes dark during install, with a primary "Update now".
//   3. ota-installing   — user tapped Update; PSRAM→flash copy runs. The panel
//                         is physically DARK here (LCD halted; PSRAM-DMA can't
//                         run while flash is written). Shown black in the proto.
//   4. ota-complete     — flash written; checkmark + primary "Restart".
//   5. ota-updated-ack  — FIRST screen after the reboot: confirms the update and
//                         shows the new version, with a "Close" button. Firmware
//                         persists a flag in NVS so this screen reappears on
//                         every boot until the user taps Close (acknowledges).

// Reusable success check (same artwork as the Setup-complete screen).
function okCheckHTML(size) {
  size = size || 130;
  return '<div class="ok-check"><svg width="' + size + '" height="' + size + '" viewBox="0 0 100 100">' +
    '<circle cx="50" cy="50" r="44" fill="none" stroke="rgba(127,180,138,0.15)" stroke-width="2.5"/>' +
    '<circle cx="50" cy="50" r="44" fill="none" stroke="#7FB48A" stroke-width="2.5" stroke-linecap="round" class="ok-ring" transform="rotate(-90 50 50)"/>' +
    '<path d="M29 51 L43 65 L71 35" fill="none" stroke="#7FB48A" stroke-width="5" stroke-linecap="round" stroke-linejoin="round" class="ok-tick"/>' +
    '</svg></div>';
}

// 1. Downloading — live progress ring while the image streams into PSRAM.
function otaDownloadingHTML(pct) {
  const R = 90, C = 2 * Math.PI * R, off = C * (1 - pct / 100);
  return setupShell('hide',
    '<div style="display:flex;flex-direction:column;align-items:center;justify-content:center;flex:1;width:100%">' +
    '<h2>Downloading firmware…</h2>' +
    '<div class="orioning-ring" style="margin-top:30px">' +
    '<svg viewBox="0 0 200 200">' +
    '<circle class="track" cx="100" cy="100" r="' + R + '" fill="none" stroke-width="8"/>' +
    '<circle class="progress" cx="100" cy="100" r="' + R + '" fill="none" stroke-width="8"' +
    ' stroke-dasharray="' + C.toFixed(1) + '" stroke-dashoffset="' + off.toFixed(1) + '"/>' +
    '</svg>' +
    '<div class="pct-label">' + pct + '%</div>' +
    '</div>' +
    '<p style="margin-top:24px;color:var(--text-2)">Keep Ori plugged in</p>' +
    '</div>'
  );
}

// 2. Firmware Install — mirrors the downloading layout (title / centre glyph /
//    short instruction), plus a primary "Update now" at the Start-button spot.
function otaReadyHTML() {
  return setupShell('hide',
    '<div style="display:flex;flex-direction:column;align-items:center;justify-content:center;flex:1;width:100%">' +
    '<h2>Firmware Install</h2>' +
    '<div class="ota-install-glyph" style="margin-top:30px">' +
    '<svg width="68" height="68" viewBox="0 0 24 24" fill="none" stroke="var(--accent)" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round">' +
    '<path d="M12 3 V14 M7 10 l5 4 5-4"/><path d="M4 17 v2 a1 1 0 0 0 1 1 h14 a1 1 0 0 0 1-1 v-2"/>' +
    '</svg>' +
    '</div>' +
    '<p style="margin-top:24px;color:var(--text-2)">Screen will go dark for a few seconds</p>' +
    '</div>' +
    '<button class="btn btn-primary" onclick="setScreen(\'ota-installing\')">Update now</button>',
    'padding-bottom:80px'
  );
}

// 3. Installing — the panel is physically dark on the device while flash is
//    written. Shown black here; auto-advances to the post-reboot ack.
let _otaInstallTimer = null;
function otaInstallingHTML() {
  clearTimeout(_otaInstallTimer);
  _otaInstallTimer = setTimeout(() => {
    // Install → reboot is atomic on the device (no "Update complete / Restart"
    // step); the post-reboot ack confirms completion.
    if (currentScreenId === 'ota-installing') setScreen('ota-updated-ack');
  }, 2400);
  return '<div class="setup" style="background:#000;justify-content:center;align-items:center">' +
    '<p style="color:#1c1c1c;font-size:20px;letter-spacing:0.5px">screen is dark while installing</p>' +
    '</div>';
}

// 4. Post-reboot acknowledgement — persisted until Close (NVS flag in firmware).
//    Title at top, then the version line, then the checkmark, then Close.
function otaUpdatedAckHTML() {
  return setupShell('hide',
    '<div style="display:flex;flex-direction:column;align-items:center;flex:1;width:100%">' +
    '<h2 style="margin-top:30px">Firmware updated</h2>' +
    '<div style="flex:1;display:flex;flex-direction:column;align-items:center;justify-content:center;width:100%">' +
    '<p style="color:var(--text-2)">Ori is now running version ' + escapeHtml(FW_VERSION) + '</p>' +
    okCheckHTML(110) +
    '</div>' +
    '</div>' +
    '<button class="btn btn-tertiary" onclick="setScreen(\'meeting-list\')">Close</button>',
    'padding-bottom:80px'
  );
}

// Error — shown if the OTA can't complete. Same title/glyph/instruction layout;
// `msg` is the user-facing reason (firmware maps each failure code to one of a
// few plain-language messages). Close dismisses back to runtime.
function otaErrorHTML(msg) {
  msg = msg || 'The update couldn’t be installed — try again from Orion';
  return setupShell('hide',
    '<div style="display:flex;flex-direction:column;align-items:center;justify-content:center;flex:1;width:100%">' +
    '<h2>Update failed</h2>' +
    '<div class="ota-error-glyph" style="margin-top:30px">' +
    '<svg width="62" height="62" viewBox="0 0 24 24"><use href="#i-warn"/></svg>' +
    '</div>' +
    '<p style="margin-top:24px;color:var(--text-2)">' + escapeHtml(msg) + '</p>' +
    '</div>' +
    '<button class="btn btn-tertiary" onclick="setScreen(\'meeting-list\')">Close</button>',
    'padding-bottom:80px'
  );
}

// Runtime re-pair — same layout as Step 4, with a Cancel button anchored at bottom
function repairPhoneHTML() {
  return '<div class="setup" style="padding-bottom:80px">' +
    brandMarkHTML(132, -2) +
    '<div style="flex:1;display:flex;flex-direction:column;align-items:center;justify-content:center;width:100%">' +
    '<p style="font-size:42px;color:var(--text-1);">Connect on iPhone</p>' +
    '<div class="ble-name" style="margin-top:20px">' + escapeHtml(BLE_NAME) + '</div>' +
    '<div class="pairing-anim" style="margin-top:24px"></div>' +
    '</div>' +
    '<button class="btn btn-tertiary" style="margin-top:20px" onclick="setScreen(\'meeting-list\')">Cancel</button>' +
    '</div>';
}

// Unpair-iPhone confirmation — shown when user long-presses the phone-disconnect
// icon and an iPhone bond already exists. "Unpair" clears the bond and proceeds
// to re-pair; "Cancel" returns to wherever the long-press was triggered from.
function unpairPhoneHTML() {
  return '<div class="setup" style="justify-content:center">' +
    '<div class="alert-card">' +
    '<div class="icon-circle">' +
    '<svg width="36" height="36" viewBox="0 0 24 24"><use href="#i-warn"/></svg>' +
    '</div>' +
    '<h3>Unpair iPhone?</h3>' +
    '<p>Notification will stop showing until you re-pair</p>' +
    '<div class="actions">' +
    '<button class="btn btn-danger" onclick="setScreen(\'repair-phone\')">Unpair</button>' +
    '<button class="btn btn-tertiary" onclick="setScreen(window._unpairPrev||\'phone-disconnected\')">Cancel</button>' +
    '</div>' +
    '</div>' +
    '</div>';
}

// ---- Keyboard mode (BLE-bridged secondary controller) ----
//
// Architecture: Ori → custom BLE GATT command → Orion (always-running
// background app) → OS HID injection or OS volume/media API → focused
// application. Orion is also the source of truth for the volume level
// and the currently-playing media metadata, both of which it pushes
// back to Ori so the HUD and the Now Playing card stay honest.
// See `.claude/rules/media-mode.md` and `.claude/rules/ble-protocol.md`
// §3 (Keyboard Command / Host Volume State / Media Metadata chars).

// Prototype-only state, just for the visual preview.
let kbdPlaying = false;
let kbdVolume = 65;    // pushed by Orion via Host Volume State char on the real device
let kbdPosition = 83;  // seconds elapsed — pushed by Orion via Media Metadata on the real device
const kbdDuration = 245; // seconds total

function fmtTime(s) {
  const m = Math.floor(s / 60);
  const sec = Math.floor(s % 60);
  return m + ':' + (sec < 10 ? '0' : '') + sec;
}

// Mock now-playing metadata. On the real device, Orion reads this from the
// host OS (Win: GlobalSystemMediaTransportControlsSessionManager;
// macOS: MediaRemote / MPNowPlayingInfoCenter) and pushes via the
// Media Metadata characteristic. A long title is used here to demonstrate
// truncation/ellipsis behavior.
const MOCK_MEDIA = {
  title: 'Industrial Symphony No. 1 — The Dream of the Brokenhearted Woman',
  artist: 'Angelo Badalamenti',
  can_seek: true,   // mirrors MediaMetadata.can_seek (ble-protocol.md v1.5)
  // set to false to preview the no-scrubber state (e.g. browser audio)
};
// To preview the empty state, swap to:
// const MOCK_MEDIA = { title: '', artist: '' };

// Three user-assignable shortcut slots. The icons here are mock defaults
// for the prototype — in production each slot's icon (and the action it
// triggers) is configured in Orion's settings UI. No text label on the
// device by design. Mock config: mute audio, mute mic, screen capture.
const KBD_SHORTCUTS = [
  { icon: 'i-vol-mute' },  // Mute audio
  { icon: 'i-mic-mute' },  // Mute mic
  { icon: 'i-screenshot' },  // Screen capture
];

function mediaModeHTML() {
  const v = kbdVolume;
  const m = MOCK_MEDIA;
  const hasMedia = !!(m.title && m.title.trim());
  const artClasses = ['kbd-art-wrap'];
  if (!kbdPlaying) artClasses.push('paused');
  if (!hasMedia) artClasses.push('empty');
  const shortcuts = KBD_SHORTCUTS.map(s =>
    '<button class="s-btn">' +
    '<svg viewBox="0 0 24 24"><use href="#' + s.icon + '"/></svg>' +
    '</button>'
  ).join('');

  return '<div class="kbd-mode">' +
    // Album art — the dominant interaction surface.
    //   tap          → play/pause
    //   horiz swipe  → prev/next track
    //   vert swipe   → volume (with momentary HUD)
    '<div class="' + artClasses.join(' ') + '" id="kbd-art-wrap">' +
    '<div class="kbd-art"></div>' +
    // Persistent paused-state overlay (visible while paused)
    '<div class="kbd-art-overlay"><svg viewBox="0 0 24 24"><use href="#i-play"/></svg></div>' +
    // Transient flash overlay (animates on tap)
    '<div class="kbd-art-flash"><svg viewBox="0 0 24 24" id="kbd-art-flash-icon"><use href="#i-play"/></svg></div>' +
    // Volume HUD — visible only during a vertical-swipe gesture
    '<div class="kbd-art-hud" id="kbd-art-hud">' +
    '<div class="hud-pct" id="hud-pct">' + v + '%</div>' +
    '<div class="hud-bar"><div class="hud-fill" id="hud-fill" style="height:' + v + '%"></div></div>' +
    '</div>' +
    // Timeline bar — only rendered when the app supports seeking (can_seek).
    // Absent or false → scrubber hidden entirely; no dead affordance.
    (hasMedia && m.can_seek ? (function () {
      const pct = kbdDuration > 0 ? (kbdPosition / kbdDuration * 100).toFixed(1) : 0;
      return '<div class="kbd-timeline">' +
        '<div class="kbd-timeline-bar">' +
        '<div class="kbd-timeline-fill" style="width:' + pct + '%">' +
        '<div class="kbd-timeline-thumb"></div>' +
        '</div>' +
        '</div>' +
        '<div class="kbd-timeline-times">' +
        '<span>' + fmtTime(kbdPosition) + '</span>' +
        '<span>' + fmtTime(kbdDuration) + '</span>' +
        '</div>' +
        '</div>';
    })() : '') +
    '</div>' +
    // Title + artist, centred below art
    '<div class="kbd-meta' + (hasMedia ? '' : ' empty') + '">' +
    '<div class="title">' + (hasMedia ? escapeHtml(m.title) : 'Nothing playing') + '</div>' +
    '<div class="artist">' + (hasMedia ? escapeHtml(m.artist) : '—') + '</div>' +
    '</div>' +
    // Three user-assignable shortcut buttons (mock: mute audio, mute mic, screen capture)
    '<div class="kbd-shortcuts">' + shortcuts + '</div>' +
    '</div>';
}

function togglePlayPause() {
  kbdPlaying = !kbdPlaying;
  const wrap = document.getElementById('kbd-art-wrap');
  if (!wrap) return;
  wrap.classList.toggle('paused', !kbdPlaying);
  const flashIcon = document.getElementById('kbd-art-flash-icon');
  if (flashIcon) {
    flashIcon.innerHTML = '<use href="#i-' + (kbdPlaying ? 'play' : 'pause') + '"/>';
  }
  wrap.classList.remove('flash');
  void wrap.offsetWidth; // restart the CSS animation
  wrap.classList.add('flash');
}

function onMediaPress(el) {
  // Visual feedback only — real device emits a BLE Keyboard Command notify
  // and Orion bridges it to the OS.
  el.animate(
    [{ transform: 'scale(1)' }, { transform: 'scale(0.92)' }, { transform: 'scale(1)' }],
    { duration: 180, easing: 'ease-out' }
  );
}

// Album-art gesture handler — three orthogonal gestures on the same surface:
//   tap         (|dx|<20 AND |dy|<20)        → play/pause
//   horizontal  (|dx|>50 AND |dx|>|dy|)      → prev / next
//   vertical    (|dy|>25 AND |dy|>|dx|)      → volume up / down + show HUD
//
// During a vertical drag the HUD overlay fades in and the volume tracks the
// swipe distance: ~200 px of vertical travel = full 0..100 range. On release
// the HUD lingers ~800 ms so the user can read the final value, then fades.
// Real device emits KeyboardCommand{op:"vol_set", arg: <0..100>} on release.
function bindAlbumArtGestures() {
  const wrap = document.getElementById('kbd-art-wrap');
  if (!wrap) return;
  const TAP_MAX = 20;
  const H_SWIPE_MIN = 50;
  const V_SWIPE_ENGAGE = 25;
  const V_SENSITIVITY = 100 / 200;
  let startX = 0, startY = 0, startVolume = 0;
  let tracking = false, verticalEngaged = false;
  let hudFadeTimer = null;

  function getXY(e) {
    if (e.touches && e.touches[0]) return { x: e.touches[0].clientX, y: e.touches[0].clientY };
    if (e.changedTouches && e.changedTouches[0]) return { x: e.changedTouches[0].clientX, y: e.changedTouches[0].clientY };
    return { x: e.clientX, y: e.clientY };
  }
  function applyVolume(v) {
    kbdVolume = Math.max(0, Math.min(100, Math.round(v)));
    const fill = document.getElementById('hud-fill');
    const pct = document.getElementById('hud-pct');
    if (fill) fill.style.height = kbdVolume + '%';
    if (pct) pct.textContent = kbdVolume + '%';
  }
  function hudShow() {
    wrap.classList.add('swiping-vert');
    wrap.classList.remove('hud-fade');
    if (hudFadeTimer) { clearTimeout(hudFadeTimer); hudFadeTimer = null; }
  }
  function hudHideSoon() {
    wrap.classList.remove('swiping-vert');
    wrap.classList.add('hud-fade');
    if (hudFadeTimer) clearTimeout(hudFadeTimer);
    hudFadeTimer = setTimeout(() => { wrap.classList.remove('hud-fade'); }, 800);
  }

  function onDown(e) {
    const p = getXY(e);
    startX = p.x; startY = p.y; startVolume = kbdVolume;
    tracking = true; verticalEngaged = false;
  }
  function onMove(e) {
    if (!tracking) return;
    const p = getXY(e);
    const dx = p.x - startX;
    const dy = p.y - startY;
    if (!verticalEngaged && Math.abs(dy) > V_SWIPE_ENGAGE && Math.abs(dy) > Math.abs(dx)) {
      verticalEngaged = true;
      hudShow();
    }
    if (verticalEngaged) {
      applyVolume(startVolume + (-dy) * V_SENSITIVITY);
      if (e.preventDefault && e.cancelable) e.preventDefault();
    }
  }
  function onUp(e) {
    if (!tracking) return;
    tracking = false;
    const p = getXY(e);
    const dx = p.x - startX;
    const dy = p.y - startY;
    const absDx = Math.abs(dx);
    const absDy = Math.abs(dy);

    if (verticalEngaged) {
      hudHideSoon();
      verticalEngaged = false;
      return;
    }
    if (absDx < TAP_MAX && absDy < TAP_MAX) {
      togglePlayPause();
    } else if (absDx > H_SWIPE_MIN && absDx > absDy) {
      const dir = dx > 0 ? 'right' : 'left';
      wrap.classList.remove('swipe-left', 'swipe-right');
      void wrap.offsetWidth;
      wrap.classList.add('swipe-' + dir);
      setTimeout(() => wrap.classList.remove('swipe-' + dir), 180);
    }
  }
  function onCancel() {
    tracking = false;
    if (verticalEngaged) hudHideSoon();
    verticalEngaged = false;
  }

  wrap.addEventListener('mousedown', onDown);
  window.addEventListener('mousemove', onMove);
  window.addEventListener('mouseup', onUp);
  wrap.addEventListener('mouseleave', onCancel);
  wrap.addEventListener('touchstart', onDown, { passive: false });
  wrap.addEventListener('touchmove', onMove, { passive: false });
  wrap.addEventListener('touchend', onUp, { passive: true });
  wrap.addEventListener('touchcancel', onCancel);
}

// Seek gesture — drag the timeline bar to jump to a position in the track.
// On the real device Ori emits KeyboardCommand{op:"seek", arg:<position_s>} on
// release; Orion bridges this to the OS via:
//   Windows: GlobalSystemMediaTransportControlsSession.TryChangePlaybackPositionAsync
//   macOS:   MRMediaRemoteSetElapsedTime (private but stable API)
function bindSeekBar() {
  const timeline = document.querySelector('#kbd-art-wrap .kbd-timeline');
  if (!timeline) return;
  let seeking = false;

  function clientX(e) {
    if (e.touches && e.touches[0]) return e.touches[0].clientX;
    if (e.changedTouches && e.changedTouches[0]) return e.changedTouches[0].clientX;
    return e.clientX;
  }

  function calcPos(cx) {
    const bar = timeline.querySelector('.kbd-timeline-bar');
    const rect = bar.getBoundingClientRect();
    const rel = Math.max(0, Math.min(cx - rect.left, rect.width));
    const pct = rect.width > 0 ? rel / rect.width * 100 : 0;
    const pos = Math.round(pct / 100 * kbdDuration);
    return { pct, pos };
  }

  function applySeek(cx) {
    const { pct, pos } = calcPos(cx);
    const fill = timeline.querySelector('.kbd-timeline-fill');
    const cur = timeline.querySelector('.kbd-timeline-times span:first-child');
    if (fill) fill.style.width = pct.toFixed(1) + '%';
    if (cur) cur.textContent = fmtTime(pos);
  }

  function onDown(e) {
    seeking = true;
    timeline.classList.add('seeking');
    applySeek(clientX(e));
    e.stopPropagation(); // prevent art-gesture handler from also starting
    if (e.cancelable) e.preventDefault();
  }
  function onMove(e) {
    if (!seeking) return;
    applySeek(clientX(e));
    if (e.cancelable) e.preventDefault();
  }
  function onUp(e) {
    if (!seeking) return;
    seeking = false;
    timeline.classList.remove('seeking');
    const { pos } = calcPos(clientX(e));
    kbdPosition = pos;
    // Real device: emit KeyboardCommand{op:"seek", arg:pos}
  }

  timeline.addEventListener('mousedown', onDown);
  window.addEventListener('mousemove', onMove);
  window.addEventListener('mouseup', onUp);
  timeline.addEventListener('touchstart', onDown, { passive: false });
  window.addEventListener('touchmove', onMove, { passive: false });
  window.addEventListener('touchend', onUp, { passive: true });
}

function ancsIconHTML(app) {
  return '<div class="ancs-icon-wrap" data-app="' + app + '" onclick="showAncsDetail(\'' + app + '\')">' +
    '<svg class="ancs-icon" viewBox="0 0 24 24"><use href="#i-' + app + '"/></svg></div>';
}

function ancsDetailHTML(app) {
  const n = ANCS_NOTIFICATIONS[app] || {
    displayName: app.charAt(0).toUpperCase() + app.slice(1),
    title: 'Notification',
    body: 'No preview available.',
    time: 'Just now',
  };
  // Silent badge — shown in the top-left corner of the overlay when the
  // notification's ANCS EventFlags SILENT bit is set (iOS delivered it silently;
  // whether it even appears depends on the Orion "show silent" toggle setting).
  const silentBadge = n.silent
    ? '<div class="ad-silent-badge">' +
      '<svg viewBox="0 0 24 24"><use href="#i-bell-off"/></svg>' +
      'Silent</div>'
    : '';
  const detailClass = 'ancs-detail' + (n.silent ? ' has-silent' : '');
  // Layout mirrors the meeting-detail overlay:
  //   app icon  ← visual anchor (no meeting equivalent)
  //   title     ← md-title (28px weight 500) — sender / notification subject
  //   body      ← md-loc  (20px secondary)   — message preview, 3-line clamp
  //   timestamp ← md-org  (18px tertiary)    — how long ago
  //   app name  ← md-time (22px weight 500)  — definitive source identifier
  //   buttons   ← replaces "Tap to dismiss" hint
  return '<div class="' + detailClass + '" onclick="event.stopPropagation()">' +
    silentBadge +
    '<div class="ad-app-icon"><svg viewBox="0 0 24 24"><use href="#i-' + app + '"/></svg></div>' +
    '<div class="ad-title">' + escapeHtml(n.title) + '</div>' +
    '<div class="ad-body">' + escapeHtml(n.body) + '</div>' +
    '<div class="ad-time">' + escapeHtml(n.time) + '</div>' +
    '<div class="ad-app-name">' + escapeHtml(n.displayName) + '</div>' +
    '<div class="ad-actions">' +
    '<button class="btn btn-primary" onclick="readAncsNotification(\'' + app + '\')">Read</button>' +
    '<button class="btn btn-tertiary" onclick="closeAncsDetail()">Close</button>' +
    '</div></div>';
}

function showAncsDetail(app) {
  const modalLayer = document.getElementById('modal-layer');
  modalLayer.innerHTML = '<div class="modal-scrim" onclick="if(event.target===this)closeAncsDetail()">' +
    ancsDetailHTML(app) + '</div>';
}

function closeAncsDetail() {
  const modalLayer = document.getElementById('modal-layer');
  if (modalLayer) modalLayer.innerHTML = '';
}

function profileDetailHTML(photoColPadTop) {
  // Mirror the live presence class from the right-panel photo so the border
  // colour in the overlay always matches the current Teams status.
  const livePhoto = document.getElementById('profile-photo');
  const presenceClass = ['presence-available', 'presence-busy', 'presence-away', 'presence-offline']
    .find(cls => livePhoto && livePhoto.classList.contains(cls)) || 'presence-offline';

  // When a measured value is supplied (from showProfileDetail), apply it as an
  // inline style so the photo lands at exactly the same Y as in calendar mode.
  const padStyle = (photoColPadTop !== undefined)
    ? ' style="padding-top:' + photoColPadTop + 'px"'
    : '';

  return '<div class="profile-box" onclick="event.stopPropagation()">' +
    '<div class="profile-body">' +
    // Left half — scrollable info block, centred text.
    '<div class="profile-info-col">' +
    '<div class="po-name">' + escapeHtml(PROFILE.name) + '</div>' +
    '<div class="po-job-title">' + escapeHtml(PROFILE.title) + '</div>' +
    (PROFILE.email ? '<div class="po-email">' + escapeHtml(PROFILE.email) + '</div>' : '') +
    (PROFILE.phone ? '<div class="po-phone">' + escapeHtml(PROFILE.phone) + '</div>' : '') +
    '</div>' +
    // Right half — photo at its exact calendar-mode position, with presence border.
    '<div class="profile-photo-col"' + padStyle + '>' +
    '<div class="profile-photo ' + presenceClass + '" style="cursor:default">' +
    escapeHtml(initialsOf(PROFILE.name)) +
    '</div>' +
    '</div>' +
    '</div>' +
    '<div class="profile-close-row"><button class="btn btn-tertiary" onclick="closeProfileDetail()">Close</button></div>' +
    '</div>';
}

function showProfileDetail() {
  // Measure the live photo's position before rendering the overlay so we can
  // pin the overlay photo to exactly the same screen coordinate.
  const livePhoto = document.getElementById('profile-photo');
  const device = document.getElementById('device');
  let photoColPadTop;
  if (livePhoto && device) {
    const photoRect = livePhoto.getBoundingClientRect();
    const deviceRect = device.getBoundingClientRect();
    // profile-box has padding-top: 84px (status-bar height); subtract it so
    // the photo-col padding accounts for the remaining offset below that.
    photoColPadTop = Math.max(0, Math.round(photoRect.top - deviceRect.top - 84));
  }
  const modalLayer = document.getElementById('modal-layer');
  modalLayer.innerHTML = '<div class="modal-scrim">' + profileDetailHTML(photoColPadTop) + '</div>';
}

function closeProfileDetail() {
  const ml = document.getElementById('modal-layer');
  if (ml) ml.innerHTML = '';
}

function readAncsNotification(app) {
  // Remove this app's icon from the status bar ANCS row (simulates iOS
  // marking the notification as read via the ANCS PositiveAction, which
  // would send an ANCS Removed event back to the device in firmware).
  const icon = document.querySelector('.ancs-icon-wrap[data-app="' + app + '"]');
  if (icon) icon.remove();
  closeAncsDetail();
}

function updateStatusDateTime() {
  const el = document.getElementById('status-datetime');
  if (!el) return;
  const d = new Date();
  const h = d.getHours().toString().padStart(2, '0');
  const m = d.getMinutes().toString().padStart(2, '0');
  const date = d.toLocaleDateString(undefined, { weekday: 'short', month: 'short', day: 'numeric' });
  el.innerHTML = '<span class="t-time">' + h + ':' + m + '</span>' +
    '<span class="t-sep">·</span>' +
    '<span class="t-date">' + date + '</span>';
}

function renderStatusBar(cfg) {
  cfg = cfg || {};
  const dt = document.getElementById('status-datetime');
  const modeSlot = document.getElementById('mode-toggle-slot');
  const ancsRow = document.getElementById('ancs-row');
  const phoneSlot = document.getElementById('phone-disconnect-slot');
  dt.classList.toggle('hidden', !!cfg.hideDateTime);
  if (!cfg.hideDateTime) updateStatusDateTime();

  // Tap the date+time block to enter Clock mode; long-press to open the Calendar.
  dt.style.cursor = 'pointer';
  bindTapAndLongPress(dt, () => setScreen('clock'), () => setScreen('calendar'));

  const pcConnected = cfg.pcConnected !== false;
  const screenMode = (SCREENS[currentScreenId] && SCREENS[currentScreenId].mode) || 'calendar';
  const isMediaMode = screenMode === 'media';
  const isClockMode = screenMode === 'clock';

  // Mode-toggle visibility:
  //   • Hidden when PC offline (Media mode is useless without Orion) — EXCEPT in
  //     Clock mode where the toggle acts as a "return" button and must stay visible.
  //   • In Clock mode: calendar icon, neutral bg — "return to previous mode".
  //   • In Calendar mode: headphones icon — "tap to enter Media".
  //   • In Media mode: calendar icon, accent-tinted — "tap to return to Calendar".
  if (!pcConnected && !isClockMode) {
    modeSlot.innerHTML = '';
    // Real device auto-reverts to Calendar mode when Orion drops.
  } else {
    let toggleClass = 'mode-toggle';
    let toggleIcon = 'i-controls';
    let toggleTitle = 'Enter Media mode';
    let toggleTarget = 'kbd-mode';
    if (isClockMode) {
      toggleIcon = 'i-cal';
      toggleTitle = 'Return to previous mode';
      toggleTarget = previousScreenId || 'meeting-list';
    } else if (isMediaMode) {
      toggleClass += ' media-mode';
      toggleIcon = 'i-cal';
      toggleTitle = 'Exit Media mode';
      toggleTarget = 'meeting-list';
    }
    modeSlot.innerHTML =
      '<div class="' + toggleClass + '" id="mode-toggle" title="' + toggleTitle + '">' +
      '<svg viewBox="0 0 24 24"><use href="#' + toggleIcon + '"/></svg>' +
      '</div>';
    document.getElementById('mode-toggle').addEventListener('click', () => {
      setScreen(toggleTarget);
    });
  }

  if (cfg.phoneConnected) {
    ancsRow.innerHTML = (cfg.ancsApps || []).map(ancsIconHTML).join('');
  } else {
    ancsRow.innerHTML = '';
  }
  if (cfg.phoneConnected) {
    phoneSlot.innerHTML = '';
  } else {
    phoneSlot.innerHTML = '<div class="phone-disconnect-wrap" id="phone-disconnect-wrap" title="Long-press to manage phone pairing"><svg class="phone-disconnect" viewBox="0 0 24 24"><use href="#i-phone-broken"/></svg></div>';
    bindLongPress(document.getElementById('phone-disconnect-wrap'), () => {
      if (cfg.phonePaired) {
        window._unpairPrev = currentScreenId;
        setScreen('unpair-phone');
      } else {
        setScreen('repair-phone');
      }
    });
  }
}

function bindLongPress(el, action, ms) {
  if (!el) return;
  ms = ms || 1200;
  let timer = null;
  const start = () => { timer = setTimeout(action, ms); };
  const stop = () => { clearTimeout(timer); };
  el.addEventListener('mousedown', start);
  el.addEventListener('touchstart', start, { passive: true });
  ['mouseup', 'mouseleave', 'touchend', 'touchcancel'].forEach(ev => el.addEventListener(ev, stop));
}

// Like bindLongPress, but also handles a short tap — and suppresses the tap
// action when the long-press already fired. Uses single-slot `on*` assignment
// (not addEventListener) so re-binding on every renderStatusBar() call doesn't
// stack duplicate handlers on the persistent status-bar element.
function bindTapAndLongPress(el, onTap, onLongPress, ms) {
  if (!el) return;
  ms = ms || 1200;
  let timer = null;
  let longPressFired = false;
  const start = () => {
    longPressFired = false;
    timer = setTimeout(() => { longPressFired = true; onLongPress(); }, ms);
  };
  const cancel = () => { clearTimeout(timer); };
  el.onmousedown = start;
  el.ontouchstart = start;
  el.onmouseup = cancel;
  el.onmouseleave = cancel;
  el.ontouchend = cancel;
  el.ontouchcancel = cancel;
  el.onclick = (e) => {
    if (longPressFired) { e.preventDefault(); return; }
    onTap();
  };
}

let currentScreenId = null;
let previousScreenId = null;  // screen before entering Clock; used for the return tap
let _setupDoneTimer = null;

function setScreen(id) {
  const cfg = SCREENS[id];
  if (!cfg) return;
  // Track previous screen so Clock/Calendar's mode-toggle can return to it.
  if (currentScreenId && currentScreenId !== 'clock' && currentScreenId !== 'calendar') {
    previousScreenId = currentScreenId;
  }
  currentScreenId = id;
  document.querySelectorAll('.nav button').forEach(b => {
    b.classList.toggle('active', b.dataset.screen === id);
  });
  document.getElementById('meta-label').textContent = cfg.label;
  document.getElementById('meta-title').textContent = cfg.title;
  document.getElementById('meta-desc').textContent = cfg.desc;
  document.getElementById('device').classList.toggle('no-status-bar', !!cfg.hideStatusBar);
  if (!cfg.hideStatusBar) renderStatusBar(cfg.statusBar);

  // Presence-border colour on the profile photo. Source: cfg.presence
  // (default 'available'). If the PC link is down (statusBar.pcConnected
  // === false), the device-side rule forces 'offline' regardless of the
  // last-cached value pushed by Orion — the photo can't claim a presence
  // we can't currently verify. See `screen-layout.md` and `ble-protocol.md`
  // Presence Status characteristic.
  const photo = document.getElementById('profile-photo');
  if (photo) {
    photo.classList.remove('presence-available', 'presence-busy', 'presence-away', 'presence-offline');
    const pcConnected = !cfg.statusBar || cfg.statusBar.pcConnected !== false;
    const presence = pcConnected ? (cfg.presence || 'available') : 'offline';
    photo.classList.add('presence-' + presence);
  }
  const setupLayer = document.getElementById('setup-layer');
  const body = document.getElementById('body');
  const left = document.getElementById('left-panel');
  if (cfg.setup) {
    setupLayer.innerHTML = cfg.setup();
    setupLayer.style.display = 'block';
    body.style.visibility = 'hidden';
  } else {
    setupLayer.innerHTML = '';
    setupLayer.style.display = 'none';
    body.style.visibility = 'visible';
    if (id === 'calendar') _calViewDate = null; // always open on the current month
    left.innerHTML = cfg.leftRender ? cfg.leftRender() : '';
    if (cfg.mode === 'media') {
      bindAlbumArtGestures();
      bindSeekBar();
    }
    if (id === 'calendar') bindCalendarNav();
  }
  const modalLayer = document.getElementById('modal-layer');
  if (cfg.modal) {
    modalLayer.innerHTML = '<div class="modal-scrim" onclick="dismissModalIfTappable()">' + cfg.modal() + '</div>';
  } else {
    modalLayer.innerHTML = '';
  }
}

function closeModal() {
  document.getElementById('modal-layer').innerHTML = '';
}

function dismissModalIfTappable() {
  // All overlays are now closed via their explicit Close button only.
  // This function is kept for future use but no screen uses tap-to-dismiss.
}

const profilePhotoEl = document.getElementById('profile-photo');
bindLongPress(profilePhotoEl, () => setScreen('factory-reset'));
profilePhotoEl.addEventListener('click', showProfileDetail);

document.querySelectorAll('.nav button').forEach(b => {
  b.addEventListener('click', () => setScreen(b.dataset.screen));
});

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
  }[c]));
}

applyProfile();
setScreen('meeting-list');

setInterval(() => {
  if (currentScreenId === 'clock') {
    document.getElementById('left-panel').innerHTML = clockHTML();
  } else if (currentScreenId === 'calendar') {
    renderCalendar();
  }
  updateStatusDateTime();
}, 30 * 1000);
