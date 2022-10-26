#include <ngx_window_filter.h>
#include <ngx_event_quic_connection.h>

#define USE_CC 1
#define USE_BBR_S               0
#define USE_LOSS_FILTER 0

#define BBR_MODE 4

#define NGX_BBR_MAX_DATAGRAMSIZE    1500
#define NGX_BBR_MIN_WINDOW          (4 * NGX_BBR_MAX_DATAGRAMSIZE)
#define NGX_BBR_MAX_WINDOW          (100 * NGX_BBR_MAX_DATAGRAMSIZE)
/* The RECOMMENDED value is the minimum of 10 * kMaxDatagramSize and max(2* kMaxDatagramSize, 14720)) */
/* same init window as cubic */
/* 32 is too aggressive. we have observed heavy bufferbloat events from online deployment */
/* 1440 * 10 / 1200 = 12 */
#define NGX_BBR_INITIAL_WINDOW  (32 * NGX_BBR_MAX_DATAGRAMSIZE) 
/* Pacing gain cycle rounds */
#define NGX_BBR_CYCLE_LENGTH    8
#define NGX_BBR_INF             0x7fffffff


/* Size of window of bandwidth filter, in rtts */
const uint32_t ngx_bbr_bw_win_size = NGX_BBR_CYCLE_LENGTH + 2;
/* Window of min rtt filter, in sec */
/* Minimum time spent in BBR_PROBE_RTT, in ms*/
const uint32_t ngx_bbr_probertt_time_ms = 200;
/* The gain of pacing rate for START_UP, 2/(ln2) */
const float ngx_bbr_high_gain = 2.885;
/* Gain in BBR_DRAIN */
const float ngx_bbr_drain_gain = 1.0 / 2.885;
/* Gain for cwnd in probe_bw, like slow start*/
const float ngx_bbr_cwnd_gain = 2;
/* Cycle of gains in PROBE_BW for pacing rate */
const float ngx_bbr_pacing_gain[] = {1.25, 0.75, 1, 1, 1, 1, 1, 1};
/* Minimum packets that need to ensure ack if there is delayed ack */
const uint32_t ngx_bbr_min_cwnd = 4 * NGX_BBR_MAX_DATAGRAMSIZE;
/* If bandwidth has increased by 1.25, there may be more bandwidth available */
const float ngx_bbr_fullbw_thresh = 1.25;
/* After 3 rounds bandwidth less than (1.25x), estimate the pipe is full */
const uint32_t ngx_bbr_fullbw_cnt = 3;
const uint32_t ngx_bbr_probertt_win_size_ms = 10000;

const uint32_t ngx_bbr_cwnd_update = 5000;

//BBR-S
 int bw[1010], tot;
 bool flag;
 bool st[1010];


static void 
ngx_bbr_enter_startup(ngx_bbr_t *bbr)
{
    bbr->mode = BBR_STARTUP;
    bbr->pacing_gain = ngx_bbr_high_gain;
    bbr->cwnd_gain = ngx_bbr_high_gain;
}

static void 
ngx_bbr_init_pacing_rate(ngx_bbr_t *bbr, ngx_sample_t *sampler)
{
    uint64_t bandwidth;
    if (sampler->srtt) {
        bbr->has_srtt = 1;
    }
    bandwidth = bbr->congestion_window * (uint64_t)MSEC2SEC
        / (sampler->srtt ? sampler->srtt : 1);
    bbr->pacing_rate = bbr->pacing_gain * bandwidth;
}

