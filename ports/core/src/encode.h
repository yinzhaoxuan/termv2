#pragma once
#include "itcore.h"
#include "screen.h"
#include <string>
namespace itcore_encode {
int encode_text(itcore::Screen *screen, const char *utf8, size_t len, itcore_buffer **out);
int encode_paste(itcore::Screen *screen, const char *utf8, size_t len, itcore_buffer **out);
int encode_key(itcore::Screen *screen, const itcore_key_event *ev, itcore_buffer **out);
int encode_mouse(itcore::Screen *screen, const itcore_mouse_event *ev, itcore_buffer **out);
void release_buf(itcore_buffer *b);
itcore_buffer *make_buf(std::string s);
}
