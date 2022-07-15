#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_event_quic_connection.h>


const float BBRHighGain = 2.885f;
const float kStartupGrowthTarget = 1.25;
const uint64_t  SMSS = 1460;
const uint64_t  BBRMinPipeCwnd = 14600;// (4 * SMSS)
const uint64_t  BBRGainCycleLen = 8;
const uint64_t  ProbeRTTInterval = 10 * 1000;
const uint64_t  ProbeRTTDuration = 200;
const uint64_t  _InitialCwnd_ = 14600;
const uint64_t  INITIAL_RTT = 333;


float pacing_gain_cycle[] = {0.75, 1, 1, 1, 1, 1, 1, 1.25};

uint64_t mymin(uint64_t a, uint64_t b) {
    if (a < b) {
        return a;
    } else {
        return b;
    }
}

uint64_t mymax(uint64_t a, uint64_t b) {
    if (a > b) {
        return a;
    } else {
        return b;
    }
}

void BBREnterStartup(BBR *bbr) {
    bbr->mode = STARTUP;
    bbr->pacing_gain = BBRHighGain;
    bbr->cwnd_gain = BBRHighGain;
}

void BBREnterDrain(BBR *bbr) {
    bbr->mode = DRAIN;
    bbr->pacing_gain = 1.0f/BBRHighGain;
    bbr->cwnd_gain = BBRHighGain;
}

void BBRAdvanceCyclePhase(BBR *bbr) {
    bbr->last_cycle_start = ngx_current_msec;
    bbr->cycle_index = (bbr->cycle_index + 1) % BBRGainCycleLen;
    bbr->pacing_gain = pacing_gain_cycle[bbr->cycle_index];
}

void BBREnterProbeBW(BBR *bbr) {
    bbr->mode = PROBE_BW;
    bbr->cwnd_gain = 2;
    bbr->pacing_gain = 1;

    /*伪代码*/          //---------------------------------------
    bbr->cycle_index = random() % (BBRGainCycleLen - 1);    //---------------------------------------
    BBRAdvanceCyclePhase(bbr);
}

void  BBREnterProbeRTT(BBR *bbr) {
    bbr->mode = PROBE_RTT;
    bbr->cwnd_gain = 1;
    bbr->cwnd_gain = 1;
}

void BBRInit(BBR *bbr) {
    bbr->start_tim = ngx_current_msec;
    bbr->timer = ngx_current_msec;
    bbr->BtlBw = 1000;
    bbr->RTprop = INITIAL_RTT;
    bbr->rtprop_stamp = ngx_current_msec;
    bbr->probe_rtt_done_stamp = 0;
    bbr->conservation = false;
    bbr->prior_cwnd = 0;
    bbr->first_sendtime = ngx_current_msec;
    bbr->start_time = ngx_current_msec;
    bbr->send_rtt = 0;
    bbr->resend_rtt = 0;
    bbr->is_send = 0;
    bbr->len = 0;

    //BBRInitRoundCounting
    bbr->round_start = false;
    bbr->round_count = 0;
    bbr->cnt = 0;
    bbr->delivered = 1;
    bbr->current_round_trip_end_ = 0;

    //BBRInitFullPipe
    bbr->is_at_full_bandwidth_ = false;
    bbr->full_bw = 0;
    bbr->full_bw_count = 0;

    //BBRInitPacingRate
    bbr->cwnd = _InitialCwnd_;
    bbr->pacing_rate = 1000; // 100K

    for (int i = 1; i < 10; i++) {
        bbr->queue[i] = 0;
    }
    bbr->queue[5] = 1000;

    init_Loss_Filter(&bbr->loss_filter);
    BBREnterStartup(bbr);

}



uint64_t GetTargetCongestionWindow(BBR *bbr, float gain) {
    uint64_t  bdp = bbr->BtlBw * bbr->RTprop;
    uint64_t  congestion_window = gain * bdp;
    if (congestion_window == 0 || congestion_window < _InitialCwnd_) {
        congestion_window = _InitialCwnd_;
    }
    bbr->target_cwnd = congestion_window;
    return bbr->target_cwnd;
}

void BBRCheckFullPipe(BBR *bbr) {
    if (bbr->is_at_full_bandwidth_ || !bbr->round_start) {
        return;
    }
    if (bbr->BtlBw > bbr->full_bw * 1.25) {
        bbr->full_bw = bbr->BtlBw;
        bbr->full_bw_count = 0;
        return;
    }
    bbr->full_bw_count++;
    if (bbr->full_bw_count >= 3) {
        bbr->is_at_full_bandwidth_ = true;
    }
}

