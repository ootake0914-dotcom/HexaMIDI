#include "rt_profile.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __NuttX__
#include <nuttx/arch.h>
#endif

struct CoreProfile g_profile[6];

uint32_t profile_get_monotonic_us(void) {
#ifdef _WIN32
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint32_t)(t.QuadPart * 1000000ull / f.QuadPart);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint32_t)(ts.tv_sec * 1000000ull + ts.tv_nsec / 1000);
    }
    return 0;
#endif
}

void profile_reset(void) {
    memset(g_profile, 0, sizeof(g_profile));
    for (int i=0;i<6;i++) g_profile[i].min_slack_us = 0xFFFFFFFFu;
}

void profile_epoch_start(uint32_t core_id, uint32_t epoch) {
    if (core_id >= 6) return;
    struct CoreProfile *cp = &g_profile[core_id];
    uint32_t idx = cp->write_idx & PROFILE_RING_MASK;
    cp->ring[idx].epoch = epoch;
    cp->ring[idx].start_us = profile_get_monotonic_us();
    cp->total_epochs++;
}

void profile_epoch_end(uint32_t core_id, uint32_t epoch) {
    if (core_id >= 6) return;
    struct CoreProfile *cp = &g_profile[core_id];
    uint32_t idx = cp->write_idx & PROFILE_RING_MASK;
    struct ProfileEntry *et = &cp->ring[idx];
    (void)epoch;
    et->end_us = profile_get_monotonic_us();
    et->duration_us = et->end_us - et->start_us;
    et->slack_us = (int32_t)BLOCK_BUDGET_US - (int32_t)et->duration_us;
    if (et->slack_us < 0) {
        cp->miss_count++;
        cp->consecutive_miss++;
        if (cp->consecutive_miss > cp->max_consecutive_miss) cp->max_consecutive_miss = cp->consecutive_miss;
    } else {
        cp->consecutive_miss = 0;
    }
    uint32_t slack_abs = et->slack_us < 0 ? 0 : (uint32_t)et->slack_us;
    if (slack_abs < cp->min_slack_us) cp->min_slack_us = slack_abs;
    if (et->duration_us > cp->max_duration_us) cp->max_duration_us = et->duration_us;
    cp->write_idx++;
}

void profile_print_stats(uint32_t core_id) {
    if (core_id >= 6) return;
    struct CoreProfile *cp = &g_profile[core_id];
    uint32_t n = cp->write_idx < PROFILE_RING_SIZE ? cp->write_idx : PROFILE_RING_SIZE;
    if (n == 0) { printf("Core[%u]: no data\n", core_id); return; }
    uint32_t durs[PROFILE_RING_SIZE];
    for (uint32_t i=0;i<n;i++) {
        uint32_t idx = (cp->write_idx - n + i) & PROFILE_RING_MASK;
        durs[i] = cp->ring[idx].duration_us;
    }
    for (uint32_t i=1;i<n;i++) {
        uint32_t key=durs[i]; int32_t j=(int32_t)i-1;
        while(j>=0 && durs[j]>key){ durs[j+1]=durs[j]; j--; }
        durs[j+1]=key;
    }
    uint32_t p50=durs[(n*50)/100];
    uint32_t p95=durs[(n*95)/100];
    uint32_t p99=durs[(n*99)/100];
    uint32_t p999=durs[(n*999)/1000];
    uint32_t mx=durs[n-1];
    uint64_t sum=0; for(uint32_t i=0;i<n;i++) sum+=durs[i];
    uint32_t avg=(uint32_t)(sum/n);
    printf("=== Core[%u] RT Profile ===\n", core_id);
    printf("epochs=%u miss=%u max_consec=%u\n", cp->total_epochs, cp->miss_count, cp->max_consecutive_miss);
    printf("duration_us: avg=%u p50=%u p95=%u p99=%u p99.9=%u max=%u\n", avg,p50,p95,p99,p999,mx);
    printf("slack min=%u budget=%u headroom=%s\n", cp->min_slack_us, BLOCK_BUDGET_US, cp->max_duration_us > BLOCK_BUDGET_US ? "VIOLATED" : "OK");
}

