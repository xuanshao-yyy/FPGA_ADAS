'use strict';
// Original bounded-live policy. Time values refer to received media, not camera exposure.
class LivePlayback {
 constructor(){this.reset();}
 reset(){this.lastEnd=null;this.step=.5;this.started=false;this.seeks=0;this.lastSeek=-1000;}
 observe(end){if(this.lastEnd!==null&&end>this.lastEnd+.01){const step=end-this.lastEnd;if(step<=.6)this.step=this.step*.5+step*.5;}this.lastEnd=end;}
 policy(current,start,end,now,seeking=false){
  if(![current,start,end,now].every(Number.isFinite)||end<=start)return {rate:1};
  const target=Math.max(.24,Math.min(.65,this.step*1.1+.03)),lag=end-current;
  const outside=current<start-.03||current>end;
  if(!seeking&&now-this.lastSeek>.25&&(outside||lag>target+.25)){
   this.lastSeek=now;this.seeks++;return {seek:Math.max(start,end-target),rate:1,lag,target};
  }
  return {rate:lag>target+.10?1.04:1,lag,target};
 }
 correct(video){
  if(!video.buffered.length)return null;
  const start=video.buffered.start(video.buffered.length-1),end=video.buffered.end(video.buffered.length-1);
  this.observe(end);const result=this.policy(video.currentTime,start,end,performance.now()/1000,video.seeking);
  if(result.seek!==undefined)video.currentTime=result.seek;
  if(video.playbackRate!==result.rate)video.playbackRate=result.rate;
  return result;
 }
}
if(typeof module!=='undefined')module.exports=LivePlayback;
if(typeof window!=='undefined')window.LivePlayback=LivePlayback;
