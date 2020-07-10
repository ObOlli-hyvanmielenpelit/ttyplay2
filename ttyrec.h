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
    struct FILEID *file_id;
    long int record_start; /* within file */
    long int position;     /* within file */
    struct timeval time_elapsed_cls; /* tv since start of all files at end */
    struct CLRSCRID *prev;
    struct CLRSCRID *next;
} Clrscr_ID;

/* Cf. init of `PControl status' if you change anything here */
typedef struct PCONTROL     /* program control/status */
{
    FILE *fp;               /* file that we're working on */
    File_ID *current_fileid;
    File_ID *index_head;
    Clrscr_ID *clrscr;      /* last CLRSCR switched to */
    struct timeval time_elapsed;
    struct timeval seek_request;
    long int position;      /* within FILE above, bytes */
} PControl;

#endif
