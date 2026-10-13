#define MAX_VOLUME 0x3fff
typedef struct { int freq, bits, channels; } audsrv_fmt_t;
int audsrv_init(void); const char *audsrv_get_error_string(void); int audsrv_set_format(audsrv_fmt_t*); int audsrv_set_volume(int);
int audsrv_wait_audio(int); int audsrv_play_audio(const char*, int);