void 
ngx_bbr_init(ngx_bbr_t *bbr, ngx_sample_t *sampler)
{
    uint64_t now = ngx_current_msec;
    ngx_win_filter_reset(&bbr->bandwidth, 0, 0);

    bbr->min_rtt = sampler->srtt ? sampler->srtt : NGX_BBR_INF;
    bbr->min_rtt_stamp = now;
    bbr->round_start = 0;
    bbr->round_cnt = 0;
    bbr->next_round_delivered = 0;
    bbr->probe_rtt_round_done = FALSE;
    bbr->probe_rtt_round_done_stamp = 0;
    bbr->packet_conservation = FALSE;
    bbr->prior_cwnd = 0;
    bbr->initial_congestion_window = NGX_BBR_INITIAL_WINDOW;
    bbr->congestion_window = bbr->initial_congestion_window;
    bbr->has_srtt = 0;
    bbr->idle_restart = 0;
    bbr->packet_conservation = 0;
    bbr->recovery_mode = BBR_NOT_IN_RECOVERY;
    bbr->just_enter_recovery_mode = FALSE;
    bbr->just_exit_recovery_mode = FALSE;
    bbr->recovery_start_time = 0;
    bbr->full_bandwidth_cnt = 0;
    bbr->full_bandwidth_reached = FALSE;
    bbr->cc_mode = BBR_NOT_IN_CC;


    init_Loss_Filter(&bbr->loss_filter);
    loss_filter = &bbr->loss_filter;
    ngx_bbr_enter_startup(bbr);
    ngx_bbr_init_pacing_rate(bbr, sampler);

    bbr->send_rtt = 0;
    bbr->resend_rtt = 0;
}

int down_cnt = 0;
bool c_cwnd;
ngx_msec_t start_time;

static uint32_t 
ngx_bbr_max_bw(ngx_bbr_t *bbr)
{
    if (USE_BBR_S) {
        return bbr->bw;
    }
    //uint32_t backup = bbr->bw;
    bbr->bw = ngx_win_filter_get(&bbr->bandwidth);
    // if (bbr->bw < backup) {
    //     c_cwnd = true;
    //     start_time = ngx_current_msec;
    // }
    rate = bbr->bw;
    return bbr->bw;
}

static void 
ngx_bbr_update_bandwidth(ngx_bbr_t *bbr, ngx_sample_t *sampler, ngx_quic_congestion_t *cg)
{
    bbr->round_start = FALSE;
    /* Check whether the data is legal */
    if (/*sampler->delivered < 0 ||*/ sampler->interval <= 0) {
        return;
    }

    /* 
     * check whether the next BBR cycle is reached
     * at the beginning of the cycle, the number of packets sent is less than or equal to
     * the maximum number of packets that have been sent when the current ack packet is sent.
     */
    if (bbr->next_round_delivered <= sampler->prior_delivered) {
        bbr->next_round_delivered = sampler->total_acked;
        bbr->round_cnt++;
        bbr->round_start = TRUE;
        bbr->packet_conservation = 0;

        if (bbr->send_rtt > 0) {
            //float loss_ = bbr->resend_rtt * 1.0 / bbr->send_rtt;
            if (bbr->resend_rtt < bbr->send_rtt) {
                insertLoss(&bbr->loss_filter, bbr->resend_rtt);
            }
            bbr->send_rtt = 0;
            bbr->resend_rtt = 0;
        }
    }
    /* FIXED: It may reduce the est. bw due to network instability. */
    /*  if (sampler->lagest_ack_time > bbr->last_round_trip_time) {
        bbr->round_cnt++;
        bbr->last_round_trip_time = ngx_current_msec;
    } */
    uint32_t bandwidth;
    /* Calculate the new bandwidth, bytes per second */
    bandwidth = 1.0 * sampler->delivered / sampler->interval * MSEC2SEC;
    if (!sampler->is_app_limited && bbr->mode == BBR_PROBE_BW) {
        if (bbr->mode == BBR_PROBE_BW && bandwidth < bbr->bw * 0.9) {
            down_cnt++;
        } else {
            down_cnt = 0;
        }
        //printf("%d\n", down_cnt);
        
    }
    if (down_cnt > 30 /* || ngx_current_msec - start_time > 10 * bbr->min_rtt*/) {
            c_cwnd = true;
            start_time = ngx_current_msec;
            //printf("%d\n", down_cnt);
        }
    if (bandwidth >= ngx_bbr_max_bw(bbr)) {
        c_cwnd = false;
    }
    //    printf("%d,%d, %f\n",bandwidth, bbr->bw, bandwidth * 1.0 / bbr->bw);

    // if (sampler->is_app_limited) {
    //     printf("app_limited\n");
    // }

    //printf("%f %d\n", bbr->pacing_gain, bandwidth);
    if (!sampler->is_app_limited && bandwidth * 1.33 < bbr->bw) {
        bbr->max_down[bbr->bw_down_cnt % 30] = bandwidth;
        bbr->bw_down_cnt++;
    } 
    else if (bandwidth * 1.33 >= bbr->bw) {
        bbr->bw_down_cnt = 0;
    }

    if (!sampler->is_app_limited || bandwidth >= ngx_bbr_max_bw(bbr)) {
        ngx_win_filter_max(&bbr->bandwidth, ngx_bbr_bw_win_size, 
                           bbr->round_cnt, bandwidth);
    }

    // if (!sampler->is_app_limited && bbr->mode == BBR_PROBE_BW) {
    //     printf("%ld,%d,%.5f,%ld,%.5f,%ld\n",ngx_current_msec - cg->start_time, bandwidth * 8, bandwidth * 1.0 / ngx_bbr_max_bw(bbr), sampler->rtt,sampler->rtt * 1.0 / bbr->min_rtt,cg->window);
    // }

    if (USE_BBR_S) {
        if (sampler->is_app_limited) return;
        bw[tot] = bandwidth;
        tot++;
        if (tot >= 100) {
            flag = true;
            tot = 0;
        }
        if (flag) {
            for (int i = 0; i < 100; i++) st[i] = false;
            for (int i = 0; i < 15; i++) {
                int max = 0, c = -1; 
                for (int i = 0; i < 100; i++) {
                    if (bw[i] > max && st[i] == false) {
                        max = bw[i];
                        c = i;
                    }
                }
                st[c] = true;
                if (i == 14) bbr->bw = max;
            }
        }
    }
}

