'use strict';
const $=id=>document.getElementById(id), names=['前视','右侧','左侧','后视'];
function bufferChange(buffer,action,signal){return new Promise((resolve,reject)=>{const clean=()=>{buffer.removeEventListener('updateend',done);buffer.removeEventListener('error',error);signal.removeEventListener('abort',error);};const done=()=>{clean();resolve();};const error=()=>{clean();reject(Error('视频缓冲已中断'));};buffer.addEventListener('updateend',done,{once:true});buffer.addEventListener('error',error,{once:true});signal.addEventListener('abort',error,{once:true});try{action();}catch(e){clean();reject(e);}});}
let token=new URLSearchParams(location.hash.slice(1)).get('token')||sessionStorage.getItem('productToken')||'',profile=null,images={},channel=0,kind='near_polygon',streamAbort=null,lastMediaGeneration=-1;
if(token){sessionStorage.setItem('productToken',token);history.replaceState(null,'',location.pathname);}
async function api(path,body){const r=await fetch('/api/'+path,{method:body===undefined?'GET':'POST',headers:{'X-Product-Token':token,...(body===undefined?{}:{'Content-Type':'application/json'})},body:body===undefined?undefined:JSON.stringify(body)});if(r.status===401){if(!$('auth').open)$('auth').showModal();throw Error('需要访问令牌');}const data=await r.json();if(!r.ok)throw Error(data.error||r.statusText);return data;}
function text(id,value){if($(id).textContent!==value)$(id).textContent=value;}
function fail(e){text('error',e.message);$('error').hidden=false;}
$('access').onclick=()=>{$('token').value=token;$('auth').showModal();};
$('authForm').onsubmit=e=>{e.preventDefault();token=$('token').value.trim();sessionStorage.setItem('productToken',token);$('auth').close();poll();};
if(!token)$('auth').showModal();
const modeHandoff=new ModeHandoff(api,fail,()=>poll());
const parkingPanel=new ParkingPanel(api,fail);
const workflow=new ProductWorkflow(api,fail,()=>token);
const labels={DRIVE:'行驶辅助',PARK:'驻车监控',PANORAMA:'360 环视',SWITCHING:'正在切换',FAULT:'需要检查',STARTING:'启动中',PREPARING:'素材准备中'};
let pollBusy=false;
async function poll(){
 if(pollBusy)return;pollBusy=true;
 try{const s=await api('status');parkingPanel.setStatus(s);workflow.setStatus(s);$('dot').style.background='#8ed9bd';text('connection','设备已连接');text('mode',labels[s.mode]||s.mode);modeHandoff.setStatus(s);$('error').hidden=!s.error;if(s.error)text('error',s.error);
 const p=s.perception||{}, views=p.channels||[];const warning=s.error||p.calibration_error;$('error').hidden=!warning;if(warning)text('error',warning);const riskLabel=v=>!v||(!v.fresh&&!v.image_fresh)?'等待新结果':v.alert_active===0&&(v.proximity_level||v.image_approaching||v.occupied||v.approaching)?'近距候选 · 变道时检查':v.image_fresh&&v.proximity_level===2?'很近！':v.image_fresh&&v.image_approaching?'快速接近':v.image_fresh&&v.proximity_level===1?'靠近':v.approaching?'快速接近':v.occupied?'近区有目标':v.person_level===2?'行人过近':v.person_level===1?'行人近距':v.image_fresh?'近区未触发':!v.calibrated?'尚未标定':'未见近区占用';
 $('risks').replaceChildren(...names.map((name,i)=>{const v=views[i],div=document.createElement('div');div.className='risk '+(v?.alert_active!==0&&(v?.fresh&&(v.occupied||v.approaching||v.person_level)||v?.image_fresh&&(v.proximity_level||v.image_approaching))?'danger':v?.fresh&&v.calibrated?'good':'');div.append(document.createTextNode(name));const b=document.createElement('strong');b.textContent=riskLabel(v);div.append(b);return div;}));
 $('fps').replaceChildren(document.createTextNode(views[0]?Number(views[0].hz).toFixed(1):'—'),Object.assign(document.createElement('small'),{textContent:' Hz'}));text('advice',s.mode==='PARK'?'驻车人车识别运行中':p.stale?'等待新的感知结果':(p.reason||p.action||'正在观察周边'));text('cloud',s.cloud_configured?'本地记录 · 后台同步已配置':'本地记录 · 云端未启用');text('uptime',Math.floor(s.uptime_s/60)+':'+String(s.uptime_s%60).padStart(2,'0'));
 text('viewTitle',s.mode==='PARK'?'四路驻车环视':'环视监控');text('modeDetail',s.mode==='PARK'?'硬件压缩传输 · 浏览器查看':'行驶界面在 RK 显示器显示');text('streamInfo',s.media.ready?'LIVE · H.264':'等待画面');
 if(s.mode==='PARK'&&s.media.ready){if(!streamAbort||lastMediaGeneration!==s.media.generation){lastMediaGeneration=s.media.generation;startStream(s.media.codec);}}
 else{if(s.mode==='SWITCHING'||modeHandoff.pending)holdStream();else stopStream();text('emptyTitle',s.mode==='FAULT'?'设备需要检查':s.mode==='SWITCHING'?'模式切换中':'行驶辅助已就位');text('emptyText',s.mode==='SWITCHING'?'正在等待板端确认与画面':s.mode==='PANORAMA'?'360 全景在 RK 显示器显示；此模式暂停驾驶识别':'驻车后，四路环视画面将在这里显示');}
 }catch(e){parkingPanel.setStatus({mode:'FAULT'});text('connection','连接未就绪');$('dot').style.background='#f4c37a';if(token)fail(e);}finally{pollBusy=false;}}
