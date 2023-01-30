import sys
import json
import time
import paramiko
from scp import SCPClient
import os

def ssh_open(admin):
    # create instance of SSHClient object
    sshroot = paramiko.SSHClient()
    # automatically add untrusted hosts
    sshroot.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    # initiate SSH connection
    sshroot.connect(admin['ip'], port=admin['port'], username=admin['username'], 
        password=admin['password'], look_for_keys=False, allow_agent=False)
    print("SSH connection established to %s" % admin['ip'])
    root = sshroot.get_transport()
    return sshroot, root

def ssh_close(ssh_obj, ssh_transp):
    ssh_obj.close()
    ssh_transp.close()

# non-interactive ssh shell
def ssh_runcmd(transp, cmd, out=False, stdout=[]):
    chan = transp.open_session()
    chan.setblocking(0)
    chan.exec_command(cmd)
    status = chan.recv_exit_status()
    stderr = []
    while chan.recv_ready():
        stdout.append(chan.recv(1000))
    if out:
        print(''.join(stdout))
    while chan.recv_stderr_ready():
        stderr.append(chan.recv_stderr(1000))
    return status, stderr

def scp(ip, prt, usr, pswd, remotepath, localpath, opt='get'):
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    connected = False
    while not connected:
        try:
            ssh.connect(hostname=ip, port=prt, username=usr, password=pswd, timeout=5)
            connected = True
        except:
            print("scp connection fails")
            exit(1)
    scp = SCPClient(ssh.get_transport())
    try:
        if opt == 'get':
            scp.get(remotepath, localpath)
        elif opt == 'put':
            scp.put(localpath, remotepath)
        scp.close()
        ssh.close()
        return True
    except:
        print("SCP fails, please check remote server manually.")
    scp.close()
    ssh.close()
    return False

def get_json(fpath):
    fp = open(fpath)
    data = json.load(fp)
    fp.close()
    return data

def dump_json(data, fpath):
    fp = open(fpath, 'w')
    json.dump(data, fp, indent=4)
    fp.close()

def print_no_line(p_str):
    sys.stdout.write(p_str)
    sys.stdout.flush()

# admin.json content (login remote test machine from the local machine)
# {
#   "ip": xx.xx.xx.xx,
#   "port": xx,
#   "username": xx,
#   "password": xx
# }
roothost = get_json('<admin.json>')