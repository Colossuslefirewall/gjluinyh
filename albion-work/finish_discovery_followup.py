from pathlib import Path
import base64,difflib,hashlib,json,re,zlib
from datetime import datetime,timezone
R=Path(r'C:\Users\admin\Desktop\Albion-Experimental-20261120')
S=R/'Source/WindowsProject1-Stable';B=R/'checkpoints/pre-discovery-performance/Source'
E=R/'evidence';D=R/'benchmarks/discovery-performance'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def digest(text):return hashlib.sha256(text.encode('utf-8')).hexdigest()
checks=json.loads((E/'final-validation.json').read_text(encoding='utf-8-sig'))
assert len(checks)==4 and all(x['returncode']==0 for x in checks),checks
assert 'CATALOG_PASS' in (R/'logs/final-candidate-windows-core.log').read_text(errors='replace')
assert 'DISCOVERY_UI_PASS' in (R/'logs/final-candidate-ui.log').read_text(errors='replace')
def live(case):
 p=D/(case+'-live.log');text=p.read_text(errors='replace')
 assert 'LIVE_PIPELINE_PASS case='+case in text,(case,text[-2000:])
 if case=='candidate':assert 'LIVE_SOURCE_PASS' in text
 metrics={}
 for match in re.finditer(r'^(\w+) mean=([-+.\deE]+) min=([-+.\deE]+) max=([-+.\deE]+) samples=(\d+)$',text,re.M):
  key,mean,low,high,count=match.groups();metrics[key]={'mean':float(mean),'min':float(low),'max':float(high),'samples':int(count)}
 assert len(metrics)==10,metrics
 cpu=re.search(r'cpu_seconds=([-+.\deE]+)',text)
 return {'log':p.relative_to(R).as_posix(),'sha256':sha(p),'cpu_seconds':float(cpu.group(1)),'metrics':metrics}
baseline,candidate=live('baseline'),live('candidate')
original=json.loads((R/'checkpoints/pre-discovery-performance/evidence/profiles-before-native-ui.json').read_text(encoding='utf-8-sig'))
assert all(Path(p).is_file() and sha(Path(p))==h for p,h in original.items()),'Original profiles changed'
executables={r'C:\Users\admin\Desktop\Albion-Merged-20261115-1235\Portable\Albion.exe':'903e5145b80256f36ce4bb6bce793ce00ff17af417d1997679c36fdbe874e849',r'C:\Users\admin\Desktop\Albion-Merged-20261115-1235\Portable\WindowsProject1.exe':'0d25efb3721c757117f0644966f162c666245fcfcbb0b0996db27b8ae8528cd'}
assert all(sha(Path(p))==h for p,h in executables.items()),'Stable executable changed'
files={};patch=[];raw={}
for rel in sorted(json.loads((E/'discovery-source-patch.json').read_text(encoding='utf-8-sig'))):
 p=S/rel;new=p.read_text(encoding='utf-8-sig');raw[rel]=sha(p);item={'sha256':digest(new)}
 old=(B/rel).read_text(encoding='utf-8-sig') if (B/rel).exists() else ''
 if (B/rel).exists():
  before,after=old.splitlines(keepends=True),new.splitlines(keepends=True)
  item.update(base_sha256=digest(old),edits=[[i,j,after[a:b]] for tag,i,j,a,b in difflib.SequenceMatcher(a=before,b=after,autojunk=False).get_opcodes() if tag!='equal'])
  rebuilt=before[:]
  for i,j,replacement in reversed(item['edits']):rebuilt[i:j]=replacement
  assert ''.join(rebuilt)==new
 else:item['full']=new
 files[rel]=item
 patch.extend(difflib.unified_diff(old.splitlines(keepends=True),new.splitlines(keepends=True),fromfile='pre-discovery/'+rel,tofile='candidate/'+rel))
