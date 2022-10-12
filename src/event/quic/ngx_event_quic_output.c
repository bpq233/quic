
/*
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_event_quic_connection.h>


#define NGX_QUIC_MAX_UDP_PAYLOAD_OUT   1252
#define NGX_QUIC_MAX_UDP_PAYLOAD_OUT6  1232

#define NGX_QUIC_MAX_UDP_SEGMENT_BUF  65487 /* 65K - IPv6 header */
#define NGX_QUIC_MAX_SEGMENTS            64 /* UDP_MAX_SEGMENTS */

#define NGX_QUIC_RETRY_TOKEN_LIFETIME     3 /* seconds */
#define NGX_QUIC_NEW_TOKEN_LIFETIME     600 /* seconds */
#define NGX_QUIC_RETRY_BUFFER_SIZE      256
    /* 1 flags + 4 version + 3 x (1 + 20) s/o/dcid + itag + token(64) */

/*
 * RFC 9000, 10.3.  Stateless Reset
 *
 * Endpoints MUST discard packets that are too small to be valid QUIC
 * packets.  With the set of AEAD functions defined in [QUIC-TLS],
 * short header packets that are smaller than 21 bytes are never valid.
 */
#define NGX_QUIC_MIN_PKT_LEN             21

#define NGX_QUIC_MIN_SR_PACKET           43 /* 5 rand + 16 srt + 22 padding */
#define NGX_QUIC_MAX_SR_PACKET         1200

#define NGX_QUIC_CC_MIN_INTERVAL       1000 /* 1s */

#define NGX_QUIC_SOCKET_RETRY_DELAY      10 /* ms, for NGX_AGAIN on write */

bool ngx_sample_check_app_limited(ngx_connection_t *c, uint32_t len);
void ngx_sample_on_sent(ngx_quic_frame_t *f, ngx_connection_t *c);
void ngx_bbr_init(ngx_bbr_t *bbr, ngx_sample_t *sampler);
void ngx_bbr_restart_from_idle(ngx_bbr_t *bbr, uint64_t conn_delivered);

static ngx_int_t ngx_quic_create_datagrams(ngx_connection_t *c);
static void ngx_quic_commit_send(ngx_connection_t *c, ngx_quic_send_ctx_t *ctx);
static void ngx_quic_revert_send(ngx_connection_t *c, ngx_quic_send_ctx_t *ctx,
    uint64_t pnum);
#if ((NGX_HAVE_UDP_SEGMENT) && (NGX_HAVE_MSGHDR_MSG_CONTROL))
static ngx_uint_t ngx_quic_allow_segmentation(ngx_connection_t *c);
static ngx_int_t ngx_quic_create_segments(ngx_connection_t *c);
static ssize_t ngx_quic_send_segments(ngx_connection_t *c, u_char *buf,
    size_t len, struct sockaddr *sockaddr, socklen_t socklen, size_t segment);
#endif
static ssize_t ngx_quic_output_packet(ngx_connection_t *c,
    ngx_quic_send_ctx_t *ctx, u_char *data, size_t max, size_t min);
static void ngx_quic_init_packet(ngx_connection_t *c, ngx_quic_send_ctx_t *ctx,
    ngx_quic_header_t *pkt, ngx_quic_path_t *path);
static ngx_uint_t ngx_quic_get_padding_level(ngx_connection_t *c);
static ssize_t ngx_quic_send(ngx_connection_t *c, u_char *buf, size_t len,
    struct sockaddr *sockaddr, socklen_t socklen);
static void ngx_quic_set_packet_number(ngx_quic_header_t *pkt,
    ngx_quic_send_ctx_t *ctx);
static size_t ngx_quic_path_limit(ngx_connection_t *c, ngx_quic_path_t *path,
    size_t size);


size_t
ngx_quic_max_udp_payload(ngx_connection_t *c)
{
    /* TODO: path MTU discovery */

#if (NGX_HAVE_INET6)
    if (c->sockaddr->sa_family == AF_INET6) {
        return NGX_QUIC_MAX_UDP_PAYLOAD_OUT6;
    }
#endif

    return NGX_QUIC_MAX_UDP_PAYLOAD_OUT;
}


ngx_int_t
ngx_quic_output(ngx_connection_t *c)
{
    size_t                  in_flight;
    ngx_int_t               rc;
    ngx_quic_congestion_t  *cg;
    ngx_quic_connection_t  *qc;

    c->log->action = "sending frames";

    qc = ngx_quic_get_connection(c);
    cg = &qc->congestion;

    in_flight = cg->in_flight;

#if ((NGX_HAVE_UDP_SEGMENT) && (NGX_HAVE_MSGHDR_MSG_CONTROL))
    if (ngx_quic_allow_segmentation(c) && 0) {
        rc = ngx_quic_create_segments(c);
    } else
#endif
    {
        rc = ngx_quic_create_datagrams(c);
    }

    if (rc != NGX_OK) {
        return NGX_ERROR;
    }

    if (in_flight == cg->in_flight || qc->closing) {
        /* no ack-eliciting data was sent or we are done */
        return NGX_OK;
    }

    if (!qc->send_timer_set) {
        qc->send_timer_set = 1;
        ngx_add_timer(c->read, qc->tp.max_idle_timeout);
    }

    ngx_quic_set_lost_timer(c);

    return NGX_OK;
}


