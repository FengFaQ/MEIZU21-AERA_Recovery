/* SPDX-License-Identifier: Apache-2.0 */
'use strict';
const backups = {folder:null,path:'',choices:[],selected:new Set(),snapshots:[],job:'',running:false,
  handling:false,writers:new Map(),readers:new Map(),directories:new Map(),finished:'',devicePickerPath:'',
  pendingWrites:new Set(),pendingBytes:0,writeFailure:null,readAhead:null,
  previous:null,folderRevision:0,generatedName:'',submittedName:'',nativeFolders:[],restoreRoot:null,selectingFolder:false};
let backupNameBase='',backupNameSequence=0;
function nextBackupName(){
  const base='AERA-'+new Date().toISOString().slice(0,19).replace(/[:T]/g,'-');
  backupNameSequence=base===backupNameBase?backupNameSequence+1:0;backupNameBase=base;
  backups.generatedName=base+(backupNameSequence?'-'+backupNameSequence:'');
  $('backupName').value=backups.generatedName;
}
function snapshotLabel(snapshot){
  const date=new Date(snapshot.time);
  const when=Number.isNaN(date.getTime())?'Unknown date':date.toLocaleString(undefined,
    {year:'numeric',month:'2-digit',day:'2-digit',hour:'2-digit',minute:'2-digit'});
  const bytes=snapshot.bytes;
  const amount=Number.isFinite(bytes)&&bytes>=0?
    new Intl.NumberFormat(undefined,{maximumFractionDigits:1}).format(bytes/1000000)+' MB':'--';
  const id=typeof snapshot.id==='string'?snapshot.id.slice(0,8):'--';
  return `${when} \u00b7 ${amount} \u00b7 ID ${id}`;
}
function backupChoice(name){return document.querySelector(`input[name="${name}"]:checked`).value}
function backupOperation(){return backupChoice('backupOperation')}
function appBackup(){return backupChoice('backupKind')==='apps'}
function computerBackup(){return backupChoice('backupSource')==='computer'}
async function waitStorageJob(job){
  updateJob(job);
  const deadline=Date.now()+120000;
  while(Date.now()<deadline){
    await new Promise(resolve=>setTimeout(resolve,500));
    const info=await api('status');updateInfo(info);updateJob(info.job||{});
    if(info.job?.id!==job.id)throw new Error('The storage operation changed. Please try again.');
    if(info.job.phase==='completed')return;
    if(['failed','cancelled'].includes(info.job.phase))throw new Error(info.job.detail||'Storage is unavailable.');
  }
  throw new Error('Storage connection is taking longer than expected. Check the device status.');
}
async function prepareBackupStorage(){
  const path=$('backupStorage').value;
  if(!path)throw new Error('Select backup storage first.');
  const storage=(state.info?.backup_storages||state.info?.storages||[]).find(item=>item.path===path);
  if(storage?.mounted!==false)return;
  const job=await api('backups',{action:'storage',kind:'recovery',source:'device',storage:path,
    partitions:[],confirm:true});
  await waitStorageJob(job);
}
function backupBlockBytes(){return Math.min(8388608,Number(state.info?.backup_block_bytes)||524288)}
function backupOperationLocked(){return ['backup','restore'].includes(state.job?.backup_action)&&['queued','installing'].includes(state.job?.phase)}
const originalBackupShowTab=showTab;
showTab=function(tab){if(backupOperationLocked()&&tab!==workspace.tab&&!['terminal','backups'].includes(tab))return;originalBackupShowTab(tab)};
function visibleBackupChoices(){
  const filter=$('backupFilter').value.trim().toLowerCase();
  return backups.choices.filter(item=>(!appBackup()||$('backupSystem').checked||!item.system)&&
    (!filter||`${item.name} ${item.package||''}`.toLowerCase().includes(filter)));
}
function renderBackupChoices(){
  const list=$('backupChoices');list.replaceChildren();
  for(const item of visibleBackupChoices()){
    const key=item.package||item.path,row=document.createElement('label'),box=document.createElement('input');
    row.className='backup-choice';box.type='checkbox';box.checked=backups.selected.has(key);
    box.onchange=()=>{if(box.checked)backups.selected.add(key);else backups.selected.delete(key);backupControls()};
    box.disabled=activeJob();const text=document.createElement('span'),name=document.createElement('b'),detail=document.createElement('small');
    name.textContent=item.name||key;detail.textContent=item.package||(item.bytes?size(item.bytes):item.path);
    text.append(name,detail);row.append(box);
    if(appBackup()){
      const plate=document.createElement('span');plate.className='backup-app-icon';
      plate.textContent=(item.name||key).slice(0,1).toUpperCase();
      if(typeof item.icon==='string'&&item.icon.startsWith('data:image/png;base64,')){
        const image=document.createElement('img');image.alt='';image.width=40;image.height=40;image.src=item.icon;
        image.onload=()=>plate.replaceChildren(image);
        image.onerror=()=>image.remove();plate.append(image);
      }
      row.append(plate);
    }
    row.append(text);list.append(row);
  }
  if(!list.childElementCount){const empty=document.createElement('p');empty.className='context-note';
    empty.textContent=appBackup()?'No apps loaded.':backupOperation()==='restore'?'No backup loaded.':'No backup partitions available.';list.append(empty)}
  backupControls();
}
function backupControls(){
  const appType=document.querySelector('input[name="backupKind"][value="apps"]');
  const vaultAvailable=!!state.info?.app_vault_available;
  if(!vaultAvailable&&appType.checked&&!backupOperationLocked()){
    document.querySelector('input[name="backupKind"][value="recovery"]').checked=true;
    backups.folder=null;backups.path='';$('backupFolderName').textContent='No folder selected';
    resetBackupChoices();return;
  }
  const restore=backupOperation()==='restore',apps=appBackup(),pc=computerBackup();
  const blocked=!state.connected||activeJob()||!!state.info?.busy||backups.handling||backups.selectingFolder;
  const locked=backupOperationLocked();
  $('backupCancel').hidden=!locked;
  $('backupCancel').disabled=!!state.job?.cancel_requested;
  $('backupCancel').textContent=state.job?.cancel_requested?'Cancelling...':
    state.job?.backup_action==='restore'?'Cancel restore':'Cancel backup';
  if(!locked&&$('backupCancelDialog').open)$('backupCancelDialog').close();
  for(const button of $('workspaceTabs').children)button.disabled=locked;
  $('backupBusy').hidden=!locked;
  $('backupBusy').textContent=locked?
    (state.job.backup_action==='restore'?'Restore in progress.':'Backup in progress.')+
    (state.job.backup_source==='computer'?' Keep this page open. Other tools are locked until completion.':' Other tools are locked until completion.') : '';
  for(const field of document.querySelectorAll('#backupChoices input, #backupFilter, #backupSystem'))field.disabled=blocked;
  $('backupSourceLabel').textContent=restore?'Source':'Destination';
  $('backupFolderField').querySelector('label').textContent=restore?(apps?'Repository folder':'Backup folder'):'Destination folder';
  $('backupDeviceField').hidden=pc&&(!restore||$('backupMethod').value!=='transfer');
  $('backupMethodField').hidden=!restore||!pc;
  $('backupMethodNote').textContent=$('backupMethod').value==='transfer'?
    'Device storage must fit the complete backup. Restore starts after the full transfer completes.':
    'A disconnected stream can leave an incomplete restore.';
  $('backupCompressionField').hidden=restore||apps;
  $('backupNameField').hidden=restore||apps;
  $('backupFolderField').hidden=!pc&&!restore;
  $('backupUserField').hidden=!apps||$('backupUser').options.length<2;$('backupSystemField').hidden=!apps||restore;
  $('backupAdvanced').hidden=restore&&!pc;
  $('backupSnapshotField').hidden=!apps||!restore;$('backupFilter').hidden=!apps;
  $('backupNativeField').hidden=apps||!restore||!pc||backups.nativeFolders.length<2;
  $('backupSelectionTitle').textContent=apps?'Apps':'Partitions';
  $('backupLoad').hidden=!restore&&!apps;
  $('vaultUnavailable').hidden=vaultAvailable&&(!apps||!state.info?.locked);
  $('vaultUnavailable').textContent=!vaultAvailable?'Install App Backup Vault on the device to enable app backups.':'Unlock Android data to access app backups.';
  $('backupRunLabel').textContent=restore?'Restore selected':'Back up selected';
  $('backupSummary').textContent=`${backups.selected.size} selected`;
  for(const id of ['backupKind','backupOperation','backupSource','backupStorage','backupUser','backupSnapshot','backupNative',
      'backupMethod','backupName','backupCompression','backupChooseFolder','backupUseFolder','backupLoad','backupSelectAll','backupSelectNone']) $(id).disabled=blocked;
  appType.disabled=blocked||!vaultAvailable;
  $('backupLoad').disabled=blocked||(restore&&pc&&!backups.folder);
  $('backupRun').disabled=blocked||!backups.selected.size||(pc&&!backups.folder)||
    (!pc&&!$('backupStorage').value)||(restore&&!pc&&!backups.path)||(apps&&(!$('backupUser').value||!state.info?.app_vault_available||state.info?.locked))||
    (restore&&apps&&!$('backupSnapshot').value)||(restore&&pc&&$('backupMethod').value==='transfer'&&!$('backupStorage').value);
}
function resetBackupChoices(){
  backups.selected.clear();backups.choices=[];backups.snapshots=[];$('backupSnapshot').replaceChildren(new Option('Select snapshot',''));
  if(!appBackup()&&backupOperation()==='backup')backups.choices=state.info?.backup_partitions||[];
  $('backupResult').textContent='';renderBackupChoices();
  if(!backups.folder&&!backups.restoreRoot)refreshRememberedFolder();
}
function backupBody(action){
  return {action,kind:backupChoice('backupKind'),source:backupChoice('backupSource'),method:$('backupMethod').value,
    storage:$('backupStorage').value,path:backups.path,name:$('backupName').value.trim(),
    user:Number($('backupUser').value||0),snapshot:$('backupSnapshot').value,
    compression:$('backupCompression').checked,partitions:[...backups.selected],folder_ready:!!backups.folder,confirm:true};
}
async function startBackupJob(action){
  if(!computerBackup()&&action!=='storage')await prepareBackupStorage();
  backups.finished='';$('backupResult').textContent='';
  const nativeBackup=action==='backup'&&!appBackup();
  if(nativeBackup&&(!$('backupName').value.trim()||$('backupName').value.trim()===backups.generatedName))nextBackupName();
  const body=backupBody(action),job=await api('backups',body);
  if(nativeBackup)backups.submittedName=body.name;
  backups.directories.clear();backups.readers.clear();
  backups.writeFailure=null;backups.readAhead=null;
  backups.job=job.id;backups.running=true;updateJob(job);backupControls();
  if(action!=='apps')pumpComputerStorage();
}
async function loadBackup(){
  if(computerBackup()&&!backups.folder&&!(appBackup()&&backupOperation()==='backup'))throw new Error('Choose the computer backup folder first.');
  await startBackupJob(appBackup()?(backupOperation()==='backup'?'apps':'snapshots'):'inspect');
}
async function chooseBackupFolder(){
  if(!computerBackup()){await prepareBackupStorage();await chooseDeviceBackupFolder($('backupStorage').value);return}
  if(!window.showDirectoryPicker)throw new Error('This browser does not support computer folder access. Device storage is still available.');
  const folder=await window.showDirectoryPicker({mode:backupOperation()==='backup'?'readwrite':'read',id:'aera-backups'});
  ++backups.folderRevision;
  await useBackupFolder(folder);
  await folderMemory(backupFolderKey(),folder);
  if(backupOperation()==='restore'){resetBackupChoices();if(backups.folder)await loadBackup()}
}
function backupFolderKey(action=backupOperation()){
  return `${state.info?.device||'recovery'}:${backupChoice('backupKind')}:${action}`;
}
async function folderMemory(key,value){
  try{return await new Promise((resolve,reject)=>{
    const opening=indexedDB.open('aera-backup-folders',1);
    opening.onupgradeneeded=()=>opening.result.createObjectStore('folders');
    opening.onerror=()=>reject(opening.error);
    opening.onsuccess=()=>{
      const db=opening.result;
      try{
        const transaction=db.transaction('folders',value===undefined?'readonly':'readwrite');
        const store=transaction.objectStore('folders'),request=value===undefined?store.get(key):store.put(value,key);
        transaction.oncomplete=()=>{db.close();resolve(value===undefined?request.result:value)};
        transaction.onerror=transaction.onabort=()=>{db.close();reject(transaction.error)};
      }catch(error){db.close();reject(error)}
    };
  })}catch{return null}
}
async function recoveryBackupFolders(folder){
  let visited=0;
  const candidates=[];
  async function inspect(directory,path,depth){
    if(++visited>2048)throw new Error('Too many folders. Choose the AERA/BACKUPS folder to narrow the search.');
    const children=[];let backup=false,incomplete=false;
    for await(const [name,handle] of directory.entries()){
      if(handle.kind==='file'){
        if(name==='.aera-incomplete')incomplete=true;
        if(/\.win(?:\d{3})?$/i.test(name))backup=true;
      }else if(handle.kind==='directory')children.push([name,handle]);
    }
    if(incomplete)return;
    if(backup){candidates.push({folder:directory,path,name:path||directory.name});return}
    if(depth>0)for(const [name,child] of children)await inspect(child,path?path+'/'+name:name,depth-1);
  }
  let anchor=folder,prefix='';
  for(const parts of [['AERA','BACKUPS'],['BACKUPS']]){
    try{
      let directory=folder;for(const part of parts)directory=await directory.getDirectoryHandle(part);
      anchor=directory;prefix=parts.join('/');break;
    }catch(error){if(error.name!=='NotFoundError'&&error.name!=='TypeMismatchError')throw error}
  }
  await inspect(anchor,prefix,2);
  return candidates.sort((a,b)=>b.name.localeCompare(a.name));
}
async function useBackupFolder(folder){
  backups.selectingFolder=true;backupControls();
  try{
  let folderName=folder.name,selected=folder;
  backups.nativeFolders=[];backups.restoreRoot=null;
  if(!appBackup()&&backupOperation()==='restore'){
    $('backupFolderName').textContent='Finding recovery backups...';
    const candidates=await recoveryBackupFolders(folder);
    if(!candidates.length)throw new Error('No completed recovery backups found in this folder.');
    backups.nativeFolders=candidates;backups.restoreRoot=folder;
    $('backupNative').replaceChildren(new Option('Select backup',''));
    candidates.forEach((item,index)=>$('backupNative').add(new Option(item.name,String(index))));
    if(candidates.length===1){selected=candidates[0].folder;folderName+=(candidates[0].path?'/'+candidates[0].path:'')}
    else selected=null;
  }
  if(appBackup()&&backupOperation()==='restore'){
    try{await folder.getFileHandle('config')}
    catch(error){
      if(error.name!=='NotFoundError')throw error;
      const repository=await(await folder.getDirectoryHandle('AERA')).getDirectoryHandle('AppBackupVault');
      await repository.getFileHandle('config');selected=repository;folderName+='/AERA/AppBackupVault';
    }
  }
  backups.folder=selected;$('backupUseFolder').hidden=true;
  backups.readers.clear();backups.directories.clear();$('backupFolderName').textContent=folderName;backups.path='';backupControls();
  }finally{backups.selectingFolder=false;backupControls()}
}
async function refreshRememberedFolder(){
  const revision=++backups.folderRevision;
  backups.previous=null;$('backupUseFolder').hidden=true;
  if(!computerBackup())return;
  let folder=await folderMemory(backupFolderKey());
  if(!folder&&appBackup()&&backupOperation()==='restore')folder=await folderMemory(backupFolderKey('backup'));
  if(revision!==backups.folderRevision||backups.folder||backups.restoreRoot||!folder)return;
  backups.previous=folder;$('backupUseFolder').hidden=false;
  $('backupUseFolder').title=`Reuse ${folder.name}`;
  const mode=backupOperation()==='backup'?'readwrite':'read';
  try{if(await folder.queryPermission({mode})==='granted'&&revision===backups.folderRevision){
    await useBackupFolder(folder);
    if(backupOperation()==='restore'&&backups.folder&&state.connected&&!activeJob()&&!state.info?.busy)await loadBackup();
  }}catch{}
}
function snapshotChoices(){
  const item=backups.snapshots.find(snapshot=>snapshot.id===$('backupSnapshot').value);
  backups.choices=item?.apps||[];backups.selected=new Set(backups.choices.map(app=>app.package));renderBackupChoices();
}
async function loadBackupIcons(job){
  const result=await api('backups/icons');
  if(result.id!==job.id||state.job?.id!==job.id)return;
  for(const items of [backups.choices,...backups.snapshots.map(snapshot=>snapshot.apps||[])])
    for(const item of items)item.icon=result.icons?.[item.package]||'';
  renderBackupChoices();
}
const oldBackupInfo=updateInfo;
updateInfo=function(info){
  oldBackupInfo(info);
  const previousStorage=$('backupStorage').value;
  options($('backupStorage'),info.backup_storages||info.storages||[],s=>
    s.mounted===false?`${s.name} / Not connected`:`${s.name} / ${size(s.free)} free`);
  if(!previousStorage&&appBackup()&&info.app_vault_storage&&
      [...$('backupStorage').options].some(item=>item.value===info.app_vault_storage))
    $('backupStorage').value=info.app_vault_storage;
  updateNetworkStorage(info);
  if(backups.memoryDevice!==info.device){
    backups.memoryDevice=info.device;
    if(!backups.folder&&!activeJob())refreshRememberedFolder();
  }
  const selected=$('backupUser').value,signature=JSON.stringify(info.users||[]);
  if($('backupUser').dataset.signature!==signature){$('backupUser').dataset.signature=signature;
    $('backupUser').replaceChildren();for(const user of info.users||[])$('backupUser').add(new Option(`${user.name||'User'} (${user.id})`,String(user.id)));
    if((info.users||[]).some(user=>String(user.id)===selected))$('backupUser').value=selected;
  }
  if(!appBackup()&&backupOperation()==='backup'){
    const changed=JSON.stringify(backups.choices)!==JSON.stringify(info.backup_partitions||[]);
    if(changed){backups.choices=info.backup_partitions||[];renderBackupChoices()}
  }
  backupControls();
};
const oldBackupJob=updateJob;
updateJob=function(job){
  oldBackupJob(job);
  document.querySelector('.log-title h3').textContent=job.nas_action?'Network storage log':job.save_path?'File transfer log':'Installer output';
  if(job.nas_action&&state.job?.id===job.id){
    $('progressLabel').textContent='Network storage';
    const titles={queued:'Network storage queued',installing:'Updating network storage',
      completed:'Network storage updated',failed:'Network storage failed'};
    $('statusTitle').textContent=titles[job.phase]||'Network storage';
    if(!job.log)$('log').textContent=job.detail||'Updating network storage';
    if(job.phase==='completed')notice(job.detail,'success');
  }
  if(!job.backup_action||state.job?.id!==job.id){backupControls();return}
  const titles={backup:'Backing up',restore:'Restoring',apps:'Reading apps',snapshots:'Reading snapshots',inspect:'Reading backup'};
  if(job.phase==='queued')$('statusTitle').textContent='Backup operation queued';
  else if(job.phase==='installing')$('statusTitle').textContent=titles[job.backup_action]||'Working';
  else if(job.phase==='completed')$('statusTitle').textContent='Backup operation complete';
  $('progressLabel').textContent=job.backup_action==='restore'?'Restore':'Backup';
  if(job.backup_transfer){
    const transfer=job.backup_transfer;
    $('bytes').textContent=(Number(transfer.bytes||0)/1000000).toFixed(1)+' MB';
    $('speed').textContent=['queued','installing'].includes(job.phase)?
      (Number(transfer.bytes_per_second||0)/1000000).toFixed(1)+' MB/s':'--';
  }
  if(['queued','installing','completed','failed','cancelled'].includes(job.phase))$('statusDetail').textContent=job.detail||'';
  if(job.phase==='installing'&&job.backup_kind==='recovery'){
    const lines=(job.detail||'').split('\n'),heading=lines[0]||'',separator=heading.indexOf(' / ');
    const stage=(separator<0?heading:heading.slice(0,separator)).trim().toLowerCase();
    if(separator>=0&&['backing','backing up','restoring','restore'].includes(stage)){
      $('statusTitle').textContent=(job.backup_action==='restore'?'Restoring ':'Backing up ')+heading.slice(separator+3);
      $('statusDetail').textContent=lines.slice(1).filter(Boolean).join(' / ');
    }
  }
  if(job.phase==='cancelled')$('statusTitle').textContent='Backup operation cancelled';
  else if(job.cancel_requested)$('statusTitle').textContent='Cancelling backup operation';
  else if(job.phase==='installing'&&job.backup_transfer?.finalizing){
    $('statusTitle').textContent='Saving file on computer';
    $('statusDetail').textContent='Keep the browser open while the computer finishes saving. ('+
      Number(job.backup_transfer.waiting_seconds||0)+' s)';
    $('progress').removeAttribute('value');$('percent').textContent='Saving';
  }
  if(job.phase==='completed')notice(job.detail||'Backup operation complete.','success');
  if(job.id===backups.job&&['completed','failed','cancelled'].includes(job.phase)&&backups.finished!==job.id){
    backups.finished=job.id;backups.running=false;const result=job.backup_result||{};
    if(job.backup_action==='backup'&&job.backup_kind==='recovery'&&$('backupName').value.trim()===backups.submittedName)
      nextBackupName();
    if(job.phase==='completed'&&['apps','snapshots'].includes(job.backup_action))
      loadBackupIcons(job).catch(()=>{});
    if(job.phase==='completed'){
      if(job.backup_action==='apps'){backups.choices=result.apps||[];backups.selected=new Set(backups.choices.filter(a=>!a.system).map(a=>a.package));renderBackupChoices()}
      if(job.backup_action==='inspect'){backups.choices=result.partitions||[];backups.selected=new Set(backups.choices.map(p=>p.path));renderBackupChoices()}
      if(job.backup_action==='snapshots'){
        backups.snapshots=result.snapshots||[];$('backupSnapshot').replaceChildren(new Option('Select snapshot',''));
        for(const snapshot of backups.snapshots)$('backupSnapshot').add(new Option(snapshotLabel(snapshot),snapshot.id));
        if(backups.snapshots.length)$('backupSnapshot').value=backups.snapshots[0].id;
        snapshotChoices();
      }
      $('backupResult').textContent=result.folder?`Saved: ${result.folder}`:result.detail||'Complete.';
    }else $('backupResult').textContent=result.error||job.detail||'Operation did not complete.';
  }
  backupControls();
};

