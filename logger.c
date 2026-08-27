#include "gateway.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <syslog.h>

static void write_log(const char *level, const char *fmt, va_list args) {
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    
    time_t now = time(NULL);
    char time_str[20];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
    
    // Konzola
    printf("[%s] [%s] %s\n", time_str, level, buf);
    
    // Syslog
    int priority = LOG_INFO;
    if (strcmp(level, "WARN") == 0) priority = LOG_WARNING;
    if (strcmp(level, "ERROR") == 0) priority = LOG_ERR;
    syslog(priority, "%s", buf);
    
    // Rotirajoči log (simulacija zapisa v datoteko)
    FILE *f = fopen("gateway.log", "a");
    if (f) {
        fprintf(f, "[%s] [%s] %s\n", time_str, level, buf);
        fclose(f);
    }
}

void log_info(const char *fmt, ...) {
    va_list args; va_start(args, fmt); write_log("INFO", fmt, args); va_end(args);
}
void log_warn(const char *fmt, ...) {
    va_list args; va_start(args, fmt); write_log("WARN", fmt, args); va_end(args);
}
void log_error(const char *fmt, ...) {
    va_list args; va_start(args, fmt); write_log("ERROR", fmt, args); va_end(args);
}
