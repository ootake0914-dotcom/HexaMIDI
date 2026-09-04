#ifndef RT_PROFILE_H_
#define RT_PROFILE_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROFILE_RING_BITS 4  // 16 entries (2KB total for 6 cores) to save RAM
#define PROFILE_RING_SIZE (1u << PROFILE_RING_BITS)
#define PROFILE_RING_MASK (PROFILE_RING_SIZE - 1)
#define BLOCK_BUDGET_US 10666u

struct ProfileEntry {
    uint32_t epoch;
    uint32_t start_us;
    uint32_t end_us;
    uint32_t duration_us;
    int32_t  slack_us;
};

struct CoreProfile {
    struct ProfileEntry ring[PROFILE_RING_SIZE];
    volatile uint32_t write_idx;
    volatile uint32_t total_epochs;
    volatile uint32_t miss_count;
    volatile uint32_t consecutive_miss;
    volatile uint32_t max_consecutive_miss;
    volatile uint32_t min_slack_us;
    volatile uint32_t max_duration_us;
};

extern struct CoreProfile g_profile[6];

void profile_reset(void);
void profile_epoch_start(uint32_t core_id, uint32_t epoch);
void profile_epoch_end(uint32_t core_id, uint32_t epoch);
void profile_print_stats(uint32_t core_id);
void profile_print_all(void);
uint32_t profile_get_monotonic_us(void);

#ifdef PROFILE_ENABLE
void profile_epoch_start_pure(uint32_t core_id, uint32_t epoch);
void profile_epoch_end_pure(uint32_t core_id, uint32_t epoch);
void profile_print_all_pure(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
