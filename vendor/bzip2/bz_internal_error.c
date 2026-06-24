#ifdef BZ_NO_STDIO
#include <stdlib.h>

void bz_internal_error(int errcode)
{
    (void)errcode;
    abort();
}
#endif