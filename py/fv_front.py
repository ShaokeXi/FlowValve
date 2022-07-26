# Description: fv front end implementation.
import os

max_pbs = 0x12ebc20
sec_pbs = 0xbebc20

def parse(fname):
    fp = open(fname)
    content = []
    cfg = {}
    for line in fp.readlines():
        line = line.strip(' \n')
        if not line: continue
        if line.startswith('#'): continue
        content.append(line)
    i = 0
    finish = False
    while not finish:
        if content[i].startswith('Node'):
            cfg['node'] = {}
            i += 1
            tks = content[i].split()
            for j in range(len(tks)):
                cfg['node'][tks[j]] = {'idx': j}
        elif content[i].startswith('Tree'):
            cfg['tree'] = {}
            i += 1
            while i < len(content):
                tks = content[i].split()
                if tks[0] not in cfg['tree']:
                    cfg['tree'][tks[0]] = []
                if len(tks) == 2:
                    features = tks[1].strip('()').split(',')
                    if tks[0] not in cfg['node']:
                        raise Exception('No scheduling node: %s' % tks[1])
                    for f in features:
                        f_n, f_v = f.split(':')
                        cfg['node'][tks[0]][f_n] = f_v
                elif len(tks) == 4:
                    cfg['tree'][tks[0]].append(tks[2])
                    features = tks[3].strip('()').split(',')
                    for f in features:
                        f_n, f_v = f.split(':')
                        if tks[2] not in cfg['node']:
                            raise Exception('No scheduling node: %s' % tks[1])
                        cfg['node'][tks[2]][f_n] = f_v
                i += 1
            finish = True
        i += 1
    return cfg

def cfg_node(cfg, n, p=None):
    cmd = {}
    if n not in cfg['node']:
        raise Exception('No scheduling node: %s' % n)
    nd = cfg['node'][n]
    if 'type' not in nd:
        raise Exception('No type info')
    if nd['type'] == 'static':
        if 'max' not in nd:
            raise Exception('No rate limiting info')
        unit = nd['max'][-1]
        r = float(nd['max'][:-1])
        if unit == 'M':
            r = float(r/1000)
        elif unit == 'K':
            r = float(r/1000000)
        nd['max'] = r
        pir = hex(int(0x9fe98*r))
        pbs = hex(max_pbs) if r >= 10 else hex(sec_pbs)
        bucket = pbs
        info = ['0'] * 3
        cmd['fv_m'] = ' '.join([bucket, '0', '0', '0', '0', pir, '0', '0'])
        cmd['fv_c'] = ' '.join([pbs]+info)
        cmd['fv_b'] = ' '.join([bucket, '0', '0', pbs])
    elif nd['type'] == 'rt_weight':
        if not p:
            raise Exception('No parent node info')
        denomi = 0
        for i in cfg['tree'][p]:
            denomi += int(cfg['node'][i]['weight'])
        val = 1 << 30 | int(nd['weight']) << 23 | denomi << 16 | cfg['node'][p]['idx']
        info = [hex(val), '0', '0']
        r = float(cfg['node'][p]['max']) * int(nd['weight']) / denomi
        pir = hex(int(0x9fe98*r))
        # should not set in cmds, only record
        nd['max'] = r
        pbs = hex(max_pbs) if r >= 10 else hex(sec_pbs)
        bucket = pbs
        cmd['fv_m'] = ' '.join([bucket, '0', '0', '0', '0', '0', '0', '0'])
        cmd['fv_c'] = ' '.join([pbs]+info)
        cmd['fv_b'] = ' '.join([bucket, '0', '0', pbs])
    elif nd['type'] == 'rt_cond':
        if not p:
            raise Exception('No parent node info')
        denomi = 0
        for i in cfg['tree'][p]:
            denomi += int(cfg['node'][i]['weight'])
        if nd['prio'] != '2' and nd['prio'] != '3':
            raise Exception('Unsupported runtime type')
        val = int(nd['prio']) << 30 | int(nd['weight']) << 23 | denomi << 16 | cfg['node'][p]['idx']
        unit = nd['cond'][-1]
        cr = int(nd['cond'][:-1])
        if unit == 'M':
            cr = int(cr/1000)
        elif unit == 'K':
            cr = int(cr/1000000)
        cnd0 = 0x9fe98 * cr
        if nd['prio'] == '2': # lower priority
            r = float(cfg['node'][p]['max']) * int(nd['weight']) / denomi
            cnd1 = int(cnd0 * int(nd['weight']) / denomi)
        elif nd['prio'] == '3': # higher priority
            r = cr * int(nd['weight']) / denomi
            cnd1 = int(cnd0 * (denomi - int(nd['weight'])) / denomi)            
        pir = hex(int(0x9fe98*r))
        info = [hex(val), hex(cnd0), hex(cnd1)]
        # should not set in cmds, only record
        nd['max'] = r
        pbs = hex(max_pbs) if r >= 10 else hex(sec_pbs)
        bucket = pbs
        cmd['fv_m'] = ' '.join([bucket, '0', '0', '0', '0', '0', '0', '0'])
        cmd['fv_c'] = ' '.join([pbs]+info)
        cmd['fv_b'] = ' '.join([bucket, '0', '0', pbs])
    return cmd

def cfg2cmd(cfg):
    cmds = {}
    root = set(list(cfg['node'].keys()))
    # find the root node
    for n in cfg['tree']:
        for d in cfg['tree'][n]:
            root.discard(d)
    if len(root) == 1:
        root = min(root)
        que = [root]
        # level traverse the scheduling tree
        while len(que):
            n = que.pop(0)
            if n in cfg['tree']:
                if n not in cmds:
                    cmds[n] = cfg_node(cfg, n)
                for d in cfg['tree'][n]:
                    cmds[d] = cfg_node(cfg, d, n)
                    que.append(d)
    else:
        # flat
        for n in root:
            cmds[n] = cfg_node(cfg, n)
    # transfer to string command
    fv_m = []
    fv_c = []
    fv_s = []
    # maintain sequence
    order_node = sorted(cfg['node'].keys(), key=lambda x: cfg['node'][x]['idx'])
    for n in order_node:
        fv_m.append(cmds[n]['fv_m'])
        fv_c.append(cmds[n]['fv_c'])
        fv_s.append(cmds[n]['fv_b'])
    str_cmds = {}
    str_cmds['fv_m'] = ' '.join(fv_m)
    str_cmds['fv_c'] = ' '.join(fv_c)
    str_cmds['fv_b'] = ' '.join(fv_s)
    return str_cmds

if __name__ == '__main__':
    os.chdir(os.path.dirname(__file__))
    cfg = parse('fv_mtcp.cfg')
    cmds = cfg2cmd(cfg)
    print("nfp-rtsym _fv_m %s" % cmds['fv_m'])
    print("nfp-rtsym _fv_c %s" % cmds['fv_c'])
    print("nfp-rtsym _fv_b %s" % cmds['fv_b'])