static ngx_int_t
ngx_quic_create_datagrams(ngx_connection_t *c)
{
    size_t                  len, min;
    ssize_t                 n;
    u_char                 *p;
    uint64_t                preserved_pnum[NGX_QUIC_SEND_CTX_LAST];
    ngx_uint_t              i, pad;
    ngx_quic_path_t        *path;
    ngx_quic_send_ctx_t    *ctx;
    ngx_quic_congestion_t  *cg;
    ngx_quic_connection_t  *qc;
    static u_char           dst[NGX_QUIC_MAX_UDP_PAYLOAD_SIZE];

    qc = ngx_quic_get_connection(c);
    cg = &qc->congestion;
    path = qc->path;

    // size_t length = 0, max_len = cg->bbrs.pacing_rate /1000 * 5 - cg->bbr.len;
    // if (USE_BBR) {
    //     // if (cg->bbr.mode == PROBE_RTT) {
    //     //     if (cg->bbr.probe_rtt_done_stamp != 0 && ngx_current_msec > cg->bbr.probe_rtt_done_stamp + cg->bbr.RTprop) {
    //     //         cg->bbr.rtprop_stamp = ngx_current_msec;
    //     //         BBRRestoreCwnd(&cg->bbr);
    //     //         cg->window = cg->bbr.cwnd;
    //     //         if (cg->bbr.is_at_full_bandwidth_) {
    //     //             BBREnterProbeBW(&cg->bbr);
    //     //         } else {
    //     //             BBREnterStartup(&cg->bbr);
    //     //         }
    //     //     }
    //     // }

    //     if (ngx_current_msec >= cg->timer) {
    //         cg->timer = ngx_current_msec + 5;
    //         ngx_add_timer(&qc->push, 5);
    //     } else {
    //         return NGX_OK;
    //     }   
    // }
    if (ngx_current_msec > cg->timer) {
        //extern int size;
        //printf("time: %lds 1s内: resend: %ld send: %ld ------%.2f 总: resend: %ld send: %ld ------%.2f BtlBW %ld\n", (ngx_current_msec - cg->bbr.start_time) / 1000, cg->bbr.resend_s, cg->bbr.send_s, cg->bbr.resend_s * 100.0 / (cg->bbr.send_s - cg->bbr.resend_s), cg->bbr.resend, cg->bbr.sum, cg->bbr.resend * 100.0 / (cg->bbr.sum - cg->bbr.resend), cg->bbr.BtlBw);
        printf("%ld,%ld,%ld,%.2f,%ld,%ld,%.2f,%d,%ld,%ld,%ld\n",ngx_current_msec - cg->start_time, cg->resend_s, cg->send_s, cg->resend_s * 100.0 / ngx_max(cg->send_s,1), cg->resend, cg->send, cg->resend * 100.0 / ngx_max(cg->send,1), cg->bbr.bw, cg->bbr.min_rtt, cg->window, cg->in_flight);
        if (ngx_current_msec - cg->start_time > 633000) {
            // extern int cnt1[100];
            // int sumc = 0, cc = 0;
            // for (int i = 0; i < 100; i++) {
            //     sumc += cnt1[i];
            // }
            // for (int i = 0; i < 100; i++) {
            //     cc += cnt1[i];
            //     printf("%.2f,", cc * 1.0 / sumc);
            // }
            // printf("\n");

            int sum = 0;
            for (int i = 0; i <= 100; i++) {
                sum += cg->bbr.loss[i];
                printf("%.2f,", sum * 1.0 / cg->send);
            }
            printf("\n");

            for (int i = 0; i <= 1050; i++) {
                printf("%d,", cg->bbr.rtt[i]);
            }
            printf("\n");
        }

        if (cg->send_s < 2000) {
            loss_rtt = 0;
            rank = 100;
        }

        cg->timer = ngx_current_msec + 1000;
        cg->send_s = 0;
        cg->resend_s = 0;
        fflush(stdout);
    }

    if (cg->in_flight == 0 && cg->sampler.is_app_limited) {
        ngx_bbr_restart_from_idle(&cg->bbr, cg->delivered);
    }

    ngx_pacing_on_timeout(&cg->pacing, c);

    
    while (cg->in_flight < cg->window || ngx_current_msec - cg->cwnd_limit_timer > 1000) {
        cg->cwnd_limit_timer = ngx_current_msec;
        if (USE_BBR && !ngx_pacing_can_write(&cg->pacing, 1252, c)) {
            break;
        }
        p = dst;

        len = ngx_min(qc->ctp.max_udp_payload_size,
                      NGX_QUIC_MAX_UDP_PAYLOAD_SIZE);

        len = ngx_quic_path_limit(c, path, len);

        pad = ngx_quic_get_padding_level(c);

        for (i = 0; i < NGX_QUIC_SEND_CTX_LAST; i++) {

            ctx = &qc->send_ctx[i];

            preserved_pnum[i] = ctx->pnum;

            if (ngx_quic_generate_ack(c, ctx) != NGX_OK) {
                return NGX_ERROR;
            }

            min = (i == pad && p - dst < NGX_QUIC_MIN_INITIAL_SIZE)
                  ? NGX_QUIC_MIN_INITIAL_SIZE - (p - dst) : 0;

            if (min > len) {
                /* padding can't be applied - avoid sending the packet */

                while (i-- > 0) {
                    ctx = &qc->send_ctx[i];
                    ngx_quic_revert_send(c, ctx, preserved_pnum[i]);
                }

                return NGX_OK;
            }

            n = ngx_quic_output_packet(c, ctx, p, len, min);
            if (n == NGX_ERROR) {
                return NGX_ERROR;
            }

            p += n;
            len -= n;
        }

        len = p - dst;
        if (ngx_sample_check_app_limited(c, len)) {
            ngx_pacing_on_app_limit(&cg->pacing);
        }
        if (len == 0) {
            break;
        }

        n = ngx_quic_send(c, dst, len, path->sockaddr, path->socklen);

        if (n == NGX_ERROR) {
            return NGX_ERROR;
        }

        if (n == NGX_AGAIN) {
            for (i = 0; i < NGX_QUIC_SEND_CTX_LAST; i++) {
                ngx_quic_revert_send(c, &qc->send_ctx[i], preserved_pnum[i]);
            }

            ngx_add_timer(&qc->push, NGX_QUIC_SOCKET_RETRY_DELAY);
            break;
        }

        for (i = 0; i < NGX_QUIC_SEND_CTX_LAST; i++) {
            ngx_quic_commit_send(c, &qc->send_ctx[i]);
        }

        path->sent += len;
        ngx_pacing_on_packet_sent(&cg->pacing, len, c);

        // if (USE_BBR) {
        //     length += len;
        //     if (length >= max_len) { 
        //     //printf("send:  %ld %ld\n", ngx_current_msec, length);
        //         cg->bbr.len = length - max_len;
        //         return NGX_OK; 
        //     }
        // }
    }

    if (!qc->push.timer_set) {
        ngx_add_timer(&qc->push, 5);
    }

    return NGX_OK;
}


