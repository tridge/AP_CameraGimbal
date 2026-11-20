#include "camera_app/binlog.h"
#include "camera_app/config.h"
#include <assert.h>
#include <unistd.h>
void ca_log(const char *format, ...) { (void)format; }
int main(int argc, char **argv)
{
    assert(argc == 2);
    struct ca_config config;
    ca_config_defaults(&config);
    assert(ca_binlog_init(argv[1]) == 0);
    assert(ca_binlog_start(&config));
    sleep(11);
    ca_binlog_stop();
    /* Restart without reinitializing the logger, as on arm/disarm. */
    assert(ca_binlog_start(&config));
    sleep(6);
    ca_binlog_close();
    return 0;
}
