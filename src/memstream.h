#ifdef __APPLE__
#include <Availability.h>
#if __MAC_OS_X_VERSION_MIN_REQUIRED < 101300
// code to run up through 10.12.4

#include <stdio.h>

#if _POSIX_C_SOURCE < 200809L

FILE *open_memstream(char **ptr, size_t *sizeloc);

#endif /* _POSIX_C_SOURCE < 200809L */
#endif // __MAC_OS_X_VERSION_MIN_REQUIRED
#endif // __APPLE__
