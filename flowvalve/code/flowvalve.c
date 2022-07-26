// Date: 2021/7/11
// Version: 7
// Description: Refine the borrowing mechanism to be more precise.
// This might be too complex, and not better than version 6.

#include <pif_common.h>
#include <pif_plugin.h>
#include <nfp/me.h>
#include <nfp/mem_atomic.h>
#include <nfp.h>
#include "flowvalve.h"

__intrinsic void fv_meter(__xrw uint32_t *mval, __emem void *addr, int colour, int rfc)
{
    SIGNAL_PAIR meter_sig;
    uint32_t high_addr, low_addr;

    /* no checks on colour and rfc */
    high_addr = (((uint64_t) addr) >> 32) << 24;
    low_addr = ((uint64_t) addr) & 0xffffffff;

#define RFC_OFF 0
#define COLOUR_OFF 1
    low_addr |= (rfc << RFC_OFF) | (colour << COLOUR_OFF);

    __asm {
        // PRM, 160
        // Atomic metering command meters the IP Packet stream and marks its packets either Red, Yellow or Green.
        // It operates within a single 128-bit cache line.
        mem[meter, *mval, high_addr, << 8, low_addr], sig_done[meter_sig];
    }
    __wait_for_all(&meter_sig);
}

__intrinsic uint32_t fv_mult32_oflow0(uint32_t v0, uint32_t v1)
{
    uint32_t result;
    uint32_t result_hi;

    __asm {
        mul_step[v0, v1], 32x32_start
        mul_step[v0, v1], 32x32_step1
        mul_step[v0, v1], 32x32_step2
        mul_step[v0, v1], 32x32_step3
        mul_step[v0, v1], 32x32_step4
        mul_step[result,--], 32x32_last
        mul_step[result_hi, --], 32x32_last2
    }

    if (result_hi > 0)
        return 0;

    return result;
}

