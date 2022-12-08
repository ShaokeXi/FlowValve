# README

FlowValve is a parallel packet scheduler designed for NP-based SmartNICs.

Refer to the paper if you are interested: https://ieeexplore.ieee.org/abstract/document/9912227


## Prerequisites
FlowValve has been tested on Netronome Agilio CX 1x40G SmartNIC: https://www.netronome.com/products/agilio-cx/.

For mTCP stack setup, please refer to: https://github.com/mtcp-stack/mtcp.

## Runing instructions

Step 1: Use py/fv_nic.py to compile flowvalve/code into one firmware file, e.g., flowvalve.nffw

Step 2: Load the firmware onto Netronome with this command

  /opt/nfp-sdk-6.1.0-preview/p4/bin/rtecli -n -p 20206 design-load -f flowvalve.nffw -p flowvalve.json -c flowvalve.p4cfg
  
Step 3: Change flow entries at runtime with this command (Optional)
  
  /opt/nfp-sdk-6.1.0-preview/p4/bin/rtecli -n -p 20206 config-reload -c <flowentry>.json
  
Step 4: Watch the log file for debug information during the firmware loading process (Optional)
  
  tail -f /var/log/nfp-sdk6-rte.log
