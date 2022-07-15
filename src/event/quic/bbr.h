
#ifndef _BBR_H_INCLUDED_
#define _BBR_H_INCLUDED_

#include <ngx_config.h>
#include <ngx_core.h>
#include <stdbool.h>
#include <loss_filter.h>


enum Mode {
    // Startup phase of the connection.
    STARTUP,
    // After achieving the highest possible bandwidth during the startup, lower
    // the pacing rate in order to drain the queue.
    DRAIN,
    // Cruising mode.
    PROBE_BW,
    // Temporarily slow down sending in order to empty the buffer and measure
    // the real minimum RTT.
    PROBE_RTT,
};

enum RecoveryState {
    // Do not limit.
    NOT_IN_RECOVERY,
    // Allow an extra outstanding byte for each byte acknowledged.
    CONSERVATION,
    // Allow two extra outstanding bytes for each byte acknowledged (slow
    // start).
    GROWTH
};

typedef struct {

    ngx_uint_t mode;
    float pacing_gain;
    float cwnd_gain;

    uint64_t  BtlBw;
    uint64_t  cycle_index;
    uint64_t  pacing_rate;
    
    uint64_t  cnt;
    uint64_t  round_count;
    uint64_t  full_bw;
    uint64_t  full_bw_count;
    uint64_t  delivered;
    uint64_t  current_round_trip_end_;
    uint64_t  target_cwnd;
    uint64_t  cwnd;
    uint64_t  prior_cwnd;
    uint64_t  queue[10];
    uint64_t  send_rtt;
    uint64_t  resend_rtt;
    uint64_t  len;
    uint64_t  ack_byte;
    

    uint64_t  sum;
    uint64_t  send_s;
    uint64_t  resend_s;
    uint64_t  resend;
    uint64_t  is_send;

    uint64_t  over_bdp;
    uint64_t  sum_inflight;

    ngx_msec_t  timer;
    ngx_msec_t  first_sendtime;
    ngx_msec_t  start_tim;
    ngx_msec_t  last_cycle_start;  // 上一次更新cycle_index的时间   
    ngx_msec_t  RTprop;
    ngx_msec_t  rtprop_stamp;  /* timestamp of min_rtt_us */
    ngx_msec_t  probe_rtt_done_stamp;
    ngx_msec_t  start_time;

    bool round_start;
    bool is_app_limit;
    bool  rtprop_expired;
    bool is_at_full_bandwidth_;
    bool conservation;

    Loss_Filter loss_filter;
    

}BBR;

void BBRInit(BBR *bbr);

void BBRCheckFullPipe(BBR *bbr);
void BBRCheckDrain(BBR *bbr, uint64_t pck_inflight);
void BBRCheckCyclePhase(BBR *bbr, uint64_t pri_inflight);
void BBRCheckProbeRTT(BBR *bbr, uint64_t pck_inflight);
void BBRUpdateBtlBw(BBR *bbr, uint64_t p_drivered, uint64_t plen, ngx_msec_t send_time, ngx_msec_t send_interval);
void BBRUpdateRTprop(BBR *bbr, ngx_msec_t sample_min_rtt);
void BBRUpdatePacingRate(BBR *bbr);
void BBRUpdateCwnd(BBR *bbr, uint64_t in_flight, uint64_t pck_diver);
void BBRSaveCwnd(BBR *bbr);
void BBRRestoreCwnd(BBR *bbr);
void BBREnterProbeBW(BBR *bbr);
void BBREnterStartup(BBR *bbr);
uint64_t mymin(uint64_t a, uint64_t b);
uint64_t mymax(uint64_t a, uint64_t b);


int size, cnt;


#endif