static uint32_t 
ngx_bbr_bdp(ngx_bbr_t *bbr)
{
    return bbr->min_rtt * ngx_win_filter_get(&bbr->bandwidth) / MSEC2SEC;
}


static uint32_t 
ngx_bbr_target_cwnd(ngx_bbr_t *bbr, float gain)
{
    if (bbr->min_rtt == NGX_BBR_INF) {
        return bbr->initial_congestion_window;
    }
    uint32_t cwnd = gain * ngx_bbr_bdp(bbr);
    return ngx_max(cwnd, NGX_BBR_MIN_WINDOW);
}

static bool 
ngx_bbr_is_next_cycle_phase(ngx_bbr_t *bbr, ngx_sample_t *sampler)
{
    bool is_full_length = (sampler->now - bbr->last_cycle_start) > bbr->min_rtt;
    uint32_t inflight = sampler->prior_inflight;
    bool should_advance_gain_cycling = is_full_length;
    if (USE_CC) {
        if (bbr->pacing_gain > 1.0) {
            should_advance_gain_cycling = is_full_length
                && (sampler->loss 
                    || inflight >= ngx_bbr_target_cwnd(bbr, bbr->pacing_gain));

        
        }
    } else {
        if (bbr->pacing_gain > 1.0) {
            should_advance_gain_cycling = is_full_length
                && (sampler->loss 
                    || inflight >= ngx_bbr_target_cwnd(bbr, bbr->pacing_gain));
        }
    }
    // if (bbr->pacing_gain > 1.0) {
    //     should_advance_gain_cycling = is_full_length 
    //             || inflight >= ngx_bbr_target_cwnd(bbr, bbr->pacing_gain);
    // }
    /* Drain to target: 1xBDP */
    if (USE_CC && !c_cwnd) {
        // if (bbr->pacing_gain < 1.0) {
        //         should_advance_gain_cycling = (sampler->now - bbr->last_cycle_start) > 3 * bbr->min_rtt || (inflight <= ngx_bbr_target_cwnd(bbr, 1.0));
        //     }
         if (bbr->pacing_gain < 1.0) {
                should_advance_gain_cycling = is_full_length || 
                (inflight <= ngx_bbr_target_cwnd(bbr, 1.0));
            }
    } else {
        if (bbr->pacing_gain < 1.0) {
                should_advance_gain_cycling = is_full_length 
                    || (inflight <= ngx_bbr_target_cwnd(bbr, 1.0));
            }
    }
    
    return should_advance_gain_cycling;
}