static void
ngx_quic_commit_send(ngx_connection_t *c, ngx_quic_send_ctx_t *ctx)
{
    ngx_queue_t            *q;
    ngx_quic_frame_t       *f;
    ngx_quic_congestion_t  *cg;
    ngx_quic_connection_t  *qc;

    qc = ngx_quic_get_connection(c);

    cg = &qc->congestion;

    while (!ngx_queue_empty(&ctx->sending)) {

        q = ngx_queue_head(&ctx->sending);
        f = ngx_queue_data(q, ngx_quic_frame_t, queue);

        ngx_queue_remove(q);

        if (f->pkt_need_ack && !qc->closing) {
            // f->first_sendtime = cg->bbr.first_sendtime;
            // f->sendtime = ngx_current_msec;
            // f->delivered = cg->bbr.delivered;
            // if (cg->in_flight * 2 < cg->window) {
            //     f->is_app_limit = true;
            // }

            ngx_sample_on_sent(f, c);
            f->sendtime = ngx_current_msec;

            ngx_queue_insert_tail(&ctx->sent, q);

            cg->in_flight += f->plen;

            // cg->bbr.send_rtt += f->plen;
            cg->send += f->plen;
            // cg->send_s += f->plen;
            // if (cg->send_s != 0) {
            //     cg->bbr.loss[(int)(cg->resend_s * 100.0 / cg->send_s)] += f->plen;
            // }

            if (f->is_resend) {
                cg->resend += f->plen;
            //     cg->resend_s += f->plen;
            //     cg->bbr.resend_rtt += f->plen;
            } 

            // extern float loss_rtt;
            // extern int rank;
            // loss_rtt = cg->resend_s * 1.0 / cg->send_s;
            // rank = Loss_Rank(&cg->bbr.loss_filter, cg->bbr.resend_rtt);
            // if (cg->bbr.resend_rtt < cg->bbr.send_rtt) {
            //     if (cg->bbr.send_rtt > 0) {
            //     loss_rtt = cg->bbr.resend_rtt * 1.0 / cg->bbr.send_rtt;
            //     } else {
            //         loss_rtt = 0;
            //     }
            //     rank = Loss_Rank(&cg->bbr.loss_filter, loss_rtt);
            // }
            
            
            // if (f->len > 0) {
            //     printf("time: %ld is_resend: %ld resend: %ld send: %ld ------%.2f len: %ld cwnd: %ld BtlBW %ld\n", ngx_current_msec - cg->bbr.start_time, f->is_resend, cg->bbr.resend, cg->bbr.sum, cg->bbr.resend * 100.0 / (cg->bbr.sum - cg->bbr.resend), f->plen, cg->window, cg->bbr.BtlBw);
            // }
            

            // if (ngx_current_msec > cg->bbr.timer) {
            //     // for (int i = 0; i < 10; i++) {
            //     //     printf("%ld ", cg->bbr.queue[i]);
            //     // }
            //     // printf("\n");

            //     printf("resend: %ld send: %ld ------%.2f cwnd: %ld inflight: %ld BtlBW: %ld RTprop %ld round: %ld\n", cg->bbr.resend, cg->bbr.sum, cg->bbr.resend * 100.0 / (cg->bbr.sum - cg->bbr.resend), cg->window, cg->window, cg->bbr.BtlBw, cg->bbr.RTprop, cg->bbr.round_count);
            //     //cg->bbr.timer = ngx_current_msec + 1000;
            // }
            

            //if (f->plen > 0)
            //printf("sendtime :  %ld  packet_len : %ld  cwnd : %ld  inflight : %ld\n", ngx_current_msec, f->plen, cg->window, cg->in_flight);

        } else {
            ngx_quic_free_frame(c, f);
        }
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic congestion send if:%uz", cg->in_flight);
}


static void
ngx_quic_revert_send(ngx_connection_t *c, ngx_quic_send_ctx_t *ctx,
    uint64_t pnum)
{
    ngx_queue_t  *q;

    while (!ngx_queue_empty(&ctx->sending)) {

        q = ngx_queue_last(&ctx->sending);
        ngx_queue_remove(q);
        ngx_queue_insert_head(&ctx->frames, q);
    }

    ctx->pnum = pnum;
}


#if ((NGX_HAVE_UDP_SEGMENT) && (NGX_HAVE_MSGHDR_MSG_CONTROL))

static ngx_uint_t
ngx_quic_allow_segmentation(ngx_connection_t *c)
{
    size_t                  bytes, len;
    ngx_queue_t            *q;
    ngx_quic_frame_t       *f;
    ngx_quic_send_ctx_t    *ctx;
    ngx_quic_connection_t  *qc;

    qc = ngx_quic_get_connection(c);

    if (!qc->conf->gso_enabled) {
        return 0;
    }

    if (qc->path->limited) {
        /* don't even try to be faster on non-validated paths */
        return 0;
    }

    ctx = ngx_quic_get_send_ctx(qc, ssl_encryption_initial);
    if (!ngx_queue_empty(&ctx->frames)) {
        return 0;
    }

    ctx = ngx_quic_get_send_ctx(qc, ssl_encryption_handshake);
    if (!ngx_queue_empty(&ctx->frames)) {
        return 0;
    }

    ctx = ngx_quic_get_send_ctx(qc, ssl_encryption_application);

    bytes = 0;

    len = ngx_min(qc->ctp.max_udp_payload_size,
                  NGX_QUIC_MAX_UDP_SEGMENT_BUF);

    for (q = ngx_queue_head(&ctx->frames);
         q != ngx_queue_sentinel(&ctx->frames);
         q = ngx_queue_next(q))
    {
        f = ngx_queue_data(q, ngx_quic_frame_t, queue);

        bytes += f->len;

        if (bytes > len * 3) {
            /* require at least ~3 full packets to batch */
            return 1;
        }
    }

    return 0;
}


static ngx_int_t
ngx_quic_create_segments(ngx_connection_t *c)
{
    size_t                  len, segsize;
    ssize_t                 n;
    u_char                 *p, *end;
    uint64_t                preserved_pnum;
    ngx_uint_t              nseg;
    ngx_quic_path_t        *path;
    ngx_quic_send_ctx_t    *ctx;
    ngx_quic_congestion_t  *cg;
    ngx_quic_connection_t  *qc;
    static u_char           dst[NGX_QUIC_MAX_UDP_SEGMENT_BUF];

    qc = ngx_quic_get_connection(c);
    cg = &qc->congestion;
    path = qc->path;

    ctx = ngx_quic_get_send_ctx(qc, ssl_encryption_application);

    if (ngx_quic_generate_ack(c, ctx) != NGX_OK) {
        return NGX_ERROR;
    }

    segsize = ngx_min(qc->ctp.max_udp_payload_size,
                      NGX_QUIC_MAX_UDP_SEGMENT_BUF);
    p = dst;
    end = dst + sizeof(dst);

    nseg = 0;

    preserved_pnum = ctx->pnum;

    for ( ;; ) {

        len = ngx_min(segsize, (size_t) (end - p));

        if (len && cg->in_flight < cg->window) {

            n = ngx_quic_output_packet(c, ctx, p, len, len);
            if (n == NGX_ERROR) {
                return NGX_ERROR;
            }

            if (n) {
                p += n;
                nseg++;
            }

        } else {
            n = 0;
        }

        if (p == dst) {
            break;
        }

        if (n == 0 || nseg == NGX_QUIC_MAX_SEGMENTS) {
            n = ngx_quic_send_segments(c, dst, p - dst, path->sockaddr,
                                       path->socklen, segsize);
            if (n == NGX_ERROR) {
                return NGX_ERROR;
            }

            if (n == NGX_AGAIN) {
                ngx_quic_revert_send(c, ctx, preserved_pnum);

                ngx_add_timer(&qc->push, NGX_QUIC_SOCKET_RETRY_DELAY);
                break;
            }

            ngx_quic_commit_send(c, ctx);

            path->sent += n;

            p = dst;
            nseg = 0;
            preserved_pnum = ctx->pnum;
        }
    }

    return NGX_OK;
}


static ssize_t
ngx_quic_send_segments(ngx_connection_t *c, u_char *buf, size_t len,
    struct sockaddr *sockaddr, socklen_t socklen, size_t segment)
{
    size_t           clen;
    ssize_t          n;
    uint16_t        *valp;
    struct iovec     iov;
    struct msghdr    msg;
    struct cmsghdr  *cmsg;

#if (NGX_HAVE_ADDRINFO_CMSG)
    char             msg_control[CMSG_SPACE(sizeof(uint16_t))
                             + CMSG_SPACE(sizeof(ngx_addrinfo_t))];
#else
    char             msg_control[CMSG_SPACE(sizeof(uint16_t))];
#endif

    ngx_memzero(&msg, sizeof(struct msghdr));
    ngx_memzero(msg_control, sizeof(msg_control));

    iov.iov_len = len;
    iov.iov_base = buf;

    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    msg.msg_name = sockaddr;
    msg.msg_namelen = socklen;

    msg.msg_control = msg_control;
    msg.msg_controllen = sizeof(msg_control);

    cmsg = CMSG_FIRSTHDR(&msg);

    cmsg->cmsg_level = SOL_UDP;
    cmsg->cmsg_type = UDP_SEGMENT;
    cmsg->cmsg_len = CMSG_LEN(sizeof(uint16_t));

    clen = CMSG_SPACE(sizeof(uint16_t));

    valp = (void *) CMSG_DATA(cmsg);
    *valp = segment;

#if (NGX_HAVE_ADDRINFO_CMSG)
    if (c->listening && c->listening->wildcard && c->local_sockaddr) {
        cmsg = CMSG_NXTHDR(&msg, cmsg);
        clen += ngx_set_srcaddr_cmsg(cmsg, c->local_sockaddr);
    }
#endif

    msg.msg_controllen = clen;

    n = ngx_sendmsg(c, &msg, 0);
    if (n < 0) {
        return n;
    }

    c->sent += n;

    return n;
}

#endif



static ngx_uint_t
ngx_quic_get_padding_level(ngx_connection_t *c)
{
    ngx_uint_t              i;
    ngx_queue_t            *q;
    ngx_quic_frame_t       *f;
    ngx_quic_send_ctx_t    *ctx;
    ngx_quic_connection_t  *qc;

    /*
     * RFC 9000, 14.1.  Initial Datagram Size
     *
     * Similarly, a server MUST expand the payload of all UDP datagrams
     * carrying ack-eliciting Initial packets to at least the smallest
     * allowed maximum datagram size of 1200 bytes.
     */

    qc = ngx_quic_get_connection(c);
    ctx = ngx_quic_get_send_ctx(qc, ssl_encryption_initial);

    for (q = ngx_queue_head(&ctx->frames);
         q != ngx_queue_sentinel(&ctx->frames);
         q = ngx_queue_next(q))
    {
        f = ngx_queue_data(q, ngx_quic_frame_t, queue);

        if (f->need_ack) {
            for (i = 0; i + 1 < NGX_QUIC_SEND_CTX_LAST; i++) {
                ctx = &qc->send_ctx[i + 1];

                if (ngx_queue_empty(&ctx->frames)) {
                    break;
                }
            }

            return i;
        }
    }

    return NGX_QUIC_SEND_CTX_LAST;
}


static ssize_t
ngx_quic_output_packet(ngx_connection_t *c, ngx_quic_send_ctx_t *ctx,
    u_char *data, size_t max, size_t min)
{
    size_t                  len, pad, min_payload, max_payload;
    u_char                 *p;
    ssize_t                 flen;
    ngx_str_t               res;
    ngx_int_t               rc;
    ngx_uint_t              nframes, expand;
    ngx_msec_t              now;
    ngx_queue_t            *q;
    ngx_quic_frame_t       *f;
    ngx_quic_header_t       pkt;
    ngx_quic_connection_t  *qc;
    static u_char           src[NGX_QUIC_MAX_UDP_PAYLOAD_SIZE];

    if (ngx_queue_empty(&ctx->frames)) {
        return 0;
    }

    ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic output %s packet max:%uz min:%uz",
                   ngx_quic_level_name(ctx->level), max, min);

    qc = ngx_quic_get_connection(c);

    ngx_quic_init_packet(c, ctx, &pkt, qc->path);

    min_payload = ngx_quic_payload_size(&pkt, min);
    max_payload = ngx_quic_payload_size(&pkt, max);

    /* RFC 9001, 5.4.2.  Header Protection Sample */
    pad = 4 - pkt.num_len;
    min_payload = ngx_max(min_payload, pad);

    if (min_payload > max_payload) {
        return 0;
    }

    now = ngx_current_msec;
    nframes = 0;
    p = src;
    len = 0;
    expand = 0;

    for (q = ngx_queue_head(&ctx->frames);
         q != ngx_queue_sentinel(&ctx->frames);
         q = ngx_queue_next(q))
    {
        f = ngx_queue_data(q, ngx_quic_frame_t, queue);

        if (!expand && (f->type == NGX_QUIC_FT_PATH_RESPONSE
                        || f->type == NGX_QUIC_FT_PATH_CHALLENGE))
        {
            /*
             * RFC 9000, 8.2.1.  Initiating Path Validation
             *
             * An endpoint MUST expand datagrams that contain a
             * PATH_CHALLENGE frame to at least the smallest allowed
             * maximum datagram size of 1200 bytes...
             *
             * (same applies to PATH_RESPONSE frames)
             */

            if (max < 1200) {
                /* expanded packet will not fit */
                break;
            }

            if (min < 1200) {
                min = 1200;

                min_payload = ngx_quic_payload_size(&pkt, min);
            }

            expand = 1;
        }

        if (len >= max_payload) {
            break;
        }

        if (len + f->len > max_payload) {
            rc = ngx_quic_split_frame(c, f, max_payload - len);

            if (rc == NGX_ERROR) {
                return NGX_ERROR;
            }

            if (rc == NGX_DECLINED) {
                break;
            }
        }

        if (f->need_ack) {
            pkt.need_ack = 1;
        }

        ngx_quic_log_frame(c->log, f, 1);

        flen = ngx_quic_create_frame(p, f);
        if (flen == -1) {
            return NGX_ERROR;
        }

        len += flen;
        p += flen;

        f->pnum = ctx->pnum;
        f->first = now;
        f->last = now;
        f->plen = 0;

        nframes++;

        if (f->flush) {
            break;
        }
    }

    if (nframes == 0) {
        return 0;
    }
    

    if (len < min_payload) {
        ngx_memset(p, NGX_QUIC_FT_PADDING, min_payload - len);
        len = min_payload;
    }
    pkt.payload.data = src;
    pkt.payload.len = len;

    res.data = data;

    ngx_log_debug6(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic packet tx %s bytes:%ui"
                   " need_ack:%d number:%L encoded nl:%d trunc:0x%xD",
                   ngx_quic_level_name(ctx->level), pkt.payload.len,
                   pkt.need_ack, pkt.number, pkt.num_len, pkt.trunc);

    if (ngx_quic_encrypt(&pkt, &res) != NGX_OK) {
        return NGX_ERROR;
    }

    ctx->pnum++;

    if (pkt.need_ack) {
        q = ngx_queue_head(&ctx->frames);
        f = ngx_queue_data(q, ngx_quic_frame_t, queue);

        f->plen = res.len;
    }

    while (nframes--) {
        q = ngx_queue_head(&ctx->frames);
        f = ngx_queue_data(q, ngx_quic_frame_t, queue);

        f->pkt_need_ack = pkt.need_ack;

        ngx_queue_remove(q);
        ngx_queue_insert_tail(&ctx->sending, q);
    }

    return res.len;
}