async function storageFetch(path,options={}){
  const response=await fetch('/api/backups/io'+path,{...options,cache:'no-store',
    headers:{Authorization:'Bearer '+state.token,...options.headers},signal:AbortSignal.timeout(15000)});
  if(!response.ok){
    const detail=await response.json().catch(()=>null);
    throw new Error(detail?.error||'Computer storage connection was interrupted.');
  }return response;
}
function safePcPath(path){
  if(typeof path!=='string'||!path.startsWith('/')||path.includes('\\')||path.includes('\0'))throw new Error('Invalid backup path.');
  const parts=path.slice(1).split('/').filter(Boolean);
  if(parts.some(part=>part==='.'||part==='..'))throw new Error('Invalid backup path.');return parts;
}
async function pcEntry(path,directory=false,create=false){
  const parts=safePcPath(path);let parent=backups.folder;
  if(!parent)throw new Error('Computer folder is unavailable.');
  if(!parts.length)return parent;
  let current='';
  for(const part of parts.slice(0,-1)){
    current+='/'+part;
    if(backups.directories.has(current))parent=backups.directories.get(current);
    else{parent=await parent.getDirectoryHandle(part,{create});backups.directories.set(current,parent)}
  }
  if(!directory)return parent.getFileHandle(parts.at(-1),{create});
  current+='/'+parts.at(-1);
  if(backups.directories.has(current))return backups.directories.get(current);
  const handle=await parent.getDirectoryHandle(parts.at(-1),{create});backups.directories.set(current,handle);return handle;
}
async function pcStat(path){
  const open=backups.writers.get(path);
  if(open)return {size:open.size,mtime:Math.floor(Date.now()/1000),directory:false};
  try{await pcEntry(path,true);return {size:0,mtime:0,directory:true}}
  catch(error){if(error.name!=='TypeMismatchError'&&error.name!=='NotFoundError')throw error}
  const file=await(await pcEntry(path)).getFile();return {size:file.size,mtime:Math.floor(file.lastModified/1000),directory:false};
}
function invalidatePcRead(path){
  backups.readers.delete(path);
  if(backups.readAhead?.path===path)backups.readAhead=null;
}
async function pcWriter(path,create=false){
  let entry=backups.writers.get(path);
  if(!entry){const handle=await pcEntry(path,false,create),file=await handle.getFile();
    entry={handle,stream:await handle.createWritable({keepExistingData:true}),size:file.size,pending:Promise.resolve(),error:null};backups.writers.set(path,entry);invalidatePcRead(path)}
  return entry;
}
async function closePcFile(path){
  const entry=backups.writers.get(path);if(entry){await entry.pending;if(entry.error)throw entry.error;await entry.stream.close();backups.writers.delete(path);invalidatePcRead(path)}
}
async function pcOperation(request){
  const {op,path,id}=request;safePcPath(path);
  if(backups.writeFailure)throw backups.writeFailure;
  if(op==='stat')return pcStat(path);
  if(op==='list'){
    const directory=await pcEntry(path,true),items=[];
    for await(const [name,handle] of directory.entries())items.push({name,directory:handle.kind==='directory'});
    items.sort((a,b)=>a.name.localeCompare(b.name));return {entries:items.slice(Number(request.offset)||0,(Number(request.offset)||0)+4096)};
  }
  if(op==='read'){
    const writer=backups.writers.get(path);if(writer)await closePcFile(path);
    let file=backups.readers.get(path);if(!file){file=await(await pcEntry(path)).getFile();backups.readers.set(path,file)}
    const offset=Number(request.offset),length=Number(request.size);
    if(!Number.isSafeInteger(offset)||offset<0||length<0||length>backupBlockBytes())throw new Error('Invalid backup block.');
    const ahead=backups.readAhead;
    const data=ahead?.path===path&&ahead.offset===offset&&ahead.length===length?await ahead.data:
      await file.slice(offset,offset+length).arrayBuffer();
    if(data instanceof Error)throw data;
    const next=offset+length;
    backups.readAhead=length<=4194304&&next<file.size?{path,offset:next,length,data:file.slice(next,next+length).arrayBuffer().catch(error=>error)}:null;
    return {readData:data};
  }
  if(op==='create'){
    if(request.exclusive){try{await pcStat(path);const error=new Error('File already exists.');error.name='InvalidModificationError';throw error}
      catch(error){if(error.name!=='NotFoundError')throw error}}
    await pcWriter(path,true);return {};
  }
  if(op==='mkdir'){await pcEntry(path,true,true);return {}}
  if(op==='close'){await closePcFile(path);return {}}
  if(op==='truncate'){const entry=await pcWriter(path);await entry.pending;if(entry.error)throw entry.error;await entry.stream.truncate(Number(request.size));entry.size=Number(request.size);return {}}
  if(op==='write'){
    const data=request.data||await(await storageFetch('/'+id)).arrayBuffer();
    if(data.byteLength!==Number(request.size)||data.byteLength>backupBlockBytes())throw new Error('Incomplete or oversized backup block.');
    const entry=await pcWriter(path);
    while(backups.pendingBytes+data.byteLength>16777216)await Promise.race(backups.pendingWrites);
    if(backups.writeFailure)throw backups.writeFailure;
    backups.pendingBytes+=data.byteLength;
    const pending=entry.pending.then(()=>{if(entry.error)throw entry.error;return entry.stream.write({type:'write',position:Number(request.offset),data})})
      .catch(error=>{entry.error=error;backups.writeFailure=error})
      .finally(()=>{backups.pendingBytes-=data.byteLength;backups.pendingWrites.delete(pending)});
    entry.pending=pending;backups.pendingWrites.add(pending);
    entry.size=Math.max(entry.size,Number(request.offset)+data.byteLength);return {};
  }
  if(op==='unlink'||op==='rmdir'){
    const parts=safePcPath(path),name=parts.pop();if(!name)throw new Error('Cannot remove the selected root folder.');
    const writer=backups.writers.get(path);if(writer){await writer.pending;await writer.stream.abort();backups.writers.delete(path)}
    const parent=await pcEntry('/'+parts.join('/'),true);await parent.removeEntry(name);invalidatePcRead(path);backups.directories.clear();return {};
  }
  if(op==='rename'){
    if(path===request.target)return {};
    safePcPath(request.target);await closePcFile(path);await closePcFile(request.target);
    const source=await pcEntry(path),file=await source.getFile(),destination=await pcEntry(request.target,false,true);
    const stream=await destination.createWritable();
    try{for(let offset=0;offset<file.size;offset+=1048576)await stream.write(await file.slice(offset,offset+1048576).arrayBuffer());await stream.close()}
    catch(error){await stream.abort().catch(()=>{});throw error}
    const parts=safePcPath(path),name=parts.pop();await(await pcEntry('/'+parts.join('/'),true)).removeEntry(name);
    invalidatePcRead(path);invalidatePcRead(request.target);backups.directories.clear();return {};
  }
  throw new Error('Unsupported backup filesystem operation.');
}
async function pumpComputerStorage(){
  if(backups.handling||!computerBackup())return;
  backups.handling=true;
  let heartbeatPending=false;
  const heartbeat=setInterval(async()=>{
    if(heartbeatPending||!backups.running||!state.connected||state.job?.id!==backups.job)return;
    heartbeatPending=true;
    try{await api('backups/heartbeat',{id:backups.job})}catch(error){}
    finally{heartbeatPending=false}
  },2000);
  try{
    let response=await storageFetch('/next');
    while(backups.running&&state.connected){
      let request;
      try{
        const frame=await response.arrayBuffer();
        if(frame.byteLength<4)throw new Error('Invalid computer storage frame.');
        const length=new DataView(frame).getUint32(0);
        if(length>65536||length>frame.byteLength-4)throw new Error('Invalid computer storage frame.');
        request=JSON.parse(new TextDecoder().decode(new Uint8Array(frame,4,length)));
        if(request.op==='write')request.data=new Uint8Array(frame,4+length);
      }
      catch(error){
        await refresh().catch(()=>{});
        if(state.job?.phase==='queued'||state.job?.phase==='installing')$('backupResult').textContent='Computer storage session ended. Check the final operation result.';
        break;
      }
      if(request.done)break;
      if(!request.id){response=await storageFetch('/next');continue}
      let reply;
      try{reply=await pcOperation(request)}
      catch(error){const codes={NotFoundError:2,NotAllowedError:13,TypeMismatchError:20,InvalidModificationError:17,QuotaExceededError:28};
        const detail=String(error.name||'Error')+': '+String(error.message||'Computer filesystem operation failed.');
        reply={errno:codes[error.name]||5,error:detail.slice(0,256)};
        if(reply.errno!==2)$('backupResult').textContent=detail}
      response=reply?.readData?await storageFetch('/exchange/'+request.id,{method:'PUT',body:reply.readData,headers:{'Content-Type':'application/octet-stream'}}):
        await storageFetch('/exchange/'+request.id,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(reply)});
    }
  }catch(error){notice(error.message)}
  finally{
    clearInterval(heartbeat);
    await Promise.all(backups.pendingWrites);
    for(const entry of backups.writers.values())await entry.stream.abort().catch(()=>{});
    backups.writers.clear();backups.readers.clear();backups.directories.clear();
    backups.readAhead=null;backups.pendingBytes=0;backups.writeFailure=null;
    backups.handling=false;backupControls();
  }
}

