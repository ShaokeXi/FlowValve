# Description: Run benchmark testing on remote machine.
import os
import sys
import zipfile
import seaborn as sns
import pandas as pd
import matplotlib.pyplot as plt
import fv_util as nutil
from fv_cfg import generate_nfp_rtsym

# directories on remote machine
pktgen_log = 'repo/pktgen-dpdk/log'
test_result_dir = '/root/repo/nfp_lab/benchmark/dpdk_log/'
script_dir = 'repo/nfp_lab/benchmark'
script_dpdk = 'p4_dpdk.sh'
nic_counter = '%s%s' % (test_result_dir, 'counter.log')
SENDDIR = 'send'
RECVDIR = 'recv'
VF_NUM = 8

'''
{
    <app_dir>: [
        {
            "fw": <firmware>,
            "cfg": <p4cfg>,
            "cases": [
                {"flow_time": integer, "pkt_len": integer}
            ],
            "log_dir": <log_dir>,
            "round": integer,
            "tool": integer (1: "pktgen", 2: "probing")
        }
    ]
}
<app_dir>: work directory.
<firware>: nic firmware to load.
<p4cfg>: p4 config file to load.
<log_dir>: log directory, also the figure name.
'''
def case_test(admin, proj_config, testcase):
    '''Return 0 on success, -1 failure.'''
    errno = -1
    r_map = { '1G': '0x9fe98', '10G': '0x63f1f0', '20G': '0xc7e3e0', '30G': '0x12bd5d0', '40G': '0x18fc7c0' }
    # run test cases
    sshroot, root = nutil.ssh_open(admin)
    for pname in testcase:
        for cases in testcase[pname]:
            # check if pass this test case
            if 'test' in cases and not cases['test']:
                continue
            # load the firmware
            # sys.stdout.write("%s - load firmware: %s.nffw and config file: %s" % (pname, cases['fw'], cases['cfg']))
            # sys.stdout.flush()
            # ret, err = nutil.ssh_runcmd(root, 'cd repo/nfp_repo/flowvalve/scripts; ./fv_load %s.nffw %s.json %s' % (cases['fw'], cases['fw'], cases['cfg']))
            # if ret or err:
                # print()
                # for line in err:
                    # print(line.strip().decode())
                # return errno
            # print("...done")
            print("%s - traffic generation tools: dpdk-pktgen" % pname)
            # remove counter.log
            nutil.ssh_runcmd(root, 'rm -rf %s' % nic_counter)
            for i in range(len(cases['cases'])):
                case = cases['cases'][i]
                _var = cases['var'] if 'var' in cases else 'pkt_size'
                # ./p4_dpdk.sh <args>
                if _var == 'pkt_size':
                    # set QoS
                    # assert(case['rate'] in r_map)
                    # q_cfg = generate_nfp_rtsym(r_map[case['rate']], 32)
                    # nutil.ssh_runcmd(root, q_cfg)
                    # print("set up flowvalve queuing parameters")
                    sys.stdout.write("case %d: flow_time %d sec, pkt_len %d bytes" 
                        % (i, case['flow_time'], case['pkt_len']))
                    sys.stdout.flush()
                    # using 8 vfs needs to specify -N 8 in cmds
                    vf_num = 8
                    cmds = "-p %s -t %d -l %d -i %d -R %s -N %d -O %d" \
                        % (pname, case['flow_time'], case['pkt_len'], cases['round'], cases['match'], vf_num, case['flow_num'])
                else:
                    print("Unknown varaible in case %d." % i)
                    continue
                cmds += " -T 1"
                ret, err = nutil.ssh_runcmd(root, 'cd %s; ./%s %s' % (script_dir, script_dpdk, cmds))
                if ret:
                    print()
                    for line in err:
                        print(line.strip().decode())
                    return errno
                print("...done")
            pktgen_zip = "pktgen-%s.zip" % pname
            # zip log files
            nutil.ssh_runcmd(root, 'cd %s; zip %s *; mv %s %s; rm *'
                % (pktgen_log, pktgen_zip, pktgen_zip, test_result_dir))
            # scp to local directory
            local_dir = os.path.join(proj_config['proj_dir'], pname, proj_config['benchmark_dir'])
            res = nutil.scp(admin['ip'], admin['port'], admin['username'], 
                '%s%s' % (test_result_dir, pktgen_zip), local_dir)
            # delete remote zip file
            nutil.ssh_runcmd(root, 'rm %s%s' % (test_result_dir, pktgen_zip))
            if res:
                # unzip correspondingly
                log_dir = os.path.join(local_dir, cases['log_dir'])
                if not os.path.exists(log_dir):
                    os.mkdir(log_dir)
                    os.mkdir(os.path.join(log_dir, SENDDIR))
                    os.mkdir(os.path.join(log_dir, RECVDIR))
                # scp counter.log to local
                nutil.scp(admin['ip'], admin['port'], admin['username'], 
                    '%s' % nic_counter, log_dir)
                nic_counter_analysis(os.path.join(log_dir, proj_config['nic_counter_log']), _var, pname)
                log_send_dir = os.path.join(log_dir, SENDDIR)
                log_recv_dir = os.path.join(log_dir, RECVDIR)
                with zipfile.ZipFile(os.path.join(local_dir, pktgen_zip), 'r') as zip_ref:
                    zip_ref.extractall(log_send_dir)
                log_list = zip_ref.namelist()
                # delete zip file
                os.remove(os.path.join(local_dir, pktgen_zip))
                split_recv_log(log_send_dir, log_recv_dir, log_list)
    root.close()
    sshroot.close()
    return 0

