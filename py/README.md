directory: py

    - fv_pktgen.py

    Run dpdk-pktgen to generate traffic on the server and transfer logs back for further analysis. (only used for debugging)

    - fv_dpdk.py

    Evaluation experiments.

    a. qos_sched - dpdk qos scheduler

    /root/repo/dpdk-stable-17.08.2/examples/fv_sched/build/qos_sched

    b. fv_flow - flowvalve scheduler

    /root/repo/dpdk-stable-17.08.2/examples/fv_flow/build/fv_flow

directory: fv_sched/code

    - fv_flow.c

    DPDK program. Test 3 packet length 64B, 1024B and 1518B thoughput and one-way delay.

For example, we test the maximum throughput of 1518B packets under fq setting.

    Step 1: Manually load firmware. (P4 server)
    cd repo/nfp_repo/flowvalve/scripts
    ./fv_load flowvalve.nffw flowvalve.json fv_v5_ip.p4cfg

    Step 2: Configure fv parameters. (Local server)
    -> fv_mtcp
        -> fv_mtcp_test('fv_mtcp_fq', proj, fv_exec, param=True, test=False)

    Step 3: Run test script. (Local server)
    python fv_dpdk.py