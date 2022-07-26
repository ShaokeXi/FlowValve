import os
import sys
import time
import zipfile
import threading
from matplotlib.font_manager import json_load
# import nic_util as nutil
import pandas as pd
import seaborn as sns
import matplotlib.pyplot as plt

sh_dir = '/root/repo/nfp_repo/flowvalve/scripts'
iperf_dir = '/root/repo/nfp_repo/iperf_p4wire/script'
tc_log = '/root/repo/nfp_repo/iperf_p4wire/log'

class CmdChannel(threading.Thread):
    def __init__(self, client, cmd):
        threading.Thread.__init__(self)
        self.client = client
        self.cmd = cmd

    def run(self):
        self.shell = self.client.invoke_shell()
        self.shell.send(self.cmd)
        self.shell.send('\n')
    
    def stop(self):
        self.shell.send('\x03')

# def tc_test(admin, proj, log_name, udp=False):
#     sshroot, root = nutil.ssh_open(admin)
#     # # clear tc qdisc
#     # nutil.ssh_runcmd(root, 'cd %s; ./tcrm.sh' % sh_dir)
#     # # set tc qdisc
#     # nutil.ssh_runcmd(root, 'cd %s; ./fv_tc' % sh_dir)
#     flow_num = 4
#     # start iperf server
#     iperf_servers = []
#     for i in range(flow_num):
#         iperf_server = CmdChannel(sshroot, 'cd %s; ./iperf_server.sh -p p%d -n %d' % (iperf_dir, i, i+1))
#         iperf_server.start()
#         iperf_servers.append(iperf_server)
#     # if not sleep here, iperf_server will not start immediately
#     time.sleep(2)
#     starts = [0, 0, 0, 15]
#     ends = [15, 45, 30, 60]
#     ftime = [15, 45, 30, 45]
#     ifstart = [False, False, False, False]
#     ifend = [False, False, False, False]
#     allend = 0
#     # use multiple Intel ports
#     done = False
#     timer = 0
#     while not done:
#         for i in range(flow_num):
#             if timer >= starts[i] and not ifstart[i]:
#                 ifstart[i] = True
#                 print('start flow ', i+1)
#                 if udp:
#                     nutil.ssh_runcmd(root, 'cd %s; ./iperf_client.sh -v v0 -u -n %d -t %d > /dev/null &' % (iperf_dir, i, ftime[i]))
#                 else:
#                     nutil.ssh_runcmd(root, 'cd %s; ./iperf_client.sh -v v0 -n %d -t %d > /dev/null &' % (iperf_dir, i, ftime[i]))
#             if timer >= ends[i] and not ifend[i]:
#                 ifend[i] = True
#                 print('\nstop flow ', i)
#                 iperf_servers[i].stop()
#                 allend += 1
#         if allend == 4: break
#         time.sleep(1)
#         timer += 1
#         sys.stdout.write('\r%d' % timer)
#         sys.stdout.flush()
#         time.sleep(1)
#     # download result
#     tc_zip = '%s.zip' % log_name
#     tc_zip_dir = '/root/repo/nfp_repo/iperf_p4wire/'
#     # zip log files
#     nutil.ssh_runcmd(root, 'cd %s; zip %s *; mv %s %s; rm *'
#         % (tc_log, tc_zip, tc_zip, tc_zip_dir))
#     # scp to local directory
#     local_dir = os.path.join(proj['proj_dir'], 'tc', proj['benchmark_dir'])
#     res = nutil.scp(admin['ip'], admin['port'], admin['username'], admin['password'], 
#         '%s%s' % (tc_zip_dir, tc_zip), local_dir)
#     # delete remote zip file
#     # nutil.ssh_runcmd(root, 'rm %s%s' % (tc_zip_dir, tc_zip))
#     if res:
#         # unzip correspondingly
#         log_dir = os.path.join(local_dir, log_name)
#         with zipfile.ZipFile(os.path.join(local_dir, tc_zip), 'r') as zip_ref:
#             zip_ref.extractall(log_dir)
#         # delete zip file
#         os.remove(os.path.join(local_dir, tc_zip))
#     sshroot.close()
#     root.close()

