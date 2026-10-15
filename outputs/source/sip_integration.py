"""Loopback SIP/RTP test with real VAD, STT, agent and TTS. No PBX required."""
import argparse, ctypes, json, math, pathlib, struct, subprocess, threading, time, wave

p=argparse.ArgumentParser();p.add_argument('--app',type=pathlib.Path,required=True)
p.add_argument('--reports',type=pathlib.Path,required=True);p.add_argument('--work',type=pathlib.Path,required=True)
p.add_argument('--runs',type=int,default=3);p.add_argument('--full-stt-context',action='store_true');p.add_argument('--with-text-load',action='store_true');a=p.parse_args()
app=a.app.resolve();out=a.reports.resolve();out.mkdir(parents=True,exist_ok=True)
folder=a.work.resolve()/str(time.time_ns());folder.mkdir(parents=True)
cfg=json.loads((app/'config.yaml').read_text(encoding='utf8'))
for key,value in cfg['models'].items():cfg['models'][key]=str(app/value)
cfg['models']['vad']=str(app/'models/silero-vad.onnx')
cfg['business']={key:str(app/'profiles/shop'/(key+'.json')) for key in ['profile','facts']}
cfg['storage']['database']=str(folder/'case.sqlite');cfg['resources']['bounded_stt_context']=not a.full_stt_context
(folder/'config.yaml').write_text(json.dumps(cfg,ensure_ascii=False),encoding='utf8')
subprocess.run([str(app/'YerelAjan.exe'),'--tts','Çalışma saatlerinizi öğrenmek istiyorum.',str(folder/'question48.wav')],stdout=subprocess.DEVNULL,check=True)
def read(path):
    with wave.open(str(path),'rb') as f:
        sr=f.getframerate();x=struct.unpack('<'+'h'*f.getnframes(),f.readframes(f.getnframes()))
        return sr,x
sr,x=read(folder/'question48.wav');x=x[::3]
with wave.open(str(folder/'question.wav'),'wb') as w:
    w.setparams((1,2,16000,0,'NONE',''));w.writeframes(struct.pack('<'+'h'*len(x),*x))
question_end=(max(i for i,v in enumerate(x) if abs(v)>500)+1)/16000

class Memory(ctypes.Structure):
    _fields_=[('cb',ctypes.c_ulong),('faults',ctypes.c_ulong)]+[(n,ctypes.c_size_t) for n in
       ['peak_working','working','peak_page','page','peak_nonpage','nonpage','commit','peak_commit','private']]
processes=[];last_cpu={};stop=threading.Event();peak=0;started=time.perf_counter()
def sample():
    global peak
    while not stop.wait(.02):
        total=0
        for proc in list(processes):
            h=ctypes.c_void_p(proc._handle);m=Memory();m.cb=ctypes.sizeof(m)
            if ctypes.windll.psapi.GetProcessMemoryInfo(h,ctypes.byref(m),m.cb):total+=m.working/1048576
            times=[ctypes.c_ulonglong() for _ in range(4)]
            if ctypes.windll.kernel32.GetProcessTimes(h,*[ctypes.byref(v) for v in times]):
                last_cpu[proc.pid]=(times[2].value+times[3].value)/10000000
        peak=max(peak,total)
threading.Thread(target=sample,daemon=True).start()
core=subprocess.Popen([str(app/'YerelAjan.exe'),'--serve','--config',str(folder/'config.yaml')],
    cwd=folder,stdout=open(folder/'core.log','w'),stderr=subprocess.STDOUT);processes.append(core)
sip=None;rows=[]
try:
    for _ in range(80):
        time.sleep(.2)
        try:
            health=subprocess.run([str(app/'YerelAjan.exe'),'--request','{"op":"health"}'],capture_output=True,timeout=1)
            if health.returncode==0:break
        except subprocess.TimeoutExpired:pass
    else:raise RuntimeError('Core not ready')
    if a.with_text_load:
        loaded=subprocess.run([str(app/'YerelAjan.exe'),'--request',json.dumps({'channel':'test','sender':'memory-load','id':'resident-llm','text':'Hangi hizmetleri veriyorsunuz?'},ensure_ascii=False)],
                              capture_output=True,encoding='utf8',timeout=24,check=True)
        (folder/'text-load.json').write_text(loaded.stdout,encoding='utf8')
    sip=subprocess.Popen([str(app/'YerelAjanSIP.exe'),'--local-test'],cwd=folder,
        stdout=open(folder/'sip.log','w'),stderr=subprocess.STDOUT);processes.append(sip);time.sleep(2)
    for i in range(a.runs):
        output=out/f'sip-{"resident" if a.with_text_load else "baseline" if a.full_stt_context else "bounded"}-{i}.wav'
        proc=subprocess.Popen([str(app/'SipProbe.exe'),str(folder/'question.wav'),str(output)],
           cwd=folder,stdout=subprocess.PIPE,stderr=subprocess.PIPE);processes.append(proc)
        stdout,stderr=proc.communicate(timeout=25)
        if proc.returncode:raise RuntimeError(stderr.decode(errors='replace'))
        sr,reply=read(output);active=[k for k,v in enumerate(reply) if abs(v)>500]
        delay=(active[0]/sr-question_end)*1000 if active else None
        rows.append({'run':i,'first_meaningful_audio_ms':delay,'recording':output.name,
            'response_seconds':len(reply)/sr,'passed':delay is not None and 0<delay<1500})
        print(json.dumps(rows[-1]),flush=True);time.sleep(.3)
finally:
    stop.set()
    for proc in [sip,core]:
        if proc and proc.poll() is None:proc.terminate();proc.wait(timeout=5)
    for proc in processes:
        if proc.poll() is None:proc.kill();proc.wait()
values=sorted(r['first_meaningful_audio_ms'] for r in rows if r['first_meaningful_audio_ms'] is not None)
report={'type':'Local UDP SIP/RTP, synthetic EMA speech input, real Silero/Whisper/agent/EMA output',
 'bounded_stt_context':not a.full_stt_context,'llm_resident_after_real_text_turn':a.with_text_load,'runs':rows,'p50_ms':values[max(0,math.ceil(len(values)*.5)-1)] if values else None,
 'p95_ms':values[max(0,math.ceil(len(values)*.95)-1)] if values else None,'p99_ms':max(values,default=None),'max_ms':max(values,default=None),
 'sampled_peak_sum_working_set_mb':peak,'cpu_seconds_sum':sum(last_cpu.values()),
 'wall_seconds':time.perf_counter()-started,'target_passed':all(r['passed'] for r in rows),
 'measurement':'Input last sample above amplitude 500 to output first sample above 500; includes RTP scheduling. Codec timing approx ±40 ms.',
 'limitations':'FAQ workload only. No external PBX, noise, real caller or universal LLM voice latency certification.'}
(out/('sip-resident-llm.json' if a.with_text_load else 'sip-baseline.json' if a.full_stt_context else 'sip-local.json')).write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
print(json.dumps(report),flush=True)