static void
ngx_quic_init_packet(ngx_connection_t *c, ngx_quic_send_ctx_t *ctx,
    ngx_quic_header_t *pkt, ngx_quic_path_t *path)
{
    ngx_quic_connection_t  *qc;

    qc = ngx_quic_get_connection(c);

    ngx_memzero(pkt, sizeof(ngx_quic_header_t));

    pkt->flags = NGX_QUIC_PKT_FIXED_BIT;

    if (ctx->level == ssl_encryption_initial) {
        pkt->flags |= NGX_QUIC_PKT_LONG | NGX_QUIC_PKT_INITIAL;

    } else if (ctx->level == ssl_encryption_handshake) {
        pkt->flags |= NGX_QUIC_PKT_LONG | NGX_QUIC_PKT_HANDSHAKE;

    } else {
        if (qc->key_phase) {
            pkt->flags |= NGX_QUIC_PKT_KPHASE;
        }
    }

    pkt->dcid.data = path->cid->id;
    pkt->dcid.len = path->cid->len;

    pkt->scid = qc->tp.initial_scid;

    pkt->version = qc->version;
    pkt->log = c->log;
    pkt->level = ctx->level;

    pkt->keys = qc->keys;

    ngx_quic_set_packet_number(pkt, ctx);
}


static ssize_t
ngx_quic_send(ngx_connection_t *c, u_char *buf, size_t len,
    struct sockaddr *sockaddr, socklen_t socklen)
{
    ssize_t          n;
    struct iovec     iov;
    struct msghdr    msg;
#if (NGX_HAVE_ADDRINFO_CMSG)
    struct cmsghdr  *cmsg;
    char             msg_control[CMSG_SPACE(sizeof(ngx_addrinfo_t))];
#endif

