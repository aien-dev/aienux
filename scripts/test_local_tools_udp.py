#!/usr/bin/env python3
"""UDP adapter oracle, synthetic result bytes; real CPL3 is covered separately in QEMU."""
import json, pathlib, socket, struct, subprocess, sys, tempfile, time
repo = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='local-tools-udp-') as tmp:
    root = pathlib.Path(tmp)
    oracle = subprocess.Popen([sys.executable,str(repo/'scripts/local_tools_model_oracle.py'),str(root)])
    relay = None
    try:
        until=time.monotonic()+5
        while not (root/'oracle-port.txt').exists():
            assert time.monotonic()<until, 'oracle startup'
            time.sleep(.02)
        model_port=int((root/'oracle-port.txt').read_text())
        reserve=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); reserve.bind(('127.0.0.1',0))
        port=reserve.getsockname()[1]; reserve.close()
        relay=subprocess.Popen([str(repo/'target/debug/allen-courier'),'--udp',str(port),
            '--model',f'127.0.0.1:{model_port}','--work',str(root/'work'),'--log-dir',str(root/'logs')],
            stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); sock.bind(('127.0.0.1',0)); sock.settimeout(.2)
        run='abc0000000000421'; under=1; seen=set(); tools=[]
        def send(kind,seq,part,total,body):
            data=f'ALLEN-COURIER v=1 run={run} kind={kind} seq={seq} part={part}/{total} bytes={len(body)}\n{body}'
            sock.sendto(data.encode(),('127.0.0.1',port))
        time.sleep(.2)
        send('HELLO',0,0,0,'')
        job='JOB 4242 do Check tools.\tCheck arithmetic, text and system.\tlocal-tools=v1'
        send('JOB',under,1,1,job)
        deadline=time.monotonic()+20
        while time.monotonic()<deadline:
            try: packet=sock.recv(4096).decode()
            except socket.timeout: continue
            head,body=packet.split('\n',1)
            fields=dict(x.split('=',1) for x in head.split(' ')[1:])
            if fields['kind']!='LINE': continue
            seq=int(fields['seq']); part=int(fields['part'].split('/')[0])
            send('GOT',seq,part,0,'')
            if (seq,part) in seen: continue
            seen.add((seq,part))
            assert seq==under, ('transport sequence is not semantic job ID',seq,under)
            if body.startswith('WAIT '): continue
            if body=='SAY 4242 1/1 VERIFIED': break
            p=body.split(' ')
            assert p[0]=='CALL' and p[1]=='4242',body
            inv,tool,total,off=map(int,p[2:6]); data=bytes.fromhex(p[6]) if p[6]!='-' else b''
            assert off==0 and len(data)==total,body
            tools.append(tool)
            if tool==1:
                assert struct.unpack('<qqq',data)==(1,20,22)
                result=struct.pack('<q',42)
            elif tool==2:
                assert data==b'one two\nthree'
                result=struct.pack('<QQQ',13,3,1)
            else:
                assert tool==3 and not data
                result=struct.pack('<QQQQQ',4,1000000,1920,1080,1)
            under+=1
            send('JOB',under,1,1,f'RESULT 4242 {inv} 2 0 {result.hex()}')
        else: raise AssertionError('no final answer before deadline')
        assert tools==[1,2,3],tools
        assert oracle.wait(timeout=3)==0
        print('LOCAL_TOOLS_UDP: PASS distinct transport/job IDs, three calls and results')
        # The HTTP model is gone. Direct arithmetic must still call the native
        # tool, accept only its bound result, and render without a second model.
        for job_id, ask, operands, state, reason, raw, said in [
            (4243, 'What is 123 multiplied by 456?', (3,123,456), 2, 0,
             struct.pack('<q',42), '42.'),
            (4244, '1 / 0', (4,1,0), 3, 2, b'', "Division by zero isn't defined."),
            (4245, '9223372036854775807 + 1', (1,9223372036854775807,1), 3, 1,
             b'', 'That result is outside the supported 64-bit integer range.'),
        ]:
            under+=1
            start=time.monotonic(); calls=0
            send('JOB',under,1,1,f'JOB {job_id} do Calculate locally.\t{ask}\tlocal-tools=v1')
            while time.monotonic()-start < 5:
                try: packet=sock.recv(4096).decode()
                except socket.timeout: continue
                head,body=packet.split('\n',1)
                fields=dict(x.split('=',1) for x in head.split(' ')[1:])
                if fields['kind']!='LINE': continue
                seq=int(fields['seq']); part=int(fields['part'].split('/')[0])
                send('GOT',seq,part,0,'')
                if (seq,part) in seen: continue
                seen.add((seq,part))
                assert seq==under, (seq,under)
                if body.startswith('WAIT '): continue
                if body.startswith('SAY '):
                    assert body==f'SAY {job_id} 1/1 {said}',body
                    assert calls==1,calls
                    break
                p=body.split(' ')
                assert p[:6]==['CALL',str(job_id),'1','1','24','0'],body
                assert struct.unpack('<qqq',bytes.fromhex(p[6]))==operands
                calls+=1; under+=1
                send('JOB',under,1,1,f'RESULT {job_id} 1 {state} {reason} {raw.hex() or "-"}')
            else: raise AssertionError('direct arithmetic stalled with model offline')
            print(f'LOCAL_DIRECT_UDP: PASS job={job_id} elapsed_ms={(time.monotonic()-start)*1000:.2f} model=offline')
    finally:
        oracle.terminate(); oracle.wait()
        if relay is not None: relay.terminate(); relay.wait()