async function chooseDeviceBackupFolder(path){
  const dialog=$('backupFolderDialog');backups.devicePickerPath=path;dialog.showModal();await browseDeviceBackupFolder(path);
}
async function browseDeviceBackupFolder(path){
  $('backupDeviceFolders').textContent='Loading folders...';
  const result=await api('files/list',{path,offset:0});backups.devicePickerPath=result.path||path;
  while(result.next!==null&&result.next!==undefined){
    const page=await api('files/list',{path:backups.devicePickerPath,offset:result.next});
    result.entries.push(...page.entries);result.next=page.next;
  }
  $('backupDevicePath').textContent=backups.devicePickerPath;$('backupDeviceFolders').replaceChildren();
  for(const entry of result.entries||[]){if(!entry.directory)continue;
    const button=document.createElement('button');button.className='backup-folder-row';button.append(icon('folder'));
    const name=document.createElement('span');name.textContent=entry.name;button.append(name);
    button.onclick=()=>browseDeviceBackupFolder(entry.path).catch(error=>notice(error.message));$('backupDeviceFolders').append(button)}
}
$('backupDevicePick').onclick=async()=>{
  backups.path=backups.devicePickerPath;$('backupFolderName').textContent=backups.path;$('backupFolderDialog').close();
  resetBackupChoices();try{await loadBackup()}catch(error){notice(error.message)}
};
$('backupDeviceUp').onclick=()=>{const root=$('backupStorage').value,path=backups.devicePickerPath;
  const parent=path.slice(0,path.lastIndexOf('/'))||root;
  browseDeviceBackupFolder(parent.length<root.length?root:parent).catch(error=>notice(error.message))};
