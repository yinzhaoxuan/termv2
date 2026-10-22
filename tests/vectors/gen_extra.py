#!/usr/bin/env python3
"""T04 补充向量（OSC/DCS/ESC/UTF-8 边界），格式与 /workspace/qa/vectors/core01_02.json 相同。"""
import json, sys
sys.path.insert(0, "/workspace/qa/vectors")
import random
def splits(data, seed=1234):  # 与 qa/vectors/gen_vectors.py 相同
    out={"whole":[[0,len(data)]],"bytewise":[[i,i+1] for i in range(len(data))]}
    if len(data)<=512: out["all_two_splits"]=[[[0,k],[k,len(data)]] for k in range(1,len(data))]
    r=random.Random(seed); cuts=sorted(r.sample(range(1,len(data)),min(len(data)-1,8))) if len(data)>1 else []
    pts=[0]+cuts+[len(data)]; out["random"]=[[pts[i],pts[i+1]] for i in range(len(pts)-1)]
    return out
E=b"\x1b"
cases={
 "osc_bel_st": E+"]0;标题".encode()+b"\x07a"+E+b"]8;;http://x"+E+b"\\b",
 "dcs_st": E+b"P1$r"+E+b"\\",
 "osc_cancel_can": E+b"]2;x\x18y",
 "osc_esc_restart": E+b"]2;z"+E+b"cQ",
 "esc_inter": E+b"(B"+E+b"#8",
 "utf8_invalid_tail": b"\xff"+"中".encode()+b"\xe4\xb8",
 "csi_prefix_mid": E+b"[1?2h",
 "csi_empty_params": E+b"[;5H"+E+b"[1;;m"+E+b"[:3m",
 "csi_cancel_can": E+b"[1\x18A",
 "utf8_surrogate": b"\xed\xa0\x80A",
 "utf8_overlong": b"\xe0\x80\x80\xf0\x8f\xbf\xbf\xf4\x90\x80\x80B",
 "osc_c0_ignored": E+b"]2;a\x01\x0ab\x07",
}
vec=[{"case":"T04-EXTRA","name":k,"hex":v.hex(),"expect":"切分无关","splits":splits(v)} for k,v in cases.items()]
json.dump(vec,open(sys.argv[1] if len(sys.argv)>1 else "extra.json","w"),ensure_ascii=False,indent=1)