void BBRCheckDrain(BBR *bbr, uint64_t pck_inflight) {
    if (bbr->mode == STARTUP && bbr->is_at_full_bandwidth_) {
        BBREnterDrain(bbr);
    }
    if (bbr->mode == DRAIN && pck_inflight <= GetTargetCongestionWindow(bbr, 1.0)) {
        BBREnterProbeBW(bbr);
    }
}


void UpdateRoundtripCounter(BBR *bbr, uint64_t p_drivered, uint64_t plen) {
    bbr->delivered += plen;
    if (p_drivered >= bbr->current_round_trip_end_) {
        //printf("%ld,%ld,%ld,%.2f,%ld,%ld,%.2f,%ld\n", ngx_current_msec - bbr->start_time, bbr->resend_rtt, bbr->send_rtt, bbr->resend_rtt * 100.0 / bbr->send_rtt, bbr->resend, bbr->sum, bbr->resend * 100.0 / bbr->sum, bbr->loss_filter.rank);
        bbr->round_start = true;
        float loss_rtt = bbr->resend_rtt * 1.0 / bbr->send_rtt;
        insertLoss(&bbr->loss_filter, loss_rtt);
        bbr->send_rtt = 0;
        bbr->resend_rtt = 0;
        if (!bbr->is_app_limit) {
            bbr->round_count++;
            bbr->queue[bbr->round_count % 10] = 0;
        }
        bbr->current_round_trip_end_ = bbr->delivered;

        // if (bbr->conservation) {
        //     printf("-------------");
        // }
        // printf("%ld %ld %ld %ld %ld %f\n", bbr->mode, bbr->round_count, bbr->BtlBw, bbr->RTprop, bbr->cwnd,  bbr->pacing_gain);

        // for (int i = 0; i < 10; i++) {
        //     printf("%ld ", bbr->queue[i]);
        // }
        // printf("\n");
        //printf("%ld %ld %ld\n", bbr->delivered, bbr->round_count, ngx_current_msec);   
         //printf("%ld %ld %ld %f %ld\n", bbr->mode, bbr->round_count, ngx_current_msec, bbr->pacing_gain, bbr->BtlBw);   
    } else {
        bbr->round_start = false;
    }
    
}

void BBRUpdateRTprop(BBR *bbr, ngx_msec_t sample_min_rtt) {
    // if (sample_min_rtt > 200) {
    //     printf("%ld\n", sample_min_rtt);
    // }
    bbr->rtprop_expired = ngx_current_msec > bbr->rtprop_stamp + ProbeRTTInterval;
    if (bbr->rtprop_expired || sample_min_rtt < bbr->RTprop || bbr->RTprop == 0) {
        bbr->RTprop = sample_min_rtt;
        bbr->rtprop_stamp = ngx_current_msec;
    }
}

void BBRUpdateBtlBw(BBR *bbr, uint64_t p_drivered, uint64_t plen, ngx_msec_t send_time, ngx_msec_t send_interval) {
    UpdateRoundtripCounter(bbr, p_drivered, plen);
    uint64_t drivered = bbr->delivered - p_drivered;
    if (drivered < 1) {
        drivered = 1;
    }
    uint64_t interval = mymax(ngx_current_msec - send_time, send_interval);
    if (interval < 1) {
        interval = 1;
    }
    uint64_t BW = drivered / interval;

    if (BW > 3900) {
        bbr->over_bdp++;
    }
    bbr->sum_inflight++;

    if (bbr->mode != STARTUP) {
        BW = mymin(BW, bbr->BtlBw * 1.25);
    }
    
    if (BW > bbr->queue[(bbr->round_count) % 10]) {
        bbr->queue[(bbr->round_count) % 10] = BW;
    }
    
    bbr->BtlBw = 300;
    for (uint64_t i = 0; i < 10; i++) {
        if (bbr->queue[i] > bbr->BtlBw) {
            bbr->BtlBw = bbr->queue[i];
        }       
    }
    bbr->first_sendtime = send_time;
    //printf("%ld %ld %ld %ld\n", drivered, interval, BW, p_drivered);
    // if (interval < 250) {
    //     printf("%ld %ld %ld %ld\n", drivered, interval, BW, p_drivered);
    // }
    //printf("%ld %ld %ld %ld\n", drivered, interval, BW, bbr->BtlBw);
}


