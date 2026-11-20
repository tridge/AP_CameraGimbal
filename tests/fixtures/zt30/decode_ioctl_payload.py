#!/usr/bin/env python3
"""Strict flat snapshot/metadata correlation; raw bytes are not API validity."""
import argparse,json,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parent
DEVICES=['sys','sensor','vif','isp','scl','venc']
RECORD=struct.Struct('<Q4I3i9I256s6I')
assert RECORD.size==352
REGISTRY=json.loads((ROOT/'registry.json').read_text())['entries']
LAYOUTS={(DEVICES.index(e['device']),int(e['request'],16)):e for e in REGISTRY}
def decode(data,footer,metadata,fixture=False):
 if data[:8]!=b'ZT30PAY1' or len(data)>2*1024*1024 or (len(data)-8)%352:raise ValueError('invalid payload header/cap/record length')
 if len(footer)!=32 or footer[:8]!=b'ZT30PAC1':raise ValueError('missing/invalid payload totals')
 count,errors,capped,unavailable,lo,hi=struct.unpack_from('<6I',footer,8);closed=lo+(hi<<32)
 if errors or capped or not closed:raise ValueError('payload write/cap/timestamp failure')
 pairs={p['call_id']:p for p in metadata['pairs']};records=[];observed={}
 expected={}
 for pair in metadata['pairs']:
  if pair['device'] not in DEVICES:continue
  key=(DEVICES.index(pair['device']),pair['command']);layout=LAYOUTS.get(key)
  if not layout:continue
  expected[(pair['call_id'],'B')]=pair
  if layout['post_snapshot']:expected[(pair['call_id'],'A')]=pair
 for offset in range(8,len(data),352):
  ns,call,tid,command,device,status,error,copied,flags,length,low,high,d0,d1,d2,d3,phase,payload,capture,major,minor,*reserved=RECORD.unpack_from(data,offset)
  layout=LAYOUTS.get((device,command));kind=chr(phase) if phase in (65,66) else '?';key=(call,kind);pair=pairs.get(call)
  if not layout or not ns or ns>closed or not call or not tid or any(reserved) or key in observed or key not in expected or flags&~31 or length!=layout['length'] or error<0:raise ValueError('invalid/duplicate/unexpected payload record')
  ids=[1,3 if device==0 else 5] if fixture and device<2 else layout['device_rdev']
  if [major,minor]!=ids or pair['device']!=DEVICES[device] or pair['command']!=command or pair['tid']!=tid or not pair['start_ns']<=ns<=pair['end_ns']:raise ValueError('payload/metadata identity or ordering mismatch')
  if kind=='B' and (status or flags&28):raise ValueError('invalid pre-call status/flags')
  if kind=='A' and (not flags&4 or bool(flags&8)!=(status==0) or status!=pair['result']):raise ValueError('invalid post-call status/flags')
  wanted=layout['pre_read_length'] if kind=='B' else layout['length']
  if flags&1:
   if d0!=length or d1 or d2!=low or d3!=high or capture!=wanted or low&3:raise ValueError('invalid masked descriptor or capture length')
   if not fixture and (low<4096 or low>=0xc0000000 or high!=(0xffffffff if low&0x80000000 else 0)):raise ValueError('invalid ARM payload pointer')
  elif any([low,high,d0,d1,d2,d3,capture]):raise ValueError('unavailable descriptor contains data')
  if flags&2:
   if not flags&1 or error or copied!=capture or any(payload[capture:]):raise ValueError('inconsistent complete snapshot or mask')
  elif any(payload) or (copied<0 and not error):raise ValueError('unavailable snapshot contains bytes or lacks error')
  rec={'ns':ns,'call_id':call,'tid':tid,'device':DEVICES[device],'command':command,'name':layout['function'],'phase':kind,'raw_status':status,'flags':flags,'length':length,'capture_length':capture,'copied':copied,'copy_error':error,'snapshot_complete':bool(flags&2),'output_validity':'not established' if kind=='A' else 'input bytes only','opaque_attributes':layout['opaque_attributes'],'data_hex':payload[:capture].hex() if flags&2 else None,'descriptor':[d0,d1,d2,d3],'rdev':[major,minor]}
  observed[key]=rec;records.append(rec)
 if count!=len(records) or unavailable!=sum(not r['snapshot_complete'] for r in records):raise ValueError('payload totals disagree with stream')
 for (call,kind),rec in observed.items():
  if kind=='A':
   pre=observed.get((call,'B'))
   if not pre or rec['ns']<pre['ns']:raise ValueError('missing/out-of-order pre snapshot')
   if rec['flags']&1 and pre['flags']&1 and rec['descriptor']!=pre['descriptor']:raise ValueError('post descriptor changed')
 missing=[{'call_id':call,'phase':phase} for call,phase in expected.keys()-observed.keys()]
 return {'records_count':count,'unavailable':unavailable,'expected_count':len(expected),'missing':missing,'coverage_complete':bool(records) and not missing,'complete':bool(records) and not missing and not unavailable and metadata['complete'],'closed_ns':closed,'scope':'sampled flat bytes, not nested pointers, API field validity or complete command history','records':records}
def main():
 p=argparse.ArgumentParser();p.add_argument('payload',type=Path);p.add_argument('counts',type=Path);p.add_argument('metadata',type=Path);args=p.parse_args()
 print(json.dumps(decode(args.payload.read_bytes(),args.counts.read_bytes(),json.loads(args.metadata.read_text())),indent=2))
if __name__=='__main__':main()
