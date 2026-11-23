import GlobalAudioCuesPanel from '../GlobalAudioCuesPanel.jsx';
import React,{useEffect,useMemo,useState} from 'react';
import {invoke} from '@tauri-apps/api/core';
import {RefreshCw,Square,Upload,Volume2,VolumeX} from 'lucide-react';
import Button from '../shell/Button';
import {createDeviceVolumeController,EMPTY_VOLUME_STATE} from './device-volume-controller.js';
import './device-volume.css';
import {useExitProtection} from '../lib/exit-protection.js';
import {compressWavFileForBoard} from '../lib/audio-cue.js';

const BUILTIN_WAV_TESTS=[
  ['done','任务完成'],['error','发生错误'],['waiting_user','等待操作'],
  ['permission_request','请求审批'],['permission_approved','审批通过'],
  ['permission_denied','审批拒绝'],['permission_expired','审批超时'],
  ['session_terminated','会话结束'],
];

export default function DeviceVolumePanel({usb,active=true}) {
  const [state,setState]=useState({...EMPTY_VOLUME_STATE});
  const control=useMemo(()=>createDeviceVolumeController({invoke,onState:setState}),[]);
  const engine=usb?.capabilities?.audioMasterVolume;
  const supportsGlobalAudioCues=usb?.capabilities?.globalAudioCues==='p4-global-audio-cues-v1';
  const supportsWavTest=usb?.capabilities?.audioTestPlayback==='usb-pcm16-test-v1'
    && usb?.capabilities?.audio?.playbackAvailable===true;
  const [wav,setWav]=useState(null);
  const [wavState,setWavState]=useState({busy:false,error:'',message:''});
  useEffect(()=>{void control.setTarget({connected:usb?.connected,boardDeviceId:usb?.boardDeviceId,bootId:usb?.bootId,connectionId:usb?.connectionId,
    portName:usb?.portName,runtime:usb?.runtime,engine});},[control,usb?.connectionId,usb?.connected,usb?.boardDeviceId,usb?.bootId,usb?.portName,usb?.runtime,engine]);
  useEffect(()=>()=>control.dispose(),[control]);
  useEffect(()=>{if(!active)return;const timer=setInterval(()=>{const s=control.getSnapshot();if(s.known&&s.draft===s.value&&!s.persisted&&!s.busy)void control.refresh();},5000);return()=>clearInterval(timer);},[active,control]);
  useExitProtection(state.busy ? "设备音量请求尚未完成" : wavState.busy ? "WAV 正在下发或播放" : "");
  const disabled=!state.available||!state.known||state.busy;
  const selectWav=async file=>{
    setWav(null);setWavState({busy:true,error:'',message:'正在转换 WAV…'});
    try{
      if(!file||!/\.wav$/i.test(file.name))throw new Error('请选择 WAV 文件');
      const bytes=await compressWavFileForBoard(file);
      const durationMs=Math.max(1,Math.round((bytes.byteLength-44)/32));
      if(durationMs>15000)throw new Error('WAV 最长支持 15 秒');
      setWav({name:file.name,bytes,durationMs});
      setWavState({busy:false,error:'',message:`已准备 ${file.name} · ${(durationMs/1000).toFixed(1)} 秒`});
    }catch(error){setWavState({busy:false,error:String(error?.message||error),message:''});}
  };
  const selectBuiltinWav=async cue=>{
    if(!cue)return;
    setWav(null);setWavState({busy:true,error:'',message:'正在载入内置音效…'});
    try{
      const response=await fetch(`/terrier-clips/${cue}.wav`);
      if(!response.ok)throw new Error('内置音效资源不可用');
      const bytes=new Uint8Array(await response.arrayBuffer());
      const durationMs=Math.max(1,Math.round((bytes.byteLength-44)/32));
      if(durationMs>15000)throw new Error('内置音效超过 15 秒限制');
      const label=BUILTIN_WAV_TESTS.find(([id])=>id===cue)?.[1]||cue;
      setWav({name:`内置：${label}`,bytes,durationMs});
      setWavState({busy:false,error:'',message:`已选择内置音效：${label} · ${(durationMs/1000).toFixed(1)} 秒`});
    }catch(error){setWavState({busy:false,error:String(error?.message||error),message:''});}
  };
  const playWav=async()=>{
    if(!wav||!usb?.boardDeviceId||wavState.busy)return;
    setWavState({busy:true,error:'',message:'正在下发并通过设备扬声器播放…'});
    try{
      const result=await invoke('usb_play_wav_test',{expectedBoardDeviceId:usb.boardDeviceId,wavBytes:Array.from(wav.bytes)});
      setWavState({busy:false,error:'',message:`播放完成 · ${((result?.durationMs||wav.durationMs)/1000).toFixed(1)} 秒`});
    }catch(error){
      const message=String(error?.message||error);
      setWavState({busy:false,error:message==='WAV 播放已停止'?'':message,message:message==='WAV 播放已停止'?'播放已停止。':''});
    }
  };
  const stopWav=async()=>{
    if(!usb?.boardDeviceId)return;
    try{await invoke('usb_stop_wav_test',{expectedBoardDeviceId:usb.boardDeviceId});}
    catch(error){setWavState(current=>({...current,error:String(error?.message||error)}));}
  };
  return <><div className="device-volume">
    <div className="device-volume__level">
      {state.known&&state.value===0?<VolumeX size={22}/>:<Volume2 size={22}/>}
      <input type="range" min="0" max="100" step="1" value={state.draft??0} disabled={disabled}
        aria-label="设备总音量" aria-valuetext={state.known?`${state.draft}%`:'尚未读取设备音量'}
        onChange={event=>control.setDraft(Number(event.target.value))}
        onPointerUp={()=>{void control.apply();}} onKeyUp={()=>{void control.apply();}}/>
      <output aria-live="polite">{state.known?`${state.draft}%`:'—'}</output>
    </div>
    <div className="device-volume__presets">
      {[0,25,50,75,100].map(value=><Button key={value} variant="ghost" size="small" disabled={disabled}
        onClick={()=>{void control.apply(value);}}>{value===0?'静音':`${value}%`}</Button>)}
      <Button variant="ghost" size="small" disabled={!state.available||state.busy}
        onClick={()=>{void control.refresh();}}><RefreshCw size={14}/>读取设备音量</Button>
    </div>
    <p className="device-volume__message" role={state.error?'alert':undefined}>
      {state.error || (!state.available ? state.reason : state.loading ? '正在读取设备音量…' : state.busy ? '正在应用，请等待设备确认…'
        : !state.known ? '请先读取设备音量' : !state.persisted ? (state.savePending===false ? '当前音量已应用，但尚未保存到设备；重启后可能恢复原值。' : '已应用，等待设备空闲后保存。') : state.value===0 ? '设备已静音。' : '音量已保存到设备。')}
    </p>
    {supportsWavTest&&<div className="device-volume__wav">
      <div className="device-volume__wav-actions">
        <select aria-label="内置设备试听音效" disabled={wavState.busy} defaultValue="" onChange={event=>{void selectBuiltinWav(event.target.value);}}>
          <option value="" disabled>选择内置音效</option>
          {BUILTIN_WAV_TESTS.map(([id,label])=><option key={id} value={id}>{label}</option>)}
        </select>
        <label className="btn btn-ghost btn-small">
          <Upload size={14}/>选择 WAV
          <input type="file" accept="audio/wav,.wav" disabled={wavState.busy} onChange={event=>{void selectWav(event.target.files?.[0]);event.target.value='';}}/>
        </label>
        <Button size="small" disabled={!wav||wavState.busy||!usb?.connected} onClick={()=>{void playWav();}}>
          <Volume2 size={14}/>下发并播放
        </Button>
        <Button variant="ghost" size="small" disabled={!wavState.busy} onClick={()=>{void stopWav();}} title="停止 WAV 播放">
          <Square size={13}/>停止
        </Button>
      </div>
      <p className="device-volume__message" role={wavState.error?'alert':undefined}>
        {wavState.error||wavState.message||'选择内置音效或最长 15 秒的本地 WAV，在设备 ES8389 扬声器上试听。'}
      </p>
    </div>}
  </div>{supportsGlobalAudioCues&&<GlobalAudioCuesPanel boardDeviceId={usb?.connected?usb?.boardDeviceId:undefined} canSyncToDevice/>}</>;
}
