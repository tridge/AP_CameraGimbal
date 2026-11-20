#!/usr/bin/env python3
"""Decode level-0 MI metadata only; sampling is not a full command count."""
import argparse,collections,json,struct
from pathlib import Path
RECORD=struct.Struct('<QIIIiiiIIIBBH4I')
DEVICES=['sys','sensor','vif','isp','scl','venc','poll','vdisp','isp_fe']
def decode(data,require_qualified_errno=False,*,_magic=b'ZT30IOT0',_devices=DEVICES):
    DEVICES=_devices
    if data[:8]!=_magic or len(data)>2*1024*1024 or (len(data)-8)%64:raise ValueError('invalid ioctl header/cap/record length')
    records=[];begins={};ends={};opens={};capped=False;device_ids={}
    for offset in range(8,len(data),64):
        ns,call,tid,command,fd,result,error,major,minor,flags,device,kind,reserved,*extra=RECORD.unpack_from(data,offset)
        if not ns or not call or not tid or fd<0 or device>=len(DEVICES) or reserved or any(extra) or kind not in b'BAOL':raise ValueError('invalid ioctl record')
        if require_qualified_errno and result!=-1 and error:raise ValueError('errno valid only for return -1 in qualified captures')
        if capped:raise ValueError('record after cap marker')
        if device in device_ids and device_ids[device]!=(major,minor):raise ValueError('device identity changed')
        device_ids[device]=(major,minor)
        record={'ns':ns,'call_id':call,'tid':tid,'command':command,'fd':fd,'result':result,'errno':error if result==-1 else 0,'recorded_errno':error,'major':major,'minor':minor,'flags':flags,'device':DEVICES[device],'type':chr(kind)}
        records.append(record)
        if kind==ord('L'):capped=True;continue
        if kind==ord('O'):
            if command or result!=fd or call in begins or call in ends or call in opens:raise ValueError('invalid/duplicate open event')
            opens[call]=record
        elif kind==ord('B'):
            if result or error or flags or call in begins or call in opens:raise ValueError('invalid/duplicate ioctl entry')
            begins[call]=record
        else:
            if flags or call in ends or call in opens:raise ValueError('invalid/duplicate ioctl return')
            ends[call]=record
    pairs=[]
    for call,end in ends.items():
        begin=begins.get(call)
        if not begin or any(begin[key]!=end[key] for key in ['tid','command','fd','major','minor','device']) or end['ns']<begin['ns']:raise ValueError('unmatched/inconsistent ioctl return')
        pairs.append({'device':begin['device'],'command':begin['command'],'call_id':call,'tid':begin['tid'],'start_ns':begin['ns'],'end_ns':end['ns'],'elapsed_ns':end['ns']-begin['ns'],'fd':begin['fd'],'result':end['result'],'errno':end['errno']})
    summary={}
    for pair in pairs:
        key=pair['device']+':'+format(pair['command'],'08x')
        item=summary.setdefault(key,{'sampled_pairs':0,'returns':collections.Counter(),'min_ns':pair['elapsed_ns'],'max_ns':pair['elapsed_ns']})
        item['sampled_pairs']+=1;item['returns'][str(pair['result'])]+=1
        if pair['result']==-1:item.setdefault('posix_errno',collections.Counter())[str(pair['errno'])]+=1
        item['min_ns']=min(item['min_ns'],pair['elapsed_ns']);item['max_ns']=max(item['max_ns'],pair['elapsed_ns'])
    unmatched=[record for call,record in begins.items() if call not in ends]
    return {'records_count':len(records),'pairs_count':len(pairs),'open_count':len(opens),'capped':capped,'unmatched_entries':unmatched,'complete':bool(records) and not capped and not unmatched,'sampling':'first16 per device + full request value; bounded1024 nonzero keys/device + zero key; exit totals required; no payloads captured','device_ids':{DEVICES[k]:list(v) for k,v in device_ids.items()},'summary':summary,'pairs':pairs,'records':records}
def validate_counts(decoded,data,*,_magic=b'ZT30IOC1',_devices=DEVICES):
    DEVICES=_devices
    if len(data)<32 or data[:8]!=_magic or (len(data)-32)%32 or len(data)>32+len(DEVICES)*1025*32:raise ValueError('invalid/missing counts snapshot')
    lo,hi,untracked,short_writes,capped,reserved=struct.unpack_from('<6I',data,8)
    closed_ns=lo+(hi<<32)
    if not closed_ns or reserved or capped>1:raise ValueError('invalid counts header')
    rows={};sampled=collections.Counter((p['device'],p['command']) for p in decoded['pairs'])
    entries=collections.Counter((p['device'],p['command']) for p in decoded['records'] if p['type']=='B')
    for offset in range(32,len(data),32):
        device,command,total,granted,skipped,*extra=struct.unpack_from('<8I',data,offset)
        if device>=len(DEVICES) or not total or granted!=min(16,total) or skipped!=total-granted or any(extra):raise ValueError('invalid counts row')
        key=(DEVICES[device],command)
        if key in rows:raise ValueError('duplicate counts key')
        rows[key]={'total_before_exit_snapshot':total,'budget_granted':granted,'budget_skipped':skipped,'paired':sampled[key],'entries':entries[key]}
    if any(key not in rows for key in sampled.keys()|entries.keys()):raise ValueError('event absent from counts snapshot')
    valid=not untracked and not short_writes and not capped and all(r['paired']==r['budget_granted']==r['entries'] for r in rows.values())
    valid=valid and all(r['ns']<=closed_ns for r in decoded['records'] if r['type'] in ['B','O'])
    decoded['count_snapshot']={'closed_ns':closed_ns,'untracked_calls':untracked,'short_writes':short_writes,'capped':bool(capped),'valid':valid,'keys':{device+':'+format(command,'08x'):row for (device,command),row in rows.items()}}
    decoded['complete']=decoded['complete'] and valid
    return decoded

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('trace',type=Path);p.add_argument('--output',type=Path);p.add_argument('--counts',type=Path);p.add_argument('--require-qualified-errno',action='store_true');a=p.parse_args()
    try:r=validate_counts(decode(a.trace.read_bytes(),require_qualified_errno=a.require_qualified_errno),(a.counts or a.trace.with_name('ioctl-counts.bin')).read_bytes())
    except (ValueError,OSError) as exc:raise SystemExit(str(exc))
    output=json.dumps(r,indent=2)+'\n'
    if a.output:a.output.write_text(output)
    else:print(output,end='')
    if not r['complete']:raise SystemExit('Incomplete/capped ioctl trace')