$('backupDeviceCancel').onclick=()=>$('backupFolderDialog').close();
$('backupChooseFolder').onclick=()=>chooseBackupFolder().catch(error=>{if(error.name!=='AbortError')notice(error.message)});
$('backupUseFolder').onclick=async()=>{
  const folder=backups.previous;if(!folder)return;
  try{
    const mode=backupOperation()==='backup'?'readwrite':'read';
    if(await folder.requestPermission({mode})!=='granted')throw new Error('Allow access to the previous backup folder, or choose another folder.');
    ++backups.folderRevision;await useBackupFolder(folder);
    if(backupOperation()==='restore'){resetBackupChoices();if(backups.folder)await loadBackup()}
  }catch(error){notice(error.message)}
};
$('backupLoad').onclick=()=>loadBackup().catch(error=>notice(error.message));
$('backupRun').onclick=()=>{
  const restore=backupOperation()==='restore',pc=computerBackup(),count=backups.selected.size;
  const target=pc?backups.folder?.name:restore?backups.path:$('backupStorage').value;
  const warning=restore?`Restore ${count} selected ${appBackup()?'apps':'partitions'} from ${target}? Existing data will be replaced.`+
    (pc&&$('backupMethod').value==='stream'?' Keep this browser connected until completion. A disconnected stream may leave an incomplete restore.':''):
    `Back up ${count} selected ${appBackup()?'apps':'partitions'} to ${target}?`;
  confirmation(restore?'Confirm restore':'Confirm backup',warning,()=>startBackupJob(backupOperation()));
};
for(const id of ['backupKind','backupOperation','backupSource','backupUser','backupStorage'])$(id).onchange=()=>{
  if(id==='backupKind'&&appBackup()&&state.info?.app_vault_storage&&
      [...$('backupStorage').options].some(option=>option.value===state.info.app_vault_storage))
    $('backupStorage').value=state.info.app_vault_storage;
  if(id!=='backupUser'){backups.folder=null;backups.path='';backups.nativeFolders=[];backups.restoreRoot=null;$('backupFolderName').textContent='No folder selected'}resetBackupChoices();
};
$('backupNative').onchange=async()=>{
  const value=$('backupNative').value,item=value===''?null:backups.nativeFolders[Number(value)];
  backups.folder=item?.folder||null;backups.path='';backups.readers.clear();backups.directories.clear();
  $('backupFolderName').textContent=backups.restoreRoot.name+(item?.path?'/'+item.path:'');
  resetBackupChoices();if(item)try{await loadBackup()}catch(error){notice(error.message)}
};
$('backupMethod').onchange=backupControls;$('backupSnapshot').onchange=snapshotChoices;
$('backupFilter').oninput=$('backupSystem').onchange=renderBackupChoices;
$('backupSelectAll').onclick=()=>{for(const item of visibleBackupChoices())backups.selected.add(item.package||item.path);renderBackupChoices()};
$('backupSelectNone').onclick=()=>{backups.selected.clear();renderBackupChoices()};
nextBackupName();
window.addEventListener('beforeunload',event=>{if(backups.running){event.preventDefault();event.returnValue=''}});

