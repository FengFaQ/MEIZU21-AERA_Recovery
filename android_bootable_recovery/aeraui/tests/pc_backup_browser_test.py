#!/usr/bin/env python3
"""Opt-in browser test with a fake installer and memory-backed PC folder."""
import pathlib
import socket
import subprocess
import sys
import tempfile

from playwright.sync_api import sync_playwright

DIRECTORY = r"""
class Folder {
  kind='directory'; children=new Map();
  constructor(name){this.name=name}
  async getDirectoryHandle(name,{create=false}={}){
    let entry=this.children.get(name);
    if(!entry&&create){entry=new Folder(name);this.children.set(name,entry)}
    if(!entry)throw new DOMException('Missing folder','NotFoundError');
    if(entry.kind!=='directory')throw new DOMException('Not a folder','TypeMismatchError');return entry;
  }
  async getFileHandle(name,{create=false}={}){
    let entry=this.children.get(name);
    if(!entry&&create){entry=new PcFile(name);this.children.set(name,entry)}
    if(!entry)throw new DOMException('Missing file','NotFoundError');
    if(entry.kind!=='file')throw new DOMException('Not a file','TypeMismatchError');return entry;
  }
  async *entries(){yield* this.children.entries()}
  async removeEntry(name){if(!this.children.delete(name))throw new DOMException('Missing entry','NotFoundError')}
}
class PcFile {
  kind='file'; data=new Uint8Array();
  constructor(name){this.name=name}
  async getFile(){return new File([this.data],this.name,{lastModified:Date.now()})}
  async createWritable({keepExistingData=false}={}){
    let data=keepExistingData?this.data.slice():new Uint8Array(),position=0;
    const resize=size=>{const next=new Uint8Array(size);next.set(data.subarray(0,size));data=next};
    return {write:async value=>{
      const at=value.type==='write'?value.position:position;
      const bytes=new Uint8Array(value.type==='write'?value.data:value);
      if(at+bytes.length>data.length)resize(at+bytes.length);data.set(bytes,at);position=at+bytes.length;
    },truncate:async size=>resize(size),close:async()=>{
      if(window.testSlowClose){window.testSlowClose=false;await new Promise(resolve=>setTimeout(resolve,3200))}
      if(window.testCloseError){window.testCloseError=false;throw new DOMException('Test file commit denied','SecurityError')}
      this.data=data;
    },abort:async()=>{}};
  }
}
window.testFolder=new Folder('Computer backups');
window.testSlowClose=true;
window.showDirectoryPicker=async()=>window.testFolder;
"""