    ngx_memzero(&msg, sizeof(struct msghdr));

    iov.iov_len = len;
    iov.iov_base = buf;

    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    msg.msg_name = sockaddr;
    msg.msg_namelen = socklen;

#if (NGX_HAVE_ADDRINFO_CMSG)
    if (c->listening && c->listening->wildcard && c->local_sockaddr) {

        msg.msg_control = msg_control;
        msg.msg_controllen = sizeof(msg_control);
        ngx_memzero(msg_control, sizeof(msg_control));

        cmsg = CMSG_FIRSTHDR(&msg);

        msg.msg_controllen = ngx_set_srcaddr_cmsg(cmsg, c->local_sockaddr);
    }
#endif

    n = ngx_sendmsg(c, &msg, 0);
    if (n < 0) {
        return n;
    }

    c->sent += n;

    return n;
}


static void
ngx_quic_set_packet_number(ngx_quic_header_t *pkt, ngx_quic_send_ctx_t *ctx)
{
    uint64_t  delta;

    delta = ctx->pnum - ctx->largest_ack;
    pkt->number = ctx->pnum;

    if (delta <= 0x7F) {
        pkt->num_len = 1;
        pkt->trunc = ctx->pnum & 0xff;

    } else if (delta <= 0x7FFF) {
        pkt->num_len = 2;
        pkt->flags |= 0x1;
        pkt->trunc = ctx->pnum & 0xffff;

    } else if (delta <= 0x7FFFFF) {
        pkt->num_len = 3;
        pkt->flags |= 0x2;
        pkt->trunc = ctx->pnum & 0xffffff;

    } else {
        pkt->num_len = 4;
        pkt->flags |= 0x3;
        pkt->trunc = ctx->pnum & 0xffffffff;
    }
}