const networkStorage={dirty:false,passwordDirty:false,key:null,signature:'',type:null,drafts:{}};
const networkPanel=document.createElement('details');networkPanel.id='networkStorage';
networkPanel.innerHTML=`<summary>Network storage</summary>
  <div class="fields">
    <p id="nasState" class="context-note" role="status">Not connected</p>
    <div class="pair-fields">
      <div><label for="nasType">Protocol</label><select id="nasType"><option value="smb">SMB</option><option value="sftp">SFTP</option></select></div>
      <div><label for="nasHost">Server</label><input id="nasHost" maxlength="128" autocomplete="off"></div>
      <div><label for="nasUser">Username</label><input id="nasUser" maxlength="128" autocomplete="off"></div>
      <div id="nasAuthField"><label for="nasAuth">Authentication</label><select id="nasAuth"><option value="password">Password</option><option value="key">Private key</option></select></div>
      <div><label id="nasPasswordLabel" for="nasPassword">Password</label><input id="nasPassword" type="password" maxlength="254" autocomplete="new-password"></div>
      <div id="nasShareField"><label for="nasShare">SMB share</label><input id="nasShare" maxlength="128"></div>
    </div>
    <div id="nasKeyField" hidden><label for="nasKey">Private key</label><input id="nasKey" type="file"><p id="nasKeyState" class="context-note">No private key saved</p></div>
    <div><label for="nasPath">Remote folder</label><input id="nasPath" maxlength="256"></div>
    <label class="arb-consent"><input id="nasCache" type="checkbox"><span>Use device write cache</span></label>
    <details><summary>Advanced</summary><div class="pair-fields fields">
      <div id="nasPortField"><label for="nasPort">SFTP port</label><input id="nasPort" type="number" min="1" max="65535" value="22"></div>
      <div id="nasDomainField"><label for="nasDomain">Workgroup / domain</label><input id="nasDomain" maxlength="128" value="WORKGROUP"></div>
    </div></details>
    <div class="actions"><button id="nasSave">Save settings</button><button id="nasConnect" class="primary">Connect</button><button id="nasDisconnect">Disconnect</button></div>
  </div>`;