p=R/'scripts/apply-discovery.py';script=p.read_text(encoding='utf-8-sig')
payload=base64.b64encode(zlib.compress(json.dumps(files,ensure_ascii=False).encode('utf-8'))).decode('ascii')
script,count=re.subn(r'^files=.*$',lambda _:"files=json.loads(zlib.decompress(base64.b64decode('"+payload+"')))",script,flags=re.M)
assert count==1,count
p.write_text(script,encoding='utf-8')
(E/'discovery-source-patch.json').write_text(json.dumps({p:f['sha256'] for p,f in files.items()},indent=2),encoding='utf-8')
(E/'discovery-performance.patch').write_text(''.join(patch),encoding='utf-8')
report={'verified_at_utc':datetime.now(timezone.utc).isoformat(),'verified':True,'source_files':raw,'catalog_and_native_ui_passed':True,'original_profile_count':len(original),'original_profiles_unchanged':True,'both_stable_executables_unchanged':True,'baseline':baseline,'candidate':candidate,'optimization_applied':False,'performance_gain_claimed':False,'scope':'One warmed 60-second generated-frame run per build, mode 0, GPU timing enabled, physical output disabled. Real DXGI capture, D3D preprocessing, TensorRT inference, decode and result publication. No detections: nonempty tracking/prediction, physical delivery and game/render latency are not established. CPU wall timings and GPU event timings are distinct. Baseline is the pre-follow-up Experimental checkpoint, not the Stable executable.'}
(E/'discovery-performance.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
lines=['# Discovery follow-up: measured scope','',report['scope'],'','## Observations','', '| Metric | Pre-follow-up Experimental | Updated Experimental |','|---|---:|---:|']
for key in baseline['metrics']:
 lines.append('| '+key+' (mean) | '+str(baseline['metrics'][key]['mean'])+' | '+str(candidate['metrics'][key]['mean'])+' |')
lines+=['','These are diagnostic observations, not a speedup or equivalence claim. The generated input is paced by Sleep(8), painting and desktop presentation; its FPS does not establish maximum inference throughput. Capture wall time covers successful acquisition calls. Backend CPU wall time includes backend submission/waits; GPU compute is CUDA-event time. Capture-to-result ends at detector publication; delivery is result-consumer delay, not physical input delivery. UI-sampled tracking/output timing is repeated snapshot sampling with aim disabled and zero objects, not active tracking latency. Rendering and per-frame preprocessing GPU distributions are not exported by this harness. Separate pipeline_breakdown logs measure CPU/shared-input preprocessing and backend stages, not end-to-end behavior.','', 'The packaged compute-8.6 engine was rejected on this compute-8.9 RTX 4050. Both live runs use the same separately built local FP32 engine; no user model was converted or replaced. No performance optimization was introduced from this limited workload.','', 'Correctness checks: discovery preserves working settings, dirty/draft edits and selections; Apply is explicit and generation/version-bound. Manual refresh works with auto off. Live model removal/replacement retains the existing inference session. All five modes remain covered by the existing native UI and estimator regressions.','', 'Raw evidence: baseline-live.log, candidate-live.log, and evidence/discovery-performance.json. Earlier CPU/shared microbenchmarks remain separate.']
(D/'REPORT.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')
p=R/'README-EXPERIMENTAL.md';text=p.read_text(encoding='utf-8-sig')
text=text.replace('- Algorithm CPU timings are not end-to-end latency. Real model/capture/inference stage distributions, GPU/VRAM under application load, physical-device delivery, live-game quality, anti-cheat compatibility, and authenticated startup remain unverified and are not certified by these tests.','- Algorithm CPU timings are not end-to-end latency. The discovery follow-up additionally exercises real capture/preprocessing/TensorRT inference on generated frames (see below). Active tracking/prediction on detections, physical-device delivery, rendering/game quality, GPU-utilization/VRAM behavior, anti-cheat compatibility and authenticated startup remain unverified.')
section='''\n## File discovery follow-up\nAutomatic discovery is on by default for each application session; either Auto button toggles the same service. Refresh remains available when Auto is off. Model folders are executable-relative `models/` and the executable directory; profiles are executable-relative `data/configs/`. Folder buttons display the exact nonrecursive paths and supported extensions. Model rows distinguish [models] and [exe].\n\nRefresh, file changes and model-list selections do not activate anything. Select a model and use **Load / reload**, or explicitly **Apply** a profile. The pre-existing one-time startup model choice is retained; files appearing later are not auto-loaded. A listed file is an unvalidated candidate, not a promise of model/schema compatibility. Settling, missing and inaccessible files are not loaded. No engine builder is invoked by discovery.\n\nRefresh preserves selected paths, working settings and dirty/name-draft edits. Profile parsing runs off the UI thread; explicit Apply rejects stale request, selection, settings revision, catalog generation and source-version results. Changed/removed active files leave loaded model sessions and settings intact; status tells you to reload/apply. Deleted selections do not silently select a neighbor.\n\nThe worker uses directory notifications, a 250 ms debounce, 750 ms settling checks, 30-second reconciliation and 5-second degraded-watcher retries. Auto off disables unsolicited discovery, while manual refresh still completes stability checks. Shutdown cancels and joins the worker. The catalog tests run in the Windows suite (not the platform-independent test glob), including directory recreation, overflow, locks, concurrent refresh, stale reads and handle cleanup.\n\nSee `evidence/discovery-performance.json`, `evidence/discovery-performance.patch` and `benchmarks/discovery-performance/REPORT.md`. One instrumented, warmed 60-second generated-frame run was measured for the pre-follow-up and updated Experimental builds on the same local FP32 engine. No detections were produced and physical output was disabled. No performance optimization or speedup claim follows from these runs. The pre-follow-up rollback checkpoint remains under `checkpoints/pre-discovery-performance`.\n'''
if '\n## File discovery follow-up\n' in text:text=text.split('\n## File discovery follow-up\n')[0]
p.write_text(text.rstrip()+'\n'+section,encoding='utf-8')
p=R/'scripts/package_candidate.py';text=p.read_text(encoding='utf-8-sig')
old="'verification_scope':'Offline synthetic/core and native settings UI only. No authenticated startup, live capture/inference, gameplay, physical HID, or end-to-end latency proof.'"
new="'discovery_performance':json.loads((C/'evidence/discovery-performance.json').read_text(encoding='utf-8')), 'verification_scope':'Offline regressions, native settings/discovery UI, and real DXGI/D3D/TensorRT generated-frame pipeline. No active-target, authenticated startup, gameplay, physical-input or full end-to-end latency proof; no performance gain claim.'"
assert old in text or new in text
p.write_text(text.replace(old,new),encoding='utf-8')
with (R/'PROGRESS.md').open('a',encoding='utf-8') as f:f.write('\nDiscovery follow-up verification passed: Windows catalog (including generation invalidation), portable/core, Release and native UI; warmed generated-frame baseline/candidate pipelines; loaded model removal/replacement continuity. Original '+str(len(original))+' profiles and both Stable executables rehashed unchanged. No performance optimization or speedup claim; packaging follows. See evidence/discovery-performance.json.\n')
print('DISCOVERY_EVIDENCE_PASS source_files='+str(len(raw))+' original_profiles='+str(len(original)))