void BBRUpdatePacingRate(BBR *bbr) {
    uint64_t target_rate = bbr->BtlBw * bbr->pacing_gain;
    if (bbr->is_at_full_bandwidth_ || target_rate > bbr->pacing_rate) {
        bbr->pacing_rate = target_rate;
    }
    if (bbr->mode == PROBE_BW && bbr->pacing_gain == 1) {
        //bbr->pacing_rate *= (0.01 * bbr->loss_filter.rank);

        float f = (1 - 10 * bbr->loss_filter.loss_now) * (1 - 10 * bbr->loss_filter.loss_now);
        if (bbr->loss_filter.loss_now > 0.1) {
            f = 0;
        }
        bbr->pacing_rate = bbr->pacing_rate * f + bbr->pacing_rate * (1 - f) * (0.01 * bbr->loss_filter.rank);
    }
    extern int buffer;
    if (size <= 0 || buffer == 0) {
        return;
    }
    u_int64_t min_rate = (u_int64_t)(((u_int64_t)(((u_int64_t)(size * 1.0 / buffer)) + 999) * 1.0) / 1000);
    //printf("%d, %d, %ld\n", size, buffer, min_rate);
    bbr->pacing_rate = mymax(bbr->pacing_rate, min_rate);
    bbr->pacing_rate = mymin(bbr->pacing_rate, bbr->BtlBw * bbr->pacing_gain);
   }


void BBRUpdateCwnd(BBR *bbr, uint64_t in_flight, uint64_t pck_diver) {
    GetTargetCongestionWindow(bbr, bbr->cwnd_gain);
    if (bbr->conservation) {
        bbr->cwnd = mymax(bbr->cwnd, in_flight + pck_diver);
    } else {
        if (bbr->is_at_full_bandwidth_) {
            bbr->cwnd = mymin(bbr->cwnd + pck_diver, bbr->target_cwnd);
        } else if (bbr->cwnd < bbr->target_cwnd || bbr->delivered < _InitialCwnd_) {
            bbr->cwnd = bbr->cwnd + pck_diver;
        }
        bbr->cwnd = mymax(bbr->cwnd, BBRMinPipeCwnd);
    }
    
    if (bbr->mode == PROBE_RTT) {
        bbr->cwnd = mymin(bbr->cwnd, BBRMinPipeCwnd);
    }
}

bool BBRIsNextCyclePhase(BBR *bbr, uint64_t pri_inflight) {
    bool is_full_length = (ngx_current_msec - bbr->last_cycle_start) > bbr->RTprop;
    if (bbr->pacing_gain == 1) {
        return is_full_length;
    }
    if (bbr->pacing_gain > 1) {
        return is_full_length && pri_inflight >= GetTargetCongestionWindow(bbr, bbr->pacing_gain);
    } else {
        return is_full_length || pri_inflight <= GetTargetCongestionWindow(bbr, 1.0);
    }
}

void BBRCheckCyclePhase(BBR *bbr, uint64_t pri_inflight) {
    if (bbr->mode == PROBE_BW && BBRIsNextCyclePhase(bbr, pri_inflight)) {
        BBRAdvanceCyclePhase(bbr);
        //printf("%ld %ld %f %ld\n", bbr->round_count, ngx_current_msec, bbr->pacing_gain, bbr->BtlBw);
    }
}

void BBRSaveCwnd(BBR *bbr) {
    if (!bbr->conservation && bbr->mode != PROBE_RTT) {
        bbr->prior_cwnd = bbr->cwnd;
    } else {
        bbr->prior_cwnd = mymax(bbr->prior_cwnd, bbr->cwnd);
    }
}

void BBRRestoreCwnd(BBR *bbr) {
    bbr->cwnd = mymax(bbr->cwnd, bbr->prior_cwnd);
}

void BBRHandleProbeRTT(BBR *bbr, uint64_t pck_inflight) {
    if (bbr->probe_rtt_done_stamp == 0 && pck_inflight <= BBRMinPipeCwnd) {
        bbr->probe_rtt_done_stamp = ngx_current_msec + ProbeRTTDuration + bbr->RTprop;
    } else if (bbr->probe_rtt_done_stamp != 0) {
        if (ngx_current_msec > bbr->probe_rtt_done_stamp) {
            bbr->rtprop_stamp = ngx_current_msec;
            BBRRestoreCwnd(bbr);
            //printf("probRTT: %ld %ld %ld\n", ngx_current_msec - bbr->probe_rtt_done_stamp, bbr->RTprop, bbr->cwnd);
            if (bbr->is_at_full_bandwidth_) {
                BBREnterProbeBW(bbr);
            } else {
                BBREnterStartup(bbr);
            }
        }
    }
}

void BBRCheckProbeRTT(BBR *bbr, uint64_t pck_inflight) {
    if (bbr->mode != PROBE_RTT && bbr->rtprop_expired) {
        BBRSaveCwnd(bbr);
        BBREnterProbeRTT(bbr);
        bbr->probe_rtt_done_stamp = 0;
    }
    if (bbr->mode == PROBE_RTT) {
        BBRHandleProbeRTT(bbr, pck_inflight);
    }
}

