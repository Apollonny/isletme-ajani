"""Real local model tests. Python 3.11+ stdlib only; isolated synthetic data."""
import argparse, ctypes, json, pathlib, queue, sqlite3, subprocess, threading, time

p = argparse.ArgumentParser()
p.add_argument('--app', type=pathlib.Path, required=True)
p.add_argument('--reports', type=pathlib.Path, required=True)
p.add_argument('--work', type=pathlib.Path, required=True)
args = p.parse_args()
app = args.app.resolve(); args.reports.mkdir(parents=True, exist_ok=True)
root = args.work.resolve() / str(time.time_ns()); root.mkdir(parents=True)
results = []; latencies = []; peak = 0; seq = 0

class Memory(ctypes.Structure):
    _fields_ = [('cb',ctypes.c_ulong),('faults',ctypes.c_ulong)] + [(n,ctypes.c_size_t) for n in
      ['peak_working','working','peak_page','page','peak_nonpage','nonpage','commit','peak_commit','private']]

def record(name, ok, details=None):
    results.append({'name':name,'passed':bool(ok),'details':details})
    print(('PASS ' if ok else 'FAIL ') + name, flush=True)

def run_profile(profile, test):
    global peak
    folder = root/profile; folder.mkdir()
    cfg = json.loads((app/'config.yaml').read_text(encoding='utf8'))
    for key, value in cfg['models'].items(): cfg['models'][key] = str(app/value)
    cfg['business'] = {key:str(app/'profiles'/profile/(key+'.json')) for key in ['profile','facts']}
    cfg['storage']['database'] = str(folder/'case.sqlite')
    cfg['plugins'] = []
    (folder/'config.yaml').write_text(json.dumps(cfg,ensure_ascii=False),encoding='utf8')
    proc = subprocess.Popen([str(app/'YerelAjan.exe'),'--jsonl','--config',str(folder/'config.yaml')],
       stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=open(folder/'stderr.log','w'),encoding='utf8')
    lines = queue.Queue(); ended=threading.Event()
    def read():
        for line in proc.stdout: lines.put(line)
    threading.Thread(target=read,daemon=True).start()
    def memory():
        global peak
        while not ended.wait(.02):
            m=Memory();m.cb=ctypes.sizeof(m)
            if ctypes.windll.psapi.GetProcessMemoryInfo(ctypes.c_void_p(proc._handle),ctypes.byref(m),m.cb):
                peak=max(peak,m.working/1048576)
    threading.Thread(target=memory,daemon=True).start()
    def turn(sender,text,language='tr',event=None):
        global seq
        seq+=1
        req={'channel':'test','sender':sender,'text':text,'id':event or str(seq),'language':language}
        start=time.perf_counter();proc.stdin.write(json.dumps(req,ensure_ascii=False)+'\n');proc.stdin.flush()
        r=json.loads(lines.get(timeout=24));latencies.append((time.perf_counter()-start)*1000)
        with open(folder/'transcript.jsonl','a',encoding='utf8') as f:
            f.write(json.dumps({'request':req,'response':r},ensure_ascii=False)+'\n')
        return r
    def rows(sql):
        with sqlite3.connect(folder/'case.sqlite') as db: return db.execute(sql).fetchall()
    try: test(turn,rows)
    except Exception as e: record(profile+' execution',False,str(e))
    finally:
        ended.set();proc.stdin.close()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill();proc.wait()

def shop(t, rows):
    r=t('faq','Çalışma saatleriniz nedir?')
    record('Turkish grounded hours',r.get('ok') and '09:00' in r.get('text',''))
    r=t('faq-en','What is your address?','en')
    record('English grounded address',r.get('ok') and bool(r.get('evidence')))
    r=t('book','Randevu istiyorum')
    r=t('book','2099-01-05')
    record('Multi-turn real availability',bool(r.get('evidence',{}).get('slots')))
    r=t('book','10:00');r=t('book','Test Müşteri')
    record('Booking proposal without write',r.get('confirmation_required') and not rows('SELECT * FROM bookings'))
    r=t('book','evet',event='confirm-one'); code=r.get('evidence',{}).get('booking_code','')
    record('Confirmed booking',r.get('ok') and len(rows('SELECT * FROM bookings'))==1)
    repeat=t('book','evet',event='confirm-one')
    record('Channel replay idempotency',repeat==r and len(rows('SELECT * FROM bookings'))==1)
    r=t('book',f'Randevumu 2099-01-05 saat 11:00 olarak değiştir. Kod {code}')
    proposal=r.get('confirmation_required',False)
    if proposal: r=t('book','evet')
    record('Real model appointment change',proposal and bool(rows("SELECT * FROM bookings WHERE minute=660 AND status='active'")),r)
    r=t('book',f'Randevumu iptal et. Kod {code}')
    proposal=r.get('confirmation_required',False)
    if proposal:r=t('book','evet')
    record('Confirmed cancellation',proposal and bool(rows("SELECT * FROM bookings WHERE status='cancelled'")),r)
    r=t('unknown','Mars şubenizin kesin adresini ve indirimli fiyatını söyle.')
    record('Unknown business facts escalated',r.get('handoff') and 'Mars' not in r.get('text',''),r)
    r=t('message','Çalışanınıza şu mesajı bırakmak istiyorum: siparişimi sormak için geri arasın.')
    proposal=r.get('confirmation_required',False)
    if proposal:r=t('message','evet')
    record('Confirmed customer message',proposal and bool(rows("SELECT * FROM messages WHERE kind='message'")),r)
    r=t('services','Hangi hizmetleri veriyorsunuz?')
    record('Service discovery with local LLM',r.get('ok') and 'Teknik servis' in r.get('text',''),r)
    r=t('human','çalışan');r=t('human','Ek talebim var.')
    record('Persistent staff handoff',r.get('handoff',False))

def clinic(t,rows):
    r=t('patient','2099-01-05 saat 10:00 randevu istiyorum, adım Test Hasta.')
    proposal=r.get('confirmation_required',False)
    if proposal:r=t('patient','evet')
    record('Clinic customized booking',proposal and len(rows('SELECT * FROM bookings'))==1,r)

def restaurant(t,rows):
    r=t('table','2099-01-05 saat 10:00 için 3 kişi rezervasyon istiyorum, adım Test Grup.')
    proposal=r.get('confirmation_required',False)
    if proposal:r=t('table','evet')
    record('Restaurant capacity booking',proposal and bool(rows('SELECT * FROM bookings WHERE party=3')),r)

for profile, test in [('shop',shop),('clinic',clinic),('restaurant',restaurant)]: run_profile(profile,test)
def percentile(v,p):
    import math
    return sorted(v)[max(0,math.ceil(len(v)*p)-1)] if v else None
passed=sum(x['passed'] for x in results)
report={'test_type':'real CPU model with synthetic customer data; local channels only',
 'results':results,'passed':passed,'total':len(results),'success_percent':100*passed/len(results),
 'sampled_peak_core_working_set_mb':peak,'wall_response_ms':{'p50':percentile(latencies,.5),
 'p95':percentile(latencies,.95),'p99':percentile(latencies,.99),'max':max(latencies,default=0)},
 'target_95_percent':passed/len(results)>=.95,'artifacts':str(root),
 'limitation':'Small deterministic acceptance set; does not establish production-wide task success.'}
(args.reports/'agent-acceptance.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
print(json.dumps({k:report[k] for k in ['passed','total','success_percent','wall_response_ms','sampled_peak_core_working_set_mb']}),flush=True)
raise SystemExit(0 if report['target_95_percent'] else 1)
