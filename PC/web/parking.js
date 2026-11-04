'use strict';
// Geometry runs in the viewing browser. RK keeps its hardware video path.
const ParkingGeometry=(()=>{
 function select(history,pts,maxAge){let before=null,after=null;for(const row of history){if(row.video_pts<=pts)before=row;else{after=row;break;}}if(!before||pts-before.video_pts>maxAge)return null;return {before,after};}
 function rectangle(ch,xy){return [(ch%2+xy[0])*640,(Math.floor(ch/2)+xy[1])*360,(xy[2]-xy[0])*640,(xy[3]-xy[1])*360];}
 function boxes(pair,ch,pts){const a=pair.before,b=pair.after,view=a.channels[ch];return view.boxes.map(box=>{const next=b?.channels[ch]?.boxes.find(v=>v.id===box.id);const ratio=next?Math.max(0,Math.min(1,(pts-a.video_pts)/(b.video_pts-a.video_pts))):0;return {...box,xyxy:box.xyxy.map((v,i)=>v+(next?next.xyxy[i]-v:0)*ratio)};});}
 return {select,rectangle,boxes};
})();
if(typeof module!=='undefined')module.exports=ParkingGeometry;
if(typeof window!=='undefined')window.ParkingGeometry=ParkingGeometry;

class ParkingPanel{
 constructor(api,fail){
  this.api=api;this.fail=fail;this.active=false;this.rows=[];this.session='';this.sequence=0;this.data=null;this.received=0;this.busy=false;this.audio=null;this.alarm='';
  this.el=id=>document.getElementById(id);this.video=this.el('video');this.canvas=this.el('parkOverlay');this.ctx=this.canvas.getContext('2d');
  this.el('showZones').onchange=()=>this.paint(this.video.currentTime);
  this.el('parkSound').onclick=async()=>{try{if(!this.audio){this.audio=new (window.AudioContext||window.webkitAudioContext)();await this.audio.resume();this.el('parkSound').textContent='声音已开启';this.beep();}else{await this.audio.close();this.audio=null;this.el('parkSound').textContent='开启告警声';}}catch(e){this.fail(e);}};
  this.el('parkRules').onclick=()=>this.openRules();this.el('closeParkRules').onclick=()=>this.el('parkingRules').close();this.el('saveParkRules').onclick=()=>this.saveRules();
  this.timer=setInterval(()=>{this.poll();if(this.active&&!this.fresh())this.offline();},500);
  const frame=(_time,meta)=>{this.paint(meta.mediaTime);this.video.requestVideoFrameCallback(frame);};
  if(this.video.requestVideoFrameCallback)this.video.requestVideoFrameCallback(frame);else this.video.addEventListener('timeupdate',()=>this.paint(this.video.currentTime));
 }
 setStatus(status){const was=this.active;this.active=status.mode==='PARK';this.el('parkingPanel').hidden=!this.active;this.el('drivePanel').hidden=this.active;this.el('parkLegend').hidden=!this.active;this.canvas.hidden=!this.active;
  if(!this.active){this.rows=[];this.data=null;this.session='';this.sequence=0;this.ctx.clearRect(0,0,1280,720);}else if(!was)this.poll();}
 set(id,value){const el=this.el(id);if(el.textContent!==value)el.textContent=value;}
 async poll(){if(!this.active||this.busy)return;this.busy=true;
  try{const data=await this.api('parking?since='+this.sequence+'&session='+encodeURIComponent(this.session));this.received=performance.now();this.data=data;
   const latest=data.latest||{};if(latest.session&&latest.session!==this.session){this.rows=[];this.sequence=0;this.session=latest.session;}
   for(const row of data.history||[]){if(row.session===this.session&&row.sequence>this.sequence){this.rows.push(row);this.sequence=row.sequence;}}
   this.rows=this.rows.slice(-32);this.update();this.paint(this.video.currentTime);
  }catch(e){this.data=null;this.set('parkState','识别连接中断');this.el('parkState').className='guardState offline';this.ctx.clearRect(0,0,1280,720);}finally{this.busy=false;}}
 fresh(){return !!(this.active&&this.data?.fresh&&performance.now()-this.received<2200);}
 offline(){this.set('parkState',this.data?.latest?.error||'检测结果已过期或尚未就绪');this.el('parkState').className='guardState offline';for(const view of this.el('parkViews').children){view.className='parkView offline';view.querySelector('strong').textContent='未就绪';}this.set('overlayStatus','检测框未就绪');this.ctx.clearRect(0,0,1280,720);}
 update(){
  const data=this.data,r=data.rules,last=data.latest||{},fresh=this.fresh();
  const views=last.channels||[],level=Math.max(0,...views.filter(v=>v.valid).map(v=>v.level));const full=views.length===4&&views.every(v=>v.valid);
  this.el('parkState').className='guardState '+(!fresh?'offline':level===2?'red':level===1?'yellow':full?'green':'offline');
  const unrecorded=views.some(v=>v.boxes.some(b=>b.level===2&&!b.recorded));
  this.set('parkState',!fresh?(last.error||'识别未就绪'):level===2?(data.record_error?'红色告警 · 记录失败':unrecorded?'红色告警 · 等待记录确认':'红色告警 · 已记录'):level===1?'黄色关注 · 接近阈值':full?'当前阈值下未触发':'部分视角画面无效');
  this.set('parkMetrics',fresh?'YOLOv8n · '+(r.sweep_hz===0?'不限速 · ':'')+'每路 '+views.map(v=>Number(v.hz||0).toFixed(1)).join(' / ')+' Hz':'等待有效模型与新画面');
  this.set('parkAge',data.age_s==null?'—':Number(data.age_s).toFixed(1)+' s');
  this.el('parkViews').replaceChildren(...['前','右','左','后'].map((name,ch)=>{const v=views[ch],div=document.createElement('div');div.className='parkView '+(!fresh||!v?.valid?'offline':v.level===2?'red':v.level===1?'yellow':'green');const label=document.createElement('span');label.textContent=name;const state=document.createElement('strong');state.textContent=!fresh||!v?.valid?'未就绪':['未触发','关注','告警'][v.level];div.append(label,state);return div;}));
  const measurements=views.flatMap(v=>(v.boxes||[]).filter(b=>!b.held).map(b=>({...b,ch:v.channel}))).sort((a,b)=>b.level-a.level||b.area-a.area);
  this.el('parkTargets').replaceChildren(...measurements.slice(0,4).map(b=>{const div=document.createElement('div');div.className='targetLine';div.textContent=['前','右','左','后'][b.ch]+' · '+['行人','骑行者','汽车','公交车','货车','自行车','摩托车'][b.category]+'　'+(b.area*100).toFixed(1)+'%';const detail=document.createElement('small');detail.textContent='底边 '+Math.round(b.foot*100)+'%'+(b.desired===2&&b.level<2?' · 确认 '+b.pending_s.toFixed(1)+'/'+r.confirm_s+' s':'')+(b.level===2?(b.recorded?' · 已记录':' · 记录未确认'):'');div.append(detail);return div;}));
  if(!measurements.length)this.set('parkTargets',fresh?'本轮未检出人车目标':'等待检测结果');
  this.set('thresholdSummary','行人 / 两轮 '+(r.person_yellow*100).toFixed(1)+'% → '+(r.person_red*100).toFixed(1)+'%　车辆 '+(r.vehicle_yellow*100).toFixed(1)+'% → '+(r.vehicle_red*100).toFixed(1)+'%');
  this.set('thresholdTiming','底边进入 '+Math.round(r.red_y*100)+'% 以下红区，并持续 '+r.confirm_s+' s 确认；保持 '+r.hold_s+' s。');
  this.set('parkRecord',data.record_error||'红色触发自动写入本地事件；不包含录像片段。');
  if(fresh&&level===2){const key=views.flatMap(v=>v.boxes).filter(b=>b.recorded).map(b=>b.event).join(',');if(key&&key!==this.alarm){this.alarm=key;this.beep();}}
 }
 beep(){if(!this.audio)return;const ctx=this.audio,o=ctx.createOscillator(),g=ctx.createGain();o.frequency.value=740;g.gain.setValueAtTime(.09,ctx.currentTime);g.gain.exponentialRampToValueAtTime(.001,ctx.currentTime+.25);o.connect(g);g.connect(ctx.destination);o.start();o.stop(ctx.currentTime+.26);}
 paint(pts){
  const c=this.ctx;c.clearRect(0,0,1280,720);if(!this.active)return;
  const fresh=this.fresh(),r=this.data?.rules;if(!fresh){this.set('overlayStatus','检测框未就绪');return;}
  const pair=ParkingGeometry.select(this.rows,pts,r.sweep_hz>0?Math.max(.9,1.5/r.sweep_hz):.9);
  if(!pair){this.set('overlayStatus','等待与画面对齐的检测结果');return;}
  this.set('overlayStatus','画面帧 '+pair.before.frame_id+' · 框龄 '+(pts-pair.before.video_pts).toFixed(1)+' s');
  c.font='17px "Microsoft YaHei", sans-serif';
  for(let ch=0;ch<4;ch++){
   const x=ch%2*640,y=Math.floor(ch/2)*360,v=pair.before.channels[ch];c.save();c.beginPath();c.rect(x,y,640,360);c.clip();
   if(this.el('showZones').checked){for(const [start,color,label] of [[r.yellow_y,'#f4c37a','黄区'],[r.red_y,'#ff9688','红区']]){c.fillStyle=color+'10';c.fillRect(x,y+start*360,640,(1-start)*360);c.strokeStyle=color+'88';c.lineWidth=1;c.setLineDash([6,9]);c.beginPath();c.moveTo(x,y+start*360);c.lineTo(x+640,y+start*360);c.stroke();c.setLineDash([]);c.fillStyle=color;c.fillText(label,x+570,y+start*360-7);}}
   if(v.valid)for(const b of ParkingGeometry.boxes(pair,ch,pts)){
    const [bx,by,w,h]=ParkingGeometry.rectangle(ch,b.xyxy),color=['#8ed9bd','#f4c37a','#ff9688'][b.level];c.strokeStyle=color;c.lineWidth=b.level===2?3:2;c.setLineDash(b.held?[6,6]:[]);c.strokeRect(bx,by,w,h);c.setLineDash([]);
    const label=['行人','骑行者','汽车','公交车','货车','自行车','摩托车'][b.category]+' '+(b.area*100).toFixed(1)+'%'+(b.held?' 短时保持':b.level===2?(b.recorded?' 已记录':' 告警'):'');const width=c.measureText(label).width+12,top=Math.max(y+30,by-25);c.fillStyle='#071014df';c.fillRect(bx,top,width,24);c.fillStyle=color;c.fillText(label,bx+6,top+18);
   }c.restore();
  }
 }
 async openRules(){try{this.rules=await this.api('parking/rules');const fields=[['person_yellow','行人 / 两轮 黄阈值 %',100],['person_red','行人 / 两轮 红阈值 %',100],['vehicle_yellow','车辆 黄阈值 %',100],['vehicle_red','车辆 红阈值 %',100],['yellow_y','黄区起点（由上至下）%',100],['red_y','红区起点（由上至下）%',100],['confirm_s','红色持续确认 / s',1],['hold_s','红色短时保持 / s',1],['sweep_hz','每路更新 / Hz（0 不限）',1],['confidence','模型置信度',1],['cooldown_s','同目标重复记录间隔 / s',1]];this.fields=fields;
   this.el('parkRuleFields').replaceChildren(...fields.map(([key,label,scale])=>{const l=document.createElement('label');l.textContent=label;const input=document.createElement('input');input.type='number';input.step=scale===100?'.1':'.05';input.id='rule_'+key;input.value=+(this.rules[key]*scale).toFixed(3);input.required=true;l.append(input);return l;}));this.el('guardEnabled').checked=!!this.rules.enabled;this.set('ruleMessage','面积按单路子画面计算；不是米制距离。');this.el('parkingRules').showModal();}catch(e){this.fail(e);}}
 async saveRules(){try{const value={...this.rules,enabled:+this.el('guardEnabled').checked};for(const [key,_label,scale] of this.fields)value[key]=+this.el('rule_'+key).value/scale;this.rules=await this.api('parking/rules',value);this.rows=[];this.sequence=0;this.set('ruleMessage','已保存，等待驻车识别确认新阈值。');await this.poll();}catch(e){this.set('ruleMessage',e.message);}}
}
if(typeof window!=='undefined')window.ParkingPanel=ParkingPanel;
