/**
 * tests/rt_deadline_profile.c
 * P0-D RT deadline profiler (measurement only, no functional change)
 * Host + NuttX (CLOCK_MONOTONIC / up_utime) compatible
 * Build: gcc -DPROFILE_ENABLE -Iinclude -Iasmp_sub tests/rt_deadline_profile.c -o /tmp/rt_profile
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef __NuttX__
#include <nuttx/arch.h>
#endif

#define TIMING_RING_BITS 10
#define TIMING_RING_SIZE (1u << TIMING_RING_BITS) // 1024
#define TIMING_RING_MASK (TIMING_RING_SIZE - 1)
#define BLOCK_BUDGET_US 10666u // 512 frames @48kHz

struct EpochTiming {
    uint32_t epoch;
    uint32_t start_us;
    uint32_t end_us;
    uint32_t duration_us;
    int32_t  slack_us;
};

struct CoreProfile {
    struct EpochTiming ring[TIMING_RING_SIZE];
    uint32_t write_idx;
    uint32_t total_epochs;
    uint32_t miss_count;
    uint32_t consecutive_miss;
    uint32_t max_consecutive_miss;
    uint32_t min_slack_us;
    uint32_t max_duration_us;
};

struct CoreProfile g_profile[6]; // 0=Main 1=Sub1 2=Sub2 3=Sub3 4=Sub4 5=Sub5

static inline uint32_t get_monotonic_us(void) {
#ifdef __NuttX__
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint32_t)(ts.tv_sec * 1000000ull + ts.tv_nsec / 1000);
    }
    return 0;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint32_t)(ts.tv_sec * 1000000ull + ts.tv_nsec / 1000);
    }
    return 0;
#endif
}

void profile_epoch_start(uint32_t core_id, uint32_t epoch) {
    if (core_id >= 6) return;
    struct CoreProfile *cp = &g_profile[core_id];
    uint32_t idx = cp->write_idx & TIMING_RING_MASK;
    cp->ring[idx].epoch = epoch;
    cp->ring[idx].start_us = get_monotonic_us();
    cp->ring[idx].end_us = 0;
    cp->ring[idx].duration_us = 0;
    cp->ring[idx].slack_us = 0;
    cp->total_epochs++;
}

void profile_epoch_end(uint32_t core_id, uint32_t epoch) {
    if (core_id >= 6) return;
    struct CoreProfile *cp = &g_profile[core_id];
    uint32_t idx = cp->write_idx & TIMING_RING_MASK;
    struct EpochTiming *et = &cp->ring[idx];
    (void)epoch;
    et->end_us = get_monotonic_us();
    et->duration_us = et->end_us - et->start_us;
    et->slack_us = (int32_t)BLOCK_BUDGET_US - (int32_t)et->duration_us;
    if (et->slack_us < 0) {
        cp->miss_count++;
        cp->consecutive_miss++;
        if (cp->consecutive_miss > cp->max_consecutive_miss) cp->max_consecutive_miss = cp->consecutive_miss;
    } else {
        cp->consecutive_miss = 0;
    }
    if ((uint32_t)et->slack_us < cp->min_slack_us) {
        cp->min_slack_us = (uint32_t)(et->slack_us < 0 ? 0 : et->slack_us);
    }
    if (et->duration_us > cp->max_duration_us) cp->max_duration_us = et->duration_us;
    cp->write_idx++;
}

void profile_print_stats(uint32_t core_id) {
    if (core_id >= 6) return;
    struct CoreProfile *cp = &g_profile[core_id];
    uint32_t n = cp->write_idx < TIMING_RING_SIZE ? cp->write_idx : TIMING_RING_SIZE;
    if (n == 0) return;
    uint32_t durs[TIMING_RING_SIZE];
    uint32_t count = n;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = (cp->write_idx - count + i) & TIMING_RING_MASK;
        durs[i] = cp->ring[idx].duration_us;
    }
    for (uint32_t i = 1; i < count; i++) {
        uint32_t key = durs[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && durs[j] > key) { durs[j+1]=durs[j]; j--; }
        durs[j+1]=key;
    }
    uint32_t p50 = durs[(count*50)/100];
    uint32_t p95 = durs[(count*95)/100];
    uint32_t p99 = durs[(count*99)/100];
    uint32_t p999 = durs[(count*999)/1000];
    uint32_t mx = durs[count-1];
    uint64_t sum=0; for(uint32_t i=0;i<count;i++) sum+=durs[i];
    uint32_t avg=(uint32_t)(sum/count);
    printf("=== Core[%u] RT Profile ===\n", core_id);
    printf("epochs=%u miss=%u max_consecutive_miss=%u\n", cp->total_epochs, cp->miss_count, cp->max_consecutive_miss);
    printf("duration_us: avg=%u p50=%u p95=%u p99=%u p99.9=%u max=%u\n", avg,p50,p95,p99,p999,mx);
    printf("slack_us: min=%u budget=%u\n", cp->min_slack_us, BLOCK_BUDGET_US);
    printf("deadline_headroom: %s (max_dur=%u)\n", cp->max_duration_us > BLOCK_BUDGET_US ? "VIOLATED" : "OK", cp->max_duration_us);
}

void profile_print_all(void) {
    for(uint32_t i=0;i<6;i++) profile_print_stats(i);
}

void profile_reset(void) {
    memset(g_profile,0,sizeof(g_profile));
    for(uint32_t i=0;i<6;i++) g_profile[i].min_slack_us=0xFFFFFFFF;
}

#ifdef PROFILE_STANDALONE_TEST
int main(void){
    profile_reset();
    for(int c=0;c<6;c++) for(int e=0;e<100;e++){ profile_epoch_start(c,e); /* fake work */ struct timespec ts={0,1000*1000}; nanosleep(&ts,NULL); profile_epoch_end(c,e); }
    profile_print_all();
    return 0;
}
#endif
