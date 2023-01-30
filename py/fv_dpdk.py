from ntpath import join
import os
import sys
import time
import paramiko
import zipfile

from fv_util import ssh_open, ssh_runcmd, scp, roothost, get_json
import numpy as np
import pandas as pd
import seaborn as sns
import matplotlib.pyplot as plt
from fv_front import parse, cfg2cmd
from fv_tc import CmdChannel

fv_dir = '/root/repo/dpdk-stable-17.08.2/examples/fv_flow/build'
fv_sched_dir = '/root/repo/dpdk-stable-17.08.2/examples/fv_sched/build'
fv_mtcp_dir = '/root/repo/dpdk-stable-17.08.2/examples/mtcpapp/build'
fv_log_dir = 'log'
fv_exec = 'fv_flow'
fv_sched_exec = 'qos_sched'
fv_mtcp_exec = 'fv_mtcp'
fv_fq_exec = 'fv_mtcp_fq'

def fv_dpdk_test(log_name, proj, d_exec=fv_dir, v_exec=fv_exec):
    sshroot, root = ssh_open(roothost)
    ftime = 60
    # begin testing
    print("begin testing", v_exec)
    for s in [1518]:
        if v_exec == fv_exec:
            fv_log = '%s_%d.log' % (v_exec, s)
            ssh_runcmd(root, 'cd %s; ./%s -p 4 -v 8 -t %d -f pcap/qos_sched_%d > %s/%s' % (d_exec, v_exec, ftime, s, fv_log_dir, fv_log))
        else:
            cmds = { 
                64: { 
                    4: ['-c', '0x3fff', '--', '--pfc', '"4,4,5,9,13"', '--pfc', '"5,5,6,10,13"', '--pfc', '"6,6,7,11,13"', '--pfc', '"7,7,8,12,13"', '--cfg', '../profile.cfg', '--pcap', 'pcap/qos_sched_64']
                },
                1024: {
                    2: ['-c', '0x27ff', '--', '--pfc', '"4,4,5,9,13"', '--pfc', '"5,5,6,9,13"', '--pfc', '"6,6,7,10,13"', '--pfc', '"7,7,8,10,13"', '--cfg', '../profile.cfg', '--pcap', 'pcap/qos_sched_1024'],
                    1: ['-c', '0x23ff', '--', '--pfc', '"4,4,5,9,13"', '--pfc', '"5,5,6,9,13"', '--pfc', '"6,6,7,9,13"', '--pfc', '"7,7,8,9,13"', '--cfg', '../profile.cfg', '--pcap', 'pcap/qos_sched_1024']
                },
                1518: {
                    2: ['-c', '0x27ff', '--', '--pfc', '"4,4,5,9,13"', '--pfc', '"5,5,6,9,13"', '--pfc', '"6,6,7,10,13"', '--pfc', '"7,7,8,10,13"', '--cfg', '../profile.cfg', '--pcap', 'pcap/qos_sched_1518'],
                    1: ['-c', '0x23ff', '--', '--pfc', '"4,4,5,9,13"', '--pfc', '"5,5,6,9,13"', '--pfc', '"6,6,7,9,13"', '--pfc', '"7,7,8,9,13"', '--cfg', '../profile.cfg', '--pcap', 'pcap/qos_sched_1518']
                }
            }
            for c in [2]:
                fv_log = '%s_%d_%d.log' % (v_exec, s, c)
                sched = CmdChannel(sshroot, 'cd %s; ./%s %s > %s/%s' % (d_exec, v_exec, ' '.join(cmds[s][c]), fv_log_dir, fv_log))
                sched.start()
                time.sleep(ftime)
                sched.stop()
    print("done")
    zip_file = 'fv.zip'
    # zip log files
    ssh_runcmd(root, 'cd %s/%s; zip %s *; mv %s %s; rm *'
        % (d_exec, fv_log_dir, zip_file, zip_file, d_exec))
    # scp to local
    local_dir = os.path.join(proj['proj_dir'], 'fv_sched', proj['benchmark_dir'])
    res = scp(roothost['ip'], roothost['port'], roothost['username'], 
        '%s/%s' % (d_exec, zip_file), local_dir)
    # delete remote zip flie
    ssh_runcmd(root, 'rm %s/%s' % (d_exec, zip_file))
    if res:
        # unzip
        log_dir = os.path.join(local_dir, log_name)
        zip_path = os.path.join(local_dir, zip_file)
        with zipfile.ZipFile(zip_path, 'r') as zip_ref:
            zip_ref.extractall(log_dir)
        time.sleep(5)
        # remove zip file
        os.remove(zip_path)
    root.close()
    sshroot.close()