static float 
ngx_bbr_get_pacing_gain(ngx_bbr_t *bbr, uint32_t cycle_idx)
{
    return ngx_bbr_pacing_gain[cycle_idx];
}


static void 
ngx_bbr_update_cycle_phase(ngx_bbr_t *bbr, ngx_sample_t *sampler)
{
    if (bbr->mode == BBR_PROBE_BW 
        && ngx_bbr_is_next_cycle_phase(bbr, sampler))
    {
        bbr->cycle_idx = (bbr->cycle_idx + 1) % NGX_BBR_CYCLE_LENGTH;
        bbr->last_cycle_start = sampler->now;
        bbr->pacing_gain = ngx_bbr_get_pacing_gain(bbr, bbr->cycle_idx);
    }
}


static void 
ngx_bbr_check_full_bw_reached(ngx_bbr_t *bbr, ngx_sample_t *sampler)
{
    /*
     * we MUST only check whether full bw is reached ONCE per RTT!!! 
     * Otherwise, startup may end too early due to multiple ACKs arrive in a RTT.
     */
    if (!bbr->round_start || bbr->full_bandwidth_reached 
        || sampler->is_app_limited)
    {
        return;
    }

    uint32_t bw_thresh = bbr->last_bandwidth * ngx_bbr_fullbw_thresh;
    if (ngx_bbr_max_bw(bbr) >= bw_thresh) {
        bbr->last_bandwidth = ngx_bbr_max_bw(bbr);
        bbr->full_bandwidth_cnt = 0;
        return;
    }
    ++bbr->full_bandwidth_cnt;
    bbr->full_bandwidth_reached = bbr->full_bandwidth_cnt >= ngx_bbr_fullbw_cnt;
}

static void 
ngx_bbr_enter_drain(ngx_bbr_t *bbr)
{
    bbr->mode = BBR_DRAIN;
    bbr->pacing_gain = ngx_bbr_drain_gain;
    bbr->cwnd_gain = ngx_bbr_high_gain;
}

static void 
ngx_bbr_enter_probe_bw(ngx_bbr_t *bbr, ngx_sample_t *sampler)
{
    bbr->mode = BBR_PROBE_BW;
    bbr->cwnd_gain = ngx_bbr_cwnd_gain;
    bbr->cycle_idx = ngx_random() % (NGX_BBR_CYCLE_LENGTH - 1);
    bbr->cycle_idx = bbr->cycle_idx == 0 ? bbr->cycle_idx : bbr->cycle_idx + 1;
    bbr->pacing_gain = ngx_bbr_get_pacing_gain(bbr, bbr->cycle_idx);
    bbr->last_cycle_start = sampler->now;
}

static void 
ngx_bbr_check_drain(ngx_bbr_t *bbr, ngx_sample_t *sampler)
{
    if (bbr->mode == BBR_STARTUP && bbr->full_bandwidth_reached) {
        ngx_bbr_enter_drain(bbr);
    }
    if (bbr->mode == BBR_DRAIN 
        && sampler->in_filght <= ngx_bbr_target_cwnd(bbr, 1.0)) {
        ngx_bbr_enter_probe_bw(bbr, sampler);
    }
        
}

static void 
ngx_bbr_enter_probe_rtt(ngx_bbr_t *bbr)
{
    bbr->mode = BBR_PROBE_RTT;
    bbr->pacing_gain = 1;
    bbr->cwnd_gain = 1;
}

static void 
ngx_bbr_save_cwnd(ngx_bbr_t *bbr)
{
    if (bbr->recovery_mode != BBR_IN_RECOVERY 
        && bbr->mode != BBR_PROBE_RTT)
    {
        bbr->prior_cwnd = bbr->congestion_window;

    } else {
        bbr->prior_cwnd = ngx_max(bbr->congestion_window, bbr->prior_cwnd);
    }
}
static void 
ngx_bbr_restore_cwnd(ngx_bbr_t *bbr)
{
    bbr->congestion_window = ngx_max(bbr->congestion_window, bbr->prior_cwnd);
}

