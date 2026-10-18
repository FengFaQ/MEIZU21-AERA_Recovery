'use strict';
const workspace = {tab: 'install', folder: null, selected: null, loading: false, action: null, slotChosen: false,
  dismissed: setting('dismissedActivity'), localJob: null, serverJobId: '', download: null,
  selection: new Map(), entries: new Map(), selectionAnchor: '', batchDownload: false, batchAbort: null, uploadQueue: null, pendingFolder: null};
const activeJob = () => state.busy || workspace.batchDownload || !!workspace.uploadQueue || ['prepared','uploading','ready','queued','installing'].includes(state.job?.phase);
const installSource = document.createElement('fieldset');
installSource.className='install-source';installSource.hidden=true;
installSource.innerHTML='<legend class="sr-only">Install source</legend><label><input type="radio" name="installSource" value="file" checked>File</label><label><input type="radio" name="installSource" value="url">Download link</label>';
const urlFields = document.createElement('div');urlFields.className='install-url';urlFields.hidden=true;
urlFields.innerHTML='<label for="downloadUrl">Direct download URL</label><input id="downloadUrl" type="url" placeholder="https://example.com/recovery.zip" autocomplete="off" spellcheck="false"><div class="context-note">Direct ZIP or IMG downloads only. No download pages, login, or extra confirmation.</div>';
$('drop').before(installSource);$('drop').after(urlFields);
const urlMode = () => !state.saveFolder && installSource.querySelector('input:checked').value==='url';
function linkedFile() {
  try {
    const url=new URL($('downloadUrl').value.trim());
    if(!['http:','https:'].includes(url.protocol)||url.username||url.password)return null;
    const candidates=[url.searchParams.get('file'),url.searchParams.get('filename'),decodeURIComponent(url.pathname)];
    const name=candidates.filter(Boolean).map(path=>path.split('/').pop()).find(name=>/\.(zip|img)$/i.test(name))||'Download.zip';
    return {url:url.href,name,image:/\.img$/i.test(name)};
  }catch{return null}
}
function renderInstallSource() {
  const available=state.connected&&!!state.info?.url_download_available&&!state.saveFolder;
  installSource.hidden=!available;
  if(!available&&!activeJob())installSource.querySelector('[value="file"]').checked=true;
  const mode=available&&urlMode(),file=mode?linkedFile():null;
  urlFields.hidden=!mode;$('drop').hidden=mode;
  $('fileInfo').hidden=mode||!state.file;
  if(mode){
    $('imageFields').hidden=!file?.image;
    $('downloadUrl').disabled=activeJob()||!!state.info?.busy;
    $('transfer').disabled=!file||activeJob()||!!state.info?.busy||!$('storage').value||(file.image&&!$('partition').value);
    $('transferLabel').textContent=file?.image?'Download & flash':'Download & install';
  }
  for(const input of installSource.querySelectorAll('input'))input.disabled=activeJob()||!!state.info?.busy;
}
installSource.onchange=()=>{if(!urlMode())$('imageFields').hidden=!/\.img$/i.test(state.file?.name||'');controls()};
$('downloadUrl').oninput=()=>controls();
$('downloadUrl').onkeydown=event=>{if(event.key==='Enter'){event.preventDefault();$('transfer').click()}};
async function downloadLink() {
  const file=linkedFile();if(!file||activeJob()||!state.info?.url_download_available)return;
  workspace.urlConfirmation='';state.busy=true;state.cancelled=false;controls();notice('');
  try {
    const job=await api('download',{url:file.url,name:file.name,kind:file.image?'img':'zip',storage:$('storage').value,
      partition:$('partition').value,both_slots:$('slots').value==='both'});
    state.file=null;state.busy=false;updateJob(job);
  }catch(error){notice(error.message)}
  finally{state.busy=false;controls()}
}
const uploadFile=transfer;
$('transfer').onclick=()=>urlMode()?downloadLink():uploadFile();
const icon = name => {
  const svg = document.createElementNS('http://www.w3.org/2000/svg','svg');
  const use = document.createElementNS(svg.namespaceURI,'use');
  svg.setAttribute('class','icon'); use.setAttribute('href','#'+name); svg.append(use); return svg;
};
const payloadActivity = {id:'', signature:'', rows:new Map()};
const payloadPanel = document.createElement('div');
payloadPanel.id='payloadProgress';payloadPanel.className='payload-progress';payloadPanel.hidden=true;
const payloadHeading=document.createElement('h3');payloadHeading.textContent='Partitions';
const payloadSummary=document.createElement('div');payloadSummary.id='payloadProgressSummary';payloadSummary.className='small muted';
const payloadList=document.createElement('div');payloadList.className='payload-progress-list';
payloadPanel.append(payloadHeading,payloadSummary,payloadList);$('installerPrompt').before(payloadPanel);
function renderPayloadProgress(job){
  const progress=job.payload_progress,partitions=progress?.partitions||[];
  payloadPanel.hidden=!progress?.available||!partitions.length;
  if(payloadPanel.hidden){payloadActivity.id='';payloadActivity.signature='';payloadActivity.rows.clear();payloadList.replaceChildren();return}
  const signature=JSON.stringify(partitions.map(partition=>partition.name));
  if(payloadActivity.id!==job.id||payloadActivity.signature!==signature){
    payloadActivity.id=job.id;payloadActivity.signature=signature;payloadActivity.rows.clear();payloadList.replaceChildren();
    for(const partition of partitions){
      const row=document.createElement('div');row.className='payload-progress-row';
      const heading=document.createElement('div');heading.className='payload-progress-heading';
      const name=document.createElement('strong');name.textContent=partition.name;
      const status=document.createElement('span');status.className='payload-progress-state';
      heading.append(name,status);
      const bar=document.createElement('progress');bar.max=100;bar.value=0;bar.setAttribute('aria-label',partition.name+' progress');
      const amount=document.createElement('div');amount.className='payload-progress-amount';
      row.append(heading,bar,amount);payloadList.append(row);
      payloadActivity.rows.set(partition.name,{row,status,bar,amount});
    }
  }
  const labels={queued:'Queued',preparing:'Preparing',writing:'Writing',written:'Awaiting readback',verifying:'Verifying',verified:'Verified',failed:'Failed',not_flashed:'Not flashed'};
  let active=0,queued=0,verified=0,failed=0,notFlashed=0;
  for(const partition of partitions){
    const {row,status,bar,amount}=payloadActivity.rows.get(partition.name);
    const phase=Object.hasOwn(labels,partition.phase)?partition.phase:'queued';
    const percent=Math.max(0,Math.min(100,Number(partition.percent)||0));
    row.dataset.phase=phase;bar.value=percent;
    status.textContent=labels[phase]+(['writing','verifying'].includes(phase)?' / '+percent+'%':'');
    const megabytes=bytes=>(Number(bytes||0)/1000000).toFixed(1)+' MB';
    amount.textContent=partition.bytes?
      (['writing','verifying','failed'].includes(phase)?megabytes(partition.done)+' / ':'')+megabytes(partition.bytes):'';
    if(['writing','verifying'].includes(phase))active++;
    else if(['queued','preparing','written'].includes(phase))queued++;
    else if(phase==='verified')verified++;
    else if(phase==='failed')failed++;
    else if(phase==='not_flashed')notFlashed++;
  }
  payloadSummary.textContent=verified+' / '+partitions.length+' verified / '+active+' active / '+queued+' queued'+
    (failed?' / '+failed+' failed':'')+(notFlashed?' / '+notFlashed+' not flashed':'');
}
function showTab(tab) {
  if (tab === 'root' && document.querySelector('[data-tab="root"]').hidden) tab = 'install';
  if (workspace.tab !== tab && ['completed','failed','cancelled'].includes(state.job?.phase)) clearActivity();
  workspace.tab = tab;
  $('workspace').firstElementChild.hidden = tab !== 'install';
  for (const name of ['root','files','device','backups']) $(name+'Panel').hidden = tab !== name;
  for (const button of $('workspaceTabs').children) {
    const selected = button.dataset.tab === tab;
    button.classList.toggle('selected',selected); button.setAttribute('aria-pressed',String(selected));
  }
  if (tab === 'files' && !workspace.folder && $('fileStorage').value) loadFolder($('fileStorage').value);
  if (tab === 'device' && state.connected) loadFlashReports();
  activityVisibility();
}
function activityVisibility() {
  const empty = !state.busy && !workspace.batchDownload && (!state.job || state.job.phase === 'idle');
  const inspection = state.job?.root_action === 'inspect' && state.job?.phase === 'completed' && workspace.tab === 'root';
  const hidden = (empty && workspace.tab !== 'install') || inspection;
  $('workspace').children[1].hidden = hidden;
  $('workspace').classList.toggle('single-pane',hidden);
}
function clearActivity() {
  if (state.job?.id&&!workspace.localJob) {workspace.dismissed=state.job.id;store('dismissedActivity',workspace.dismissed)}
  workspace.localJob=null;
  $('rootInspection').hidden=true;
  originalUpdateJob({phase:'idle'});$('log').textContent='No active operation.';
  renderPayloadProgress({});
  $('installerDetails')?.setAttribute('hidden','');notice('');activityVisibility();
}
const originalConnected = connected;
connected = function(yes) { originalConnected(yes); $('workspaceTabs').hidden = !yes; if (yes) showTab(workspace.tab); };
const originalControls = controls;
controls = function() {
  originalControls();
  $('transferLabel').textContent=state.saveFolder?'Upload file':/\.img$/i.test(state.file?.name||'')?'Transfer & flash':'Transfer & install';
  $('transfer').hidden=state.job?.phase==='ready'&&!state.saveFolder;
  const downloading=workspace.download||workspace.batchAbort;
  if(downloading){$('cancel').hidden=false;$('cancel').disabled=false;$('cancel').textContent='Cancel download';}
  else $('cancel').textContent=state.busy||state.job?.phase==='uploading'?'Cancel upload':'Discard transfer';
  $('fileUploadCancel').hidden=(!state.saveFolder&&!state.job?.save_path)||(!state.busy&&!['prepared','uploading','ready'].includes(state.job?.phase));
  $('fileUploadCancel').disabled=state.cancelled||state.job?.owned===false;
  if(downloading){$('fileUploadCancel').hidden=false;$('fileUploadCancel').disabled=false;}
  $('fileUploadCancel').lastChild.textContent=downloading?' Cancel download':' Cancel upload';
  if(workspace.uploadQueue){
    $('cancel').hidden=$('fileUploadCancel').hidden=false;
    $('cancel').disabled=$('fileUploadCancel').disabled=workspace.uploadQueue.cancelled;
    $('cancel').textContent='Cancel uploads';
    $('fileUploadCancel').lastChild.textContent=workspace.uploadQueue.cancelled?' Cancelling...':' Cancel uploads';
    $('removeFile').disabled=true;$('drop').setAttribute('aria-disabled','true');
  }
  const imageTarget=state.info?.partitions.find(partition=>partition.path===$('partition').value);
  if(imageTarget&&!imageTarget.slot_select){$('slots').disabled=true;$('slots').value='current';}
  const blocked = !state.connected || activeJob() || workspace.batchDownload || !!state.info?.busy;
  for (const id of ['rootInspect','rootRefresh','rootPatch','rootRollback','rootManager'])
    $(id).disabled = blocked || !state.info?.root_available || !!state.info?.locked || !$('rootSlot').value;
  for (const button of document.querySelectorAll('[data-reboot]')) button.disabled = blocked;
  for (const id of ['fileUpload','folderNew','folderUp','folderRefresh','fileRename','fileDelete','fileDownload','fileInstall'])
    $(id).disabled = blocked || workspace.loading || !workspace.folder || (id.startsWith('file') && id !== 'fileUpload' && !workspace.selected);
  $('fileStorage').disabled = blocked || workspace.loading;
  const selected=Array.from(workspace.selection.values());
  $('fileDelete').disabled=blocked||workspace.loading||!selected.length;
  $('fileDownload').disabled=blocked||workspace.loading||!selected.length||selected.some(entry=>!entry.regular&&!entry.directory);
  $('fileSelectAll').disabled=blocked||workspace.loading||!workspace.entries.size;
  $('fileClearSelection').disabled=blocked||workspace.loading;
  for(const input of $('fileList').querySelectorAll('input'))input.disabled=blocked||workspace.loading;
  for(const button of $('fileList').querySelectorAll('button'))button.disabled=blocked||workspace.loading||button.dataset.special==='true';
  if (state.saveFolder) {
    $('imageFields').hidden = true; $('storage').disabled = true;
    $('transfer').disabled = !state.connected || !state.file || activeJob() || !!state.info?.busy;
  } else $('transfer').disabled = $('transfer').disabled || !!state.info?.busy;
  if(workspace.batchDownload){$('transfer').disabled=true;$('install').disabled=true;}
  if (state.job?.save_path) $('install').textContent = 'Save file';
  renderPackageInfo($('packageInfo'),!state.file||state.job?.name===state.file.name?state.job?.package_info:null);
  const packageInfo=state.job?.package_info;
  if(packageInfo?.error||(packageInfo?.arb_decision==='downgrade'&&!packageInfo?.advanced_available)) $('install').disabled=true;
  renderInstallSource();
  activityVisibility();
};
const originalUpdateInfo = updateInfo;
updateInfo = function(info) {
  originalUpdateInfo(info);
  document.querySelector('[data-tab="root"]').hidden = !info.root_enabled;
  if (!info.root_enabled && workspace.tab === 'root') showTab('install');
  $('workspaceTabs').hidden = !state.connected;
  options($('fileStorage'),info.storages||[],s=>`${s.name} / ${size(s.free)} free`);
  const slotSignature = `${info.boot_slot}|${info.slot}`;
  if ($('rootSlot').dataset.signature !== slotSignature) {
    const selected = $('rootSlot').value; $('rootSlot').replaceChildren();
    const active = (info.slot||'').toLowerCase().replace(/^_/,'') || info.boot_slot || '';
    $('rootSlot').add(new Option('Select slot',''));
    for (const slot of ['a','b']) $('rootSlot').add(new Option(`Slot ${slot.toUpperCase()}${slot===active?' / active':' / inactive'}${slot===info.boot_slot?' / running recovery':''}`,slot));
    $('rootSlot').value = workspace.slotChosen && selected ? selected : active;
    $('rootSlot').dataset.signature = slotSignature;
  }
  $('rootTarget').textContent = $('rootSlot').value ? 'init_boot_'+$('rootSlot').value : 'init_boot';
  $('rootUnavailable').hidden = !!info.root_available && !info.locked;
  $('rootUnavailable').textContent = info.locked ? 'Unlock internal storage for verified rollback backups.' :
    'This device does not expose the init_boot / KernelSU patching backend. Root is unavailable; other tools remain usable.';
  const memory = /MemAvailable:\s+(\d+) kB/.exec(info.memory||'');
  const total = /MemTotal:\s+(\d+) kB/.exec(info.memory||'');
  const uptime = Math.floor(parseFloat(info.uptime||'0') / 60);
  const values = [
    ['Device',info.device],['Version',info.version],['Kernel',info.kernel],['Architecture',info.architecture],
    ['Recovery slot',info.boot_slot?.toUpperCase()||'Unknown'],['Selected slot',info.slot||'--'],
    ['Storage',info.locked?'Locked':'Unlocked'],['Battery',info.battery?`${info.battery}% / ${info.battery_status||''}`:'Unavailable'],
    ['Memory',memory&&total?`${size(Number(memory[1])*1024)} available / ${size(Number(total[1])*1024)}`:'Unavailable'],
    ['Uptime',`${Math.floor(uptime/60)}h ${uptime%60}m`],
    ['Wi-Fi address',info.ip_address||'Not connected'],['USB address',info.usb_address||'--']
  ];
  const details = $('deviceDetails'); details.replaceChildren();
  for (const [label,value] of values) {
    const row=document.createElement('div'),term=document.createElement('dt'),detail=document.createElement('dd');
    term.textContent=label;detail.textContent=value||'--';row.append(term,detail);details.append(row);
  }
  $('certificateFingerprint').textContent = (info.certificate||'').match(/.{1,2}/g)?.join(':')||'--';
  $('certificateState').textContent = info.persistent_certificate ? 'Private device authority / SHA-256 fingerprint' :
    'Temporary authority: writable persistent data is unavailable. Do not permanently trust this session certificate.';
  controls();
  if(workspace.pendingFolder&&workspace.tab==='files'&&!info.busy&&!activeJob()&&!workspace.loading){
    const pending=workspace.pendingFolder;workspace.pendingFolder=null;loadFolder(pending.path,pending.append);
  }
};
const originalUpdateJob = updateJob;
function renderUploadProgress(job) {
  const transfer=job.upload_transfer;
  if(!transfer?.active)return;
  const percent=job.bytes?100*Math.min(job.received,job.bytes)/job.bytes:0;
  $('statusTitle').textContent=job.cancel_requested?'Cancelling transfer':transfer.finalizing?'Finishing transfer':job.from_url?'Downloading on recovery':'Receiving file on recovery';
  $('statusDetail').textContent=job.name;
  $('progressLabel').textContent='Transfer';$('progress').value=percent;$('percent').textContent=Math.floor(percent)+'%';
  if(!job.bytes){$('progress').removeAttribute('value');$('percent').textContent='Downloading';}
  const mb=bytes=>(Number(bytes||0)/1000000).toFixed(1)+' MB';
  $('bytes').textContent=mb(job.received)+(job.bytes?' / '+mb(job.bytes):'');
  const seconds=Math.max(0,Math.round(transfer.remaining_seconds));
  const eta=transfer.remaining_seconds>=0?' / ~'+Math.floor(seconds/60)+':'+String(seconds%60).padStart(2,'0')+' remaining':'';
  $('speed').textContent=transfer.finalizing?'Finishing up':mb(transfer.bytes_per_second)+'/s'+eta;
}
function renderRootInspection(job) {
  const panel=$('rootInspection');
  const visible=job.root_action==='inspect'&&job.phase==='completed'&&job.root_slot===$('rootSlot').value;
  panel.hidden=!visible;
  if(!visible)return;
  const signature=JSON.stringify([job.id,job.root_inspection,job.detail,state.info?.slot]);
  if(panel.dataset.signature===signature)return;
  panel.dataset.signature=signature;panel.replaceChildren();
  const result=job.root_inspection;
  const title=document.createElement('h3');
  if(!result?.target){
    title.textContent='Target inspection';
    const detail=document.createElement('div');detail.className='legacy-result';
    detail.textContent=job.detail||'Inspection finished.';panel.append(title,detail);return;
  }
  title.textContent=!result.inspected?'Inspection incomplete':result.patched?'Root patch detected':'No KernelSU patch detected';
  title.classList.toggle('verified',!!result.verified);panel.append(title);
  const details=document.createElement('dl');details.className='review';
  const active=(state.info?.slot||state.info?.boot_slot||'').toLowerCase().replace(/^_/,'');
  const rows=[['Target',result.target+(active?` / ${job.root_slot===active?'active':'inactive'} slot`:'')],
    ['Kernel',result.kernel],
    ['Root',!result.inspected?'Unknown':result.patched?(result.provider||'KernelSU-compatible'):'No KernelSU LKM detected'],
    ['Version',!result.inspected?'Unknown':result.patched?(result.version||'Unknown / no AERA receipt'):'Not applicable'],
    ['Verification',!result.inspected?'Could not verify':result.verified?'Verified AERA patch':
      result.patched?'Detected / no AERA verification':'No KernelSU LKM detected']];
  for(const [label,value] of rows){
    const row=document.createElement('div'),term=document.createElement('dt'),detail=document.createElement('dd');
    term.textContent=label;detail.textContent=value||'Unknown';row.append(term,detail);details.append(row);
  }
  panel.append(details);
  if(!result.inspected){const detail=document.createElement('p');detail.className='context-note';detail.textContent=result.detail;panel.append(detail);}
  const offline=document.createElement('div');offline.className='offline-module';offline.textContent='Available offline module';
  const module=document.createElement('strong');
  const providers={kernelsu:'KernelSU',next:'KernelSU Next',sukisu:'SukiSU Ultra'};
  module.textContent=result.offline_available?`${providers[result.offline_provider]||result.offline_provider} ${result.offline_version||'/ version unknown'}`:
    'No matching module / check latest release';offline.append(module);panel.append(offline);
}
updateJob = function(job) {
  if(workspace.batchDownload&&job!==workspace.localJob)return;
  if (state.busy) {
    if(state.upload&&job.id===state.job?.id){
      state.receivingJob=job;
      renderUploadProgress(job);
      if(job.cancel_requested||job.phase==='cancelled'){
        state.cancelled=true;state.upload.abort();notice('Upload cancelled. No installation was started.');
      }
    }
    return;
  }
  if(workspace.localJob){
    if(job.id&&job.id!==workspace.serverJobId&&job.id!==workspace.localJob.id)workspace.localJob=null;
    else job=workspace.localJob;
  }
  if (job.id===workspace.dismissed && ['completed','failed','cancelled'].includes(job.phase)) job={phase:'idle'};
  originalUpdateJob(job);
  renderUploadProgress(job);
  if(job.from_url&&job.phase==='ready'){
    $('statusDetail').textContent='Downloaded / ready to install';
    if(job.owned!==false&&workspace.urlConfirmation!==job.id){workspace.urlConfirmation=job.id;showConfirmation();}
  }
  renderRootInspection(job);
  if (job.root_action) {
    $('progressLabel').textContent = 'Root operation';
    $('statusTitle').textContent = job.phase === 'completed' ? 'Root operation complete' : job.phase === 'failed' ? 'Root operation failed' : job.phase === 'queued' ? 'Root operation queued' : 'Working on root';
    if (job.root_action==='inspect') {
      $('statusTitle').textContent=job.phase==='completed'?'Target inspection':job.phase==='failed'?'Inspection failed':'Inspecting target';
      if(job.phase==='completed'){$('statusDetail').textContent='';notice('');}
    } else if (job.phase === 'completed') notice('Root operation completed.','success');
  } else if (job.save_path) {
    $('progressLabel').textContent='Upload';
    if(!job.log&&job.phase==='completed')$('log').textContent='File saved to '+job.save_path;
    $('statusTitle').textContent = job.phase === 'ready' ? 'Ready to save' : job.phase === 'completed' ? 'File saved' : $('statusTitle').textContent;
    if (job.phase === 'completed') notice(job.detail,'success');
  } else if(job.download_name){
    $('progressLabel').textContent='Download';
    $('statusTitle').textContent=job.phase==='completed'?'Download complete':'Download interrupted';
    notice(job.detail,job.phase==='completed'?'success':'');
  }
  renderInstallerDetails(job.presentation);
  renderPayloadProgress(job);
  if(['fast','selected','direct','extract'].includes(job.install_method)) {
    const extract=job.install_method==='extract';
    $('progressLabel').textContent=extract?'Extraction':'Payload flash';
    const titles={queued:extract?'Extraction queued':'Payload flash queued',installing:extract?'Extracting images':'Flashing payload images',
      completed:extract?'Images extracted':'Payload flash complete',failed:extract?'Extraction failed':'Payload flash failed'};
    if(titles[job.phase])$('statusTitle').textContent=titles[job.phase];
    if(job.phase==='completed')notice(extract?'Images extracted and verified.':'Payload flash completed.','success');
  }
  activityVisibility();
  controls();
};
const originalSelectFile = selectFile;
selectFile = function(file) {
  if(workspace.uploadQueue)return;
  if (!activeJob()) state.saveFolder = '';
  originalSelectFile(file);
  if(state.file===file&&file&&/\.img$/i.test(file.name)){
    const name=file.name.toLowerCase().replace(/\.img$/,'');
    const target=state.info?.partitions.find(partition=>partition.path.split('/').pop()===name);
    if(target)$('partition').value=target.path;
    controls();
  }
};
const originalConfirmation = showConfirmation;
const installationMethod=()=>$('installMethodFields').hidden?'normal':$('installMethod').value;
const selectedPayloadImages=()=>Array.from($('payloadChoices').querySelectorAll('input:checked')).map(input=>input.value);
function renderPayloadSelection() {
  const method=installationMethod(),info=state.job?.package_info;
  const advanced=['selected','direct','extract'].includes(method);
  $('payloadSelection').hidden=!advanced;
  if(!advanced)return;
  $('payloadChoices').replaceChildren();
  for(const partition of info?.partitions||[]) {
    const row=document.createElement('label');row.className='payload-choice';
    const input=document.createElement('input');input.type='checkbox';input.value=partition.name;
    input.disabled=!(method==='extract'?partition.extractable:partition.flash_available);
    const text=document.createElement('span'),name=document.createElement('strong'),detail=document.createElement('small');
    name.textContent=partition.name;
    detail.textContent=size(partition.bytes)+(input.disabled?' / unavailable':method!=='extract'&&partition.protected?' / protected; select manually to override':'');
    text.append(name,detail);row.append(input,text);$('payloadChoices').append(row);
    input.onchange=updateInstallationChoice;
  }
}
$('payloadAll').onclick=()=>{
  const info=state.job?.package_info,extract=installationMethod()==='extract';
  for(const input of $('payloadChoices').querySelectorAll('input'))
    if(!input.disabled&&(extract||!info.partitions.find(partition=>partition.name===input.value)?.protected)) input.checked=true;
  updateInstallationChoice();
};
$('payloadNone').onclick=()=>{for(const input of $('payloadChoices').querySelectorAll('input'))input.checked=false;updateInstallationChoice();};
function updateConfirmEnabled() {
  const info=state.job?.package_info;
  const method=installationMethod(),extract=method==='extract',advanced=['selected','direct','extract'].includes(method);
  $('confirmInstall').disabled=state.job?.phase!=='ready'||workspace.confirming||workspace.cancellingConfirmation||
    !!info?.error||(!extract&&(info?.arb_decision==='downgrade'||
      (info?.arb_decision==='upgrade'&&!$('arbAcknowledged').checked)))||
    (method==='fast'&&(!info?.fast_available||!$('fastAcknowledged').checked))||
    (advanced&&(!info?.advanced_available||!selectedPayloadImages().length||
      (method==='direct'&&!info?.direct_available)||(!extract&&!$('fastAcknowledged').checked)));
}
function updateInstallationChoice() {
  const info=state.job?.package_info;
  const method=installationMethod(),fast=method==='fast',extract=method==='extract',advanced=['selected','direct','extract'].includes(method);
  const names=selectedPayloadImages(),images=(info?.partitions||[]).filter(partition=>names.includes(partition.name));
  const overridden=images.filter(partition=>partition.protected).map(partition=>partition.name);
  $('fastConsent').hidden=method==='normal'||extract;
  $('arbConsent').hidden=extract||info?.arb_decision!=='upgrade';
  $('flashConsentText').textContent=fast||method==='direct'?
    'I understand this experimental flash writes to the current slot and bypasses the normal OTA installer.':
    'I understand the selected images overwrite current-slot partitions without running the normal OTA installer.';
  $('confirmWarning').textContent=fast?
    `Writes payload images directly to current slot ${info.fast_slot}, with SHA-256 readback verification.${info.fast_protected?.length?' Protected partitions are not written: '+info.fast_protected.join(', ')+'.':''} No temporary extraction, slot switch, normal OTA compatibility checks, snapshot rollback or postinstall. An interrupted flash may leave this slot unbootable. Do not reboot after failure; reflash known-good firmware.`:
    extract?'Saves verified images in a new folder. No device partitions are written.':advanced?
    `${method==='direct'?'Streams selected images directly, without temporary extraction, and verifies SHA-256 readback.':'Extracts and verifies all selected images before writing.'} Writes to current slot ${info.fast_slot}. No normal OTA compatibility checks, snapshot rollback, postinstall or slot switch. An interrupted flash can leave this slot unbootable.${overridden.length?' Manual protection override: '+overridden.join(', ')+' will be overwritten.':''}`:workspace.installWarning;
  $('confirmInstall').textContent=fast?'Fast flash':extract?'Extract images':advanced?'Flash selected':state.job?.save_path?'Save file':state.job?.partition?'Flash image':'Install ZIP';
  $('methodSummary').hidden=!info?.is_payload;
  $('methodSummary').textContent=extract?`Destination: ${state.info?.extraction_root||'Current storage / AERA/Extracted'}`:info?.is_payload?
    `${fast||advanced?'Target: current slot '+info.fast_slot+' (unchanged)':'Method: normal package installer'}\nKeep current ABL: ${state.info?.preserve_abl?'On / '+(overridden.includes('abl')?'manual override':fast||advanced?'not flashed':'protected'):'Off'}\nKeep AERA installed: ${state.info?.preserve_recovery?'On / '+(overridden.includes('recovery')?'manual override':fast||advanced?'not flashed':'protected'):'Off'}`:'';
  $('payloadCount').textContent=`${names.length} selected / ${size(images.reduce((total,partition)=>total+(partition.bytes||0),0))}`;
  updateConfirmEnabled();
}
$('installMethod').onchange=()=>{$('fastAcknowledged').checked=false;renderPayloadSelection();updateInstallationChoice();};
$('fastAcknowledged').onchange=updateConfirmEnabled;
showConfirmation = function() {
  originalConfirmation();
  if (state.job?.save_path && state.job.phase === 'ready') {
    $('confirmTitle').textContent = 'Save verified file?'; $('confirmInstall').textContent = 'Save file';
    $('review').replaceChildren();reviewLine('File',state.job.name);reviewLine('Size',size(state.job.bytes));reviewLine('Destination',state.job.save_path);
    $('confirmWarning').textContent = 'Save this file without flashing it. Existing files are never overwritten.';
  } else $('confirmWarning').textContent = 'The file has been transferred and verified. Installation will run on the device.';
  const info=state.job?.package_info;
  if(state.job?.partition&&!state.info?.partitions.find(target=>target.path===state.job.partition)?.slot_select){
    for(const row of $('review').children)if(row.firstElementChild.textContent==='Slots')row.lastElementChild.textContent='Single partition';
  }
  const imageWarning=state.job?.partition?'Flashing overwrites the selected partition. A wrong image or firmware rollback can leave the device unbootable.':'';
  if(imageWarning)$('confirmWarning').textContent=imageWarning;
  renderPackageInfo($('confirmPackage'),info);
  if(info?.is_payload) $('confirmTitle').textContent='Review Android update';
  $('arbAcknowledged').checked=false;$('arbConsent').hidden=info?.arb_decision!=='upgrade';
  $('confirmInstall').disabled=!!info?.error||info?.arb_decision==='downgrade'||info?.arb_decision==='upgrade';
  if(state.job?.source_path){
    reviewLine('Source',state.job.source_path);
    $('confirmWarning').textContent=(imageWarning?imageWarning+'\n\n':'')+'This file is already on the device. Installation will use it directly and will not delete it afterward.';
  }
  workspace.installWarning=$('confirmWarning').textContent;
  $('installMethod').value='normal';$('fastAcknowledged').checked=false;
  $('installMethodFields').hidden=!info?.is_payload||!!info?.incremental;
  $('installMethod').options[1].disabled=!info?.fast_available;
  for(const option of Array.from($('installMethod').options).slice(2))
    option.disabled=!info?.advanced_available||(option.value==='direct'&&!info?.direct_available);
  $('fastUnavailable').hidden=!!info?.fast_available||!info?.is_payload;
  $('fastUnavailable').textContent=info?.fast_reason||'Use normal installation for this package.';
  renderPayloadSelection();updateInstallationChoice();
};
$('install').onclick = showConfirmation;
const originalDiscard=discard;
$('cancel').onclick=()=>{const download=workspace.download||workspace.batchAbort;if(download)download.abort();else if(workspace.uploadQueue)cancelFileUploads();else originalDiscard()};
async function cancelConfirmation() {
  if(workspace.confirming||workspace.cancellingConfirmation)return;
  if(state.job?.phase!=='ready'){$('confirm').close();return;}
  workspace.cancellingConfirmation=true;
  const id=state.job.id;
  $('confirmCancel').disabled=true;$('confirmInstall').disabled=true;
  try{
    const job=await api('cancel',{id});
    updateJob(job);
    state.file=null;state.saveFolder='';state.saveRoot='';$('fileInput').value='';
    $('fileInfo').hidden=true;$('imageFields').hidden=true;$('packageInfo').hidden=true;
    clearActivity();$('confirm').close();controls();
  }catch(error){notice(error.message);}
  finally{
    workspace.cancellingConfirmation=false;$('confirmCancel').disabled=false;
    updateConfirmEnabled();
  }
}
$('confirmCancel').onclick=cancelConfirmation;
$('confirm').oncancel=event=>{event.preventDefault();cancelConfirmation();};
$('arbAcknowledged').onchange=updateConfirmEnabled;
$('confirmInstall').onclick=async()=>{
  if(state.job?.phase!=='ready'||$('confirmInstall').disabled||workspace.confirming||workspace.cancellingConfirmation)return;
  workspace.confirming=true;$('confirmInstall').disabled=true;$('confirmCancel').disabled=true;
  try{updateJob(await api('install',{id:state.job.id,confirm:true,arb_acknowledged:$('arbAcknowledged').checked,
    install_method:$('installMethodFields').hidden?'normal':$('installMethod').value,
    partitions:selectedPayloadImages(),
    fast_acknowledged:$('fastAcknowledged').checked}));$('confirm').close();notice('')}
  catch(error){notice(error.message);$('confirm').close()}
  finally{workspace.confirming=false;updateConfirmEnabled();$('confirmCancel').disabled=false;}
};
function confirmation(title,text,run,name) {
  workspace.action = run; $('actionTitle').textContent=title; $('actionText').textContent=text;
  $('actionField').hidden = name === undefined; $('actionName').value = name||'';
  $('actionRun').disabled = false; $('actionRun').textContent = 'Confirm'; $('actionConfirm').showModal();
  if (name !== undefined) { $('actionName').focus(); $('actionName').select(); }
}
async function runAction() {
  if (!workspace.action || $('actionRun').disabled) return;
  $('actionRun').disabled=true;
  try { await workspace.action($('actionName').value.trim()); $('actionConfirm').close(); }
  catch(error) { $('actionConfirm').close(); notice(error.message); }
  finally { workspace.action=null; $('actionRun').disabled=false; await refresh().catch(()=>{}); }
}
$('actionRun').onclick=runAction;$('actionCancel').onclick=()=>$('actionConfirm').close();
$('actionName').onkeydown=event=>{if(event.key==='Enter'){event.preventDefault();runAction()}};
for(const button of $('workspaceTabs').children) button.onclick=()=>showTab(button.dataset.tab);
$('rootSlot').onchange=()=>{ workspace.slotChosen=true;$('rootTarget').textContent='init_boot_'+$('rootSlot').value;renderRootInspection(state.job||{});controls(); };
async function rootOperation(action) {
  const slot=$('rootSlot').value,provider=$('rootProvider').value;
  const run=async()=>{notice('');updateJob(await api('root',{action,slot,provider,confirm:true}));};
  if(action==='inspect'||action==='refresh') {try{await run()}catch(error){notice(error.message)}return}
  const providerName=$('rootProvider').selectedOptions[0].textContent;
  const text=action==='patch'?`${providerName}\nTarget: init_boot_${slot}\n\nThe target boot kernel must match an exact module. AERA saves a verified full rollback image before writing. This does not switch slots or reboot.`:
    action==='rollback'?`Restore the latest verified AERA backup to init_boot_${slot}. This overwrites the selected partition.`:
    `${providerName}\nDownload and stage the manager APK for installation when Android starts. This does not patch a partition.`;
  confirmation(action==='patch'?'Patch selected slot?':action==='rollback'?'Restore root backup?':'Install manager APK?',text,run);
}
for(const [id,action] of [['rootInspect','inspect'],['rootRefresh','refresh'],['rootPatch','patch'],['rootRollback','rollback'],['rootManager','manager']]) $(id).onclick=()=>rootOperation(action);
for(const button of document.querySelectorAll('[data-reboot]')) button.onclick=()=>{
  const target=button.dataset.reboot;
  confirmation(target==='poweroff'?'Power off device?':`Reboot to ${button.textContent.trim()}?`,
    'The connection will close. No slot is changed by this command.',async()=>{
      await api('reboot',{target,confirm:true});notice(target==='poweroff'?'Power off requested.':'Reboot requested.','success');
    });
};
async function loadFolder(path,append=false) {
  if(activeJob()||state.info?.busy){workspace.pendingFolder={path,append};$('fileCount').textContent='Waiting for recovery';return;}
  workspace.pendingFolder=null;
  if(workspace.loading)return;workspace.loading=true;controls();
  if(!append){$('fileList').replaceChildren();const waiting=document.createElement('div');waiting.className='file-empty';waiting.textContent='Loading folder...';$('fileList').append(waiting);workspace.selection.clear();workspace.entries.clear();workspace.selectionAnchor='';updateSelection();}
  try{
    const result=await api('files/list',{path,offset:append?workspace.folder?.next||0:0});
    workspace.folder=result;$('folderPath').textContent=result.path;$('fileCount').textContent=result.total+' entries';
    if(!append)$('fileList').replaceChildren();
    for(const entry of result.entries){
      const row=document.createElement('div');row.className='file-row';row.dataset.path=entry.path;
      const open=document.createElement('button');open.className='entry-open';
      open.dataset.special=String(!entry.directory&&!entry.regular);open.disabled=!entry.directory&&!entry.regular;
      if(entry.directory||entry.regular){
        workspace.entries.set(entry.path,entry);
        const choice=document.createElement('input');choice.type='checkbox';choice.setAttribute('aria-label','Select '+entry.name);
        choice.onchange=()=>{if(choice.checked)workspace.selection.set(entry.path,entry);else workspace.selection.delete(entry.path);workspace.selectionAnchor=entry.path;updateSelection()};
        row.append(choice);
      }
      const text=document.createElement('div');text.style.minWidth='0';
      const name=document.createElement('div');name.className='entry-name';name.textContent=entry.name;
      const meta=document.createElement('div');meta.className='entry-meta';
      meta.textContent=(entry.directory?'Folder':entry.regular?size(entry.bytes):'Special / link')+' / '+new Date(entry.modified*1000).toLocaleString();
      text.append(name,meta);open.append(icon(entry.directory?'folder':'file'),text);row.append(open);
      open.onclick=event=>{if(entry.directory&&!event.ctrlKey&&!event.metaKey&&!event.shiftKey)loadFolder(entry.path);else selectEntry(entry,event)};
      open.oncontextmenu=event=>{event.preventDefault();if(!open.disabled)selectEntry(entry,{ctrlKey:true})};
      $('fileList').append(row);
    }
    if(!result.total){const empty=document.createElement('div');empty.className='file-empty';empty.textContent='Empty folder';$('fileList').append(empty);}
    $('filesMore').hidden=result.next===null;$('folderUp').disabled=result.path===result.root;
    $('fileSelectAllLabel').textContent=result.next===null?'Select all':'Select loaded';updateSelection();
    if(result.truncated)notice('This folder has more than 10,000 entries. Only the first scanned entries are listed.','');
  }catch(error){notice(error.message);if(!append)$('fileList').replaceChildren();}
  finally{workspace.loading=false;controls();if(workspace.folder)$('folderUp').disabled=$('folderUp').disabled||workspace.folder.path===workspace.folder.root;}
}
function selectEntry(entry,event={}){
  if(event.shiftKey&&workspace.selectionAnchor){
    const paths=Array.from(workspace.entries.keys());
    const from=paths.indexOf(workspace.selectionAnchor),to=paths.indexOf(entry.path);
    if(from>=0&&to>=0)for(const path of paths.slice(Math.min(from,to),Math.max(from,to)+1))workspace.selection.set(path,workspace.entries.get(path));
  }else if(event.ctrlKey||event.metaKey){
    if(workspace.selection.has(entry.path))workspace.selection.delete(entry.path);else workspace.selection.set(entry.path,entry);
  }else{workspace.selection.clear();workspace.selection.set(entry.path,entry)}
  workspace.selectionAnchor=entry.path;updateSelection();
}
function updateSelection(){
  const entries=Array.from(workspace.selection.values()),single=entries.length===1?entries[0]:null;
  workspace.selected=single;
  for(const row of $('fileList').querySelectorAll('.file-row')){
    const selected=workspace.selection.has(row.dataset.path);row.classList.toggle('selected',selected);
    const input=row.querySelector('input');if(input)input.checked=selected;
  }
  $('fileSelection').hidden=!entries.length;
  $('selectedName').textContent=single?single.name:entries.length+' selected';
  const bytes=entries.reduce((total,entry)=>total+(entry.regular?entry.bytes:0),0);
  $('selectedDetails').textContent=single?(single.directory?'Only empty folders can be deleted.':size(single.bytes)+' / '+new Date(single.modified*1000).toLocaleString()):size(bytes)+' / '+entries.filter(entry=>entry.directory).length+' folders';
  $('fileDownload').hidden=entries.some(entry=>!entry.regular&&!entry.directory);
  $('fileDownload').lastChild.textContent=entries.length>1?' Download selected':' Download';
  $('fileRename').hidden=!single;
  $('fileInstall').hidden=!single?.regular||!single.bytes||!(/\.(zip|img)$/i.test(single.name));
  $('fileInstallLabel').textContent=/\.img$/i.test(single?.name||'')?'Flash image':'Install ZIP';
  $('fileSelectedCount').textContent=entries.length?entries.length+' selected':'';
  $('fileClearSelection').hidden=!entries.length;
  $('fileSelectAll').checked=!!workspace.entries.size&&entries.length===workspace.entries.size;
  $('fileSelectAll').indeterminate=!!entries.length&&entries.length<workspace.entries.size;
  controls();
}
$('fileSelectAll').onchange=()=>{workspace.selection.clear();if($('fileSelectAll').checked)for(const [path,entry] of workspace.entries)workspace.selection.set(path,entry);updateSelection()};
$('fileClearSelection').onclick=()=>{workspace.selection.clear();updateSelection()};
async function prepareExistingFile(entry,image={}){
  if(!entry||activeJob())return;
  state.busy=true;controls();notice('Reading package information...');
  try{
    const job=await api('files/prepare',{path:entry.path,...image});
    state.busy=false;state.file=null;state.saveFolder='';state.saveRoot='';
    showTab('install');
    $('fileInfo').hidden=false;$('fileName').textContent=entry.name;
    $('fileDetails').textContent=size(entry.bytes)+' / On device';$('imageFields').hidden=true;
    updateJob(job);notice('');showConfirmation();
  }catch(error){notice(error.message)}
  finally{state.busy=false;controls()}
}
$('fileInstall').onclick=()=>{
  const entry=workspace.selected;if(!entry||activeJob())return;
  if(!/\.img$/i.test(entry.name)){prepareExistingFile(entry);return;}
  $('localImageName').textContent=entry.name;
  options($('localImagePartition'),state.info.partitions||[],partition=>partition.name,'Select partition');
  const name=entry.name.toLowerCase().replace(/\.img$/,'');
  $('localImagePartition').value=state.info.partitions.find(partition=>partition.path.split('/').pop()===name)?.path||'';
  $('localImageSlots').value='current';updateLocalImageTarget();$('localImageConfirm').showModal();
};
function updateLocalImageTarget(){
  const partition=state.info?.partitions.find(item=>item.path===$('localImagePartition').value);
  $('localImageReview').disabled=!partition;
  $('localImageSlots').disabled=!!partition?.logical||!partition?.slot_select;
  if($('localImageSlots').disabled)$('localImageSlots').value='current';
}
$('localImagePartition').onchange=updateLocalImageTarget;
$('localImageCancel').onclick=()=>$('localImageConfirm').close();
$('localImageReview').onclick=()=>{
  if($('localImageReview').disabled)return;
  const target={partition:$('localImagePartition').value,both_slots:$('localImageSlots').value==='both'};
  $('localImageConfirm').close();prepareExistingFile(workspace.selected,target);
};
$('fileStorage').onchange=()=>{workspace.folder=null;loadFolder($('fileStorage').value)};
$('folderRefresh').onclick=()=>loadFolder(workspace.folder.path);$('folderUp').onclick=()=>loadFolder(workspace.folder.parent);
$('filesMore').onclick=()=>loadFolder(workspace.folder.path,true);
$('folderNew').onclick=()=>confirmation('New folder',workspace.folder.path,async name=>{await api('files/mkdir',{path:workspace.folder.path,name,confirm:true});await loadFolder(workspace.folder.path)},'');
$('fileRename').onclick=()=>{const entry=workspace.selected;if(!entry||activeJob())return;confirmation('Rename',entry.name,async name=>{await api('files/rename',{path:entry.path,name,confirm:true});await loadFolder(workspace.folder.path)},entry.name)};
$('fileDelete').onclick=()=>{
  const entries=Array.from(workspace.selection.values());if(!entries.length||activeJob())return;
  const folder=workspace.folder.path;
  const list=entries.slice(0,10).map(entry=>entry.name).join('\n')+(entries.length>10?'\n+ '+(entries.length-10)+' more':'');
  confirmation(entries.length===1?'Delete selected item?':'Delete '+entries.length+' selected items?',list+'\n\nThis cannot be undone. Only empty folders can be deleted.',async()=>{
    state.busy=true;controls();let removed=0;
    try{for(const entry of entries){await api('files/delete',{path:entry.path,confirm:true});removed++}notice(removed+' items deleted.','success')}
    catch(error){throw new Error(removed+' / '+entries.length+' deleted. '+error.message)}
    finally{state.busy=false;await loadFolder(folder);controls()}
  });
};
async function downloadEntry(entry,directory=null,batchAbort=null){
  if(!entry||state.busy)return false;
  workspace.downloadError='';
  let writer=null,watchdog=null;
  const abort=batchAbort||new AbortController();let complete=false;
  try{
    if(directory){
      try{await directory.getFileHandle(entry.name);throw new Error(entry.name+' already exists in the chosen computer folder. Nothing was overwritten.')}
      catch(error){if(error.name!=='NotFoundError')throw error}
      if(abort.signal.aborted)throw new DOMException('Download cancelled.','AbortError');
      const handle=await directory.getFileHandle(entry.name,{create:true});writer=await handle.createWritable();
    }else if(entry.bytes>64*1024*1024){
      if(typeof window.showSaveFilePicker!=='function'){
        confirmation('Download large file?',entry.name+'\n\nThis browser cannot stream into a local file. Import the recovery certificate first, then continue with a normal browser download.',async()=>{
          const result=await api('files/download',{path:entry.path});const link=document.createElement('a');link.href=result.url;link.download=entry.name;link.click();
          notice('Download sent to your browser. Check its Downloads panel for certificate warnings.','success');
        });return;
      }
      const handle=await window.showSaveFilePicker({suggestedName:entry.name});
      writer=await handle.createWritable();
    }
    if(!workspace.batchDownload){
      const serverJobId=workspace.localJob?workspace.serverJobId:state.job?.id||'';
      if(['completed','failed','cancelled'].includes(state.job?.phase))clearActivity();
      workspace.serverJobId=serverJobId;notice('');
    }
    workspace.download=abort;
    state.busy=true;controls();activityVisibility();
    $('statusTitle').textContent='Downloading file';$('statusDetail').textContent=entry.relative||entry.name;
    $('progressLabel').textContent='Download';$('progress').value=0;$('percent').textContent='0%';
    const result=await api('files/download',{path:entry.path});
    let lastProgress=performance.now();const started=lastProgress;
    watchdog=setInterval(()=>{if(performance.now()-lastProgress>60000)abort.abort()},5000);
    const response=await fetch(result.url,{headers:{Authorization:'Bearer '+state.token},signal:abort.signal,cache:'no-store'});
    if(!response.ok){let error;try{error=await response.json()}catch{}throw new Error(error?.error||'Download failed.');}
    const reader=response.body.getReader();const chunks=[];let received=0;
    for(;;){
      const {value,done}=await reader.read();if(done)break;
      received+=value.length;
      if(received>entry.bytes||(!writer&&received>64*1024*1024)){
        await reader.cancel();throw new Error('The file changed during the download. Please refresh the folder.');
      }
      if(writer)await writer.write(value);else chunks.push(value);
      lastProgress=performance.now();const percent=entry.bytes?Math.min(100,100*received/entry.bytes):100;
      $('progress').value=percent;$('percent').textContent=Math.floor(percent)+'%';
      $('bytes').textContent=size(received)+' / '+size(entry.bytes);
      $('speed').textContent=size(received/Math.max(.1,(lastProgress-started)/1000))+'/s';
    }
    if(received!==entry.bytes)throw new Error('The file changed or the download was incomplete.');
    if(writer){await writer.close();writer=null;}
    else{const url=URL.createObjectURL(new Blob(chunks,{type:'application/octet-stream'}));const link=document.createElement('a');link.href=url;link.download=entry.name;link.click();setTimeout(()=>URL.revokeObjectURL(url),30000);}
    state.busy=false;
    if(!workspace.batchDownload){
      workspace.localJob={id:'download-'+crypto.randomUUID(),phase:'completed',download_name:entry.name,
        detail:'Downloaded '+entry.name+'.',bytes:entry.bytes,received:entry.bytes,progress:100,
        log:'Downloaded '+entry.name+' to this computer.'};
      updateJob(workspace.localJob);
    }
    complete=true;
  }catch(error){
    const message=error.name==='AbortError'?'Download interrupted.':error.message;
    workspace.downloadError=message;
    if(workspace.download&&!workspace.batchDownload){
      state.busy=false;workspace.localJob={id:'download-'+crypto.randomUUID(),phase:'failed',download_name:entry.name,detail:message};
      updateJob(workspace.localJob);
    }else if(!workspace.batchDownload&&error.name!=='AbortError')notice(message);
  }
  finally{
    clearInterval(watchdog);if(writer)await writer.abort().catch(()=>{});
    workspace.download=null;state.busy=false;controls();activityVisibility();
  }
  return complete;
}
$('fileDownload').onclick=async()=>{
  const entries=Array.from(workspace.selection.values());if(!entries.length||activeJob())return;
  if(entries.length===1&&!entries[0].directory){await downloadEntry(entries[0]);return}
  const folders=entries.some(entry=>entry.directory);
  if(folders&&typeof window.showDirectoryPicker!=='function'){
    notice('Folder downloads require a browser with computer folder access, such as Chrome, Edge or Opera.');return;
  }
  let directory=null;
  try{if(typeof window.showDirectoryPicker==='function')directory=await window.showDirectoryPicker({id:'aera-files-download',mode:'readwrite'})}
  catch(error){if(error.name!=='AbortError')notice(error.message);return}
  const serverJobId=workspace.localJob?workspace.serverJobId:state.job?.id||'';
  clearActivity();workspace.serverJobId=serverJobId;
  const abort=new AbortController();workspace.batchDownload=true;workspace.batchAbort=abort;
  notice('Downloading selected items...','progress');controls();
  let completed=0,files=0,foldersSaved=0,skipped=0,visited=0;
  const checkCancelled=()=>{if(abort.signal.aborted)throw new DOMException('Download cancelled.','AbortError')};
  async function copyEntry(entry,parent,relative=entry.name,depth=0){
    checkCancelled();
    if(depth>128||++visited>100000)throw new Error('The folder contains too many entries or nesting levels. Download smaller subfolders separately.');
    if(!entry.directory){
      if(!entry.regular){skipped++;return}
      if(!await downloadEntry({...entry,relative},parent,abort))throw new Error(workspace.downloadError||'Download interrupted.');
      files++;return;
    }
    try{await parent.getDirectoryHandle(entry.name);throw new Error(relative+' already exists in the chosen computer folder. Nothing was overwritten.')}
    catch(error){if(error.name!=='NotFoundError')throw error}
    checkCancelled();
    const target=await parent.getDirectoryHandle(entry.name,{create:true});foldersSaved++;
    let offset=0;
    do{
      checkCancelled();state.busy=true;controls();
      $('statusTitle').textContent='Reading folder';$('statusDetail').textContent=relative;
      $('progressLabel').textContent='Download';$('progress').removeAttribute('value');$('percent').textContent='Reading';
      let listing;
      try{listing=await api('files/list',{path:entry.path,offset})}finally{state.busy=false}
      checkCancelled();
      if(listing.truncated)throw new Error(relative+' has more than 10,000 entries and cannot be downloaded completely.');
      for(const child of listing.entries)await copyEntry(child,target,relative+'/'+child.name,depth+1);
      if(listing.next!==null&&(!Number.isInteger(listing.next)||listing.next<=offset))throw new Error('Recovery returned an invalid folder page.');
      offset=listing.next;
    }while(offset!==null);
  }
  try{
    for(const entry of entries){
      await copyEntry(entry,directory);
      completed++;
    }
    const detail=files+' files and '+foldersSaved+' folders saved.'+(skipped?' '+skipped+' links or special files skipped.':'');
    workspace.localJob={id:'download-'+crypto.randomUUID(),phase:'completed',download_name:folders?'Folders':'Selected files',
      detail,progress:100,log:detail};updateJob(workspace.localJob);notice(detail,'success');
  }catch(error){
    const detail=(error.name==='AbortError'?'Download cancelled.':error.message)+' '+
      files+' files saved; '+completed+' / '+entries.length+' selected items completed. Partial folders may remain on the computer.';
    workspace.localJob={id:'download-'+crypto.randomUUID(),phase:'failed',download_name:'Selected items',detail};
    updateJob(workspace.localJob);notice(detail);
  }finally{state.busy=false;workspace.batchDownload=false;workspace.batchAbort=null;controls()}
};
$('fileUpload').onclick=()=>$('storageFile').click();
$('fileUploadCancel').onclick=()=>{const download=workspace.download||workspace.batchAbort;if(download)download.abort();else if(workspace.uploadQueue)cancelFileUploads();else discard()};
async function cancelFileUploads(){
  const queue=workspace.uploadQueue;if(!queue||queue.cancelled)return;
  queue.cancelled=true;state.cancelled=true;
  $('fileUploadStatus').textContent='Cancelling uploads / '+queue.completed+' of '+queue.total+' files saved';controls();
  // A save already submitted may finish, but no further file may start.
  if(!queue.saving)await originalDiscard();
}
$('storageFile').onchange=async()=>{
  const files=Array.from($('storageFile').files);$('storageFile').value='';
  if(!files.length||activeJob()||!workspace.folder)return;
  const destination=workspace.folder.path,root=workspace.folder.root;
  const queue={cancelled:false,saving:false,total:files.length,completed:0};workspace.uploadQueue=queue;
  const status=$('fileUploadStatus');status.hidden=false;let error='';
  try{
    for(const [index,file] of files.entries()){
      if(queue.cancelled)break;
      status.textContent='File '+(index+1)+' of '+queue.total+' / '+file.name;
      state.file=file;state.saveFolder=destination;state.saveRoot=root;
      $('fileInfo').hidden=false;$('fileName').textContent=file.name;$('fileDetails').textContent=size(file.size)+' / Save to '+destination;
      $('imageFields').hidden=true;notice('');controls();
      const received=await transfer();
      if(queue.cancelled||state.cancelled)break;
      if(received?.phase!=='ready'||!received.save_path)throw new Error(state.transferError||received?.detail||'Upload failed.');
      queue.saving=true;let saved;
      try{saved=await api('install',{id:received.id,confirm:true})}finally{queue.saving=false}
      updateJob(saved);
      if(saved.phase!=='completed')throw new Error(saved.detail||'Could not save the uploaded file.');
      queue.completed++;
    }
  }catch(failure){error=failure.message}
  finally{
    workspace.uploadQueue=null;
    const detail=queue.completed+' / '+queue.total+' files uploaded.'+
      (queue.cancelled?' Remaining uploads cancelled.':error?' Queue stopped: '+error:'');
    status.textContent=detail;notice(detail,!queue.cancelled&&!error?'success':'');
    state.file=null;state.saveFolder='';state.saveRoot='';$('fileInfo').hidden=true;
    controls();await loadFolder(destination);
  }
};
window.addEventListener('beforeunload',event=>{
  if(workspace.uploadQueue||workspace.batchDownload){event.preventDefault();event.returnValue=''}
});
function downloadText(text,name,type='text/plain') {
  const url=URL.createObjectURL(new Blob([text],{type}));const link=document.createElement('a');link.href=url;link.download=name;link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
}
async function authorizedText(path) {
  const response=await fetch('/api/'+path,{headers:{Authorization:'Bearer '+state.token},cache:'no-store',signal:AbortSignal.timeout(15000)});
  if(!response.ok){let error;try{error=await response.json()}catch{}throw new Error(error?.error||'Recovery could not provide this file.');}return response.text();
}
$('certificateDownload').onclick=async()=>{try{downloadText(await authorizedText('certificate'),'AERA-device.crt','application/x-pem-file')}catch(error){notice(error.message)}};
function renderInstallerDetails(presentation){
  let panel=$('installerDetails');
  if(!panel){panel=document.createElement('div');panel.id='installerDetails';panel.className='installer-details';$('statusTitle').closest('.status-head').before(panel);}
  panel.hidden=!presentation||!Object.keys(presentation).length;panel.replaceChildren();if(panel.hidden)return;
  const title=document.createElement('h3');title.textContent=presentation.package||'Package installation';panel.append(title);
  const rows=[['Device',presentation.device],['Author',presentation.author],
    ['Stage',presentation.stage_count?`${presentation.stage} / ${presentation.stage_count}`:'']];
  const details=document.createElement('dl');details.className='review';
  for(const [name,value] of rows){if(!value)continue;const row=document.createElement('div'),label=document.createElement('dt'),text=document.createElement('dd');label.textContent=name;text.textContent=value;row.append(label,text);details.append(row);}
  panel.append(details);
  for(const value of [presentation.stage_title,presentation.stage_detail,
      presentation.rebooting?`Rebooting to ${presentation.reboot_target||'Android'} in ${presentation.reboot_seconds}s`:'']){
    if(value){const text=document.createElement('p');text.textContent=value;panel.append(text);}
  }
}
function renderPackageInfo(panel,info){
  panel.hidden=!info||!Object.keys(info).length;
  const signature=JSON.stringify(info||{});if(panel.dataset.signature===signature)return;
  panel.dataset.signature=signature;panel.replaceChildren();if(panel.hidden)return;
  if(info.error){const error=document.createElement('div');error.className='notice error';error.textContent=info.error;panel.append(error);return;}
  if(!info.is_payload){const text=document.createElement('p');text.textContent=info.summary;panel.append(text);return;}
  const title=document.createElement('h3');title.textContent=info.build||'Android update';panel.append(title);
  const rows=(parent,values)=>{
    const list=document.createElement('dl');list.className='review';
    for(const [name,value] of values){if(value===undefined||value===null||value==='')continue;const row=document.createElement('div'),label=document.createElement('dt'),text=document.createElement('dd');label.textContent=name;text.textContent=String(value);row.append(label,text);list.append(row)}
    parent.append(list);
  };
  rows(panel,[['Device',info.device],['Android',info.android],['Security patch',info.security_patch],
    ['Update type',info.incremental?'Incremental OTA':'Full OTA'],['Payload size',size(info.payload_bytes)],
    ['Package ARB',info.arb_available?info.arb_index:'Unavailable']]);
  if(['upgrade','downgrade'].includes(info.arb_decision)){
    const warning=document.createElement('div');warning.className='notice'+(info.arb_decision==='downgrade'?' error':'');
    warning.textContent=info.arb_decision==='downgrade'?'Installation blocked: this package lowers the firmware ARB index and can brick the device.':
      'This update raises the firmware ARB index. It may permanently prevent returning to older firmware.';
    panel.append(warning);
  }
  const details=document.createElement('details'),summary=document.createElement('summary');summary.textContent='Full information';details.append(summary);
  rows(details,[[info.name_from_filename?'Package name':'Version',info.build],['Product',info.device],['Device codename',info.codename],
    ['Build ID',info.build_id],['System fingerprint',info.fingerprint],['Partition coverage',info.partial?'Partial update':'Full update'],
    ['Expanded images',size(info.expanded_bytes)],['Format revision',info.format],['Dynamic partitions',info.dynamic_partitions?'Yes':'No'],
    ['Snapshots',info.snapshots?'Requested by package':'Not requested'],['Virtual A/B compression',info.compression?'Requested by package':'Not requested'],
    ['Operations',info.operations],['Operation types',info.operation_types],['ARB source',info.arb_detail],
    ['Slot A ARB',info.slot_a_arb],['Slot B ARB',info.slot_b_arb]]);
  const heading=document.createElement('h3');heading.textContent='Partitions';details.append(heading);
  rows(details,(info.partitions||[]).map(partition=>[partition.name,size(partition.bytes)]));panel.append(details);
}
$('recoveryLog').onclick=async()=>{try{downloadText(await authorizedText('log'),'AERA-recovery.log')}catch(error){notice(error.message)}};
for(const [id,path,name] of [['logcatLog','log/logcat','AERA-logcat.txt'],['kernelLog','log/kernel','AERA-kernel.log']]) $(id).onclick=async()=>{
  $(id).disabled=true;const text=$(id).textContent;$(id).textContent='Collecting...';
  try{downloadText(await authorizedText(path),name)}catch(error){notice(error.message)}
  finally{$(id).textContent=text;$(id).disabled=false;}
};
async function loadFlashReports() {
  const panel=$('flashReports');panel.textContent='Loading...';
  try {
    const result=await api('log/flashes');panel.replaceChildren();
    if(!result.reports?.length){panel.textContent='No flash reports available.';return;}
    for(const report of result.reports){
      const row=document.createElement('div');row.className='flash-report';
      const text=document.createElement('div'),title=document.createElement('strong'),detail=document.createElement('small');
      title.textContent=report.result;
      detail.textContent=new Date(report.timestamp*1000).toLocaleString()+' / '+size(report.bytes);
      text.append(title,detail);
      const button=document.createElement('button');button.title='Download flash report';button.setAttribute('aria-label',button.title);
      button.append(icon('download'));
      button.onclick=async()=>{
        button.disabled=true;
        try{
          const response=await fetch('/api/log/flash/'+encodeURIComponent(report.name),{
            headers:{Authorization:'Bearer '+state.token},cache:'no-store',signal:AbortSignal.timeout(60000)});
          if(!response.ok)throw new Error('Flash report could not be downloaded. Refresh the list and try again.');
          const url=URL.createObjectURL(await response.blob()),link=document.createElement('a');
          link.href=url;link.download=report.name;link.click();setTimeout(()=>URL.revokeObjectURL(url),30000);
        }catch(error){notice(error.message)}finally{button.disabled=false;}
      };
      row.append(text,button);panel.append(row);
    }
  }catch(error){panel.textContent=error.message;}
}
$('deviceRefresh').onclick=async()=>{try{await refresh();await loadFlashReports()}catch(error){notice(error.message)}};
showTab('install');if(state.info)updateInfo(state.info);controls();
