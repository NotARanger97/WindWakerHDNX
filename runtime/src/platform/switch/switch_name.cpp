// Thread label for the Switch profiler (host::thread_label is set by host::set_thread_name).
#include "platform/host.h"

void switch_thread_name(char* out, size_t size) { host::get_thread_name(out, size); }
