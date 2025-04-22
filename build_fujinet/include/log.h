/* Minimal log.h for standalone testing */
#ifndef LOG_H
#define LOG_H

#include <stdio.h>

#define Log_print printf
#define Log_flushlog() fflush(stdout)

#endif /* LOG_H */