static void 
ngx_bbr_exit_probe_rtt(ngx_bbr_t *bbr, ngx_sample_t *sampler)
{
    if (bbr->full_bandwidth_reached) {
        ngx_bbr_enter_probe_bw(bbr, sampler);

    } else {
        ngx_bbr_enter_startup(bbr);
    }
}

static void 
ngx_bbr_check_probe_rtt_done(ngx_bbr_t *bbr, ngx_sample_t *sampler)
{
    if (!bbr->probe_rtt_round_done_stamp 
        || sampler->now < bbr->probe_rtt_round_done_stamp) 
    {
        return;
    }
    /* schedule the next probeRTT round */
    bbr->min_rtt_stamp = sampler->now;
    ngx_bbr_restore_cwnd(bbr);
    ngx_bbr_exit_probe_rtt(bbr, sampler);
    //printf("Exit probe_RTT: %ld\n", ngx_current_msec);
}

static uint32_t 
ngx_bbr_probe_rtt_cwnd(ngx_bbr_t *bbr)
{
    return ngx_bbr_min_cwnd;
}

static void 
ngx_bbr_update_min_rtt(ngx_bbr_t *bbr, ngx_sample_t *sampler, ngx_quic_congestion_t *cg)
{
    bool min_rtt_expired = sampler->now > 
                      (bbr->min_rtt_stamp + ngx_bbr_probertt_win_size_ms);
    if (sampler->rtt <= bbr->min_rtt || min_rtt_expired) {
        bbr->min_rtt = sampler->rtt;
        bbr->min_rtt_stamp = sampler->now;
    }
    if (min_rtt_expired && bbr->mode != BBR_PROBE_RTT 
        && !bbr->idle_restart)
    {   
        //printf("Enter probe_RTT: %ld\n", ngx_current_msec);
        ngx_bbr_enter_probe_rtt(bbr);
        ngx_bbr_save_cwnd(bbr);
        bbr->probe_rtt_round_done_stamp = 0;
    }
    if (bbr->mode == BBR_PROBE_RTT)
    {
        /* Ignore low rate samples during this mode. */ 
        cg->app_limited = (cg->delivered 
            + cg->in_flight)? (cg->delivered
                + cg->in_flight) : 1;
        if ((!bbr->probe_rtt_round_done_stamp 
            && (sampler->in_filght <= ngx_bbr_probe_rtt_cwnd(bbr))))
        {
            bbr->probe_rtt_round_done_stamp = sampler->now + 
                                              ngx_min(2*sampler->srtt, 
                                              ngx_bbr_probertt_time_ms);                                 
            bbr->probe_rtt_round_done = FALSE;
            bbr->next_round_delivered = sampler->total_acked;

        } else if (bbr->probe_rtt_round_done_stamp) {
            if (bbr->round_start) {
                bbr->probe_rtt_round_done = TRUE;
            }
            if (bbr->probe_rtt_round_done) {
                ngx_bbr_check_probe_rtt_done(bbr, sampler);
            }
        }
    }

    /* Restart after idle ends only once we process a new S/ACK for data */
    if (sampler->delivered > 0) {
        bbr->idle_restart = 0;
    }
}

static void 
_ngx_bbr_set_pacing_rate_helper(ngx_bbr_t *bbr, float pacing_gain)
{
    uint32_t bandwidth, rate;
    bandwidth = ngx_bbr_max_bw(bbr);
    // if (pacing_gain < 1.25 && (bbr->cc_mode == BBR_RECOVERY_CC || bbr->cc_mode == BBR_IN_CC)) {
    //     pacing_gain = 0.65;
    // }
    rate = bandwidth * pacing_gain;
    if (bbr->full_bandwidth_reached || rate > bbr->pacing_rate) {
        bbr->pacing_rate = rate;
    }
}