void update_fv_class(__mem struct fv_meter *m, __mem struct fv_class *c) {
    // the timestamp counter increments every 16 ME clock cycles
    uint64_t ctime = me_tsc_read();
    uint32_t ctime_upper, ptime_upper, ctime_lower, ptime_lower, tdelta, pir;
    uint8_t shift = 0x12, type, pidx, numer, denomi;
    __xread struct fv_class c_rd;
    __xread struct fv_meter m_rd, m_rdp;
    __xwrite struct fv_meter m_wr;

    // read class info
    mem_read32(&c_rd, c, sizeof(c_rd));
    mem_read_atomic(&m_rd, m, sizeof(m_rd));
    type = c_rd.info_v[0] >> 30;
    if (type) {
        /* runtime value */
        pidx = c_rd.info_v[0] & 0xff;
        mem_read_atomic(&m_rdp, &fv_m[pidx], sizeof(m_rdp));
        switch (type) {
            case 1:
                if (m_rd.last_update_low > m_rdp.last_update_low + interval) {
                    numer = (c_rd.info_v[0] >> 23) & 0x7f;
                    denomi = (c_rd.info_v[0] >> 16) & 0x7f;
                    pir = m_rdp.pir * numer / denomi;
                }
                else if (m_rdp.cur_pir < m_rdp.pir) {
                    numer = (c_rd.info_v[0] >> 23) & 0x7f;
                    denomi = (c_rd.info_v[0] >> 16) & 0x7f;
                    pir = (m_rdp.pir - m_rdp.cur_pir) * numer / denomi;
                }
                else{
                    /* set to a tiny value */
                    pir = 1;
                }
                break;
            case 2:
                if (m_rdp.pir < c_rd.info_v[1]) {
                    numer = (c_rd.info_v[0] >> 23) & 0x7f;
                    denomi = (c_rd.info_v[0] >> 16) & 0x7f;
                    pir = m_rdp.pir * numer / denomi;
                    if (pir == 0)
                        pir = 1;
                }
                else {
                    pir = c_rd.info_v[2];
                }
                break;
            case 3:
                if (m_rdp.pir < c_rd.info_v[1]) {
                    numer = (c_rd.info_v[0] >> 23) & 0x7f;
                    denomi = (c_rd.info_v[0] >> 16) & 0x7f;
                    pir = m_rdp.pir * numer / denomi;
                    if (pir == 0)
                        pir = 1;
                }
                else {
                    pir = m_rdp.pir - c_rd.info_v[2];
                }
                break;
        }
    }
    else {
        /* predifined */
        pir = m_rd.pir;
    }
    m_wr.pir = pir;

    ctime >>= shift;

    ctime_upper = (ctime >> 32) & 0xffffff;
    ctime_lower = ctime & 0xffffffff;

    ptime_upper = m_rd.last_update_high & 0xffffff;
    ptime_lower = m_rd.last_update_low;

    if (ctime_upper < ptime_upper)
        goto no_change; /* jump back in time */

    if (ctime_upper == ptime_upper && ctime_lower <= ptime_lower)
        goto no_change; /* jump back in time, or no time passed */

    if (ctime_upper - ptime_upper > 1) {
        /* the upper bit changed multiple times:
         * so reset the buckets and the timer
         */
        m_wr.bucket = c_rd.pbs;
        m_wr.last_update_high = ctime_upper;
        m_wr.last_update_low = ctime_lower;

        mem_write_atomic(&m_wr, (__mem void *)m, sizeof(m_wr));
        return;
    }

    /* check if we wrapped */
    if (ctime_lower <= ptime_lower)
        tdelta = ptime_lower - ctime_lower;
    else
        tdelta = ctime_lower - ptime_lower;


    {
        uint32_t Pdelta, newP;
        /* be careful to use new */
        Pdelta = fv_mult32_oflow0(tdelta, pir);
        if (Pdelta == 0) { /* mult overflowed */
            m_wr.bucket = c_rd.pbs;
        } else {
            newP = m_rd.bucket + Pdelta;
            /* either add oveflow or exceed max bkt size */
            if (newP < Pdelta || newP > c_rd.pbs)
                newP = c_rd.pbs;
            m_wr.bucket = newP;
        }
        if (tdelta) {
            m_wr.cur_pir = (m_rd.cnt_tk / tdelta + m_rd.cur_pir) / 2;
            m_wr.cnt_tk = 0;
        }

        m_wr.last_update_high = ctime_upper;
        m_wr.last_update_low = ctime_lower;
        mem_write_atomic(&m_wr, (__mem void *)m, sizeof(m_wr));
    }
    
    return;
no_change:
    {
        __xwrite uint32_t val;
        __mem uint8_t *tmr_addr;

        val = ctime_upper;

        tmr_addr = ((__mem uint8_t *)m) +
                   offsetof(struct fv_meter, last_update_high);
        mem_write_atomic(&val,
                         (__mem void *)tmr_addr,
                         sizeof(uint32_t));
    }

}

