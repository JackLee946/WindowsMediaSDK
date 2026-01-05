#include <stdio.h>

// librtmp/rtmp.c references these globals under _DEBUG for raw socket dump.
// We don't use this feature in our SDK build; provide safe NULL definitions.
FILE *netstackdump = NULL;
FILE *netstackdump_read = NULL;


