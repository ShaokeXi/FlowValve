#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP 0x0806
#define IPPROTO_UDP 17
#define IPPROTO_TCP 6

header_type eth_t {
    fields {
        dstAddr: 48;
        srcAddr: 48;
        etherType: 16;
    }
}

header_type ipv4_t {
    fields {
        version: 4;
        ihl: 4;
        diffserv: 8;
        totalLen: 16;
        identification: 16;
        flags: 3;
        fragOffset: 13;
        ttl: 8;
        protocol: 8;
        hdrChecksum: 16;
        srcAddr: 32;
        dstAddr: 32;
    }
}

header_type arp_t {
    fields {
        hwType: 16;
        protoType: 16;
        hwAddrLen: 8;
        protoAddrLen: 8;
        opcode: 16;
        srcHwAddr: 48;
        srcProtoAddr: 32;
        dstHwAddr: 48;
        dstProtoAddr: 32;
    }

}

header_type udp_t {
    fields {
        srcPort: 16;
        dstPort: 16;
        length_: 16;
        checksum: 16;
    }
}

header_type tcp_t {
    fields {
        srcPort : 16;
        dstPort : 16;
        seqNo : 32;
        ackNo : 32;
        dataOffset : 4;
        res : 4;
        flags : 8;
        window : 16;
        checksum : 16;
        urgentPtr : 16;
    }
}

header_type ext_meta_t {
    fields {
        meter_color : 2;
        // each label is 5 bit length, at most 8 layers
        idxNum: 4;
        flowIdx: 40;
        // bitmap help index
        subNum: 8;
        flowSub: 32;
    }
}

header eth_t eth;
header arp_t arp;
header ipv4_t ipv4;
header udp_t udp;
header tcp_t tcp;
metadata ext_meta_t ext_meta;

primitive_action fv_schedule();

parser start {
    return parse_eth;
}

parser parse_eth {
    extract(eth);
    return select(latest.etherType) {
        ETHERTYPE_IPV4: parse_ipv4;
        ETHERTYPE_ARP: parse_arp;
        default: ingress;
    }
}

parser parse_ipv4 {
    extract(ipv4);
    return select(latest.protocol) {
        IPPROTO_UDP: parse_udp;
        IPPROTO_TCP: parse_tcp;
        default: ingress;
    }
}

parser parse_arp {
    extract(arp);
    return ingress;
}

parser parse_udp {
    extract(udp);
    return ingress;
}

parser parse_tcp {
    extract(tcp);
    return ingress;
}

// @pragma netro meter_drop_red
// meter vf_meters {
//     type: bytes;
//     result: ext_meta.meter_color;
//     instance_count : 8;
// }


// action act_meter(meter_idx) {
//     //Implicit drop when metered to red
//     execute_meter(vf_meters, meter_idx, ext_meta.meter_color);
// }

action act_flow(inum, idx, sub, snum) {
    modify_field(ext_meta.idxNum, inum);
    modify_field(ext_meta.flowIdx, idx);
    modify_field(ext_meta.flowSub, sub);
    modify_field(ext_meta.subNum, snum);
}

action act_schedule() {
    fv_schedule();
}

action act_forward(port) {
    modify_field(standard_metadata.egress_spec, port);
}

action act_drop(){
    drop();
}

table ip_forward {
    reads {
        ipv4.dstAddr: exact;
    }
    actions {
        act_forward;
        act_drop;
    }
}

table arp_forward {
    reads {
        arp.dstProtoAddr: exact;
    }
    actions {
        act_forward;
        act_drop;
    }
}

table port_forward {
    reads {
        standard_metadata.ingress_port: exact;
    }
    actions {
        act_forward;
        act_drop;
    }
}

// table tbl_ingress_meter {
//     reads {
//         standard_metadata.ingress_port: exact;
//         tcp.srcPort: exact;
//         tcp.dstPort: exact;
//         udp.srcPort: exact;
//         udp.dstPort: exact;
//     }
//     actions {
//         act_meter;
//     }
// }

// table tbl_egress_meter {
//     reads {
//         standard_metadata.egress_port: exact;
//         tcp.srcPort: exact;
//         tcp.dstPort: exact;
//         udp.srcPort: exact;
//         udp.dstPort: exact;
//     }
//     actions {
//         act_meter;
//     }
// }

table tbl_flow {
    reads {
        tcp.dstPort: exact;
    }
    actions {
        act_flow;
    }
}

table tbl_schedule {
    actions {
        act_schedule;
    }
}

control ingress {
    if (valid(ipv4)) {
        apply(port_forward);
        apply(ip_forward);
    }
    else if (valid(arp)) {
        apply(arp_forward);
    }
    // apply(tbl_flow);
    // apply(tbl_schedule);
}