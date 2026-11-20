#include <cstdlib>
#include <cstdio>
#include <cstdarg>
static bool fail_malloc;
static void *buffer_malloc(size_t size) { return fail_malloc ? nullptr : malloc(size); }
// Inject a failed second formatting pass as well as a failed sizing pass.
static unsigned format_calls, fail_format_call;
static int buffer_vsnprintf(char *data, size_t size, const char *format, va_list args)
{
    if (fail_format_call && ++format_calls == fail_format_call) return -1;
    return vsnprintf(data, size, format, args);
}
#define malloc buffer_malloc
#define vsnprintf buffer_vsnprintf
#include "../APC_StringBuffer.h"
#undef vsnprintf
#undef malloc
#include <cassert>

int main()
{
    APC_StringBuffer response;
    assert(response.append_html("<&\"'"));
    assert(strcmp(response.data(), "&lt;&amp;&quot;&#39;") == 0);
    assert(response.appendf(" %u", 123U));
    const size_t length = response.size();
    volatile size_t oversized = SIZE_MAX;
    assert(!response.append_n("", oversized));
    assert(!response.append_html_n("", oversized));
    assert(response.size() == length);
    char *body = response.release();
    assert(response.data()[0] == 0 && response.size() == 0);
    response.reset();
    assert(body == nullptr); // A failed response must never publish partial HTML.
    free(body);
    assert(response.append_url("space &?"));
    assert(strcmp(response.data(), "space%20%26%3F") == 0);
    response.reset();
    assert(response.append_log_html("\033[31mred<&\r\n", 12));
    assert(strcmp(response.data(), "red&lt;&amp;\n") == 0);
    response.reset();
    // Each failure must poison an otherwise valid partial response by itself.
    assert(response.append("partial"));
    assert(!response.append_html_n("", oversized));
    assert(!response.valid() && response.release() == nullptr);
    assert(response.append("partial"));
    fail_malloc = true;
    assert(!response.append_html_n("key", 3));
    fail_malloc = false;
    assert(!response.valid() && response.release() == nullptr);
    for (unsigned pass = 1; pass <= 2; pass++) {
        assert(response.append("partial"));
        format_calls = 0;
        fail_format_call = pass;
        assert(!response.appendf("%s", "formatted"));
        fail_format_call = 0;
        assert(!response.valid() && response.release() == nullptr);
    }
    // Exercise reallocation and termination after release/reset reuse.
    for (unsigned i = 0; i < 10000; i++) assert(response.append("x"));
    assert(response.size() == strlen(response.data()));
    return 0;
}
