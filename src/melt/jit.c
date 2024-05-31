#include <math.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <fcntl.h>

#include "JitControl.pb-c.h"
#include "JitStatus.pb-c.h"

static JitStatus jit_status = JIT_STATUS__INIT;
static double fps_multiplier;

static int melt_sock_fd = -1; // UNIX domain socket for melt/jit messaging
static struct sockaddr_un jit_sock_addr; // last known socket address of jit
static socklen_t jit_sock_addr_len = 0;


static void jit_action( mlt_producer producer, char *value )
{
    mlt_properties properties = MLT_PRODUCER_PROPERTIES( producer );
    mlt_consumer consumer = mlt_properties_get_data( properties, "transport_consumer", NULL );
    mlt_properties jack = mlt_properties_get_data( MLT_CONSUMER_PROPERTIES( consumer ), "jack_filter", NULL );

    JitControl *const jit_control = (JitControl*) value;
    switch (jit_control->type) {
        case CONTROL_TYPE__PAUSE:
            if (mlt_producer_get_speed( producer ) != 0) {
                mlt_producer_set_speed( producer, 0 );
                mlt_consumer_purge( consumer );
                if (jit_status.playing) {
                    if (jit_control->has_seek_position) {
                        mlt_producer_seek( producer, llround(fps_multiplier * jit_control->seek_position));
                    } else {
                        //mlt_producer_seek( producer, mlt_consumer_position( consumer ) - 2 );
                    }
                }
            }
            mlt_events_fire( jack, "jack-stop", mlt_event_data_none() );
            jit_status.playing = 0;
            break;
        case CONTROL_TYPE__PLAY:
            if ( !jack || mlt_producer_get_speed( producer ) != 0 ) {
                mlt_producer_set_speed( producer, jit_control->play_rate );
            }
            mlt_consumer_purge( consumer );
            mlt_events_fire( jack, "jack-start", mlt_event_data_none() );
            jit_status.playing = 1;
            break;

        case CONTROL_TYPE__PLAY_RATE:
            mlt_producer_set_speed( producer, jit_control->play_rate );
            break;

        case CONTROL_TYPE__SEEK:
            mlt_consumer_purge( consumer );
            mlt_producer_seek( producer, llround(fps_multiplier * jit_control->seek_position));
            fire_jack_seek_event(jack,  llround(fps_multiplier * jit_control->seek_position));
            break;
        case CONTROL_TYPE__SEEK_REL: ;
            const mlt_position pos = mlt_producer_position(producer) + (jit_control->seek_position < 0 ? floor(fps_multiplier * jit_control->seek_position) : ceil(fps_multiplier * jit_control->seek_position));
            mlt_consumer_purge( consumer );
            mlt_producer_seek( producer, pos);
            fire_jack_seek_event(jack, pos);
            break;
        case CONTROL_TYPE__QUIT:
            mlt_properties_set_int( properties, "done", 1 );
            mlt_events_fire( jack, "jack-stop", mlt_event_data_none() );
            break;
        default:
            break;
    }
    mlt_properties_set_int( MLT_CONSUMER_PROPERTIES( consumer ), "refresh", 1 );
}

static JitControl *read_control() {
    static uint8_t buf[1 * 1024 * 1024]; // 1 MB
    fd_set set;
    FD_ZERO(&set);
    FD_SET(melt_sock_fd, &set);
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    if (melt_sock_fd < 0 || select(melt_sock_fd + 1, &set, NULL, NULL, &timeout) <= 0) {
        return NULL;
    }

    jit_sock_addr_len = sizeof jit_sock_addr;
    const ssize_t r = recvfrom(melt_sock_fd, buf, sizeof buf, 0, (struct sockaddr*) &jit_sock_addr, &jit_sock_addr_len);
    if (r < 1) {
        perror("read");
        exit(1);
    } else if (r == sizeof buf) {
        fprintf(stderr, "read buffer overflow\n");
        exit(1);
    }
    return jit_control__unpack(NULL, r, buf);
}

static void write_status(JitStatus *const jit_status) {
    static uint8_t *buf = NULL;
    static int buf_len = 0;

    if (melt_sock_fd < 0 || jit_sock_addr_len <= 0) {
        return;
    }

    int len = jit_status__get_packed_size(jit_status);
    if (buf_len < len) {
        buf = realloc(buf, len);
        if (!buf) {
            perror("realloc");
            exit(1);
        }
        buf_len = len;
    }

    jit_status__pack(jit_status, buf);
    if (sendto(melt_sock_fd, buf, len, 0, (struct sockaddr*) &jit_sock_addr, jit_sock_addr_len) != len) {
        perror("sendto");
        exit(1);
    }
}

