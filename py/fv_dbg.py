# Description: Debug nfp counter.
import json
import os

os.chdir(os.path.dirname(__file__))
fp = open('fv.log')
log = []
info = {}
m_flg = False
b_flg = False
s_flg = False
c_map = {0: 'R0', 1: 'A1', 2: 'R1', 3: 'A2', 4: 'A3'}
s_map = {0: 'green', 2: 'red'}
for line in fp.readlines():
    log.append(line.strip(' \n'))
i = 0
cnt = 1
while i < len(log):
    line = log[i]
    if line.startswith('_fv_m'):
        m_flg = True
    elif line.startswith('_fv_b'):
        b_flg = True
    elif line.startswith('_fv_s'):
        s_flg = True
    elif m_flg:
        if line.startswith('*'):
            m_flg = False
        elif line.startswith('0x'):
            info[cnt] = {}
            info[cnt]['fv_m'] = {}
            for j in range(5):
                line = log[i]
                tks = line.split()
                tks.extend(log[i+1].split())
                i += 2
                if len(tks) == 1: continue
                info[cnt]['fv_m'][c_map[j]] = {'cur_pir': int(tks[4], 16), 'pir': int(tks[5], 16)}            
        else:
            raise Exception('Unknown token')
    elif b_flg:
        if line.startswith('*'):
            b_flg = False
        elif line.startswith('0x'):
            info[cnt]['fv_b'] = {}
            for j in range(5):
                line = log[i]
                tks = line.split()
                i += 1
                if len(tks) == 1: continue
                info[cnt]['fv_b'][c_map[j]] = {'bpir': int(tks[1], 16) & 0x7fffffff}
        else:
            raise Exception('Unknown token')
    elif s_flg:
        if line.startswith('*'):
            s_flg = False
            cnt += 1
        elif line.startswith('0x'):
            info[cnt]['fv_s'] = {}
            tks = []
            for j in range(4):
                tks.extend(log[i].split())
                i += 1
            for j in range(5):
                info[cnt]['fv_s'][c_map[j]] = {'status': s_map[int(tks[j*3], 16)]}
        else:
            raise Exception('Unknown token')
    i += 1
fp.close()
fp = open('fv_dbg.json', 'w')
json.dump(info, fp, indent=4)
fp.close()