#ifndef MPV_CLIENT_H
#define MPV_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mpv_format {
    MPV_FORMAT_NONE             = 0,
    MPV_FORMAT_STRING           = 1,
    MPV_FORMAT_OSD_STRING       = 2,
    MPV_FORMAT_FLAG             = 3,
    MPV_FORMAT_INT64            = 4,
    MPV_FORMAT_DOUBLE           = 5,
    MPV_FORMAT_NODE             = 6,
    MPV_FORMAT_NODE_ARRAY       = 7,
    MPV_FORMAT_NODE_MAP         = 8,
    MPV_FORMAT_BYTE_ARRAY       = 9
} mpv_format;

typedef enum mpv_event_id {
    MPV_EVENT_NONE              = 0,
    MPV_EVENT_SHUTDOWN          = 1,
    MPV_EVENT_LOG_MESSAGE       = 2,
    MPV_EVENT_GET_PROPERTY_REPLY= 3,
    MPV_EVENT_SET_PROPERTY_REPLY= 4,
    MPV_EVENT_COMMAND_REPLY     = 5,
    MPV_EVENT_START_FILE        = 6,
    MPV_EVENT_END_FILE          = 7,
    MPV_EVENT_FILE_LOADED       = 8,
    MPV_EVENT_IDLE              = 11,
    MPV_EVENT_TICK              = 14,
    MPV_EVENT_PLAYBACK_RESTART  = 21,
    MPV_EVENT_PROPERTY_CHANGE   = 22,
    MPV_EVENT_QUEUE_OVERFLOW    = 24,
    MPV_EVENT_HOOK              = 25
} mpv_event_id;

typedef struct mpv_event_property {
    const char *name;
    mpv_format format;
    void *data;
} mpv_event_property;

typedef struct mpv_event_end_file {
    int reason;
    int error;
    int64_t playlist_entry_id;
    int64_t playlist_insert_id;
    int playlist_insert_num_entries;
} mpv_event_end_file;

typedef struct mpv_event {
    mpv_event_id event_id;
    int error;
    uint64_t reply_userdata;
    void *data;
} mpv_event;

typedef struct mpv_event_log_message {
    const char *prefix;
    const char *level;
    const char *text;
    int log_level;
} mpv_event_log_message;

typedef struct mpv_handle mpv_handle;

// Function pointer signatures for dynamic loading
typedef mpv_handle *(*fn_mpv_create)(void);
typedef int (*fn_mpv_initialize)(mpv_handle *ctx);
typedef void (*fn_mpv_destroy)(mpv_handle *ctx);
typedef void (*fn_mpv_terminate_destroy)(mpv_handle *ctx);
typedef int (*fn_mpv_command)(mpv_handle *ctx, const char **args);
typedef int (*fn_mpv_command_string)(mpv_handle *ctx, const char *args);
typedef int (*fn_mpv_set_option)(mpv_handle *ctx, const char *name, mpv_format format, void *data);
typedef int (*fn_mpv_set_option_string)(mpv_handle *ctx, const char *name, const char *data);
typedef int (*fn_mpv_get_property)(mpv_handle *ctx, const char *name, mpv_format format, void *data);
typedef int (*fn_mpv_set_property)(mpv_handle *ctx, const char *name, mpv_format format, void *data);
typedef int (*fn_mpv_set_property_string)(mpv_handle *ctx, const char *name, const char *data);
typedef int (*fn_mpv_observe_property)(mpv_handle *ctx, uint64_t reply_userdata, const char *name, mpv_format format);
typedef mpv_event *(*fn_mpv_wait_event)(mpv_handle *ctx, double timeout);
typedef const char *(*fn_mpv_error_string)(int error);
typedef int (*fn_mpv_request_log_messages)(mpv_handle *ctx, const char *min_level);
typedef void (*fn_mpv_free)(void *data);

// Extension API signatures for real-time PCM tapping (custom mpv build)
typedef void (*mpv_pcm_callback_fn)(const float *data, int frames, int channels, int sample_rate, void *userdata);
typedef void (*fn_mpv_set_pcm_callback)(mpv_handle *ctx, mpv_pcm_callback_fn cb, void *userdata);

#ifdef __cplusplus
}
#endif

#endif // MPV_CLIENT_H