void update_fv_borrow(__mem struct fv_meter *m, __mem struct fv_borrow *b, __mem struct fv_class *c) {
    // the timestamp counter increments every 16 ME clock cycles
    uint64_t ctime = me_tsc_read();
    uint32_t ctime_upper, ptime_upper, ctime_lower, ptime_lower, tdelta, pir;
    uint8_t shift = 0x12;
    __mem uint8_t *offset;
    __xread uint32_t cpir, ccpir, mlower;
    __xread struct fv_borrow b_rd;
    __xwrite struct fv_borrow b_wr;

    // read class info
    offset = ((__mem uint8_t *)m) + offsetof(struct fv_meter, pir);
    mem_read_atomic(&cpir, (__mem void *)offset, sizeof(cpir));
    offset = ((__mem uint8_t *)m) + offsetof(struct fv_meter, cur_pir);
    mem_read_atomic(&ccpir, (__mem void *)offset, sizeof(ccpir));
    offset = ((__mem uint8_t *)m) + offsetof(struct fv_meter, last_update_low);
    mem_read_atomic(&mlower, (__mem void *)offset, sizeof(mlower));
    mem_read_atomic(&b_rd, b, sizeof(b_rd));
    b_wr = b_rd;

    ctime >>= shift;

    ctime_upper = (ctime >> 32) & 0xffffff;
    ctime_lower = ctime & 0xffffffff;

    ptime_upper = b_rd.last_update_high & 0xffffff; /* reuse this field to store pir */
    ptime_lower = b_rd.last_update_low;
    
    if (ctime_lower > mlower + expire) {
        /* both cur_pir and pir can be expired */
        update_fv_class(m, c);
        pir = cpir;
    }
    else if (ccpir >= cpir) {
        pir = 1; /* distinguish from multiply overflow */
    }
    else {
        pir = cpir - ccpir;
    }
    pir = (pir + ptime_upper) / 2;
    /* debug */
    ctime_upper = pir;
    if (ctime_lower == ptime_lower) {
        goto no_change;
    }

    /* check if we wrapped */
    if (ctime_lower <= ptime_lower)
        tdelta = ptime_lower - ctime_lower;
    else
        tdelta = ctime_lower - ptime_lower;
    {
        uint32_t Pdelta, newP;
        /* be careful to use new */
        Pdelta = fv_mult32_oflow0(tdelta, pir);
        if (Pdelta == 0) { /* mult overflowed */
            b_wr.bucket = b_rd.pbs;
        } else {
            newP = b_rd.bucket + Pdelta;
            /* either add oveflow or exceed max bkt size */
            if (newP < Pdelta || newP > b_rd.pbs)
                newP = b_rd.pbs;
            b_wr.bucket = newP;
        }

        b_wr.last_update_high = ctime_upper;
        b_wr.last_update_low = ctime_lower;
        mem_write_atomic(&b_wr, (__mem void *)b, sizeof(b_wr));
    }
    
    return;
no_change:
    {
        __xwrite uint32_t val;
        __mem uint8_t *tmr_addr;

        val = ctime_upper;

        tmr_addr = ((__mem uint8_t *)b) +
                   offsetof(struct fv_borrow, last_update_high);
        mem_write_atomic(&val,
                         (__mem void *)tmr_addr,
                         sizeof(uint32_t));
    }
}

int fv_class_execute(__mem struct fv_meter *m, __mem struct fv_class *c, int color, int tick) {

    __xrw uint32_t xval32;
    __mem uint8_t *offset;

    xval32 = 1 << 31;
    
    offset = ((__mem uint8_t *)m) +
             offsetof(struct fv_meter, last_update_high);

    mem_test_set(&xval32,
                 (__mem void *)offset,
                 sizeof(xval32));

    /* check the original test value */
    if ((xval32 >> 31) == 0) {
        /* update meter timers */
        update_fv_class(m, c);
    }

    xval32 = tick;
    
    offset = ((__mem uint8_t *)m) +
             offsetof(struct fv_meter, bucket);
    // fv_meter(__xrw uint32_t *mval, __emem void *addr, int colour, int rfc)
    // RFC_2697 0
    fv_meter(&xval32, (__mem void *)offset, color, 0);

    return xval32; /* return colour result of mem_meter */
}

void fv_status_execute(__mem struct fv_status *s, __mem struct fv_meter *m) {
    
    __xread struct fv_status s_rd;
    __xwrite struct fv_status s_wr;
    __xread uint32_t plower, cpir, ccpir;
    __mem uint8_t *offset;
    
    uint64_t ctime = me_tsc_read();
    uint32_t t1, t2, tmp_ccpir;
    uint8_t shift = 0x12, v;

    offset = ((__mem uint8_t *)m) + offsetof(struct fv_meter, pir);
    mem_read_atomic(&cpir, (__mem void *)offset, sizeof(cpir));
    offset = ((__mem uint8_t *)m) + offsetof(struct fv_meter, cur_pir);
    mem_read_atomic(&ccpir, (__mem void *)offset, sizeof(ccpir));
    offset = ((__mem uint8_t *)m) + offsetof(struct fv_meter, last_update_low);
    mem_read_atomic(&plower, (__mem void *)offset, sizeof(plower));
    mem_read_atomic(&s_rd, (__mem void *)s, sizeof(s_rd));

    ctime >>= shift;
    t1 = (ctime >> 32) & 0xffffff;
    t2 = ctime & 0xffffffff;
    if (t2 > expire + plower) {
        tmp_ccpir = 0;
    }
    else {
        tmp_ccpir = ccpir;
    }
    if (cpir - (cpir >> 3) <= tmp_ccpir) {
        v = PIF_METER_COLOUR_RED;
    } else {
        v = PIF_METER_COLOUR_GREEN;
    }

    s_wr.last_update_high = t1;
    s_wr.last_update_low = t2;
    s_wr.status = v;
    // update corresponding meter status
    mem_write_atomic(&s_wr, (__mem void *)s, sizeof(s_wr));
    return;
}