# def tc_netperf(admin, proj, log_name):
#     # measure latency using netperf on tc fq
#     sshroot, root = nutil.ssh_open(admin)
#     flows = {
#         1: ('192.168.11.100', 5001),
#         2: ('192.168.11.101', 5002),
#         3: ('192.168.11.102', 5003),
#         4: ('192.168.11.103', 5004)
#     }
#     for i in flows:
#         out = 'netserver-%d.log' % flows[i][1]
#         print("latency test on flow %d" % flows[i][1])
#         nutil.ssh_runcmd(root, 'cd %s; ip netns exec net-v0 netperf -H %s -t TCP_STREAM -p %d -l 20 -- -o min_latency,max_latency,mean_latency,stddev_latency,throughput > %s &' % (tc_log, flows[i][0], flows[i][1], out))
#     # should be enough
#     time.sleep(30)
#      # download result
#     tc_zip = '%s.zip' % log_name
#     tc_zip_dir = '/root/repo/nfp_repo/iperf_p4wire/'
#     # zip log files
#     nutil.ssh_runcmd(root, 'cd %s; zip %s *; mv %s %s; rm *'
#         % (tc_log, tc_zip, tc_zip, tc_zip_dir))
#     # scp to local directory
#     local_dir = os.path.join(proj['proj_dir'], 'tc', proj['benchmark_dir'])
#     res = nutil.scp(admin['ip'], admin['port'], admin['username'], admin['password'], 
#         '%s%s' % (tc_zip_dir, tc_zip), local_dir)
#     # delete remote zip file
#     # nutil.ssh_runcmd(root, 'rm %s%s' % (tc_zip_dir, tc_zip))
#     if res:
#         # unzip correspondingly
#         log_dir = os.path.join(local_dir, log_name)
#         with zipfile.ZipFile(os.path.join(local_dir, tc_zip), 'r') as zip_ref:
#             zip_ref.extractall(log_dir)
#         # delete zip file
#         os.remove(os.path.join(local_dir, tc_zip))
#     sshroot.close()
#     root.close()

def tc_plot(proj, log_name):
    log_dir = os.path.join(proj['proj_dir'], 'tc', proj['benchmark_dir'], log_name)
    fig_dir = os.path.join(proj['proj_dir'], 'tc', proj['figure_dir'])
    # interval = 10
    f_map = { '5001': 0, '5002': 0, '5003': 0, '5004': 15 }
    # for p in range(8):
    #     f_map['500%d' % (p+1)] = p * interval
    # {'port': {time: throughput}}
    raw_data = {}
    for f in os.listdir(log_dir):
        port = f.split('_')[-1].strip('.txt')
        if port in f_map:
            raw_data[port] = {}
            start = f_map[port]
            fp = open(os.path.join(log_dir, f))
            for line in fp.readlines():
                tks = line.split()
                if len(tks) == 8:
                    t1, t2 = tks[2].split('-')
                    deltat = float(t2) - float(t1)
                    if deltat > 0.1 and deltat <= 0.5:
                        tput = float(tks[6])
                        if tks[7] == 'Mbits/sec':
                            tput /= 1000
                        elif tks[7] == 'Kbits/sec':
                            tput /= 1000000
                    raw_data[port][float(t1)+start] = tput
        # add start and finish points
        raw_data[port][start+len(raw_data[port])/5-0.19] = 0
        raw_data[port][start+0.01] = 0
    # format printing
    data = {}
    for port in raw_data:
        for t in raw_data[port]:
            if t not in data:
                data[t] = [0, 0, 0, 0]
            data[t][int(port)-5001] = raw_data[port][t]
    for t in data:
        t0 = data[t][0] if data[t][0] else ''
        t1 = data[t][1] if data[t][1] else ''
        t2 = data[t][2] if data[t][2] else ''
        t3 = data[t][3] if data[t][3] else ''
        # print("{} {} {} {} {}".format(t, data[t][0], data[t][1], data[t][2], data[t][3]))
        print("{},{},{},{},{}".format(t, t0, t1, t2, t3))
    # line plot
    # data = {'Time (s)': [], 'Throughput (Gbps)': [], 'Flow': []}
    # app_map = { 0: 'NC', 1: 'WS', 2: 'KVS', 3: 'ML'}
    # for p in raw_data:
    #     for t in raw_data[p]:
    #         data['Time (s)'].append(t)
    #         data['Throughput (Gbps)'].append(raw_data[p][t])
    #         data['Flow'].append(app_map[int(p)-5001])
    # df = pd.DataFrame(data)
    # fig, ax = plt.subplots(figsize=(4, 2))
    # # sns.set_style('whitegrid', {'grid.linestyle': '--'})
    # ax = sns.lineplot(x='Time (s)', y='Throughput (Gbps)', data=df, style='Flow', hue='Flow')
    # # ax.set(title=log_name)
    # ax.set_xlim(left=0)
    # ax.set_ylim(bottom=0, top=10)
    # ax.legend(bbox_to_anchor=(0.98, -0.25), ncol=4, frameon=False)
    # fig.subplots_adjust(bottom=0.28, right=0.98, left=0.16, top=0.91)
    # plt.grid(True, axis='y', linestyle='--', alpha=0.5)
    # # plt.savefig(os.path.join(fig_dir, '%s.png' % log_name))
    # plt.show()

if __name__ == "__main__":
    os.chdir(os.path.dirname(__file__))
    # admin = nutil.roothost
    proj = json_load('fv.json')
    # tc_test(admin, proj, 'tc_hie_10g')
    tc_plot(proj, 'tc_hie_10g')
    # tc_netperf(admin, proj, 'tc_lat_fq')