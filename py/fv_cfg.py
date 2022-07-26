import os
import math
import json
import copy

rule_tmpl = {
    "action": {
        "data": {},
        "type": ""
    },
    "name": "",
    "match": {}
}

def qos_flat(snum, blen):
    rules = []
    port = 5001
    for i in range(snum):
        tmp = {}
        # action field
        tmp["action"] = {}
        tmp["action"]["type"] = "act_flow"
        tmp["action"]["data"] = {}
        tmp["action"]["data"]["snum"] = {}
        tmp["action"]["data"]["snum"]["value"] = snum
        tmp["action"]["data"]["sub"] = {}
        tmp["action"]["data"]["sub"]["value"] = hex(int(bin(1<<i)[2:].zfill(int(math.pow(2,blen))), 2))
        tmp["action"]["data"]["inum"] = {}
        tmp["action"]["data"]["inum"]["value"] = 1
        tmp["action"]["data"]["idx"] = {}
        tmp["action"]["data"]["idx"]["value"] = hex(int('{:<040}'.format(bin(i)[2:].zfill(blen)), 2))
        # name field
        tmp["name"] = {}
        tmp["name"] = "flow" + str(port)
        # match field
        tmp["match"] = {}
        tmp["match"]["tcp.dstPort"] = {}
        tmp["match"]["tcp.dstPort"]["value"] = port
        rules.append(tmp)
        port = port + 1
    return rules

def qos_flat2(i_map, blen):
    rules = []
    for i in i_map:
        for p in i_map[i]:
            entry = copy.deepcopy(rule_tmpl)
            entry['name'] = 'flow' + str(p)
            entry['match']['tcp.dstPort'] = { 'value': p }
            entry['action']['data']['snum'] = { 'value': len(i_map) }
            entry['action']['data']['sub'] = { 'value': hex(int(bin(1<<i)[2:].zfill(int(math.pow(2,blen))), 2)) }
            entry['action']['data']['inum'] = { 'value': 1 }
            entry['action']['data']['idx'] = { 'value': hex(int('{:<040}'.format(bin(i)[2:].zfill(blen)), 2)) }
            entry['action']['type'] = "act_flow"
            rules.append(entry)
    return rules

# dpdk traffic generator
def to_vf(n):
    ports = ["v0.0", "v0.1", "v0.2", "v0.3", "v0.4", "v0.5", "v0.6", "v0.7"]
    start = 0x111111111110
    rules = []
    for k in range(len(ports)):
        entry = copy.deepcopy(rule_tmpl)
        entry['name'] = 'to_%s' % ports[k]
        entry['match']['eth.dstAddr'] = {
            'value': start + k
        }
        entry['action']['data']['port'] = {
            'value': ports[k]
        }
        entry['action']['type'] = "act_forward"
        rules.append(entry)
    return rules

# mTCP
def port_forward(wires):
    rules = []
    for w in wires:
        entry = copy.deepcopy(rule_tmpl)
        entry['name'] = 'to_%s' % w
        entry['match']['standard_metadata.ingress_port'] = {
            'value': w
        }
        entry['action']['data']['port'] = {
            'value': wires[w]
        }
        entry['action']['type'] = "act_forward"
        rules.append(entry)
    return rules

def dpdk_config(n, fcfg):
    p4cfg = {}
    p4cfg["tables"] = { 'tbl_flow': {}, 'tbl_forward': {}, 'tbl_schedule': {} }
    p4cfg["tables"]["tbl_flow"]["rules"] = qos_flat(n, 5)
    p4cfg["tables"]["tbl_forward"]["rules"] = to_vf(8)
    p4cfg['tables']['tbl_schedule']['default_rule'] = { 'action': { 'type': 'act_schedule' }, 'name': 'default' }
    fp = open(fcfg, 'w')
    json.dump(p4cfg, fp, indent=4)
    fp.close()

def mtcp_config(fcfg):
    p4cfg = {}
    p4cfg["tables"] = { 'tbl_flow': {}, 'port_forward': {}, 'tbl_schedule': {} }
    i_map = {
        1: [5001, 5005, 5009, 5013],
        3: [5002, 5006, 5010, 5014],
        5: [5003, 5007, 5011, 5015],
        6: [5004, 5008, 5012, 5016]
    }
    p4cfg["tables"]["tbl_flow"]["rules"] = qos_flat2(i_map, 5)
    wires = {
        'v0.0': 'p0', 'p0': 'v0.0', 'v0.1': 'p2', 'p2': 'v0.1', 
        'v0.2': 'p1', 'p1': 'v0.2', 'v0.3': 'p3', 'p3': 'v0.3'
    }
    p4cfg["tables"]["port_forward"]["rules"] = port_forward(wires)
    p4cfg['tables']['tbl_schedule']['default_rule'] = { 'action': { 'type': 'act_schedule' }, 'name': 'default' }
    fp = open(fcfg, 'w')
    json.dump(p4cfg, fp, indent=4)
    fp.close()

def generate_nfp_rtsym(rate, n):
    base = 'nfp-rtsym _mm'
    unit = ' 0xbebc20 0 0 0xbebc20 ' + str(rate) + ' 0 0 0'
    cmd = [base]
    for i in range(n):
        cmd.append(unit)
    cmd = ''.join(cmd)
    return cmd

if __name__ == "__main__":
    os.chdir(os.path.dirname(__file__))
    # dpdk_config(32, 'fv_32.p4cfg')
    mtcp_config('fv_mtcp_wq.p4cfg')