int fv_borrow_execute(__mem struct fv_meter *m, __mem struct fv_borrow *b, __mem struct fv_class *c, int color, int tick){

    __xrw uint32_t xval32;
    __mem uint8_t *offset;

    xval32 = 1 << 31;
    
    offset = ((__mem uint8_t *)b) +
             offsetof(struct fv_borrow, last_update_high);

    mem_test_set(&xval32,
                 (__mem void *)offset,
                 sizeof(xval32));

    /* check the original test value */
    if ((xval32 >> 31) == 0) {
        update_fv_borrow(m, b, c);
    }

    xval32 = tick;
    
    offset = ((__mem uint8_t *)b) +
             offsetof(struct fv_meter, bucket);
    // fv_meter(__xrw uint32_t *mval, __emem void *addr, int colour, int rfc)
    // RFC_2697 0
    fv_meter(&xval32, (__mem void *)offset, color, 0);

    return xval32; /* return colour result of mem_meter */
}

// Red(2b10), Yellow(2b01), Green(2b00)
// the least significant bit indicates meter[0]
// in the bitmap 1 indicates no permission
int pif_plugin_fv_schedule(EXTRACTED_HEADERS_T *headers, MATCH_DATA_T *match_data) {

    __mem uint8_t *addr;
    __xrw uint32_t xfer;
    __xread struct fv_meter m_rd;
    __gpr uint32_t idx, sib, color, i, mask2;
    __gpr uint64_t mask1 = 0xf800000000, fidx = pif_plugin_meta_get__ext_meta__flowIdx__1(headers);
    // idx is specific label values
    __gpr uint32_t fidx0 = pif_plugin_meta_get__ext_meta__flowIdx__0(headers);
    // inum indicates label numbers
    __gpr uint32_t inum = pif_plugin_meta_get__ext_meta__idxNum(headers);
    // snum indicates sibling numbers
    __gpr uint32_t snum = pif_plugin_meta_get__ext_meta__subNum(headers);
    // sub indicated sibling where may borrow bandwidth
    __gpr uint32_t fsub = pif_plugin_meta_get__ext_meta__flowSub(headers);
    PIF_PLUGIN_ipv4_T *ipv4 = pif_plugin_hdr_get_ipv4(headers);
    __gpr uint32_t tlen = PIF_HEADER_GET_ipv4___totalLen(ipv4);
    fidx <<= 32;
    fidx |= fidx0;
    // update all the inner class meter and status
    for(i = 0; i < inum; i++) {
        idx = (fidx & mask1) >> ((LAYERNUM - i) * 5);
        mask1 >>= 5;
        color = fv_class_execute(&fv_m[idx], &fv_c[idx], 0, tlen);
        mem_add32_imm(tlen, &fv_m[idx].cnt_tk);
        fv_status_execute(&fv_s[idx], &fv_m[idx]);
    }

    // if there are enough tokens, direct pass
    if(color != PIF_METER_COLOUR_RED) {
        return PIF_PLUGIN_RETURN_FORWARD;
    }

    // otherwise, try to borrow credits
    mask2 = 0x1;
    for(i = 0; i < snum; i++) {
        sib = fsub & mask2;
        mask2 <<= 1;
        if(sib == 0) {
            addr = ((__mem uint8_t *)&fv_s[i]) +
               offsetof(struct fv_status, status);
            mem_read32(&xfer, addr, sizeof(uint32_t));
            if(xfer != PIF_METER_COLOUR_RED) {
                color = fv_borrow_execute(&fv_m[i], &fv_b[i], &fv_c[i], 0, tlen);
                if(color != PIF_METER_COLOUR_RED) {
                    return PIF_PLUGIN_RETURN_FORWARD;
                }
            }
            fv_status_execute(&fv_s[i], &fv_m[i]);
        }
    }
    return PIF_PLUGIN_RETURN_DROP;
}