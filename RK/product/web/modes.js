'use strict';
// One state-driven handoff layer. No animation loop or additional polling timer.
class ModeHandoff {
 constructor(api,fail,refresh){
  this.api=api;this.fail=fail;this.refresh=refresh;this.status=null;this.pending='';this.mediaReady=false;this.generation=-1;
  this.el=id=>document.getElementById(id);
  for(const [id,target] of [['drive','DRIVE'],['park','PARK'],['panorama','PANORAMA'],['returnDrive','DRIVE']])this.el(id).onclick=()=>this.request(target);
 }
 async request(target){
  if(this.pending||this.status?.mode==='SWITCHING'||this.status?.mode==='PREPARING'||this.status?.mode===target)return;
  this.pending=target;this.paint();
  try{await this.api('mode',{mode:target});this.refresh();}
  catch(e){this.pending='';this.paint();this.fail(e);}
 }
 setStatus(status){
  this.status=status;
  if(this.pending&&((status.mode===this.pending&&status.requested===this.pending)||status.error&&status.mode!=='SWITCHING'))this.pending='';
  if(status.mode!=='PARK'||status.media?.generation!==this.generation)this.mediaReady=false;
  this.generation=status.media?.generation;this.paint();
 }
 ready(){this.mediaReady=true;this.paint();}
 paint(){
  const s=this.status||{mode:'STARTING',requested:'DRIVE'},labels={DRIVE:'驾驶辅助',PARK:'驻车守护',PANORAMA:'360 环视'};
  const switching=Boolean(this.pending)||['STARTING','SWITCHING','PREPARING'].includes(s.mode),target=this.pending||s.requested||s.mode;
  const waitingVideo=s.mode==='PARK'&&!this.mediaReady;
  this.el('modeTransition').hidden=!(switching||waitingVideo);
  this.el('transitionTitle').textContent=switching?'正在进入'+(labels[target]||'工作界面'):'驻车视频正在连接';
  this.el('transitionPhase').textContent=this.pending&&s.mode!=='SWITCHING'?'模式请求已发送，等待设备确认':s.phase||'正在等待设备确认与新画面';
  this.el('transitionNote').textContent=waitingVideo&&!switching?'视频缓冲就绪后显示；你可随时返回驾驶。':'交接期间保持上一画面；它不是实时画面。';
  this.el('returnDrive').hidden=!['PARK','PANORAMA','FAULT'].includes(s.mode);
  this.el('returnDrive').disabled=switching;
  for(const [id,mode] of [['drive','DRIVE'],['park','PARK'],['panorama','PANORAMA']]){
   const button=this.el(id);button.disabled=switching;button.classList.toggle('active',s.mode===mode&&!switching);
   button.setAttribute('aria-pressed',String(s.mode===mode&&!switching));
  }
  this.el('switchInfo').textContent=switching?(s.phase||'等待设备切换'):
    s.fpga?(s.fpga.pending?'FPGA 正在切换':'FPGA 已回报：'+(labels[s.fpga.mode]||s.fpga.mode)+' · 帧 '+s.fpga.frame):'FPGA 控制尚未确认';
 }
}
if(typeof module!=='undefined')module.exports=ModeHandoff;