def nic_cnt2int(cnt_str):
    vals = cnt_str.split()
    res = 0
    base = 1
    for i in range(len(vals)):
        res += base * int(vals[i], 16)
        base *= 0xffffffff
    return res

# TODO: no support for multiple round analysis
'''
Return value: data = {
    <x_var_value>: {
        'rx': xx,
        'no credit': xx,
        'flowcache hit': xx,
        'flowcache miss': xx
    }
}
'''
def nic_counter_analysis(counter_log, _var, prog_name):
    data = {}
    # figure x-axis: default packet size
    var_map = { 'pkt_size': 1, 'flow_num': -1, 'opt_num': -1 }
    x_idx = var_map[_var]
    if os.path.exists(counter_log):
        fp = open(counter_log)
        prx = 0
        pcre = 0
        phit = 0
        pmiss = 0
        for line in fp.readlines():
            if line.startswith(prog_name):
                tks = line.split("-")
                # figure x-axis: default packet size
                x_var = int(tks[x_idx])
                data[x_var] = {}
            elif line.startswith('rx'):
                rx = nic_cnt2int(line.split(':')[1])
                data[x_var]['rx'] = rx - prx
                prx = rx
            elif line.startswith('no credits'):
                cre = nic_cnt2int(line.split(':')[1])
                data[x_var]['no credits'] = cre - pcre
                pcre = cre
            elif line.startswith('flowcache hit'):
                hit = nic_cnt2int(line.split(':')[1])
                data[x_var]['flowcache hit'] = hit - phit
                phit = hit
            elif line.startswith('flowcache miss'):
                miss = nic_cnt2int(line.split(':')[1])
                data[x_var]['flowcache miss'] = miss - pmiss
                pmiss = miss
            else:
                assert(nic_cnt2int(line.split(':')[1]) < 100)
        fp.close()
        nutil.dump_json(data, counter_log)
    return data

def split_recv_log(log_send_dir, log_recv_dir, log_list):
    for f in log_list:
        flog = os.path.join(log_send_dir, f)
        if os.path.exists(flog):
            content = ""
            fp = open(flog)
            new_fp = open(os.path.join(log_recv_dir, f), 'w')
            cp = 1
            for line in fp.readlines():
                tks = line.split()
                if tks:
                    if float(tks[-1]) != VF_NUM and cp:
                        new_fp.write(line)
                    else:
                        cp = 0
                        content += line
            fp.close()
            new_fp.close()
            fp = open(flog, 'w')
            fp.write(content)
            fp.close()

def pktgen_rate_analysis(filters, case, log_dir):
    data = {}
    pps = 'packets'
    bps = 'bytes'
    units = [pps, bps]
    data['data'] = {}
    # used as figure name
    data['outdir'] = case['log_dir']
    for side in filters:
        data['data'][side] = {}
        directions = filters[side]
        if os.path.exists(log_dir):
            # process pktgen log files
            for f in os.listdir(os.path.join(log_dir, side)):
                fp = open(os.path.join(log_dir, side, f))
                tks = f.strip(".log").split("-")
                # figure x-axis: default packet size
                x_var = int(tks[1])
                if 'var' in case:
                    if case['var'] == 'flow_num':
                        x_var = int(tks[-1])
                    elif case['var'] == 'opt_num':
                        x_var = int(tks[-1])
                rno = int(tks[3])-1
                for line in fp.readlines():
                    # single port statistics
                    if line.startswith('Forward'):
                        if not data['data'][side]:
                            for ui in units:
                                data['data'][side][ui] = {}
                                for di in directions:
                                    data['data'][side][ui][di] = {x_var: [0]*case['round']}
                        else:
                            for ui in units:
                                for di in directions:
                                    if x_var not in data['data'][side][ui][di]:
                                        data['data'][side][ui][di][x_var] = [0]*case['round']
                    elif line.startswith('RX-packets/s') and 'rx' in data['data'][side][pps]:
                        data['data'][side][pps]['rx'][x_var][rno] += float(line.split(':')[1].strip())/1000000
                    elif line.startswith('TX-packets/s') and 'tx' in data['data'][side][pps]:
                        data['data'][side][pps]['tx'][x_var][rno] += float(line.split(':')[1].strip())/1000000
                    elif line.startswith('RX-MBs/s') and 'rx' in data['data'][side][bps]:
                        data['data'][side][bps]['rx'][x_var][rno] += float(line.split(':')[1].strip())
                    elif line.startswith('TX-MBs/s') and 'tx' in data['data'][side][bps]:
                        data['data'][side][bps]['tx'][x_var][rno] += float(line.split(':')[1].strip())
        else:
            print("no directory: %s" % log_dir)
            return
    return data

