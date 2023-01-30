# Description: This file provides interfaces to compile Netronome Apps locally.
import os
import sys
import subprocess
import fv_util as nutil

def nic_app_build(app, p4_bin_dir, p4, sandbox_c, out_nffw):
    bat_file = '%s%s.bat' % (p4_bin_dir, app)
    if not os.path.exists(bat_file):
        cmds = [
            r'@echo off',
            r'set MSYS2_ARG_CONV_EXCL=*',
            r'set SDKDIR=%~dp0..\..',
            r'pushd %SDKDIR%',
            r'set SDKDIR=%CD%',
            r'popd',
            r'set SDKDIR=%SDKDIR:\=/%',
            r'set PATH=%~dp0;%~dp0msys64\usr\bin;%~dp0msys64\mingw64\bin;%PATH%'
        ]
        cmds = ['\n'.join(cmds)]
        chip = 'nfp-4xxx-b0'
        platform = 'hydrogen'
        configs = [
            'nfp4build', '-o', out_nffw, '-4', p4, '-s', chip, '-l', platform, 
            '--no-nfcc-ng', '--reduced-thread-usage', '--no-shared-codestore', '--debug-info', 
            '--nfp4c_p4_version', '14', '--nfp4c_p4_compiler', 'hlir', '--nfirc_default_table_size', '65536', 
            '--nfirc_all_header_ops', '--nfirc_implicit_header_valid', '--nfirc_zero_new_headers', 
            '--nfirc_multicast_group_count', '16', '--nfirc_multicast_group_size', '16']
        if sandbox_c:
            configs.extend(['-c', sandbox_c])
        cmds.append(' '.join(configs))
        fp = open(bat_file, 'w')
        fp.writelines('\n'.join(cmds))
        fp.close()
    proc = subprocess.Popen(bat_file, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    while True:
        out = proc.stdout.readline()
        ret_code = proc.poll()
        if ret_code is not None:
            break
        if out:
            print(out.strip().decode())
    if ret_code:
        out, err = proc.communicate()
        print(err.strip().decode())
    else:
        print('Successfully build %s.' % app)
    os.remove(bat_file)
    return ret_code

def nic_firmware_upload(admin, local_nffw, local_design, remote_nffw, remote_design):
    nutil.print_no_line("upload %s to %s" % (local_nffw, remote_nffw))
    nutil.scp(admin['ip'], admin['port'], admin['username'], admin['password'],
        remote_nffw, local_nffw, 'put')
    print("...done")
    nutil.print_no_line("upload %s to %s" % (local_design, remote_design))
    nutil.scp(admin['ip'], admin['port'], admin['username'], admin['password'],
        remote_design, local_design, 'put')
    print("...done")

def nic_app(admin, proj_config, app, build=True, upload=True):
    app_dir = os.path.join(proj_config['proj_dir'], app)
    app_code_dir = os.path.join(app_dir, proj_config['code_dir'])
    app_config = proj_config['apps'][app]
    p4 = os.path.join(app_code_dir, app_config['p4'])
    sandbox_c = None
    if 'sandbox_c' in app_config:
        sandbox_c = os.path.join(app_code_dir, app_config['sandbox_c'])
    local_out_dir = os.path.join(app_code_dir, proj_config['firmware_dir'])
    out_nffw = os.path.join(local_out_dir, app_config['firmware'])
    out_design = os.path.join(local_out_dir, app_config['pif_design'])
    remote_app_dir = "%s/%s/%s" % (proj_config['remote_benchmark_dir'], app, proj_config['remote_firmware_dir'])
    remote_nffw = "%s/%s" % (remote_app_dir, app_config['remote_firmware'])
    remote_design = "%s/%s" % (remote_app_dir, app_config['remote_pif_design'])
    if build:
        nic_app_build(app, proj_config['p4_bin_dir'], p4, sandbox_c, out_nffw)
        os.remove(proj_config['make_file'])
    if upload:
        nic_firmware_upload(admin, out_nffw, out_design, remote_nffw, remote_design)

if __name__ == '__main__':
    os.chdir(os.path.dirname(__file__))
    admin = nutil.roothost
    proj = nutil.get_json('fv.json')
    nic_app(admin, proj, 'flowvalve', build=True, upload=True)