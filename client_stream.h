#pragma once

#include <quicly.h>

#ifndef quicly_error_t
typedef int32_t quicly_error_t;
#endif

quicly_error_t client_on_stream_open(quicly_stream_open_t *self, quicly_stream_t *stream);
void client_set_quit_after(int seconds);