$('backupsPanel').append(networkPanel);
function networkFields(){
  const sftp=$('nasType').value==='sftp',key=sftp&&$('nasAuth').value==='key';
  $('nasAuthField').hidden=!sftp;$('nasKeyField').hidden=!key;
  $('nasShareField').hidden=sftp;$('nasPortField').hidden=!sftp;$('nasDomainField').hidden=sftp;
  $('nasPasswordLabel').textContent=key?'Key passphrase (optional)':'Password';
}
function showNetworkProfile(nas){
  for(const field of ['type','host','user','auth','share','path','port','domain'])
    $('nas'+field[0].toUpperCase()+field.slice(1)).value=nas[field]||({type:'smb',auth:'password',port:'22',domain:'WORKGROUP'}[field]||'');
  networkStorage.type=$('nasType').value;
  networkStorage.passwordDirty=!!nas.passwordDirty;
  networkStorage.key=nas.key||null;
  $('nasPassword').value=nas.passwordDirty?nas.password||'':'';
  $('nasKey').value='';
  $('nasCache').checked=nas.cache_mode==='data';
  $('nasPassword').placeholder=nas.password_saved?'Saved credential':'';
  $('nasKeyState').textContent=nas.key?.name||(nas.key_available?`Saved: ${nas.key_name||'Private key'}`:'No private key saved');
  networkFields();
}
function updateNetworkStorage(info){
  const nas=info.nas||{};networkPanel.hidden=!nas.supported;
  const signature=JSON.stringify(nas);
  if(!networkStorage.dirty&&signature!==networkStorage.signature){
    networkStorage.signature=signature;
    showNetworkProfile(nas);
  }
  $('nasState').textContent=nas.error||(nas.mounted?'Connected':nas.status||'Not connected');
  const blocked=!state.connected||activeJob()||!!info.busy;
  for(const input of networkPanel.querySelectorAll('input,select'))input.disabled=blocked||!!nas.mounted;
  $('nasConnect').disabled=$('nasSave').disabled=blocked||!!nas.mounted||!!info.locked;
  $('nasDisconnect').disabled=blocked||!nas.mounted;
}
networkPanel.oninput=event=>{
  if(event.target.id==='nasType'){
    const type=$('nasType').value,previous={type:networkStorage.type};
    for(const field of ['host','user','auth','share','path','port','domain'])
      previous[field]=$('nas'+field[0].toUpperCase()+field.slice(1)).value;
    previous.cache_mode=$('nasCache').checked?'data':'off';
    previous.password=$('nasPassword').value;previous.passwordDirty=networkStorage.passwordDirty;
    previous.password_saved=!!$('nasPassword').placeholder;previous.key=networkStorage.key;
    previous.key_name=networkStorage.key?.name||state.info?.nas?.profiles?.[networkStorage.type]?.key_name||'';
    previous.key_available=!!state.info?.nas?.key_available;
    networkStorage.drafts[networkStorage.type]=previous;
    showNetworkProfile(networkStorage.drafts[type]||state.info?.nas?.profiles?.[type]||{type,port:type==='smb'?'445':'22'});
  }
  networkStorage.dirty=true;if(event.target.id==='nasPassword')networkStorage.passwordDirty=true;
  networkFields();
};
$('nasKey').onchange=()=>{
  networkStorage.key=$('nasKey').files[0]||null;networkStorage.dirty=true;
  $('nasKeyState').textContent=networkStorage.key?.name||'No file selected';
};
async function networkAction(action){
  const body={action};
  if(action!=='disconnect'){
    for(const field of ['type','host','user','auth','share','path','port','domain'])
      body[field]=$('nas'+field[0].toUpperCase()+field.slice(1)).value;
    body.cache_mode=$('nasCache').checked?'data':'off';
    body.key_name=networkStorage.key?.name||state.info?.nas?.profiles?.[body.type]?.key_name||'';
    if(networkStorage.passwordDirty)body.password=$('nasPassword').value;
    if(networkStorage.key){
      if(networkStorage.key.size>65536)throw new Error('Private key must be smaller than 64 KB.');
      body.private_key=await networkStorage.key.text();
    }
  }
  const job=await api('nas',body);
  if(body.type)delete networkStorage.drafts[body.type];
  networkStorage.dirty=false;networkStorage.passwordDirty=false;networkStorage.key=null;
  networkStorage.signature='';$('nasPassword').value='';$('nasKey').value='';
  await waitStorageJob(job);
}
for(const [id,action] of [['nasSave','save'],['nasConnect','connect'],['nasDisconnect','disconnect']])
  $(id).onclick=()=>networkAction(action).catch(error=>notice(error.message));