ngx_int_t
ngx_quic_negotiate_version(ngx_connection_t *c, ngx_quic_header_t *inpkt)
{
    size_t             len;
    ngx_quic_header_t  pkt;
    static u_char      buf[NGX_QUIC_MAX_UDP_PAYLOAD_SIZE];

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "sending version negotiation packet");

    pkt.log = c->log;
    pkt.flags = NGX_QUIC_PKT_LONG | NGX_QUIC_PKT_FIXED_BIT;
    pkt.dcid = inpkt->scid;
    pkt.scid = inpkt->dcid;

    len = ngx_quic_create_version_negotiation(&pkt, buf);

#ifdef NGX_QUIC_DEBUG_PACKETS
    ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic vnego packet to send len:%uz %*xs", len, len, buf);
#endif

    (void) ngx_quic_send(c, buf, len, c->sockaddr, c->socklen);

    return NGX_DONE;
}


ngx_int_t
ngx_quic_send_stateless_reset(ngx_connection_t *c, ngx_quic_conf_t *conf,
    ngx_quic_header_t *pkt)
{
    u_char    *token;
    size_t     len, max;
    uint16_t   rndbytes;
    u_char     buf[NGX_QUIC_MAX_SR_PACKET];

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic handle stateless reset output");

    if (pkt->len <= NGX_QUIC_MIN_PKT_LEN) {
        return NGX_DECLINED;
    }

    if (pkt->len <= NGX_QUIC_MIN_SR_PACKET) {
        len = pkt->len - 1;

    } else {
        max = ngx_min(NGX_QUIC_MAX_SR_PACKET, pkt->len * 3);

        if (RAND_bytes((u_char *) &rndbytes, sizeof(rndbytes)) != 1) {
            return NGX_ERROR;
        }

        len = (rndbytes % (max - NGX_QUIC_MIN_SR_PACKET + 1))
              + NGX_QUIC_MIN_SR_PACKET;
    }

    if (RAND_bytes(buf, len - NGX_QUIC_SR_TOKEN_LEN) != 1) {
        return NGX_ERROR;
    }

    buf[0] &= ~NGX_QUIC_PKT_LONG;
    buf[0] |= NGX_QUIC_PKT_FIXED_BIT;

    token = &buf[len - NGX_QUIC_SR_TOKEN_LEN];

    if (ngx_quic_new_sr_token(c, &pkt->dcid, conf->sr_token_key, token)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    (void) ngx_quic_send(c, buf, len, c->sockaddr, c->socklen);

    return NGX_DECLINED;
}


ngx_int_t
ngx_quic_send_cc(ngx_connection_t *c)
{
    ngx_quic_frame_t       *frame;
    ngx_quic_connection_t  *qc;

    qc = ngx_quic_get_connection(c);

    if (qc->draining) {
        return NGX_OK;
    }

    if (qc->closing
        && ngx_current_msec - qc->last_cc < NGX_QUIC_CC_MIN_INTERVAL)
    {
        /* dot not send CC too often */
        return NGX_OK;
    }

    frame = ngx_quic_alloc_frame(c);
    if (frame == NULL) {
        return NGX_ERROR;
    }

    frame->level = qc->error_level;
    frame->type = qc->error_app ? NGX_QUIC_FT_CONNECTION_CLOSE_APP
                                : NGX_QUIC_FT_CONNECTION_CLOSE;
    frame->u.close.error_code = qc->error;
    frame->u.close.frame_type = qc->error_ftype;

    if (qc->error_reason) {
        frame->u.close.reason.len = ngx_strlen(qc->error_reason);
        frame->u.close.reason.data = (u_char *) qc->error_reason;
    }

    ngx_quic_queue_frame(qc, frame);

    qc->last_cc = ngx_current_msec;

    return ngx_quic_output(c);
}


ngx_int_t
ngx_quic_send_early_cc(ngx_connection_t *c, ngx_quic_header_t *inpkt,
    ngx_uint_t err, const char *reason)
{
    ssize_t            len;
    ngx_str_t          res;
    ngx_quic_frame_t   frame;
    ngx_quic_header_t  pkt;

    static u_char       src[NGX_QUIC_MAX_UDP_PAYLOAD_SIZE];
    static u_char       dst[NGX_QUIC_MAX_UDP_PAYLOAD_SIZE];

    ngx_memzero(&frame, sizeof(ngx_quic_frame_t));
    ngx_memzero(&pkt, sizeof(ngx_quic_header_t));

    frame.level = inpkt->level;
    frame.type = NGX_QUIC_FT_CONNECTION_CLOSE;
    frame.u.close.error_code = err;

    frame.u.close.reason.data = (u_char *) reason;
    frame.u.close.reason.len = ngx_strlen(reason);

    len = ngx_quic_create_frame(NULL, &frame);
    if (len > NGX_QUIC_MAX_UDP_PAYLOAD_SIZE) {
        return NGX_ERROR;
    }

    ngx_quic_log_frame(c->log, &frame, 1);

    len = ngx_quic_create_frame(src, &frame);
    if (len == -1) {
        return NGX_ERROR;
    }

    pkt.keys = ngx_quic_keys_new(c->pool);
    if (pkt.keys == NULL) {
        return NGX_ERROR;
    }

    if (ngx_quic_keys_set_initial_secret(c->pool, pkt.keys, &inpkt->dcid)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    pkt.flags = NGX_QUIC_PKT_FIXED_BIT | NGX_QUIC_PKT_LONG
                | NGX_QUIC_PKT_INITIAL;

    pkt.num_len = 1;
    /*
     * pkt.num = 0;
     * pkt.trunc = 0;
     */

    pkt.version = inpkt->version;
    pkt.log = c->log;
    pkt.level = inpkt->level;
    pkt.dcid = inpkt->scid;
    pkt.scid = inpkt->dcid;
    pkt.payload.data = src;
    pkt.payload.len = len;

    res.data = dst;

    if (ngx_quic_encrypt(&pkt, &res) != NGX_OK) {
        return NGX_ERROR;
    }

    if (ngx_quic_send(c, res.data, res.len, c->sockaddr, c->socklen) < 0) {
        return NGX_ERROR;
    }

    return NGX_DONE;
}


ngx_int_t
ngx_quic_send_retry(ngx_connection_t *c, ngx_quic_conf_t *conf,
    ngx_quic_header_t *inpkt)
{
    time_t             expires;
    ssize_t            len;
    ngx_str_t          res, token;
    ngx_quic_header_t  pkt;

    u_char             buf[NGX_QUIC_RETRY_BUFFER_SIZE];
    u_char             dcid[NGX_QUIC_SERVER_CID_LEN];

    expires = ngx_time() + NGX_QUIC_RETRY_TOKEN_LIFETIME;

    if (ngx_quic_new_token(c, c->sockaddr, c->socklen, conf->av_token_key,
                           &token, &inpkt->dcid, expires, 1)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    ngx_memzero(&pkt, sizeof(ngx_quic_header_t));
    pkt.flags = NGX_QUIC_PKT_FIXED_BIT | NGX_QUIC_PKT_LONG | NGX_QUIC_PKT_RETRY;
    pkt.version = inpkt->version;
    pkt.log = c->log;

    pkt.odcid = inpkt->dcid;
    pkt.dcid = inpkt->scid;

    /* TODO: generate routable dcid */
    if (RAND_bytes(dcid, NGX_QUIC_SERVER_CID_LEN) != 1) {
        return NGX_ERROR;
    }

    pkt.scid.len = NGX_QUIC_SERVER_CID_LEN;
    pkt.scid.data = dcid;

    pkt.token = token;

    res.data = buf;

    if (ngx_quic_encrypt(&pkt, &res) != NGX_OK) {
        return NGX_ERROR;
    }

#ifdef NGX_QUIC_DEBUG_PACKETS
    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic packet to send len:%uz %xV", res.len, &res);
#endif

    len = ngx_quic_send(c, res.data, res.len, c->sockaddr, c->socklen);
    if (len < 0) {
        return NGX_ERROR;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "quic retry packet sent to %xV", &pkt.dcid);

    /*
     * RFC 9000, 17.2.5.1.  Sending a Retry Packet
     *
     * A server MUST NOT send more than one Retry
     * packet in response to a single UDP datagram.
     * NGX_DONE will stop quic_input() from processing further
     */
    return NGX_DONE;
}


ngx_int_t
ngx_quic_send_new_token(ngx_connection_t *c, ngx_quic_path_t *path)
{
    time_t                  expires;
    ngx_str_t               token;
    ngx_quic_frame_t       *frame;
    ngx_quic_connection_t  *qc;

    qc = ngx_quic_get_connection(c);

    expires = ngx_time() + NGX_QUIC_NEW_TOKEN_LIFETIME;

    if (ngx_quic_new_token(c, path->sockaddr, path->socklen,
                           qc->conf->av_token_key, &token, NULL, expires, 0)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    frame = ngx_quic_alloc_frame(c);
    if (frame == NULL) {
        return NGX_ERROR;
    }

    frame->level = ssl_encryption_application;
    frame->type = NGX_QUIC_FT_NEW_TOKEN;
    frame->u.token.length = token.len;
    frame->u.token.data = token.data;

    ngx_quic_queue_frame(qc, frame);

    return NGX_OK;
}


ngx_int_t
ngx_quic_send_ack(ngx_connection_t *c, ngx_quic_send_ctx_t *ctx)
{
    size_t                  len, left;
    uint64_t                ack_delay;
    ngx_buf_t              *b;
    ngx_uint_t              i;
    ngx_chain_t            *cl, **ll;
    ngx_quic_frame_t       *frame;
    ngx_quic_connection_t  *qc;

    qc = ngx_quic_get_connection(c);

    ack_delay = ngx_current_msec - ctx->largest_received;
    ack_delay *= 1000;
    ack_delay >>= qc->tp.ack_delay_exponent;

    frame = ngx_quic_alloc_frame(c);
    if (frame == NULL) {
        return NGX_ERROR;
    }

    ll = &frame->data;
    b = NULL;

    for (i = 0; i < ctx->nranges; i++) {
        len = ngx_quic_create_ack_range(NULL, ctx->ranges[i].gap,
                                        ctx->ranges[i].range);

        left = b ? b->end - b->last : 0;

        if (left < len) {
            cl = ngx_quic_alloc_chain(c);
            if (cl == NULL) {
                return NGX_ERROR;
            }

            *ll = cl;
            ll = &cl->next;

            b = cl->buf;
            left = b->end - b->last;

            if (left < len) {
                return NGX_ERROR;
            }
        }

        b->last += ngx_quic_create_ack_range(b->last, ctx->ranges[i].gap,
                                             ctx->ranges[i].range);

        frame->u.ack.ranges_length += len;
    }

    *ll = NULL;

    frame->level = ctx->level;
    frame->type = NGX_QUIC_FT_ACK;
    frame->u.ack.largest = ctx->largest_range;
    frame->u.ack.delay = ack_delay;
    frame->u.ack.range_count = ctx->nranges;
    frame->u.ack.first_range = ctx->first_range;

    ngx_quic_queue_frame(qc, frame);

    return NGX_OK;
}


ngx_int_t
ngx_quic_send_ack_range(ngx_connection_t *c, ngx_quic_send_ctx_t *ctx,
    uint64_t smallest, uint64_t largest)
{
    ngx_quic_frame_t       *frame;
    ngx_quic_connection_t  *qc;

    qc = ngx_quic_get_connection(c);

    frame = ngx_quic_alloc_frame(c);
    if (frame == NULL) {
        return NGX_ERROR;
    }

    frame->level = ctx->level;
    frame->type = NGX_QUIC_FT_ACK;
    frame->u.ack.largest = largest;
    frame->u.ack.delay = 0;
    frame->u.ack.range_count = 0;
    frame->u.ack.first_range = largest - smallest;

    ngx_quic_queue_frame(qc, frame);

    return NGX_OK;
}


ngx_int_t
ngx_quic_frame_sendto(ngx_connection_t *c, ngx_quic_frame_t *frame,
    size_t min, ngx_quic_path_t *path)
{
    size_t                  min_payload, pad;
    ssize_t                 len, sent;
    ngx_str_t               res;
    ngx_quic_header_t       pkt;
    ngx_quic_send_ctx_t    *ctx;
    ngx_quic_connection_t  *qc;

    static u_char           src[NGX_QUIC_MAX_UDP_PAYLOAD_SIZE];
    static u_char           dst[NGX_QUIC_MAX_UDP_PAYLOAD_SIZE];

    qc = ngx_quic_get_connection(c);
    ctx = ngx_quic_get_send_ctx(qc, ssl_encryption_application);

    ngx_quic_init_packet(c, ctx, &pkt, path);

    min = ngx_quic_path_limit(c, path, min);

    min_payload = min ? ngx_quic_payload_size(&pkt, min) : 0;

    pad = 4 - pkt.num_len;
    min_payload = ngx_max(min_payload, pad);

    len = ngx_quic_create_frame(NULL, frame);
    if (len > NGX_QUIC_MAX_UDP_PAYLOAD_SIZE) {
        return NGX_ERROR;
    }

    ngx_quic_log_frame(c->log, frame, 1);

    len = ngx_quic_create_frame(src, frame);
    if (len == -1) {
        return NGX_ERROR;
    }

    if (len < (ssize_t) min_payload) {
        ngx_memset(src + len, NGX_QUIC_FT_PADDING, min_payload - len);
        len = min_payload;
     }

    pkt.payload.data = src;
    pkt.payload.len = len;

    res.data = dst;

    if (ngx_quic_encrypt(&pkt, &res) != NGX_OK) {
        return NGX_ERROR;
    }

    ctx->pnum++;

    sent = ngx_quic_send(c, res.data, res.len, path->sockaddr, path->socklen);
    if (sent < 0) {
        return NGX_ERROR;
    }

    path->sent += sent;

    return NGX_OK;
}


static size_t
ngx_quic_path_limit(ngx_connection_t *c, ngx_quic_path_t *path, size_t size)
{
    off_t  max;

    if (path->limited) {
        max = path->received * 3;
        max = (path->sent >= max) ? 0 : max - path->sent;

        if ((off_t) size > max) {
            ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                           "quic path limit %uz - %O", size, max);
            return max;
        }
    }

    return size;
}