# extract log to get packet throughput
def fv_dpdk_log_throughput(log_name):
    log_dir = os.path.join(proj['proj_dir'], 'fv_sched', proj['benchmark_dir'], log_name)
    for f in os.listdir(log_dir):
        data = {}
        fp = open(os.path.join(log_dir, f))
        for line in fp.readlines():
            if line.startswith('USER1:'):
                data['role'] = 'fv_fllow'
                tks = line.split(' ')
                if tks[4] == 'RX':
                    if tks[3] not in data:
                        data[tks[3]] = []
                    data[tks[3]].append(int(tks[5]))
            elif line.startswith('QoS rx:'):
                data['role'] = 'qos_sched'
                tks = line.split(' ')
                if tks[2] not in data:
                    data[tks[2]] = []
                data[tks[2]].append(int(tks[3])-int(tks[5]))
        fp.close()
        res = []
        for i in data:
            if i == 'role': continue
            # check if all 8 ports report
            if len(data[i]) != 8 and data['role'] == 'fv_flow': continue
            elif len(data[i]) != 4 and data['role'] == 'fv_sched': continue
            res.append(sum(data[i]))
        print('%s: %.2f Mpps' % (f, sum(res)/len(res)/1000000))

def fv_dpdk_log_latency(log_name):
    log_dir = os.path.join(proj['proj_dir'], 'fv_sched', proj['benchmark_dir'], log_name)
    for f in os.listdir(log_dir):
        lat = []
        fp = open(os.path.join(log_dir, f))
        for line in fp.readlines():
            if line.startswith('APP: [CPU') or line.startswith('USER1: '):
                tks = line.split(' ')
                if tks[4] == 'latency':
                    for i in [6, 9, 12, 15]:
                        if int(tks[i]):
                            lat.append(int(tks[i]))
        fp.close()
        print('%s avg (us): %.2f' % (f, np.mean(lat)))
        print('%s std (us): %.2f' % (f, np.std(lat)))

def fv_dpdk_latency_plot():
    data = { 
        'Scheduler': ['TC-HTB', 'FlowValve', 'FlowValve', 'DPDK QoS'], 
        'Network Speed': ['10Gbps', '10Gbps', '40Gbps', '40Gbps'], 
        'Latency (us)': [36.74, 36.00, 162.93, 70.38]
    }
    err = [348.25, 0.30, 0.30, 83.29]
    fig, ax = plt.subplots(figsize=(4, 2.3))
    df = pd.DataFrame(data)
    sns.barplot(x='Network Speed', y='Latency (us)', hue='Scheduler', data=df)
    fig.subplots_adjust(bottom=0.25, right=0.93, left=0.17)
    plt.grid(True, axis='y', linestyle='--', alpha=0.5)
    plt.show()

def fv_dpdk_plot(log_name, proj, v_exec=fv_exec):
    log_file = os.path.join(proj['proj_dir'], 'dpdk', proj['benchmark_dir'], log_name, '%s.log' % v_exec)
    fig_dir = os.path.join(proj['proj_dir'], 'dpdk', proj['figure_dir'])
    fp = open(log_file)
    raw_data = {}
    for line in fp.readlines():
        tks = line.split()
        if len(tks) > 10 and tks[0] == 'USER1:':
            if tks[4] == 'RX':
                raw_data[int(tks[3])] = {}
                raw_data[int(tks[3])][tks[9].strip('flow')] = tks[10]
                raw_data[int(tks[3])][tks[12].strip('flow')] = tks[13]
                raw_data[int(tks[3])][tks[15].strip('flow')] = tks[16]
                raw_data[int(tks[3])][tks[18].strip('flow')] = tks[19]
    t_min = min(raw_data)
    t_max = max(raw_data)
    raw_data[t_min-0.01] = {'1': 0}
    raw_data[t_max+0.01] = {'4': 0}
    data = {'Time (s)': [], 'Throughput (Gbps)': [], 'Flow': []}
    for t in raw_data:
        for f in raw_data[t]:
            data['Time (s)'].append(t)
            data['Throughput (Gbps)'].append(int(raw_data[t][f])*1518*8/1000000000)
            data['Flow'].append('F %d' % int(f))
    df = pd.DataFrame(data)
    plt.figure(figsize=(5, 4))
    sns.set_style('whitegrid', {'grid.linestyle': '--'})
    sns.set_palette('tab10')
    ax = sns.lineplot(x='Time (s)', y='Throughput (Gbps)', data=df, style='Flow', hue='Flow')
    ax.set(title=log_name)
    ax.set_ylim(bottom=0)
    ax.legend(ncol=4)
    plt.savefig(os.path.join(fig_dir, '%s.png' % log_name))
    plt.show()

