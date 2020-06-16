#ifndef __TTYREC_H__
#define __TTYREC_H__

#include "sys/time.h"

typedef struct header {
    struct timeval tv;
    int len;
} Header;

/* for indexing/seeking, by ObOlli */
typedef struct FILEID
{
    char *filename;
    struct FILEID *prev;
    struct FILEID *next;
    struct timeval time_elapsed_file; /* tv since start of all files at end */
    struct CLRSCRID *first_clrscr;
    struct CLRSCRID *last_clrscr;
} File_ID;
typedef struct CLRSCRID
{
    struct FILEID *fileidx;
    long int record_start; /* within file */
    long int position;     /* within file */
    struct timeval time_elapsed_cls; /* tv since start of all files at end */
    struct CLRSCRID *prev;
    struct CLRSCRID *next;
} Clrscr_ID;

#endif