void profile_print_all(void) {
    for(uint32_t i=0;i<6;i++) profile_print_stats(i);
}

#ifdef PROFILE_ENABLE
static struct CoreProfile g_profile_pure[1];

void profile_epoch_start_pure(uint32_t core_id, uint32_t epoch) {
    if (core_id != 5) return;
    struct CoreProfile *cp = &g_profile_pure[0];
    uint32_t idx = cp->write_idx & PROFILE_RING_MASK;
    cp->ring[idx].epoch = epoch;
    cp->ring[idx].start_us = profile_get_monotonic_us();
    cp->total_epochs++;
}

void profile_epoch_end_pure(uint32_t core_id, uint32_t epoch) {
    if (core_id != 5) return;
    struct CoreProfile *cp = &g_profile_pure[0];
    uint32_t idx = cp->write_idx & PROFILE_RING_MASK;
    struct ProfileEntry *et = &cp->ring[idx];
    (void)epoch;
    et->end_us = profile_get_monotonic_us();
    et->duration_us = et->end_us - et->start_us;
    et->slack_us = (int32_t)BLOCK_BUDGET_US - (int32_t)et->duration_us;
    if (et->slack_us < 0) {
        cp->miss_count++;
        cp->consecutive_miss++;
        if (cp->consecutive_miss > cp->max_consecutive_miss) cp->max_consecutive_miss = cp->consecutive_miss;
    } else cp->consecutive_miss = 0;
    uint32_t slack_abs = et->slack_us < 0 ? 0 : (uint32_t)et->slack_us;
    if (slack_abs < cp->min_slack_us) cp->min_slack_us = slack_abs;
    if (et->duration_us > cp->max_duration_us) cp->max_duration_us = et->duration_us;
    cp->write_idx++;
}

void profile_print_all_pure(void) {
    printf("=== Sub5 Pure-DSP Profile ===\n");
    // Pure is stored in g_profile_pure[5]
    struct CoreProfile *cp = &g_profile_pure[0];
    uint32_t n = cp->write_idx < PROFILE_RING_SIZE ? cp->write_idx : PROFILE_RING_SIZE;
    if (n==0){ printf("Core[5] pure: no data\n"); return; }
    uint32_t durs[PROFILE_RING_SIZE];
    for(uint32_t i=0;i<n;i++){ uint32_t idx=(cp->write_idx - n + i) & PROFILE_RING_MASK; durs[i]=cp->ring[idx].duration_us; }
    for(uint32_t i=1;i<n;i++){ uint32_t key=durs[i]; int32_t j=(int32_t)i-1; while(j>=0 && durs[j]>key){durs[j+1]=durs[j]; j--;} durs[j+1]=key; }
    uint32_t p50=durs[(n*50)/100]; uint32_t p95=durs[(n*95)/100]; uint32_t p99=durs[(n*99)/100]; uint32_t p999=durs[(n*999)/1000]; uint32_t mx=durs[n-1];
    uint64_t sum=0; for(uint32_t i=0;i<n;i++) sum+=durs[i]; uint32_t avg=(uint32_t)(sum/n);
    printf("epochs=%u miss=%u max_consec=%u\n", cp->total_epochs, cp->miss_count, cp->max_consecutive_miss);
    printf("duration_us: avg=%u p50=%u p95=%u p99=%u p99.9=%u max=%u\n", avg,p50,p95,p99,p999,mx);
    printf("slack min=%u budget=%u headroom=%s\n", cp->min_slack_us, BLOCK_BUDGET_US, cp->max_duration_us > BLOCK_BUDGET_US ? "VIOLATED" : "OK");
}
#endif
