#include <ctype.h>
#include "util.h"

/* Compatibility function for ARM macOS */
#if defined(__APPLE__) && defined(__arm64__)
int Util_stricmp(const char *str1, const char *str2) {
    int retval;
    while ((retval = tolower((unsigned char)*str1) - tolower((unsigned char)*str2++)) == 0) {
        if (*str1++ == '\0')
            break;
    }
    return retval;
}
#endif
