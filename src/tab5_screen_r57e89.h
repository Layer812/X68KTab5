#pragma once
#include <stdint.h>
typedef struct {
 uint64_t frame_requests,frame_consumed,present_opportunities,present_consumed;
 uint64_t seal_armed,seal_arm_rejected,seal_tries,seal_block_pending,seal_block_retry,seal_block_reset,seal_nochange,seal_epoch_drop;
 uint64_t presenter_calls,presenter_rejects,display_ok,display_fail;
 uint64_t mailbox_posts,mailbox_duplicates,mailbox_offer_suppressed,mailbox_claims,mailbox_swaps,mailbox_edges;
 uint64_t rescue_try,rescue_ok,rescue_busy,rescue_budget;
 uint32_t pending_tickets,retry_debt,seal_requested,admission_open;
} tab5_screen_r57e89_stats_t;
void tab5_screen_r57e89_get_stats(tab5_screen_r57e89_stats_t *out);
