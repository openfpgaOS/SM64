import json,struct,statistics,sys,bisect,gzip
from pathlib import Path
H=Path(__file__).resolve().parent
cols=['index','start_us','work_us','rendered','render_cycles','audio_cycles','start_cycles','game_tick','level','area','next_demo','demo_active','action','x','y','z','camera_x','camera_y','camera_z','acquire_cycles','flip_cycles','ring_spins','ring_min_free','words','cached_triangles','legacy_triangles','vertex_loads','vertex_cycles','emit_cycles','texture_cycles','matrix_cycles','reserved']
def load(p):
 rows=[];head=None
 for slot in range(6,9):
  b=gzip.decompress((p/f'slot_{slot}.bin.gz').read_bytes());hd=struct.unpack_from('<32I',b);assert hd[0]==0x31424d53 and hd[1] in (1,2) and hd[2]==32,hd[:7]
  if head: assert hd[5:7]==head[5:7]
  head=hd;rows.extend(struct.iter_unpack('<32I',b[128:128+hd[3]*128]))
 assert len(rows)==4800 and [r[0] for r in rows]==list(range(4800))
 return rows,head
for arg in sys.argv[1:]:
 p=H/arg;rows,head=load(p);mhz=head[5]/1e6
 if head[1]==2:
  cols[6]='present_count';cols[31]='last_presented_us'
 result={'run':arg,'cpu_hz':head[5],'hw_features':hex(head[6]),'format_version':head[1],'columns':cols,'rows':len(rows),'scenes':[]}
 # Split contiguous level/demo groups so title/inter-level transitions are explicit.
 groups=[]
 for r in rows:
  key=(r[8],r[9],r[10],r[11])
  if not groups or groups[-1][0]!=key:groups.append((key,[]))
  groups[-1][1].append(r)
 for key,g in groups:
  if len(g)<30:continue
  drawn=[r for r in g if r[3]];duration=((g[-1][1]+g[-1][2]-g[0][1])&0xffffffff)/1e6
  wall=[((b[1]-a[1])&0xffffffff) for a,b in zip(drawn,drawn[1:])]
  if not drawn:continue
  intervals=[]
  # Complete nonoverlapping windows of 30 fixed simulation ticks.
  for i in range(0,len(g)-30,30):
   dt=((g[i+30][1]-g[i][1])&0xffffffff)/1e6
   intervals.append(sum(r[3] for r in g[i:i+30])/dt)
  d={'key':key,'first':g[0][0],'count':len(g),'seconds':duration,'rendered':len(drawn),'render_fps':len(drawn)/duration,'min_30_tick_window_fps':min(intervals) if intervals else None,'work_ms_mean':statistics.mean(r[2] for r in g)/1000,'render_ms_mean':statistics.mean(r[4] for r in drawn)/mhz/1000,'acquire_ms_mean':statistics.mean(r[19] for r in drawn)/mhz/1000,'audio_ms_mean':statistics.mean(r[5] for r in g)/mhz/1000,'render_interval_p95_ms':sorted(wall)[int((len(wall)-1)*.95)]/1000 if wall else None,'render_interval_max_ms':max(wall)/1000 if wall else None,'ring_spins_mean':statistics.mean(r[21] for r in drawn),'cached_triangles':sum(r[24] for r in drawn),'legacy_triangles':sum(r[25] for r in drawn),'vertex_loads':sum(r[26] for r in drawn)}
  for col in [27,28,29,30]:d[cols[col].replace('_cycles','_ms_mean')]=statistics.mean(r[col] for r in drawn)/mhz/1000
  if head[1]==2:
   presents=[]
   for r in g:
    if r[6] and (not presents or presents[-1][6]!=r[6]):presents.append(r)
   if len(presents)>1:
    times=[0]
    gaps=[]
    for a,b in zip(presents,presents[1:]):
     times.append(times[-1]+((b[31]-a[31])&0xffffffff))
     gaps.append((b[6]-a[6])&0xffffffff)
    starts=set(t for t in times if t+1000000<=times[-1])
    starts.update(t-1000000 for t in times if t-1000000>=times[0])
    windows=[bisect.bisect_left(times,t+1000000)-bisect.bisect_left(times,t) for t in starts]
    d['present_average_fps']=((presents[-1][6]-presents[0][6])&0xffffffff)*1e6/times[-1]
    d['present_unobserved_events']=sum(x-1 for x in gaps)
    d['present_min_1s_fps']=min(windows) if windows and all(x==1 for x in gaps) else None
    d['present_max_interval_ms']=max(b-a for a,b in zip(times,times[1:]))/1000
  if head[1]==2 and key[-1] and len(g)>180:
   steady=g[90:-30]
   presents=[]
   for r in steady:
    if r[6] and (not presents or presents[-1][6]!=r[6]):presents.append(r)
   if len(presents)>1:
    times=[0];gaps=[]
    for a,b in zip(presents,presents[1:]):
     times.append(times[-1]+((b[31]-a[31])&0xffffffff));gaps.append((b[6]-a[6])&0xffffffff)
    starts=set(t for t in times if t+1000000<=times[-1])
    starts.update(t-1000000 for t in times if t-1000000>=times[0])
    windows=[bisect.bisect_left(times,t+1000000)-bisect.bisect_left(times,t) for t in starts]
    d['steady']={'skip_first_sim_ticks':90,'skip_last_sim_ticks':30,'sim_ticks':len(steady),
      'average_present_fps':((presents[-1][6]-presents[0][6])&0xffffffff)*1e6/times[-1],
      'unobserved_presents':sum(x-1 for x in gaps),
      'minimum_one_second_fps':min(windows) if windows and all(x==1 for x in gaps) else None,
      'maximum_present_interval_ms':max(b-a for a,b in zip(times,times[1:]))/1000,
      'render_ms_mean':statistics.mean(r[4] for r in steady if r[3])/mhz/1000,
      'work_ms_mean':statistics.mean(r[2] for r in steady)/1000}
  result['scenes'].append(d)
 (p/'frames.json').write_text(json.dumps(rows)+'\n');(p/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
 print(json.dumps({k:v for k,v in result.items() if k!='columns'},indent=2))