def fv_mtcp_test(log_name, proj, v_exec=fv_mtcp_exec, param=False, test=True):
    sshroot, root = ssh_open(roothost)
    if param:
        cfg = parse('fv_mtcp.cfg')
        cmds = cfg2cmd(cfg)
        print("update fv_m: %s" % cmds['fv_m'])
        ssh_runcmd(root, 'nfp-rtsym _fv_m %s' % cmds['fv_m'])
        print("update fv_c: %s" % cmds['fv_c'])
        ssh_runcmd(root, 'nfp-rtsym _fv_c %s' % cmds['fv_c'])
        # print("update fv_b: %s" % cmds['fv_b'])
        # ssh_runcmd(root, 'nfp-rtsym _fv_b %s' % cmds['fv_b'])
        time.sleep(5)
    if test:
        print("begin testing")
        fv_log = '%s.log' % v_exec
        ssh_runcmd(root, 'cd %s; ./%s 10.0.0.100 -p 1 2> %s/%s' % (fv_mtcp_dir, v_exec, fv_log_dir, fv_log))
        print("done")
        zip_file = 'fv_mtcp.zip'
        # zip log files
        ssh_runcmd(root, 'cd %s/%s; zip %s *; mv %s %s; rm *'
            % (fv_mtcp_dir, fv_log_dir, zip_file, zip_file, fv_mtcp_dir))
        # scp to local
        local_dir = os.path.join(proj['proj_dir'], 'fv_sched', proj['benchmark_dir'])
        res = scp(roothost['ip'], roothost['port'], roothost['username'], 
            '%s/%s' % (fv_mtcp_dir, zip_file), local_dir)
        # delete remote zip flie
        ssh_runcmd(root, 'rm %s/%s' % (fv_mtcp_dir, zip_file))
        if res:
            # unzip
            log_dir = os.path.join(local_dir, log_name)
            zip_path = os.path.join(local_dir, zip_file)
            with zipfile.ZipFile(zip_path, 'r') as zip_ref:
                zip_ref.extractall(log_dir)
            time.sleep(5)
            # remove zip file
            os.remove(zip_path)
    root.close()
    sshroot.close()

def fv_mtcp_log(log_name, proj, v_exec=fv_mtcp_exec, tx=True, p_map=None):
    log_file = os.path.join(proj['proj_dir'], 'fv_sched', proj['benchmark_dir'], log_name, '%s.log' % v_exec)
    fp = open(log_file)
    raw_data = {}
    cnt = 0
    no = 0
    pps = 0
    for line in fp.readlines():
        if line.startswith('[CPU'):
            tks = line.split()
            if not p_map:
                if tks[2] == 'dpdk0':
                    if cnt not in raw_data:
                        raw_data[cnt] = {}
                    fid = int(tks[1].strip(']'))
                    if fid < 4:
                        if tx: raw_data[cnt][fid] = float(tks[13])
                        if not tx: raw_data[cnt][fid] = float(tks[8])
            else:
                if tks[2] != 'dpdk0':
                    no = int(tks[3])
                    if no not in raw_data:
                        raw_data[no] = {}
                    fid = p_map[tks[5]]
                    if fid not in raw_data[no]:
                        raw_data[no][fid] = 0
                    raw_data[no][fid] += float(tks[6])
        elif line.startswith('[ ALL ]'):
            tks = line.split()
            pps += int(tks[7])
            cnt += 1
    if not p_map:
        raw_data[cnt-0.4] = {}
        for i in range(4):
            raw_data[cnt-0.4][i] = 0
    else:
        raw_data[0] = {}
        raw_data[no+0.5] = {}
        for i in range(4):
            raw_data[0][i] = 0
            raw_data[no+0.5][i] = 0
    print("avg packet throughput: %.2f" % (pps/cnt/1000000))
    return raw_data