let backupCancellation=null;
$('backupCancel').onclick=()=>{
  if(!backupOperationLocked()||state.job.cancel_requested)return;
  backupCancellation={id:state.job.id,restore:state.job.backup_action==='restore',second:false};
  $('backupCancelTitle').textContent=backupCancellation.restore?'Cancel restore':'Cancel backup';
  $('backupCancelWarning').textContent=backupCancellation.restore?
    'Stopping a restore can leave incomplete data. Keep the device in recovery.':
    'The unfinished backup cannot be used for restore. Stop this backup now?';
  $('backupCancelConfirm').textContent=backupCancellation.restore?'Continue':'Stop backup';
  $('backupCancelDialog').showModal();
};
$('backupCancelKeep').onclick=()=>{$('backupCancelDialog').close();backupCancellation=null};
$('backupCancelConfirm').onclick=async()=>{
  const pending=backupCancellation;
  if(!pending||pending.id!==state.job?.id||!backupOperationLocked())return;
  if(pending.restore&&!pending.second){
    pending.second=true;$('backupCancelTitle').textContent='Confirm restore cancellation';
    $('backupCancelWarning').textContent='Complete a full restore before booting Android. Stop this restore now?';
    $('backupCancelConfirm').textContent='Stop restore';return;
  }
  $('backupCancelConfirm').disabled=true;
  try{updateJob(await api('backups/cancel',{id:pending.id,confirm:true,acknowledge_partial_restore:pending.restore}));$('backupCancelDialog').close()}
  catch(error){notice(error.message)}
  finally{$('backupCancelConfirm').disabled=false}
};
resetBackupChoices();