static void 
ngx_bbr_set_pacing_rate(ngx_bbr_t *bbr, ngx_sample_t *sampler)
{
    if (!bbr->has_srtt && sampler->srtt) {
        ngx_bbr_init_pacing_rate(bbr, sampler);
    }
    _ngx_bbr_set_pacing_rate_helper(bbr, bbr->pacing_gain);

    // if (USE_LOSS_FILTER) {
        
    //     if (bbr->mode == BBR_PROBE_BW && bbr->pacing_gain == 1) {
    //         //bbr->pacing_rate *= (0.01 * bbr->loss_filter.rank);

    //         float f = (1 - 10 * bbr->loss_filter.loss_now) * (1 - 10 * bbr->loss_filter.loss_now);
    //         if (bbr->loss_filter.loss_now > 0.1) {
    //             f = 0;
    //         }
    //         bbr->pacing_rate = bbr->pacing_rate * f + bbr->pacing_rate * (1 - f) * (0.01 * bbr->loss_filter.rank);
    //     }
    //     // extern int buffer;
    //     // if (size <= 0 || buffer == 0) {
    //     //     return;
    //     // }
    //     // u_int64_t min_rate = (u_int64_t)(((u_int64_t)(((u_int64_t)(size * 1.0 / buffer)) + 999) * 1.0) / 1000);
    //     // //printf("%d, %d, %ld\n", size, buffer, min_rate);
    //     // bbr->pacing_rate = mymax(bbr->pacing_rate, min_rate);
    //     // bbr->pacing_rate = mymin(bbr->pacing_rate, bbr->bw * bbr->pacing_gain);
    // }

    if (bbr->pacing_rate == 0) {
        ngx_bbr_init_pacing_rate(bbr, sampler);
    }
}

static void 
ngx_bbr_modulate_cwnd_for_recovery(ngx_bbr_t *bbr, ngx_sample_t *sampler, ngx_quic_congestion_t *cg)
{
    if (sampler->loss > 0) {
        /* to avoid underflow of unsigned numbers */
        if (bbr->congestion_window 
            > sampler->loss) 
        {
            bbr->congestion_window -= sampler->loss;

        } else {
            bbr->congestion_window = 0;
        }
        bbr->congestion_window = ngx_max(bbr->congestion_window, 
            NGX_BBR_MAX_DATAGRAMSIZE);
    }
    if (bbr->just_enter_recovery_mode) {
        bbr->just_enter_recovery_mode = FALSE;
        bbr->packet_conservation = 1;
        bbr->next_round_delivered = sampler->total_acked;
        bbr->congestion_window = cg->in_flight + 
                                 ngx_max(sampler->acked, 
                                         NGX_BBR_MAX_DATAGRAMSIZE);

    } else if (bbr->just_exit_recovery_mode) {
        /* 
         * exit recovery mode once any packet sent
         * during the recovery epoch is acked.
         */
        bbr->just_exit_recovery_mode = FALSE;
        bbr->packet_conservation = 0;
        ngx_bbr_restore_cwnd(bbr);
    }
    if (bbr->packet_conservation) {
        bbr->congestion_window = ngx_max(bbr->congestion_window, 
                                 cg->in_flight + 
                                 sampler->acked);
    }
}

void 
ngx_bbr_reset_cwnd(ngx_bbr_t *bbr)
{
    ngx_bbr_save_cwnd(bbr);
    /* reduce cwnd to the minimal value */
    bbr->congestion_window = NGX_BBR_MIN_WINDOW;
    /* cancel recovery state */
    if (bbr->recovery_mode == BBR_IN_RECOVERY) {
        bbr->recovery_mode = BBR_NOT_IN_RECOVERY;
        bbr->packet_conservation = 0;
        /* we do not restore cwnd here */
    }
    /* reset recovery start time in any case */
    bbr->recovery_start_time = 0;
    /* If losses happened, we do not increase cwnd beyond target_cwnd. */
}