def fv_mtcp_plot(log_name, raw_data, proj, interval=0.2):
    fig_dir = os.path.join(proj['proj_dir'], 'fv_sched', proj['figure_dir'])
    data = {'Time (seconds)': [], 'Throughput (Gbps)': [], 'Flow': []}
    app_map = { 0: 'NC', 1: 'WS', 2: 'KVS', 3: 'ML'}
    for t in raw_data:
        for f in raw_data[t]:
            data['Time (seconds)'].append(t*interval)
            data['Throughput (Gbps)'].append(raw_data[t][f])
            data['Flow'].append(app_map[int(f)])
    df = pd.DataFrame(data)
    fig, ax = plt.subplots(figsize=(4, 2.5))
    sns.set_palette('tab10')
    sns.lineplot(x='Time (seconds)', y='Throughput (Gbps)', data=df, style='Flow', hue='Flow', ax=ax)
    # sns.set_style('whitegrid', {'grid.linestyle': '--'})
    # ax.set(title='FlowValve')
    ax.set_ylim(bottom=0)
    ax.set_xlim(left=0)
    ax.legend(bbox_to_anchor=(1.04, -0.25), ncol=4, frameon=False)
    # left parameter needs tuning
    fig.subplots_adjust(bottom=0.28, right=0.98, left=0.16)
    plt.grid(True, axis='y', linestyle='--', alpha=0.5)
    plt.savefig(os.path.join(fig_dir, '%s.png' % log_name))
    plt.show()

def fv_mtcp(proj):
    # fv_mtcp_test('fv_mtcp_wq_1', proj, v_exec=fv_fq_exec, param=True, test=True)
    # fv_mtcp_test('fv_mtcp_v6_10g', proj, v_exec=fv_mtcp_exec, param=True, test=False)
    # fv_mtcp_test('fv_mtcp_fq', proj, v_exec=fv_exec, param=True, test=False)
    p_map = {
        '5001': 0, '5005': 0, '5009': 0, '5013': 0,
        '5002': 1, '5006': 1, '5010': 1, '5014': 1,
        '5003': 2, '5007': 2, '5011': 2, '5015': 2,
        '5004': 3, '5008': 3, '5012': 3, '5016': 3
    }
    # raw_data = fv_mtcp_log('fv_mtcp_fq', proj, fv_fq_exec, p_map=p_map)
    # fv_mtcp_plot('fv_mtcp_wq_1', raw_data, proj)
    # raw_data = fv_mtcp_log('fv_mtcp_v6_10g', proj, fv_mtcp_exec)
    # fv_mtcp_plot('fv_mtcp_v6_10g', raw_data, proj)
    raw_data = fv_mtcp_log('fv_mtcp_v6_10g', proj, fv_mtcp_exec)
    for t in raw_data:
        a1 = raw_data[t][0] if raw_data[t][0] else ''
        a2 = raw_data[t][1] if raw_data[t][1] else ''
        a3 = raw_data[t][2] if raw_data[t][2] else ''
        a4 = raw_data[t][3] if raw_data[t][3] else ''
        print("{},{},{},{},{}".format(t*0.2, a1, a2, a3, a4))

if __name__ == '__main__':
    os.chdir(os.path.dirname(__file__))
    proj = get_json('fv.json')
    fv_mtcp(proj)
    # fv_dpdk_test('qos_sched', proj, d_exec=fv_sched_dir, v_exec=fv_sched_exec)
    # fv_dpdk_test('fv_flow_22_1_28', proj, d_exec=fv_dir, v_exec=fv_exec)
    # fv_dpdk_log_throughput('qos_sched')
    # fv_dpdk_log_latency('fv_flow')