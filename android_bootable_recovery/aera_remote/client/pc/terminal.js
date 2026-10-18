/* SPDX-License-Identifier: Apache-2.0 */
'use strict';
const terminalView={running:false,opening:false,closing:false,generation:0};
const terminalBlocked=()=>!state.connected;
function terminalMessage(text){
  $('terminalState').textContent=text;
  $('terminalEmpty').textContent=text;
}
function terminalDrop(){
  terminalView.generation++;
  terminalView.running=false;
  $('terminalFrame').hidden=true;
  $('terminalFrame').src='about:blank';
  $('terminalEmpty').hidden=false;
}
async function closeTerminal(message='Closed'){
  if(terminalView.closing)return;
  terminalView.closing=true;
  terminalDrop();terminalMessage(message);controls();
  try{if(state.connected)await api('terminal/close',{});}
  catch(error){if(state.connected)terminalMessage(error.message);}
  finally{terminalView.closing=false;controls();}
}
async function openTerminal(){
  if(terminalView.opening||terminalView.closing||terminalView.running||terminalBlocked())return;
  terminalView.opening=true;
  const generation=++terminalView.generation;
  terminalMessage('Starting shell...');controls();
  try{
    const result=await api('terminal/open',{});
    if(generation!==terminalView.generation||terminalBlocked()){
      if(state.connected)await api('terminal/close',{});
      return;
    }
    terminalView.running=true;
    $('terminalFrame').src=result.url;
    $('terminalFrame').hidden=false;
    $('terminalEmpty').hidden=true;
    terminalMessage('Connected');
  }catch(error){terminalMessage(error.message);}
  finally{terminalView.opening=false;controls();}
}
const terminalShowTab=showTab;
showTab=function(tab){
  if(tab==='terminal'&&terminalBlocked()&&!terminalView.running)return;
  terminalShowTab(tab);
  $('terminalPanel').hidden=workspace.tab!=='terminal';
  if(workspace.tab==='terminal')openTerminal();
};
const terminalActivityVisibility=activityVisibility;
activityVisibility=function(){
  terminalActivityVisibility();
  if(workspace.tab==='terminal'){
    $('workspace').children[1].hidden=true;
    $('workspace').classList.add('single-pane');
  }
};
const terminalControls=controls;
controls=function(){
  terminalControls();
  const blocked=terminalBlocked();
  const busy=terminalView.opening||terminalView.closing;
  document.querySelector('[data-tab="terminal"]').disabled=blocked&&!terminalView.running;
  $('terminalRestart').disabled=blocked||busy||state.info?.terminal_available===false;
  $('terminalClose').disabled=busy||!terminalView.running;
  $('terminalFullscreen').disabled=workspace.tab!=='terminal';
  if(terminalView.running)terminalMessage('Connected');
};
const terminalConnected=connected;
connected=function(yes){
  terminalConnected(yes);
  if(!yes){terminalDrop();terminalMessage('Disconnected');}
};
$('terminalRestart').onclick=async()=>{
  if(terminalView.running&&!confirm('Restart the terminal? Running shell commands will be stopped.'))return;
  await closeTerminal();
  await openTerminal();
};
$('terminalClose').onclick=()=>closeTerminal();
$('terminalFullscreen').onclick=async()=>{
  try{
    if(document.fullscreenElement)await document.exitFullscreen();
    else await $('terminalPanel').requestFullscreen();
  }catch(error){notice(error.message,'error');}
};
controls();