async function events(){try{workflow.renderEvents(await api('events'));}catch(_){}}
let objectURL=null;
const livePlayback=new LivePlayback();
setInterval(()=>{if(streamAbort){const info=livePlayback.correct($('video'));if(info)text('liveBuffer','播放缓冲 '+Math.max(0,info.lag).toFixed(2)+' s');}},200);
function holdStream(){if(streamAbort){streamAbort.abort();streamAbort=null;}$('video').pause();$('viewLabels').hidden=true;}
function stopStream(){if(streamAbort){streamAbort.abort();streamAbort=null;}$('video').style.display='none';$('placeholder').hidden=false;$('viewLabels').hidden=true;if(objectURL){$('video').removeAttribute('src');$('video').load();URL.revokeObjectURL(objectURL);objectURL=null;}}
async function startStream(codec){stopStream();const mime='video/mp4; codecs="'+codec+'"';if(!window.MediaSource||!MediaSource.isTypeSupported(mime)){fail(Error('此浏览器不支持该 H.264 视频格式，请尝试新版 Chrome / Edge'));return;}
 const abort=new AbortController();streamAbort=abort;livePlayback.reset();$('video').addEventListener('loadeddata',()=>{if(streamAbort===abort)modeHandoff.ready();},{once:true,signal:abort.signal});const source=new MediaSource();objectURL=URL.createObjectURL(source);$('video').src=objectURL;
 try{await new Promise((resolve,reject)=>{source.addEventListener('sourceopen',resolve,{once:true});abort.signal.addEventListener('abort',()=>reject(Error('Aborted')),{once:true});});if(abort.signal.aborted)return;const buffer=source.addSourceBuffer(mime);const response=await fetch('/api/stream',{headers:{'X-Product-Token':token},signal:abort.signal});if(!response.ok)throw Error('视频暂不可用，请重新连接');const reader=response.body.getReader();
 while(!abort.signal.aborted){const {done,value}=await reader.read();if(done)break;await bufferChange(buffer,()=>buffer.appendBuffer(value),abort.signal);
 if(buffer.buffered.length){livePlayback.correct($('video'));const start=buffer.buffered.start(0),end=buffer.buffered.end(buffer.buffered.length-1);if(end-start>3&&$('video').currentTime-start>1.3){await bufferChange(buffer,()=>buffer.remove(start,$('video').currentTime-1),abort.signal);}}
 $('video').style.display='block';$('placeholder').hidden=true;$('viewLabels').hidden=false;$('video').play().catch(()=>{});
 }
 }catch(e){if(!abort.signal.aborted)fail(e);}finally{if(streamAbort===abort){streamAbort=null;$('video').style.display='none';$('placeholder').hidden=false;$('viewLabels').hidden=true;}}
}
$('reconnect').onclick=()=>{stopStream();poll();};
const canvas=$('canvas'),ctx=canvas.getContext('2d');
function storeFields(){if(!profile)return;const z=profile.channels[channel];z.verified=+$('verified').checked;z.metric_verified=+$('metricVerified').checked;z.area_vehicle=+$('areaVehicle').value;z.area_small=+$('areaSmall').value;z.near_m=+$('nearM').value;z.stop_m=+$('stopM').value;try{z.ground_world=JSON.parse($('world').value);}catch(e){if(z.metric_verified)throw Error('实测点 JSON 格式错误');}}
function fields(){const z=profile.channels[channel];$('verified').checked=!!z.verified;$('metricVerified').checked=!!z.metric_verified;$('areaVehicle').value=z.area_vehicle;$('areaSmall').value=z.area_small;$('nearM').value=z.near_m;$('stopM').value=z.stop_m;$('world').value=JSON.stringify(z.ground_world||[[-1,1],[1,1],[1,5],[-1,5]]);draw();}
function draw(){ctx.fillStyle='#08151b';ctx.fillRect(0,0,1280,720);if(images[channel])ctx.drawImage(images[channel],0,0,1280,720);if(!profile)return;const points=profile.channels[channel][kind]||[];ctx.strokeStyle=kind==='near_polygon'?'#8ed9bd':'#f4c37a';ctx.fillStyle=kind==='near_polygon'?'#8ed9bd25':'#f4c37a25';ctx.lineWidth=4;ctx.beginPath();points.forEach((p,i)=>i?ctx.lineTo(p[0]*1280,p[1]*720):ctx.moveTo(p[0]*1280,p[1]*720));if(points.length>=3)ctx.closePath();ctx.fill();ctx.stroke();points.forEach((p,i)=>{ctx.beginPath();ctx.fillStyle='#fff';ctx.arc(p[0]*1280,p[1]*720,7,0,Math.PI*2);ctx.fill();ctx.font='24px sans-serif';ctx.fillText(i+1,p[0]*1280+12,p[1]*720-12);});}
$('calibrate').onclick=async()=>{try{profile=await api('calibration');channel=0;$('channel').value='0';fields();$('calibration').showModal();}catch(e){fail(e);}};
$('closeCalibration').onclick=()=>$('calibration').close();$('channel').onchange=()=>{try{storeFields();channel=+$('channel').value;fields();}catch(e){text('calMessage',e.message);}};$('pointKind').onchange=()=>{kind=$('pointKind').value;draw();};
$('clearPoints').onclick=()=>{profile.channels[channel][kind]=[];if(kind==='near_polygon')$('verified').checked=false;else $('metricVerified').checked=false;draw();};
canvas.onclick=e=>{if(!images[channel]){text('calMessage','请先获取当前画面，再标定');return;}const r=canvas.getBoundingClientRect(),z=profile.channels[channel];if(!z[kind])z[kind]=[];if(z[kind].length>=4){text('calMessage','已有四个点；需要重画时点击“重画当前区域”');return;}z[kind].push([(e.clientX-r.left)/r.width,(e.clientY-r.top)/r.height]);draw();};
$('snapshot').onclick=async()=>{try{const before=await api('snapshot-meta');await api('snapshot',{});text('snapshotInfo','正在获取四路原始画面…');let meta;
 for(let i=0;i<16;i++){await new Promise(r=>setTimeout(r,250));meta=await api('snapshot-meta');if(meta.monotonic_s&&meta.monotonic_s!==before.monotonic_s)break;}
 if(!meta?.channels?.some(v=>v.valid)||meta.monotonic_s===before.monotonic_s)throw Error('未获得新画面，请确认行驶程序和视频源正在运行');
 for(let i=0;i<4;i++)if(meta.channels[i]?.valid){const r=await fetch('/api/snapshot/'+i,{headers:{'X-Product-Token':token}});if(!r.ok)continue;const url=URL.createObjectURL(await r.blob());const image=new Image();await new Promise((resolve,reject)=>{image.onload=resolve;image.onerror=reject;image.src=url;});images[i]=image;URL.revokeObjectURL(url);}
 text('snapshotInfo','已获取原始 1280 × 720 画面 · 标定须与当前镜头和素材对应');draw();
 }catch(e){text('calMessage',e.message);}};
$('saveCalibration').onclick=async()=>{try{storeFields();profile=await api('calibration',profile);text('calMessage','已保存；等待识别程序重新载入');}catch(e){text('calMessage',e.message);}};
setInterval(poll,1000);setInterval(events,3000);poll();events();
