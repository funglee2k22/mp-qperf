#include "server_stream.h"

#include <ev.h>
#include <stdbool.h>
#include <quicly/streambuf.h>

typedef struct
{
    uint32_t min_rtt;
    uint32_t srtt;
    uint32_t cwnd_exiting_slow_start;
} path_stats;

typedef struct
{
    uint64_t target_offset;
    uint64_t acked_offset;
    quicly_stream_t *stream;
    int report_id;
    int report_second;
    uint64_t report_num_packets_sent;
    uint64_t report_num_packets_lost;
    uint64_t total_num_packets_sent;
    uint64_t total_num_packets_lost;
    uint64_t total_num_bytes_sent;
    path_stats path[2]; 
    ev_timer report_timer;
} server_stream;

static int report_counter = 0;

static void print_report(server_stream *s)
{
    quicly_stats_t stats[2];
    quicly_get_stats(s->stream->conn, &stats[0]);
    s->report_num_packets_sent = stats[0].num_packets.sent - s->total_num_packets_sent;
    s->report_num_packets_lost = stats[0].num_packets.lost - s->total_num_packets_lost;
    s->total_num_packets_sent = stats[0].num_packets.sent;
    s->total_num_packets_lost = stats[0].num_packets.lost;
    s->total_num_bytes_sent = stats[0].num_bytes.sent;
    s->path[0].min_rtt = stats[0].rtt.minimum;
    s->path[0].srtt = stats[0].rtt.smoothed;
    s->path[0].cwnd_exiting_slow_start = stats[0].cc.cwnd_exiting_slow_start;
    /*
    quicly_get_path_stats(s->stream->conn, &stats[1], 1);
    s->path[1].min_rtt = stats[1].rtt.minimum;
    s->path[1].srtt = stats[1].rtt.smoothed;
    s->path[1].cwnd_exiting_slow_start = stats[1].cc.cwnd_exiting_slow_start;
    */
    printf("connection %i second %i gput mbps: %ld pkts sent: %"PRIu64" pkts lost: %"PRIu64"\n",
           s->report_id, s->report_second, stats[0].delivery_rate.smoothed*8/1024/1024,
           s->report_num_packets_sent, s->report_num_packets_lost);
    /*
    for (int i=0; i<2; i++)
        printf("  path %i send wnd: %"PRIu32" srtt ms: %d\n",
            i, stats[i].cc.cwnd, s->path[i].srtt);
    */
    fflush(stdout);
    ++s->report_second;
}

static void server_report_cb(EV_P, ev_timer *w, int revents)
{
    print_report((server_stream*)w->data);
}

static void server_stream_destroy(quicly_stream_t *stream, quicly_error_t err)
{
    server_stream *s = (server_stream*)stream->data;
    print_report(s);
    printf("connection %i total pkts sent: %"PRIu64" total pkts lost: %"PRIu64" total bytes sent: %ld\n",
            s->report_id, s->total_num_packets_sent, s->total_num_packets_lost, s->total_num_bytes_sent);
    ev_timer_stop(EV_DEFAULT, &s->report_timer);
    free(s);
}

static void server_stream_send_shift(quicly_stream_t *stream, size_t delta)
{
    server_stream *s = stream->data;
    s->acked_offset += delta;
}

static void server_stream_send_emit(quicly_stream_t *stream, size_t off, void *dst, size_t *len, int *wrote_all)
{
    server_stream *s = stream->data;
    uint64_t data_off = s->acked_offset + off;

    if(data_off + *len < s->target_offset) {
        *wrote_all = 0;
    } else {
        printf("done sending\n");
        *wrote_all = 1;
        *len = s->target_offset - data_off;
        assert(data_off + *len == s->target_offset);
    }

    memset(dst, 0x58, *len);
}

static void server_stream_send_stop(quicly_stream_t *stream, quicly_error_t err)
{
    printf("server_stream_send_stop stream-id=%li\n", stream->stream_id);
    fprintf(stderr, "received STOP_SENDING: %i\n", err);
}

static void server_stream_receive(quicly_stream_t *stream, size_t off, const void *src, size_t len)
{
    //print_escaped((const char*)src, len);
    quicly_stream_sync_recvbuf(stream, len);

    if(quicly_recvstate_transfer_complete(&stream->recvstate)) {
        printf("request received, sending data\n");
        quicly_stream_sync_sendbuf(stream, 1);
        ev_timer_start(EV_DEFAULT, &((server_stream*)stream->data)->report_timer);
    }
}

static void server_stream_receive_reset(quicly_stream_t *stream, quicly_error_t err)
{
    printf("server_stream_receive_reset stream-id=%li\n", stream->stream_id);
    fprintf(stderr, "received RESET_STREAM: %i\n", err);
}

static const quicly_stream_callbacks_t server_stream_callbacks = {
    &server_stream_destroy,
    &server_stream_send_shift,
    &server_stream_send_emit,
    &server_stream_send_stop,
    &server_stream_receive,
    &server_stream_receive_reset
};

quicly_error_t server_on_stream_open(quicly_stream_open_t *self, quicly_stream_t *stream)
{
    server_stream *s = malloc(sizeof(server_stream));
    s->target_offset = UINT64_MAX;
    s->acked_offset = 0;
    s->stream = stream;
    s->report_id = report_counter++;
    s->report_second = 0;
    s->report_num_packets_sent = 0;
    s->report_num_packets_lost = 0;
    s->total_num_packets_sent = 0;
    s->total_num_packets_lost = 0;
    for (int i=0; i<2; i++)
        memset(&s->path[i], 0, sizeof(s->path[i]));

    ev_timer_init(&s->report_timer, server_report_cb, 1.0, 1.0);
    s->report_timer.data = s;

    stream->data = s;
    stream->callbacks = &server_stream_callbacks;

    return 0;
}