static void 
ngx_bbr_set_cwnd(ngx_bbr_t *bbr, ngx_sample_t *sampler, ngx_quic_congestion_t *cg)
{
    if (sampler->acked != 0) {

        uint32_t target_cwnd;
        target_cwnd = ngx_bbr_target_cwnd(bbr, bbr->cwnd_gain);

        ngx_bbr_modulate_cwnd_for_recovery(bbr, sampler, cg);
        if (!bbr->packet_conservation) {
            if (bbr->full_bandwidth_reached) {
                /* additive increasing target_cwnd */
                bbr->congestion_window = ngx_min(target_cwnd, 
                                                bbr->congestion_window + 
                                                sampler->acked);

            } else if (bbr->congestion_window < target_cwnd 
                    || cg->delivered < bbr->initial_congestion_window)
            {
                bbr->congestion_window += sampler->acked;
            }
        }
        bbr->congestion_window = ngx_max(bbr->congestion_window, ngx_bbr_min_cwnd);
    }
    if (bbr->mode == BBR_PROBE_RTT) {
        bbr->congestion_window = ngx_min(bbr->congestion_window, 
                                         ngx_bbr_probe_rtt_cwnd(bbr));
    }
}

void 
ngx_bbr_on_lost(ngx_bbr_t *bbr, ngx_msec_t lost_sent_time)
{
    /* 
     * Unlike the definition of "recovery epoch" for loss-based CCs, 
     * for the sake of resistance to losses, we MUST refresh the end of a 
     * recovery epoch if further losses happen in the epoch. Otherwise, the
     * ability of BBR to sustain network where high loss rate presents 
     * is hampered because of frequently entering packet conservation state. 
     */
    ngx_bbr_save_cwnd(bbr);
    bbr->recovery_start_time = ngx_current_msec;
    /* If losses happened, we do not increase cwnd beyond target_cwnd. */
}

static void
ngx_bbr_update_recovery_mode(ngx_bbr_t *bbr, ngx_sample_t *sampler)
{
    if (sampler->po_sent_time <= bbr->recovery_start_time 
        && bbr->recovery_mode == BBR_NOT_IN_RECOVERY)
    {
        bbr->just_enter_recovery_mode = TRUE;
        bbr->recovery_mode = BBR_IN_RECOVERY;

    } 
    else if (sampler->po_sent_time > bbr->recovery_start_time 
             && bbr->recovery_mode == BBR_IN_RECOVERY)
    {
        /* exit recovery mode once any packet sent during the recovery epoch is acked. */
        bbr->recovery_mode = BBR_NOT_IN_RECOVERY;
        bbr->just_exit_recovery_mode = TRUE;
        bbr->recovery_start_time = 0;
    }
}

ngx_msec_t ttt;
u_int64_t crtt = 0;
int cc_cnt = 0;

