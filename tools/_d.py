import sys, time, hashlib, serial, cbor2, importlib.util
sp=importlib.util.spec_from_file_location("ota","tools/mock_orion_ota.py"); ota=importlib.util.module_from_spec(sp); sp.loader.exec_module(ota)
fw=open(r"firmware/.pio/build/ori/firmware.bin","rb").read()
def frame(op,p=b""): return b"\x4F\x54"+bytes([op])+len(p).to_bytes(3,"little")+p
def P(*a): print(*a,flush=True)
ser=serial.Serial(); ser.port="COM72"; ser.baudrate=115200; ser.timeout=0.1; ser.dtr=True; ser.rts=False; ser.open()
r=ota.SerialFrameReader(ser)
N=98304; payload=fw[:N]; dg=hashlib.sha256(payload).digest(); acked=[0]
def onf(op,p):
    if op==5: acked[0]=cbor2.loads(p).get("bytes_received",0)
    elif op in (3,8): P("   <"+ota.OTA_OP_NAME[op]+">", ota._decode_reason(p))
    elif op==7: P("   <VALIDATED>")
    elif op==2: pass
def plog(o,p): P("   [ori-frame]",ota.OTA_OP_NAME.get(o,o))
time.sleep(0.3); r.drain(lambda o,p:None)
P("BEGIN total=%d"%N); ser.write(frame(0x01,cbor2.dumps({"fw_version":"1.0.1","total_size":N,"sha256":dg}))); ser.flush()
fr=r.wait_frame(8,{0x02,0x03,0x08},onf); P("READY?",fr and ota.OTA_OP_NAME.get(fr[0]))
if fr and fr[0]==0x02:
    sent=0; W=16384; nextlog=16384
    for i in range(0,N,4096):
        w0=time.monotonic()
        while sent-acked[0]>=W:
            r.drain(onf)
            if time.monotonic()-w0>6: P("   [STALL] sent=%d acked=%d"%(sent,acked[0])); break
            time.sleep(0.001)
        else:
            ser.write(frame(0x04,payload[i:i+4096])); sent+=len(payload[i:i+4096]); r.drain(onf)
            if sent>=nextlog: P("   sent=%d acked=%d"%(sent,acked[0])); nextlog+=16384
            continue
        break
    P("END sent=%d acked=%d"%(sent,acked[0])); ser.write(frame(0x06)); ser.flush()
    t=time.monotonic()+10
    while time.monotonic()<t: r.drain(onf); time.sleep(0.05)
    P("final acked=%d/%d"%(acked[0],N))
ser.close()