def main():
    with tempfile.TemporaryDirectory(prefix="pc-browser-", dir=pathlib.Path.home()) as root:
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        process = subprocess.Popen([sys.argv[1], root, str(port), "--demo"], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, text=True)
        try:
            assert process.stdout.readline().strip() == "READY"
            with sync_playwright() as playwright:
                browser = playwright.chromium.launch(executable_path="/opt/google/chrome/chrome",
                                                     args=["--no-sandbox"], headless=True)
                page = browser.new_page(ignore_https_errors=True, bypass_csp=True,
                                        viewport={"width": 1440, "height": 1000})
                errors = []
                heartbeats = []
                heartbeat_connections = []
                page.on("pageerror", lambda error: errors.append(str(error)))
                page.on("response", lambda response: heartbeats.append(response.status)
                        if response.url.endswith('/api/backups/heartbeat') else None)
                page.on("response", lambda response: heartbeat_connections.append(response.headers.get('connection'))
                        if response.url.endswith('/api/backups/heartbeat') and response.status == 200 else None)
                page.add_init_script(DIRECTORY)
                page.goto(f"https://127.0.0.1:{port}")
                page.locator("#connectButton").click()
                page.locator("[data-tab=backups]").wait_for(state="visible")
                payload_page = browser.new_page(ignore_https_errors=True, bypass_csp=True,
                                                viewport={"width": 1440, "height": 1000})
                payload_page.on("pageerror", lambda error: errors.append(str(error)))
                payload_page.goto(f"https://127.0.0.1:{port}")
                payload_page.evaluate("""()=>{
                  state.info={partitions:[]};connected(true);
                  window.payloadFixture={id:'payload-ui-test',phase:'installing',install_method:'fast',progress:42,
                    payload_progress:{available:true,lanes:2,partitions:[
                      {name:'system',phase:'writing',bytes:2000000000,done:1000000000,percent:50},
                      {name:'vendor',phase:'verifying',bytes:1000000000,done:250000000,percent:25},
                      {name:'boot',phase:'verified',bytes:10000000,done:10000000,percent:100},
                      {name:'dtbo',phase:'written',bytes:10000000,done:10000000,percent:100},
                      {name:'recovery',phase:'not_flashed',bytes:10000000,done:0,percent:0},
                      {name:'very_long_partition_name_<img_onerror=alert(1)>',phase:'failed',bytes:10000000,done:5000000,percent:50},
                      {name:'modem',phase:'queued',bytes:10000000,done:0,percent:0}]}};
                  updateJob(payloadFixture);
                  const row=document.querySelector('.payload-progress-row');
                  row.dataset.stability='retained';payloadFixture.payload_progress.partitions[0].percent=60;
                  updateJob(payloadFixture);
                  if(document.querySelector('.payload-progress-row')!==row||row.querySelector('progress').value!==60)
                    throw new Error('Payload rows were replaced on progress update');
                }""")
                assert payload_page.locator('#payloadProgress').is_visible()
                assert payload_page.locator('.payload-progress-row').count() == 7
                assert 'Writing / 60%' in payload_page.locator('.payload-progress-state').first.inner_text()
                assert payload_page.locator('.payload-progress-amount').first.inner_text() == '1000.0 MB / 2000.0 MB'
                assert 'Verifying / 25%' in payload_page.locator('.payload-progress-state').nth(1).inner_text()
                assert '1 / 7 verified / 2 active / 2 queued / 1 failed / 1 not flashed' == payload_page.locator('#payloadProgressSummary').inner_text()
                assert payload_page.locator('#payloadProgress img').count() == 0
                for width in (1440, 390, 320):
                    payload_page.set_viewport_size({"width": width, "height": 1000})
                    assert payload_page.evaluate("""()=>{
                      const panel=$('payloadProgress'),bounds=panel.getBoundingClientRect();
                      return bounds.left>=0&&bounds.right<=innerWidth&&panel.scrollWidth<=panel.clientWidth+1&&
                        [...panel.querySelectorAll('.payload-progress-row')].every(row=>row.scrollWidth<=row.clientWidth+1);
                    }""")
                    if width != 320:
                        payload_page.screenshot(path=f"/tmp/aera-pc-payload-{width}.png", full_page=True)
                payload_page.evaluate("updateJob({...payloadFixture,phase:'failed'})")
                assert payload_page.locator('#payloadProgress').is_visible()
                payload_page.evaluate("updateJob({phase:'idle',install_method:'normal'})")
                assert payload_page.locator('#payloadProgress').is_hidden()
                payload_page.close()
                page.locator("[data-tab=backups]").click()
                page.locator("#backupChooseFolder").click()
                page.locator("#backupSelectAll").click()
                assert page.locator("#backupRun").is_enabled()
                page.screenshot(path="/tmp/aera-pc-backup-desktop.png", full_page=True)
                page.locator("#backupRun").click()
                page.locator("#actionRun").click()
                page.locator('[data-tab="files"]:disabled').wait_for()
                assert page.locator('#backupBusy').is_visible()
                page.wait_for_function("state.job.backup_transfer?.finalizing===true", timeout=30000)
                assert page.locator('#statusTitle').inner_text()=='Saving file on computer'
                assert page.locator('#percent').inner_text()=='Saving'
                assert page.locator('#progress').get_attribute('value') is None
                page.wait_for_function("backups.finished && state.job.phase==='completed'", timeout=30000)
                assert 200 in heartbeats
                assert heartbeat_connections and all(value=='keep-alive' for value in heartbeat_connections)
                first_name=page.evaluate('backups.submittedName')
                assert page.locator('#bytes').inner_text()=='67.1 MB'
                assert page.evaluate("""()=>{
                  const previous=state.job;
                  updateJob({...previous,phase:'installing',backup_transfer:{bytes:120000000,bytes_per_second:87500000}});
                  const correct=$('speed').textContent==='87.5 MB/s'&&$('bytes').textContent==='120.0 MB';
                  updateJob(previous);return correct;
                }""")
                assert page.evaluate(r"""()=>{
                  const previous=state.job;
                  for(const action of ['backup','restore']){
                    updateJob({...previous,phase:'installing',backup_kind:'recovery',backup_action:action,detail:'backing / Super (system system_ext product vendor)\n4584MB of 18032MB (25%)'});
                    if($('statusTitle').textContent!==(action==='restore'?'Restoring ':'Backing up ')+'Super (system system_ext product vendor)')return false;
                    if($('statusDetail').textContent.includes('backing /'))return false;
                  }
                  updateJob(previous);return true;
                }""")
                assert page.locator('#backupName').input_value()!=first_name
                page.evaluate('window.firstBackupName=backups.submittedName')
                assert page.evaluate("""async()=>{
                  const root=await(await(await testFolder.getDirectoryHandle('AERA')).getDirectoryHandle('BACKUPS')).getDirectoryHandle('test-device');
                  const folder=await root.getDirectoryHandle(firstBackupName);
                  const file=await(await folder.getFileHandle('data.ext4.win')).getFile();
                  const bytes=new Uint8Array(await file.arrayBuffer());
                  return bytes.length===8*8388608&&bytes.every((value,index)=>value===97+Math.floor(index/8388608));
                }""")
                first_job=page.evaluate('backups.finished')
                page.locator('#backupRun:enabled').wait_for()
                page.locator('#backupRun').click();page.locator('#actionRun').click()
                page.wait_for_function("previous=>backups.finished&&backups.finished!==previous&&state.job.phase==='completed'",arg=first_job,timeout=30000)
                assert page.evaluate('backups.submittedName')!=first_name
                assert page.evaluate("""async()=>{
                  const root=await(await(await testFolder.getDirectoryHandle('AERA')).getDirectoryHandle('BACKUPS')).getDirectoryHandle('test-device');
                  return (await root.getDirectoryHandle(firstBackupName)).name!==(await root.getDirectoryHandle(backups.submittedName)).name;
                }""")
                page.locator('input[name="backupOperation"][value="restore"]').check()
                page.locator("#backupChooseFolder").click()
                assert page.locator('#backupNativeField').is_visible()
                assert page.locator('#backupNative option').count()==3
                assert page.locator('#backupRun').is_disabled()
                page.locator('#backupNative').select_option(label='AERA/BACKUPS/test-device/'+first_name)
                page.wait_for_function("backups.finished && state.job.phase==='completed' && state.job.backup_action==='inspect'", timeout=30000)
                assert page.evaluate("""async()=>{
                  const candidates=await recoveryBackupFolders(backups.folder);
                  if(candidates.length!==1||candidates[0].folder!==backups.folder)return false;
                  const root=await(await(await testFolder.getDirectoryHandle('AERA')).getDirectoryHandle('BACKUPS')).getDirectoryHandle('test-device');
                  const partial=await root.getDirectoryHandle('partial',{create:true});
                  await partial.getFileHandle('boot.emmc.win',{create:true});await partial.getFileHandle('.aera-incomplete',{create:true});
                  return (await recoveryBackupFolders(testFolder)).length===2;
                }""")
                assert page.locator("#backupChoices input:checked").count() == 1
                page.locator("#backupRun").click()
                page.locator("#actionRun").click()
                page.wait_for_function("backups.finished && state.job.phase==='completed' && state.job.backup_action==='restore'", timeout=30000)
                page.locator("#backupAdvanced summary").click()
                page.locator("#backupMethod").select_option("transfer")
                page.set_viewport_size({"width": 390, "height": 844})
                page.screenshot(path="/tmp/aera-pc-backup-mobile.png", full_page=True)
                assert page.evaluate("document.documentElement.scrollWidth<=window.innerWidth")
                page.locator('input[name="backupSource"][value="device"]').check()
                assert page.locator("#backupDeviceField").is_visible()
                assert page.locator("#backupMethodField").is_hidden()
                page.locator('input[name="backupOperation"][value="backup"]').check()
                assert page.locator("#backupFolderField").is_hidden()
                page.locator('input[name="backupKind"][value="apps"]').check()
                page.locator("#backupLoad").click()
                page.wait_for_function("backups.finished && state.job.phase==='completed' && state.job.backup_action==='apps'", timeout=30000)
                assert page.locator("#backupChoices").inner_text().find("test.app") >= 0
                assert page.locator('#backupChoices b').inner_text() == 'Test application'
                page.wait_for_function("document.querySelector('#backupChoices img')?.naturalWidth>0")
                assert page.evaluate("async()=>!(await api('status')).job.backup_result.apps[0].icon")
                page.screenshot(path='/tmp/aera-pc-backup-apps.png', full_page=True)
                assert page.locator("#backupUser option").count() == 2
                assert page.evaluate("""async()=>{
                  backups.folder=testFolder;backups.writers.clear();
                  const entry=await pcWriter('/write-failure',true);
                  entry.stream.write=async()=>{throw new DOMException('Disk full','QuotaExceededError')};
                  await pcOperation({op:'write',path:'/write-failure',id:1,offset:0,size:4194304,data:new Uint8Array(4194304)});
                  await entry.pending;
                  let failed=false;try{await closePcFile('/write-failure')}catch(error){failed=error.name==='QuotaExceededError'}
                  await entry.stream.abort();backups.writers.clear();backups.writeFailure=null;
                  return failed&&backups.pendingBytes===0;
                }""")
                assert page.evaluate("""async()=>{
                  backups.folder=testFolder;
                  const entry=await pcWriter('/bounded-queue',true);
                  let release;const waiting=new Promise(resolve=>release=resolve);
                  entry.stream.write=async()=>waiting;
                  const block=new Uint8Array(8388608);
                  await pcOperation({op:'write',path:'/bounded-queue',id:1,offset:0,size:block.length,data:block});
                  await pcOperation({op:'write',path:'/bounded-queue',id:2,offset:block.length,size:block.length,data:block});
                  let accepted=false;
                  const third=pcOperation({op:'write',path:'/bounded-queue',id:3,offset:2*block.length,size:block.length,data:block}).then(()=>accepted=true);
                  await new Promise(resolve=>setTimeout(resolve,30));
                  const bounded=!accepted&&backups.pendingBytes===16777216;
                  release();await third;await closePcFile('/bounded-queue');
                  return bounded&&backups.pendingBytes===0;
                }""")
                page.locator('input[name="backupOperation"][value="restore"]').check()
                assert page.evaluate("""async()=>{
                  const root=new testFolder.constructor('test'),aera=await root.getDirectoryHandle('AERA',{create:true});
                  const vault=await aera.getDirectoryHandle('AppBackupVault',{create:true});
                  await vault.getFileHandle('config',{create:true});
                  await useBackupFolder(root);
                  if(backups.folder!==vault||$('backupFolderName').textContent!=='test/AERA/AppBackupVault')return false;
                  await useBackupFolder(vault);return backups.folder===vault;
                }""")
                assert page.evaluate("""async()=>{
                  const folder=await navigator.storage.getDirectory();
                  await folderMemory('test-persistence',folder);
                  const remembered=await folderMemory('test-persistence');
                  return remembered&&await remembered.isSameEntry(folder);
                }""")
                assert page.evaluate("""async()=>{
                  const previous=state.job,originalApi=api,calls=[];
                  api=async(path,body)=>{calls.push({path,body});return {...state.job,cancel_requested:true}};
                  state.job={id:'restore-confirmation-test',phase:'installing',backup_action:'restore'};
                  backupControls();$('backupCancel').click();
                  $('backupCancelConfirm').click();
                  const first=calls.length===0&&backupCancellation.second&&$('backupCancelDialog').open;
                  $('backupCancelConfirm').click();await new Promise(resolve=>setTimeout(resolve,20));
                  const second=calls.length===1&&calls[0].path==='backups/cancel'&&calls[0].body.acknowledge_partial_restore;
                  api=originalApi;state.job=previous;backupCancellation=null;backupControls();
                  return first&&second;
                }""")
                process.stdin.write('vault-off\n');process.stdin.flush()
                page.wait_for_function("state.info?.app_vault_available===false")
                assert page.locator('input[name="backupKind"][value="apps"]').is_disabled()
                assert page.locator('input[name="backupKind"][value="recovery"]').is_checked()
                assert page.locator('#vaultUnavailable').is_visible()
                process.stdin.write('vault-on\n');process.stdin.flush()
                page.wait_for_function("state.info?.app_vault_available===true")
                assert page.locator('input[name="backupKind"][value="apps"]').is_enabled()
                page.locator('input[name="backupKind"][value="recovery"]').check()
                page.locator('input[name="backupOperation"][value="backup"]').check()
                page.locator('input[name="backupSource"][value="computer"]').check()
                page.locator('#backupChooseFolder').click();page.locator('#backupSelectAll').click()
                page.evaluate('window.testCloseError=true')
                page.locator('#backupRun').click();page.locator('#actionRun').click()
                page.wait_for_function("state.job.phase==='failed'",timeout=30000)
                assert 'SecurityError: Test file commit denied' in page.locator('#backupResult').inner_text()
                assert not errors, errors
                browser.close()
                print("PC streaming writes, readback, restore, PC/device selection, app discovery and mobile layout passed.")
        finally:
            process.stdin.write("quit\n")
            process.stdin.flush()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            process.stdin.close()
            process.stdout.close()


if __name__ == "__main__":
    main()