def plot_rate(filters, x_var, case, log_dir, fig_dir):
    # sns.set_context("paper")
    sns.set_theme(style="darkgrid")
    raw_data = pktgen_rate_analysis(filters, case, log_dir)
    if not raw_data:
        return
    title = raw_data['outdir']
    data = {
        'packets': {x_var: [], 'direction': [], 'throughput (Mpps)': [], 'round': []}, 
        'bytes': {x_var: [], 'direction': [], 'throughput (MB/s)': [], 'round': []}
    }
    for side in raw_data['data']:
        case_data = raw_data['data'][side]
        # if not case_data:
        #     break
        unit_map = {'packets': 'Mpps', 'bytes': 'MB/s'}
        for ui in case_data:
            for di in case_data[ui]:
                for ln in case_data[ui][di]:
                    for i in range(len(case_data[ui][di][ln])):
                        data[ui][x_var].append(int(ln))
                        data[ui]['direction'].append('%s-%s' % (side, di))
                        data[ui]['throughput (%s)'%unit_map[ui]].append(float(case_data[ui][di][ln][i]))
                        data[ui]['round'].append(i)
    for ui in data:
        # only draw pps
        if ui == 'packets':
            continue
        df = pd.DataFrame(data[ui])
        ax = sns.lineplot(x=x_var, y='throughput (%s)'%unit_map[ui], 
            data=df, style='direction', hue='direction', markers=True)
        ax.set(title="%s-%s"%(title, ui))
        ax.set_ylim(bottom=0)
        height = ax.get_ylim()[1] - ax.get_ylim()[0]
        width = ax.get_xlim()[1] - ax.get_xlim()[0]
        gb = df.drop(columns=['round']).groupby([x_var, 'direction'])
        for key, gv in gb:
            x = key[0]
            z = key[1]
            y = gv['throughput (%s)'%unit_map[ui]].mean()
            if z.split('-')[1] == 'rx':
                plt.text(x=x-width*0.05, y=y-height*0.05, s='({},{:.2f})'.format(x, y), fontsize=7)
                # plt.text(x=x, y=y-height*0.05, s='({},{:.2f})'.format(x, y), fontsize=7)
            else:
                plt.text(x=x-width*0.05, y=y+height*0.03, s='({},{:.2f})'.format(x, y), fontsize=7)
                # plt.text(x=x, y=y+height*0.03, s='({},{:.2f})'.format(x, y), fontsize=7)
        plt.savefig(os.path.join(fig_dir, "%s-%s.png" % (title, ui[0])))
        plt.show()

if __name__ == "__main__":
    os.chdir(os.path.dirname(__file__))
    testcase = {
        "flowvalve": [
            {
                "fw": "flowvalve_prior",
                "cfg": "fv.p4cfg",
                "cases": [
                    {
                        "flow_time": 10,
                        "flow_num": 2,
                        "rate": '10G',
                        "pkt_len": 1518
                    }
                ],
                "log_dir": "fv_prior",
                "round": 3,
                "tool": 1,
                "var": "pkt_size",
                "match": "fv",
                "test": 1
            }
        ]
    }
    admin = nutil.roothost
    proj = nutil.get_json('fv.json')
    case = testcase['flowvalve'][0]
    log_dir = os.path.join('..', proj['app_dir'], proj['benchmark_dir'], case['log_dir'])
    fig_dir = os.path.join('..', proj['app_dir'], proj['benchmark_dir'], proj['figure_dir'])
    case_test(admin, proj, testcase)
    plot_rate({'send': ['tx'], 'recv': ['rx']}, 'pkt size (B)', case, log_dir, fig_dir)