static mlt_producer find_producer_avformat(mlt_producer p) {

    mlt_tractor tractor = (mlt_tractor) p;
    mlt_multitrack multitrack = mlt_tractor_multitrack(tractor);
    mlt_playlist playlist = (mlt_playlist) mlt_multitrack_track(multitrack, 0);
    return mlt_properties_get_data(MLT_PRODUCER_PROPERTIES(mlt_playlist_get_clip(playlist, 0)), "_cut_parent", NULL);
}

static void open_status_pipe(void) {
    melt_sock_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (melt_sock_fd < 0) {
        perror("socket");
        exit(2);
    }

    memset(&jit_sock_addr, 0, sizeof jit_sock_addr);
    jit_sock_addr.sun_family = AF_UNIX;
    snprintf(jit_sock_addr.sun_path, sizeof jit_sock_addr, "/tmp/melt-sock-%lld", (long long) getppid());
    fprintf(stdout, "Creating status socket: %s\n", jit_sock_addr.sun_path);
    fflush(stdout);

    if (bind(melt_sock_fd, (struct sockaddr*) &jit_sock_addr, sizeof jit_sock_addr) < 0) {
        perror("bind");
        exit(2);
      }
    fprintf(stdout, "Status socket created\n");
    fflush(stdout);
}

static void print_media_info(void) {
    mlt_producer av = find_producer_avformat(melt);
    jit_status.media_info = calloc(1, sizeof (MediaInfo));
    media_info__init(jit_status.media_info);
    jit_status.media_info->n_streams = mlt_properties_get_int(MLT_PRODUCER_PROPERTIES(av), "meta.media.nb_streams");
    jit_status.media_info->streams = calloc(jit_status.media_info->n_streams, sizeof (Stream*));
    const int frame_rate_num = mlt_properties_get_int(MLT_PRODUCER_PROPERTIES(av), "meta.media.frame_rate_num");
    const int frame_rate_den = mlt_properties_get_int(MLT_PRODUCER_PROPERTIES(av), "meta.media.frame_rate_den");
    if (frame_rate_num > 0 && frame_rate_den > 0) {
        jit_status.has_frame_rate = 1;
        jit_status.frame_rate = ((double) frame_rate_num) / ((double) frame_rate_den);
    }
    for (int i = 0; i < jit_status.media_info->n_streams; i++) {
        Stream *s = calloc(sizeof (Stream), 1);
        stream__init(s);
        jit_status.media_info->streams[i] = s;
        s->has_type = 1;
        s->type = STREAM_TYPE__UNKNOWN;
        char key[100];
        sprintf(key, "meta.media.%d.stream.type", i);
        char *value = mlt_properties_get(MLT_PRODUCER_PROPERTIES(av), key);
        if (!value) {
            continue;
        } else if (!strcmp(value, "audio")) {
            s->type = STREAM_TYPE__AUDIO;
            s->audio = calloc(1, sizeof (AudioStream));
            audio_stream__init(s->audio);
            sprintf(key, "meta.media.%d.codec.channels", i);
            s->audio->has_channels = 1;
            s->audio->channels = mlt_properties_get_int(MLT_PRODUCER_PROPERTIES(av), key);
            jit_status.has_total_channels = 1;
            jit_status.total_channels += s->audio->channels;
            sprintf(key, "meta.attr.%d.stream.language.markup", i);
            value = mlt_properties_get(MLT_PRODUCER_PROPERTIES(av), key);
            if (value) {
                s->audio->language = strdup(value);
            }
        } else if (!strcmp(value, "video")) {
            s->type = STREAM_TYPE__VIDEO;
            s->video = calloc(1, sizeof (VideoStream));
            video_stream__init(s->video);
            sprintf(key, "meta.media.%d.stream.frame_rate", i);
            s->video->has_frame_rate = 1;
            s->video->frame_rate = mlt_properties_get_double(MLT_PRODUCER_PROPERTIES(av), key);
            if (!jit_status.has_frame_rate) {
                jit_status.has_frame_rate = 1;
                jit_status.frame_rate = s->video->frame_rate;
            }
            sprintf(key, "meta.media.%d.codec.width", i);
            s->video->has_width = 1;
            s->video->width = mlt_properties_get_int(MLT_PRODUCER_PROPERTIES(av), key);
            sprintf(key, "meta.media.%d.codec.height", i);
            s->video->has_height = 1;
            s->video->height = mlt_properties_get_int(MLT_PRODUCER_PROPERTIES(av), key);
        }
    }
}
