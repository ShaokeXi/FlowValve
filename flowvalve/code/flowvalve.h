#ifndef __MY_METER_H__
#define __MY_METER_H__

#define S32 sizeof(uint32_t)
#define S64 sizeof(uint64_t)
#define ARRAYSIZE 32
#define STACKSIZE 8
#define LAYERNUM 7
#define NONE 0xff

struct fv_meter {
    uint32_t bucket;
    union {
        struct meter_last_update_high {
            unsigned int locked:1;
            unsigned int reserved:7;
            unsigned int last_update_upper:24;
        };
        uint32_t last_update_high;
    };
    uint32_t last_update_low;
    uint32_t cnt_tk;
    uint32_t cur_pir;
    uint32_t pir;
    uint32_t reserve[2];
};

struct fv_borrow {
    uint32_t bucket;
    union {
        struct borrow_last_update_high {
            unsigned int locked:1;
            unsigned int reserved:7;
            unsigned int last_update_upper:24;
        };
        uint32_t last_update_high;
    };
    uint32_t last_update_low;
    uint32_t pbs;
};

struct fv_class {
    uint32_t pbs;
    union {
        struct info {
            uint32_t type:2;    /* 0: static, 1: runtime */
            uint32_t numer:7;
            uint32_t denomi:7;
            uint32_t pidx:16;
            uint32_t cond_0;
            uint32_t cond_1;
        };
        uint32_t info_v[3];
    };
};

struct fv_status {
    uint32_t status;    /* meter color */
    uint32_t last_update_high;
    uint32_t last_update_low;
};

__export __mem __align16 struct fv_meter fv_m[ARRAYSIZE];
__export __mem __align16 struct fv_borrow fv_b[ARRAYSIZE];
__export __mem struct fv_class fv_c[ARRAYSIZE];
__export __mem struct fv_status fv_s[ARRAYSIZE];
__export __mem uint32_t interval = 0x10;
__export __mem uint32_t expire = 0x100;

__intrinsic void fv_meter(__xrw uint32_t *mval, __emem void *addr, int colour, int rfc);
__intrinsic uint32_t fv_mult32_oflow0(uint32_t v0, uint32_t v1);
void update_fv_class(__mem struct fv_meter *m, __mem struct fv_class *c);
void update_fv_borrow(__mem struct fv_meter *m, __mem struct fv_borrow *b, __mem struct fv_class *c);
int fv_class_execute(__mem struct fv_meter *m, __mem struct fv_class *c, int color, int tick);
int fv_borrow_execute(__mem struct fv_meter *m, __mem struct fv_borrow *b, __mem struct fv_class *c, int color, int tick);
void fv_status_execute(__mem struct fv_status *s, __mem struct fv_meter *m);

#endif