void
ngx_bbr_update_cc_mode(ngx_bbr_t *bbr, ngx_sample_t *sampler, ngx_quic_congestion_t *cg)
{
    if (ngx_current_msec - start_time > 1 * bbr->min_rtt) {
        c_cwnd = false;
    }

    if (bbr->mode == BBR_PROBE_BW) {
        // if (c_cwnd) {
        //     bbr->cwnd_gain = 2;
        //     return;
        // }
        if (sampler->loss) {
            bbr->cwnd_gain = ngx_min(sampler->rtt * 1.0 / bbr->min_rtt, 2.0);
        } else {
            bbr->cwnd_gain = ngx_min(bbr->cwnd_gain + sampler->acked * 1.0 / sampler->in_filght, 2.0);
        }
    }

    if (sampler->rtt > 2.5 * bbr->min_rtt && bbr->bw_down_cnt >= 30 && sampler->loss) {
        uint32_t max_down = 0;
        for (int i = 0; i < 30; i++) {
            max_down = ngx_max(max_down, bbr->max_down[i]);
        }
        ngx_win_filter_reset(&bbr->bandwidth, bbr->round_cnt, max_down);
    }

    //printf("%d %f\n", loss_up, bbr->cwnd_gain);
    
    // if (sampler->loss) {
    //     printf("%ld\n", sampler->srtt);
    // }
    return;

    if (ngx_current_msec - bbr->cc_start_time > 8 * bbr->min_rtt && bbr->cc_mode == BBR_PROBE_CC) {
        bbr->cc_mode = BBR_NOT_IN_CC;
        bbr->probe_rtt = 0;
        bbr->cc_rtt = 0;
    }

    if (sampler->rtt > crtt) {
        cc_cnt++;
    } else if (sampler->rtt < crtt) {
        cc_cnt = 0;
    }
    crtt = sampler->rtt;
    if (cc_cnt == 10) {
        cc_cnt = 0;
        bbr->cc_mode = BBR_IN_CC;
        bbr->cc_rtt = sampler->srtt;
        ttt = ngx_current_msec;
    }

    if (bbr->cc_mode == BBR_NOT_IN_CC
        && sampler->srtt >= bbr->min_rtt * 1.25 && cg->in_flight >= ngx_bbr_target_cwnd(bbr, 1.25))
    {
        //printf("probe %ld\n", bbr->probe_rtt);
        bbr->cc_mode = BBR_PROBE_CC;
        bbr->probe_rtt = sampler->srtt;
        bbr->cc_start_time = ngx_current_msec;
    } 
    else if (bbr->cc_mode == BBR_PROBE_CC
             && sampler->srtt >= bbr->probe_rtt * 1.25 && sampler->loss)
    {
        bbr->cc_mode = BBR_IN_CC;
        bbr->cc_rtt = sampler->srtt;
        ttt = ngx_current_msec;
    }
    else if (bbr->cc_mode == BBR_IN_CC)
    {
        //printf("%ld\n",bbr->cc_rtt);
        if (sampler->srtt >= bbr->cc_rtt * 1.25) {
            bbr->cc_mode = BBR_RECOVERY_CC;
        }
        if (ngx_current_msec - ttt > 3 * bbr->min_rtt && cg->in_flight <= ngx_bbr_target_cwnd(bbr, 1.05)) {
            bbr->cc_mode = BBR_NOT_IN_CC;
        }
    }
    else if (bbr->cc_mode == BBR_RECOVERY_CC)
    {
        if (bbr->bw_down_cnt >= 30) {
            uint32_t max_down = 0;
            for (int i = 0; i < 30; i++) {
                max_down = ngx_max(max_down, bbr->max_down[i]);
            }
            ngx_win_filter_reset(&bbr->bandwidth, bbr->round_cnt, max_down);
            bbr->bw_down_cnt = 0;
        }
        if (ngx_current_msec - ttt > 3 * bbr->min_rtt && cg->in_flight <= ngx_bbr_target_cwnd(bbr, 1.05)) {
            bbr->cc_mode = BBR_NOT_IN_CC;
        }
    }
}

void 
ngx_bbr_on_ack(ngx_bbr_t *bbr, ngx_sample_t *sampler, ngx_quic_congestion_t *cg)
{
    /* Update model and state */
    // if (sampler->loss) {
    //     printf("%ld\n", sampler->rtt);
    // }
    ngx_bbr_update_bandwidth(bbr, sampler,cg);
    ngx_bbr_update_cycle_phase(bbr, sampler);
    ngx_bbr_check_full_bw_reached(bbr, sampler);
    ngx_bbr_check_drain(bbr, sampler);
    ngx_bbr_update_min_rtt(bbr, sampler, cg);

    ngx_bbr_update_recovery_mode(bbr, sampler);
    if (USE_CC) {
        ngx_bbr_update_cc_mode(bbr,sampler, cg);
    }
    /* Update control parameter */
    ngx_bbr_set_pacing_rate(bbr, sampler);
    ngx_bbr_set_cwnd(bbr, sampler, cg);
}

void 
ngx_bbr_restart_from_idle(ngx_bbr_t *bbr, uint64_t conn_delivered)
{
    //printf("restart_from_idle\n");
    uint64_t now = ngx_current_msec;
    bbr->idle_restart = 1;
    ngx_sample_t sampler = {.now = now, .total_acked = conn_delivered};

    if (bbr->mode == BBR_PROBE_BW) {
        _ngx_bbr_set_pacing_rate_helper(bbr, 1.0);
        if (bbr->pacing_rate == 0) {
            ngx_bbr_init_pacing_rate(bbr, &sampler);
        }
    } else if (bbr->mode == BBR_PROBE_RTT) {
        ngx_bbr_check_probe_rtt_done(bbr, &sampler);
    }
}

