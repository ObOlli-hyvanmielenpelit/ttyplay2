/*
 * Copyright (c) 2000 Satoru Takabayashi <satoru@namazu.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#ifdef USE_CURSES
#include <curses.h>
#else
#include <termios.h>
#endif
#include <sys/time.h>
#include <string.h>
#include <signal.h>

#include "ttyrec.h"
#include "io.h"

#define DEBUG
#ifdef DEBUG
#undef DEBUG_INDEX  /* debug index creation */
#undef DEBUG_SEEK   /* debug seeking by time offset */
#undef DEBUG_JUMP  /* debug jumping to next/prev file/clrscr */

#include <libgen.h>
#include <sys/stat.h>
#endif

#define FAIL 0
#define SUCCESS 1
/* time from clrscr record start till we switch back to its start
    instead of the previous record. */
#define SWITCH_LATENCY 10   /* seconds */
#define JUMPBASE 15         /* base of how much to jump, sec    */
#define JUMP_SCALE 10       /* scaling for next bigger jump     */
#define BUFSIZE 8192        /* max record length (investigated length 4095) */

/* The role of termios, (n)curses, ANSI escape codes and charsets may 
    be a bit confusing. This is because of historical raisins: curses 
    was built on top of tty/termio layer to handle moving around screen 
    and printing characters there (DEC vt52 of 1974 was the first with 
    this capability,) as early teletypes were just typewriter/printers 
    with their own ways, and termio was pretty much just a layer to 
    tell system, and control, their capabilities. Since the codes for 
    curses handling were somewhat tty capability dependent, in late 
    1970's ANSI codes were standardized, and all later tty types honour 
    them.

    The introduction of extended ASCII charsets (CP850, ISO8859-n, UTF 
    to mention a few) was a still later development, from when the 
    US IT industry realized there's a need for l10n, in 1980's through 
    early 2000's (though UTF-8 is still not completely established, 
    in 2020,) and is a completely different problem on top of terminal 
    capability issues stated in previous paragraph.

    Cf. rfc3629, Nov 2003, for UTF-8 standard.
    For linux, see also console_codes(4) man page.
    For ncurses, https://docs.freebsd.org/doc/4.3-RELEASE/usr/share/doc/ncurses/ncurses-intro.html

    Well, at least now you know somewhat more. -ObOlli */

/* ANSI escape sequence for clear screen then position cursor at top left */
#define CLRSCR "\x1b[2J"

typedef double	(*WaitFunc)	(struct timeval prev, 
				 struct timeval cur, 
				 double speed, int *key);
typedef int	(*ReadFunc)	(FILE *fp, Header *h, char **buf);
typedef void	(*WriteFunc)	(char *buf, int len);
typedef void	(*ProcessFunc)	(FILE *fp, double speed, 
				 ReadFunc read_func, WaitFunc wait_func);

#ifdef DEBUG
/* translate timeval to f */
#define tv2f(tv) ((float) tv.tv_sec + (float) tv.tv_usec/1000000)
#endif

/* status of the program, init to zero for proper error behaviour 
    NB, this *MUST* be changed if struct PControl changes! */
static PControl status = {
    NULL, NULL, /* working file: fp, current_fileid */
    NULL,       /* index_head   */
    NULL,       /* last clrscr  */
    {0, 0},     /* timeval time_elapsed */
    {0, 0},     /* timeval seek_request */
    0           /* position in-file */
};

/* From glibc-2.2.3 (libc4.18) manual
  (https://ftp.gnu.org/old-gnu/Manuals/glibc-2.2.3/html_node/libc_418.html)
        "It is often necessary to subtract two values of type struct timeval 
        or struct timespec. Here is the best way to do this. It works even 
        on some peculiar operating systems where the tv_sec member has an
        unsigned type."

    The implementation below is not as complex as the one given, but it
    explains some of the funnity of the carry. The sample given in 
    gcc-2.2.3 returns negativity of the result, and result itself as 
    modified first parameter.

    NB. This means the timeval arithmetic below is not designed to be 
    completely portable.
    --ObOlli                                                    */    

struct timeval
timeval_diff (struct timeval tv1, struct timeval tv2)
{
    struct timeval diff;

    diff.tv_sec = tv2.tv_sec - tv1.tv_sec;
    diff.tv_usec = tv2.tv_usec - tv1.tv_usec;
    if (diff.tv_usec < 0) {
	diff.tv_sec--;
	diff.tv_usec += 1000000;
    }

    return diff;
}

struct timeval
timeval_div (struct timeval tv1, double n)
{
    double x = ((double)tv1.tv_sec  + (double)tv1.tv_usec / 1000000.0) / n;
    struct timeval div;
    
    div.tv_sec  = (int)x;
    div.tv_usec = (x - (int)x) * 1000000;

    return div;
}

/* Keeping with original code, will not attempt complete portability
    in timeval_sub and timeval_add -ObOlli */
struct timeval
timeval_sub (struct timeval tv1, struct timeval tv2)
{
    struct timeval subt;

    subt.tv_sec = tv1.tv_sec - tv2.tv_sec;
    subt.tv_usec = tv1.tv_usec - tv2.tv_usec;
    if (subt.tv_usec < 0) {
    	subt.tv_sec--;
	    subt.tv_usec += 1000000;
    }

    return subt;
}

struct timeval
timeval_add (struct timeval tv1, struct timeval tv2)
{
    struct timeval sum;

    sum.tv_usec = tv1.tv_usec + tv2.tv_usec;
    sum.tv_sec = tv1.tv_sec + tv2.tv_sec;
    if (sum.tv_usec > 1000000) {
        sum.tv_sec++;
        sum.tv_usec -= 1000000;
    }

    return sum;
}

void free_clrscrid(Clrscr_ID *clsid_ptr)
{
    if(clsid_ptr->next)
        free_clrscrid(clsid_ptr->next);
    free(clsid_ptr);
}

void free_fileid(File_ID *fileid_ptr)
{
    if(fileid_ptr->next)
        free_fileid(fileid_ptr->next);
    free_clrscrid(fileid_ptr->first_clrscr);
    free(fileid_ptr->filename);
    free(fileid_ptr);
}

/* free() with some sanity checks added */
int release_buffer(char **buf, char *caller)
{
    if(caller == NULL) {
        fprintf(stderr, "ERROR: anonymous call to buffer release\n");
        return(FAIL);
    }
    if(buf == NULL && caller != NULL) {
        fprintf(stderr, "ERROR: re-releasing buffer, called by %s\n", caller);
        return(FAIL);
    }
    free(*buf);
    buf = NULL;
    return SUCCESS;
}

/* update status structure */
void update_status(Clrscr_ID *clrscr, int position, struct timeval time_elapsed)
{
    status.clrscr = clrscr;
    status.position = position;
    status.time_elapsed = time_elapsed;
    status.current_fileid = status.clrscr->file_id;
    /* update fp, too */
    fseek(status.fp, status.position, SEEK_SET);
}

/* index_one_file returns length of file in timeval */
struct timeval index_one_file(File_ID *file_id, struct timeval whence_in_cls)
{
    int cur_record, bytes_read, clrscr_pos;
    FILE *fp = efopen(file_id->filename, "r");
    Header cur_header, prev_header;
    char buf[BUFSIZE+1], *clrscr_loc; /* leave space in buf for null */
    Clrscr_ID *prev_clrscr, *cur_clrscr;
    int prev_was_cls = 0;

    prev_clrscr = NULL;
#ifdef DEBUG_INDEX
    int iteration_count = 0;
    int cls_count = 0;
#endif
    prev_header.tv.tv_sec=0;    /* never happens, this is seconds since epoch */
    while (1)
    {
        cur_record = ftell(fp);             /* to set start of current header */
#ifdef DEBUG_INDEX
        iteration_count++;
#endif
        if (read_header(fp, &cur_header) == 0)      /* read the header  */
            break;                                  /* EOF              */
        if (cur_header.len > BUFSIZE) {
            printf("Record payload of %d exceeds buffer size %d. This is fatal, exiting.", 
                   cur_header.len, BUFSIZE);
            exit(EXIT_FAILURE);
        }
        if (prev_header.tv.tv_sec == 0)     /* first header of file */
            prev_header = cur_header;       /* init for time arithmetic */

        bytes_read = fread(buf, sizeof(char), cur_header.len, fp); /* record payload*/
        buf[bytes_read] = 0;                                /* for strstr, need it? */
        /* keep track of time, for each and every record    */
        whence_in_cls = timeval_add(whence_in_cls, 
                    timeval_sub(cur_header.tv, prev_header.tv));

        if (!(clrscr_loc = strstr(buf, CLRSCR))) {
            prev_header = cur_header;                       /* no CLRSCR in record */
            continue;
        }

        /* here we have header and payload with CLRSCR */
        clrscr_pos = clrscr_loc - buf;      /* position of clrscr in buf    */
        cur_clrscr = (Clrscr_ID*) malloc(sizeof(Clrscr_ID));
#ifdef DEBUG_INDEX
        fprintf(stderr, "CLRSCR malloc'd, record #%d at %db %.6fs\n", 
            iteration_count, cur_record, tv2f(whence_in_cls));
#endif

        /* update uplink to file*/
        cur_clrscr->file_id = file_id;

        /* chain first clrscr to last file's last */
        if (prev_clrscr == NULL) {          /* first clrscr of file */
            if(file_id->prev != NULL) {  /* not first file of the set */
                prev_clrscr = file_id->prev->last_clrscr;
                prev_clrscr->next = cur_clrscr;
                prev_clrscr->time_elapsed_cls = whence_in_cls;
            } /* else: for first clrscr of first file, NULL is just fine */
            file_id->first_clrscr = cur_clrscr;
        } else {            /* just chain us in current file's chain */
            cur_clrscr->prev = prev_clrscr;
            prev_clrscr->next = cur_clrscr;
            /* update previous clrscr's time_elapsed, too */
            prev_clrscr->time_elapsed_cls = whence_in_cls;
        }

        /* init of the rest of cur_clrscr is straightforward */
        cur_clrscr->next = NULL;
        cur_clrscr->file_id = file_id;
        cur_clrscr->record_start = cur_record;              /* pointer into file    */
        cur_clrscr->position = cur_record + sizeof(cur_header) + clrscr_pos;

        /* update prev_clrscr ending time */
        /* for next iteration */
        prev_clrscr = cur_clrscr;
        prev_header = cur_header;
#ifdef DEBUG_INDEX
        cls_count++;
#endif
    }
    /* update file_id relevant fields   */
    file_id->last_clrscr = cur_clrscr;

    efclose(fp);

#ifdef DEBUG_INDEX
    fprintf(stderr, "file done at %.6fs, %d records.\n", 
        tv2f(whence_in_cls), iteration_count);
#endif
    /* last CLRSCR-record goes till EOF, which is when we are */
    cur_clrscr->time_elapsed_cls = whence_in_cls;
    return(whence_in_cls);
}

/* creates file index, returns pointer to index head    */
File_ID * create_file_index(int start_arg, int argc, char **argv)
{
    File_ID *cur_fileid, *prev_file, *first_file;
    struct timeval whence_in_file;

    int argp;

    for (argp = start_arg; argp < argc; argp++)
    {
        cur_fileid = (File_ID*) malloc(sizeof(File_ID));
#ifdef DEBUG_INDEX
        char *fn = strdup(argv[argp]);
        fprintf(stderr, "\nFile_ID malloc'd for %s\n", basename(fn));
        free(fn);
#endif
        if (argp == start_arg)
        {
            prev_file = NULL;
            first_file = cur_fileid;
            whence_in_file.tv_sec = whence_in_file.tv_usec = 0;
        }
        cur_fileid->prev = prev_file;
        if(prev_file != NULL)
            prev_file->next = cur_fileid;
        cur_fileid->next = NULL;
        cur_fileid->filename = strdup(argv[argp]);
        whence_in_file = index_one_file(cur_fileid, whence_in_file);
        /* link the per-file clrscr chain with that of previous file */
        if(cur_fileid->prev != NULL) {  /* the prev file has to exist, of course */
            cur_fileid->first_clrscr->prev = cur_fileid->prev->last_clrscr;
            cur_fileid->prev->last_clrscr = cur_fileid->first_clrscr->prev;
        }

        /* for next iteration */
        prev_file = cur_fileid;
    }
#ifdef DEBUG_INDEX
    fprintf(stderr, "\n*** indexing complete *** \n");
    fprintf(stderr, "Index structure:\n");
    File_ID *f, *g;
    Clrscr_ID *c;
    int i, j;
    i = j = 0;
    c = first_file->first_clrscr;
    f = c->file_id;
    g = NULL;
    if (c->prev == NULL)
        fprintf(stderr, "Sanity check: first clrscr->prev is null. Good.\n");
    else
        fprintf(stderr, "Sanity check *FAIL*: first clrscr->prev is *NOT* null.\n");
    if (f->prev == NULL)
        fprintf(stderr, "Sanity check: first file_id->prev is null. Good.\n");
    else {
        fprintf(stderr, "Sanity check: first file_id->prev is not null, but let's see... ");
        if(f->prev == f) 
            fprintf(stderr, "it points to itself. Good.\n");
        else
            fprintf(stderr, "it to random address. This is *BAD*.\n");
    }
    while(1) {
        if(f != g) {
            char *fn = strdup(f->filename);
            fprintf(stderr, "File_ID #%d %s ends at %fs\n", 
                    ++i, basename(fn), tv2f(f->last_clrscr->time_elapsed_cls));
            free(fn);
            if(f->prev == NULL)
                fprintf(stderr, "Checking linkage... at first file, ->prev is NULL\n");
            else fprintf(stderr, "Checking linkage... to prev: %s, from prev: %s\n",
                            f->prev == NULL || f->prev == g ? "ok" : "FAIL",
                            g->next == f ? "ok" : "FAIL");
            g = f;
        }
        fprintf(stderr, "\tClrscr_ID #%d record at %d actual pos %d ends at %fs\n",
                ++j, c->record_start, c->position, tv2f(c->time_elapsed_cls));
        if(!c->next) {
            fprintf(stderr, "Sanity check: final clrscr->next is null. Good.\n");
            /* else we burn in infinite loop and crash in segfault ;) */
            if(c->file_id->next == NULL)
                fprintf(stderr, "Sanity check: final File_ID->next is null. Good.\n\n");
            else
                fprintf(stderr, "Sanity check *FAIL*: final File_ID->next is *NOT* null.\n\n");
            break;
        } else
            c = c->next;    /* keep going on */
        f = c->file_id;
    }    
#endif
    return (first_file);
}

/* switch to whicever file asked. does not update status.clrscr, 
    that's the duty of the caller.
    return FAIL/SUCCESS */ 
int switch_to_file(File_ID *target) 
{
    /* first make sure we're indexed, and we were passed a file */
    if (!status.index_head || target == NULL)
        return FAIL;

#ifdef DEBUG
    struct timeval time_at_switch = status.time_elapsed;
#endif
    if(target->prev == NULL)      /* first file of set */
        status.time_elapsed.tv_sec = status.time_elapsed.tv_usec = 0;
    else
        status.time_elapsed = target->prev->last_clrscr->time_elapsed_cls;
#ifdef DEBUG
    char *fn = strdup(target->filename);
    fprintf(stderr, "Opening file %s, time changes from %.6fs to %.6fs\n", 
            basename(fn), tv2f(time_at_switch), tv2f(status.time_elapsed));
    free(fn);             
#endif
    status.current_fileid = target;
    if(!status.fp)
        status.fp = efopen(target->filename, "r");
    else
        freopen(target->filename, "r", status.fp);
    assert(status.fp != NULL);
    return SUCCESS;
}

/* jump_next_file gets parameter of how many'th to jump to, sign 
    indicates direction. 
    NB. returns zero on success, how many were not moved over 
    to on fail. Be wary: this is somewhat counterintuitive 
    error behaviour. */ 
int jump_next_file(int direction)
{
    if(direction < 0) {
        if(status.current_fileid->prev == status.current_fileid)
            /* already at first file */
            return(direction);
        else {
            status.current_fileid = status.current_fileid->prev;
            direction = jump_next_file(direction+1);
        }
    }

    if(direction > 0) {
        if(status.current_fileid->next == NULL)
            /* we're at last file */
            return(direction);
        else {
            status.current_fileid = status.current_fileid->next;
            direction = jump_next_file(direction-1);
        }
    }

    return(direction);
}

/* special case the first file jump, to allow for SWITCH_LATENCY */
int jump_file(int direction)
{
    /* first make sure we're running on indexed files */
    if(!status.index_head)
        return direction;   /* fail, nothing done */

    /* add 1 if seeking back, to make first case that of beginning 
        of the current file. Note that this has to precede the case
        of direction == 0, to effect that particular case. */
    if (direction < 0) {
        direction++;   /* we actually jump one less backwards */ 
#ifdef DEBUG_JUMP
        char *fp = strdup(status.current_fileid->filename);
        fprintf(stderr, "File jump of %d requested, file %s\n", 
                direction, basename(fp));
        free(fp);
#endif
        int delta = timeval_sub(status.time_elapsed, 
                                status.current_fileid->last_clrscr->time_elapsed_cls).tv_sec;
        delta -= SWITCH_LATENCY;
        /* and one more time elapsed from SOF is less than SWITCH_LATENCY */
        if(delta < 0) { 
            direction--;
#ifdef DEBUG_JUMP
            fprintf(stderr, "...and to prev file since we're near SOF.\n");
#endif
        }
    }

    if(direction == 0) {
        /* we jump to start of the file. update status and fp,
            then return without jumping on */
        update_status(status.current_fileid->first_clrscr, 0, 
            status.current_fileid->prev == status.current_fileid ?
                (struct timeval) {0, 0} : 
                status.current_fileid->prev->last_clrscr->time_elapsed_cls);
        return(0);
    }

    /* jump the n'th file by recursion as requested */ 
    direction = jump_next_file(direction);
    if(!switch_to_file(status.current_fileid))
        exit(EXIT_FAILURE); /* should not happen */
    
    struct timeval time_elapsed;
    if(status.current_fileid->prev == status.current_fileid)
        time_elapsed.tv_sec = time_elapsed.tv_usec = 0;
    else
        time_elapsed = status.current_fileid->prev->last_clrscr->time_elapsed_cls;

    update_status(status.current_fileid->first_clrscr, 0, 
        status.current_fileid->prev == status.current_fileid ?
            (struct timeval) {0, 0} : 
            status.current_fileid->prev->last_clrscr->time_elapsed_cls);

    return(direction);
}

/* see comments on jump*file above, we just work with Clrscr_ID, 
    not File_ID. */
int jump_clrscr(int direction) 
{
    /* WIP: we can't really trust status.clrscr is up to date, since the normal
        operation is just pulling stuff from file and pushing it to screen */

    if(direction < 0) {
        if(status.clrscr->prev == status.clrscr) {  /* SOF */
            if(!switch_to_file(status.clrscr->file_id->prev))
                return direction;   /* no previous file */
            status.clrscr = status.current_fileid->last_clrscr;
        } else {
            status.clrscr = status.clrscr->prev;
        }
        /* jumping on implemented as recursion, non-zero return means S/EOF */
        if(jump_clrscr(direction+1) != 0)
            return direction;
    }

    if(direction > 0) {     /* mirror of the above */
        if(status.clrscr->next == NULL) {           /* EOF */
            if(!switch_to_file(status.clrscr->file_id->next))
                return direction;   /* no next file */
            status.clrscr = status.current_fileid->first_clrscr;
        } else {
            status.clrscr = status.clrscr->next;
        }
        if(jump_clrscr(direction-1) != 0)
            return direction;            
    }

    return(direction);  /* success */
}

/* seek_index sets struct status to correct file and header position.
    returns FAIL/SUCCESS */
int seek_index(struct timeval seek_target)
{
    File_ID *cur_fileid;
    Clrscr_ID *cur_clrscr;
    struct timeval tdelta;

#ifdef DEBUG_SEEK
    fprintf(stderr, "Seeking from %lds to %lds\n",
                    status.time_elapsed.tv_sec, seek_target.tv_sec);
#endif

    /* since clrscr_id is chained from beginning to end, all we need
        is find the correct one */
    cur_clrscr = status.index_head->first_clrscr;

    while(1) {
        tdelta = timeval_diff(cur_clrscr->time_elapsed_cls, seek_target);
        if(tdelta.tv_sec <= 0)
            break;
        if(cur_clrscr->next == NULL)
            break;
        cur_clrscr = cur_clrscr->next;
    }

#ifdef DEBUG_SEEK
    fprintf(stderr, "seek_index: found clrscr at %ldb ranging %.6fs through ", 
            cur_clrscr->record_start, 
            cur_clrscr->prev == NULL ? 
                0 : tv2f(cur_clrscr->prev->time_elapsed_cls));
    if(cur_clrscr->next == NULL) 
        fprintf(stderr, "the end\n");
    else 
        fprintf(stderr, "%.6f\n", tv2f(cur_clrscr->time_elapsed_cls));
#endif

    /* switch fp to whichever file/record the index points to */
    cur_fileid = cur_clrscr->file_id;
#ifdef DEBUG_SEEK
    char *fn = strdup(cur_fileid->filename);
    fprintf(stderr, "seek_index: switching to file %s\n", basename(fn));
    free(fn);
#endif
    status.current_fileid = cur_fileid;        /* propagate result upwards */
    if(!switch_to_file(status.current_fileid))
        return FAIL;
    update_status(cur_clrscr, cur_clrscr->record_start, 
            /* the elapsed time is found at end of previous clrscr, if any */
            cur_clrscr->prev == NULL ? 
                (struct timeval) {0, 0} : cur_clrscr->prev->time_elapsed_cls);
    return SUCCESS;
}   

double
ttywait (struct timeval prev, struct timeval cur, double speed, int *key)
{
    static struct timeval drift = {0, 0};
    struct timeval start;
    struct timeval diff = timeval_diff(prev, cur);
    fd_set readfs;

    gettimeofday(&start, NULL);

    /* pause is coded into speed as negative value; 
        absolute value tell us what speed was, for resuming */
    assert(speed != 0);
    diff = timeval_diff(drift, 
        timeval_div(diff, 
            speed<0 ? -speed : speed));
    if (diff.tv_sec < 0) {
        diff.tv_sec = diff.tv_usec = 0;
    }

    FD_SET(STDIN_FILENO, &readfs);
    /* 
     * We use select() for sleeping with subsecond precision.
     * select() is also used to wait user's input from a keyboard.
     *
     * Save "diff" since select(2) may overwrite it to {0, 0}. 
     */
    struct timeval orig_diff = diff;
    if(speed <0) {  /* paused? */
#ifdef DEBUG
        fprintf(stderr, "Paused at %.3fs\n", tv2f(status.time_elapsed));
#endif
        select(1, &readfs, NULL, NULL, NULL);
    } else {
        select(1, &readfs, NULL, NULL, &diff);
    }

    diff = orig_diff;  /* Restore the original diff value. */
    if (FD_ISSET(0, &readfs)) { /* a user hits a character? */
        char c, c2, c3;
        read(STDIN_FILENO, &c, 1); /* drain the character */
        switch (c) {
            case '+':
                speed *= 2;
                break;
            case '-':
                speed /= 2;
                break;
            case '1':
                speed = 1.0;
                break;
            case 'p':
                speed = -speed; /* speed <0 means pause */
                break;
            /* some keys are passed upwards, to effect some
                program control actions:
                    q - quit
                some of which are seek-like:
                    f - next file, d - previous file, 
                    c - next CLRSCR, x - prev CLRSCR */
            case 'q':
            case 'f':
            case 'd':
            case 'x':
                *key = c;
                break;
            case '\033':    /* ESC starts a key sequence        */
                read(STDIN_FILENO, &c2, 1); /* drain the next character */
                switch (c2) {
                    case 'O':    /* for arrow keys (don't ask me)  */
                        read(STDIN_FILENO, &c3, 1); /* drain the next character */
                        switch (c3) {
                            case 'D':   /* left arrow  */
                                status.seek_request.tv_sec += (speed * -JUMPBASE);
                                break;
                            case 'C':   /* right arrow   */
                                status.seek_request.tv_sec += (speed * JUMPBASE);
                                break;
                            case 'A':   /* up arrow     */
                                status.seek_request.tv_sec += (speed * -JUMPBASE * JUMP_SCALE);
                                break;
                            case 'B':   /* down arrow   */
                                status.seek_request.tv_sec += (speed * JUMPBASE * JUMP_SCALE);
                                break;
                            /******** WIP */
                            /* This may be a bit ugly: jump to start by directly adjusting status */
                            case 'H':   /* Home */
                                update_status(status.index_head->first_clrscr, 0, (struct timeval) {0, 0});
                                break;
                            /* For jump-to-end we seek to current time, and trust require-q-to-quit 
                                to handle the rest. As future is not supposed to have happened yet,
                                and as the game start shouldn't have happened before epoch, this 
                                should be good enough. */
                            case 'F':   /* End */
                                gettimeofday(&(status.seek_request), NULL);
                                break;
                            default:    /* unknown esc-O sequence  */
#ifdef DEBUG                   
                                fprintf(stderr, "Unimplemented ESC code O%c at ttywait()\n", c);
#endif
                                break;
                        }
                        break;
                    case '[':    /* for PgUp, PgDown */
                        read(STDIN_FILENO, &c3, 1); /* drain the next character */
                        switch (c3) {
                            case '5':   /* PgUp     */
                                status.seek_request.tv_sec += (speed * -JUMPBASE * JUMP_SCALE * JUMP_SCALE);
                                break;
                            case '6':   /* PgDown   */
                                status.seek_request.tv_sec += (speed * JUMPBASE * JUMP_SCALE * JUMP_SCALE);
                                break;
                            default: /* unknown esc-[ sequence  */
#ifdef DEBUG
                              fprintf(
                                  stderr,
                                  "Unimplemented ESC code [%c at ttywait()\n",
                                  c);
#endif
                                break;
                        }
                        break;
                    default:
#ifdef DEBUG
                      fprintf(stderr,
                              "Unimplemented keycode at ttywait(): %c (0x%x)\n",
                              c, c);
#endif
                        break;
                }
                drift.tv_sec = drift.tv_usec = 0;
        } 
    } else {
        struct timeval stop;
        gettimeofday(&stop, NULL);
        /* Hack to accumulate the drift */
        if (diff.tv_sec == 0 && diff.tv_usec == 0)
            diff = timeval_diff(drift, diff); // diff = 0 - drift.
            
        drift = timeval_diff(diff, timeval_diff(start, stop));
    }
    return speed;
}

double ttynowait(struct timeval prev, struct timeval cur, double speed,
                     int *key) 
{
    /* do nothing */
    return 0; /* Speed isn't important. */
}

int ttyread(FILE * fp, Header * h, char **buf) 
{
    if (read_header(fp, h) == 0) 
	    return 0;

    *buf = malloc(h->len);
    if (*buf == NULL) {
	perror("malloc");
    }
	
    if (fread(*buf, 1, h->len, fp) == 0) {
	perror("fread");
    }
    return 1;
}

int
ttypread (FILE *fp, Header *h, char **buf)
{
    /*
     * Read persistently just like tail -f.
     */
    while (ttyread(fp, h, buf) == 0) {
	struct timeval w = {0, 250000};
	select(0, NULL, NULL, NULL, &w);
	clearerr(fp);
    }
    return 1;
}

void
ttywrite (char *buf, int len)
{
    fwrite(buf, 1, len, stdout);
}

void
ttynowrite (char *buf, int len)
{
    /* do nothing */
}

/* get timeval of the header pointed to by status.fp */
struct timeval get_header_time(ReadFunc read_func)
{
    Header h;
    char *buf;
    /* make sure we know where to go back to */
    status.position = ftell(status.fp);
    if(!read_func(status.fp, &h, &buf))
        exit(FAIL); /* TBD(?): not prepared for EOF */
    /* we don't need the playload for anything, really */
    if(!release_buffer(&buf, "get_header_time"))
        exit(EXIT_FAILURE);
    fseek(status.fp, status.position, SEEK_SET);  /* seek back */
    return(h.tv);
}

void
ttyplay (FILE *fp, double speed, ReadFunc read_func, 
	 WriteFunc write_func, WaitFunc wait_func)
{
    int first_time = 1;
    struct timeval prev;
    /* zero seek_request flag/distance and time_elapsed */
    status.seek_request.tv_sec = status.seek_request.tv_usec = 0;
    status.time_elapsed.tv_sec = status.time_elapsed.tv_usec = 0;
    /* here starts transition to use `PControl *status' */
    status.fp = fp;

    setbuf(stdout, NULL);
    setbuf(status.fp, NULL);

    while (1) {
        char *buf;
        Header h;

        if (read_func(status.fp, &h, &buf) == 0) {
            /* EOF; if we work with indexed files, switch to ->next */
            if(status.index_head && status.current_fileid->next) {
#ifdef DEBUG
                struct timeval time_at_switch = status.time_elapsed;
#endif
                status.current_fileid = status.current_fileid->next;
                freopen(status.current_fileid->filename, "r", status.fp);
                assert(status.fp != NULL);
                update_status(status.current_fileid->first_clrscr, 0, 
                    status.current_fileid->prev == status.current_fileid ?
                        (struct timeval) {0, 0} :
                        status.current_fileid->prev->last_clrscr->time_elapsed_cls);
#ifdef DEBUG
                char *fn = strdup(status.current_fileid->filename);
                fprintf(stderr, "Opening %s, time changes from %.6fs to %.6fs\n\n", 
                    basename(fn), tv2f(time_at_switch), tv2f(status.time_elapsed));
                free(fn);            
#endif
                if(!release_buffer(&buf, "ttyplay switch to next file"))
                    exit(EXIT_FAILURE);
                continue;
            }
            /* WIP: does switching time to negative work for q-to-quit? */
            else speed = -speed;
        } 

        if (!first_time) {
            int key = 0;    /* in case wait_func returns the keypress */
            speed = wait_func(prev, h.tv, speed, &key);

            int result = 0;
            switch(key) {       /* keycode passed us by ttywait()? */
                case 0:         /* none */
                    break;
                case 'q':
                    return;     /* quit */
                case 'f':
                    result = jump_file(+1);
                    h.tv = get_header_time(read_func);
#ifdef DEBUG_JUMP
                    if(result != 0)
                       fprintf(stderr, "FTI: file jump +1 returned %d\n", result);
#endif
                    break;
                case 'd':
                    result = jump_file(-1);
                    h.tv = get_header_time(read_func);
#ifdef DEBUG_JUMP
                    if(result != 0)
                        fprintf(stderr, "FYI: file jump -1 returned %d\n", result);
#endif
                    break;
                case 'c':
                    result = jump_clrscr(+1);
                    h.tv = get_header_time(read_func);
#ifdef DEBUG_JUMP
                    if(result != 0)
                        fprintf(stderr, "FYI: clrscr jump +1 returned %d\n", result);
#endif
                    break;
                case 'x':
                    result = jump_clrscr(-1);
                    h.tv = get_header_time(read_func);
#ifdef DEBUG_JUMP
                    if(result != 0)
                        fprintf(stderr, "FYI: clrscr jump -1 returned %d\n", result);
#endif
                    break;
                default:
#ifdef DEBUG
                    fprintf(stderr, "Unimplemented key request at ttyplay(): %c\n (0x%x)", key, key);
#endif
                break;
            }

            /* use index_head as flag we indeed have files to seek in */
            if (status.index_head != NULL && status.seek_request.tv_sec != 0) {
                struct timeval seek_target = 
                    timeval_add(status.time_elapsed, status.seek_request);
                /* seek_index seeks header preceding CLRSCR and 
                    adjusts status.fp to point to this file/pos. 
                    returns timeval elapsed from start-of-all.          */
#ifdef DEBUG_SEEK
                char *fn = strdup(status.current_fileid->filename);
                fprintf(stderr, "Seek %lds requested at %.3fs %ldb of %s, seek_target %.3fs\n", 
                        status.seek_request.tv_sec, tv2f(status.time_elapsed),
                        ftell(status.fp), basename(fn), tv2f(seek_target));
                free(fn);
                struct stat stat_before, stat_after;
                fstat(status.fp, &stat_before);
#endif
                if(! seek_index(seek_target))
                    exit(EXIT_FAILURE);     /* TBD: add msg like "seek failed" */
#ifdef DEBUG_SEEK
                fprintf(stderr, "Position at clrscr record %lds\n", status.time_elapsed.tv_sec);
                fstat(status.fp, &stat_after);
                if(stat_before.st_ino == stat_after.st_ino)
                    fprintf(stderr, "File seek did NOT change inode. BAD!\n");
                else
                    fprintf(stderr, "File seek DID change inode. Good.\n");
#endif
                /* Now we're to CLRSCR record start, next sub-CLRSCR seek   */
                long int cur_pos = ftell(fp);  /* for reseeking back to start-of-record */
                int first_loop = 1;
                struct timeval time_diff;
                while(read_func(fp, &h, &buf)) {
                    if (first_loop) { /* first iteration  */
                        time_diff.tv_sec = time_diff.tv_usec = 0;
                        first_loop = 0;
                    } else {
                        time_diff = timeval_diff(prev, h.tv);
                        if (timeval_sub(seek_target, 
                                timeval_add(status.time_elapsed, time_diff)).tv_sec < 0) {
#ifdef DEBUG_SEEK
                            fprintf(stderr, "Quitting seek: next step would go into future by %lds\n",
                                -(timeval_sub(seek_target,
                                timeval_add(status.time_elapsed, time_diff)).tv_sec));
#endif                        
                            write_func(buf, h.len);     /* output the record    */
                            if(!release_buffer(&buf, "ttyplay sub-clrscr seek place #1"))
                                exit(EXIT_FAILURE);
                            break;
                        }
                    }

                    cur_pos = ftell(fp);
                    status.time_elapsed = timeval_add(status.time_elapsed, time_diff);   /* where-we-are */
                    write_func(buf, h.len);             /* output the record    */
                    if(!release_buffer(&buf, "ttyplay sub-clrscr seek place #2"))
                        exit(EXIT_FAILURE);
                    prev = h.tv;
                }
                /* sub-CLRSCR seek ends here, reposition back to 
                   preceding recordfp & clear seek pos/flag         */
                fseek(fp, cur_pos, SEEK_SET);
                status.seek_request.tv_sec = status.seek_request.tv_usec = 0;   /* seek all done    */
#ifdef DEBUG_SEEK
                struct timeval offset = timeval_diff(seek_target, status.time_elapsed);
                fprintf(stderr, "Seek complete at position %.3fs %ldb, offset %.3fs\n\n", 
                        tv2f(status.time_elapsed), cur_pos, tv2f(offset));
#endif
            }
            status.time_elapsed = timeval_add(status.time_elapsed, timeval_sub(h.tv, prev));
        }
        /* here ends transition to use `PControl *status' */
        first_time = 0;

        write_func(buf, h.len);
        if(!release_buffer(&buf, "ttyplay end of loop"))
            exit(EXIT_FAILURE);
 
        prev = h.tv;
   }
}

void
ttyskipall (FILE *fp)
{
    /*
     * Skip all records.
     */
    ttyplay(fp, 0, ttyread, ttynowrite, ttynowait);
}

void ttyplayback (FILE *fp, double speed, 
		  ReadFunc read_func, WaitFunc wait_func)
{
    ttyplay(fp, speed, ttyread, ttywrite, wait_func);
}

void ttypeek (FILE *fp, double speed, 
	      ReadFunc read_func, WaitFunc wait_func)
{
    ttyskipall(fp);
    ttyplay(fp, speed, ttypread, ttywrite, ttynowait);
}

void
help (void)
{
    printf("ttyplay2 is somewhat more advanced version of ttyplay, that understands\n");
    printf("multiple files, and enables jumping and seeking within and between files\n");
    printf("\n");
    printf("Commands:\n");
    printf("    q: quit\n");
    printf("    1: normal playback speed\n");
    printf("        +: double current playback speed\n");
    printf("        -: halve current playback speed\n");
    printf("    p: pause:\n");
    printf("    d/f: jump to previous/next file\n");
    printf("    x/c: jump to previous/next CLRSCR\n");
    printf("    back/forward arrow: seek %d seconds back/forward\n", JUMPBASE);    
    printf("    up/down arrow: seek %d seconds back/forward\n", JUMPBASE*JUMP_SCALE);
    printf("    PgUp/PgDown: seek %d seconds back/forward\n", JUMPBASE*JUMP_SCALE*JUMP_SCALE);
    printf("    Home/End: jump to start/end of all files\n");
    exit(0); /* it's OK */
}

void
usage (void)
{
    printf("Usage: ttyplay [OPTION] [FILE]\n");
    printf("  -s SPEED Set speed to SPEED [1.0]\n");
    printf("  -n       No wait mode\n");
    printf("  -p       Peek another person's ttyrecord\n");
    printf("  -u       utf-8 mode (default: no)\n");
    printf("  -8       8-bit mode (opposite of utf8)\n");
    printf("  -? or -h print help screen\n");
    exit(EXIT_FAILURE);
}

/*
 * We do some tricks so that select(2) properly works on
 * STDIN_FILENO in ttywait().
 */
FILE *
input_from_stdin (void)
{
    FILE *fp;
    int fd = edup(STDIN_FILENO);
    edup2(STDOUT_FILENO, STDIN_FILENO);
    return efdopen(fd, "r");
}

#ifdef USE_CURSES
void
initcurses (int utf8_mode)
{
  const char *defterm = "xterm";
  printf(CLRSCR);
  if (!getenv("TERM")) setenv("TERM", defterm, 1);
  initscr();
  cbreak ();
  noecho ();
  nonl ();
  intrflush (stdscr, FALSE);
  keypad (stdscr, TRUE);
#define USE_NCURSES_COLOR
#ifdef USE_NCURSES_COLOR
  start_color();
  use_default_colors();

  init_pair(COLOR_BLACK, COLOR_WHITE, COLOR_BLACK);
  init_pair(COLOR_RED, COLOR_RED, COLOR_BLACK);
  init_pair(COLOR_GREEN, COLOR_GREEN, COLOR_BLACK);
  init_pair(COLOR_YELLOW, COLOR_YELLOW, COLOR_BLACK);
  init_pair(COLOR_BLUE, COLOR_BLUE, COLOR_BLACK);
  init_pair(COLOR_MAGENTA, COLOR_MAGENTA, COLOR_BLACK);
  init_pair(COLOR_CYAN, COLOR_CYAN, COLOR_BLACK);
  init_pair(COLOR_WHITE, COLOR_WHITE, COLOR_BLACK);
  init_pair(9, 0, COLOR_BLACK);
  init_pair(10, COLOR_BLACK, COLOR_BLACK);
  init_pair(11, -1, -1);

  if (utf8_mode) {
#ifdef DEBUG
    fprintf(stderr, "echoing `set UTF-8 mode' (esc-%%G) to term.\n\n");
#endif /* DEBUG */
    (void) write(1, "\033%G", 3);   /* select UTF-8 charset */
  } else {
#ifdef DEBUG
    fprintf(stderr, "echoing `set 8-bit mode' (non-UTF8, esc-%%@) to term.\n\n");
#endif /* DEBUG */
    (void) write(1, "\033%@", 3);   /* iso646/iso8859-1 */
  }
#endif /* USE_NCURSES_COLOR */
  clear();
  refresh();
}
#else
static struct termios old, new;
#endif

void
interrupt(int n)
{
#ifdef USE_CURSES
    endwin();
#else
    tcsetattr(0, TCSANOW, &old);  /* Return terminal state */
#endif
    exit(n);
}

int main(int argc, char **argv)
{
    double speed = 1.0;
    ReadFunc read_func  = ttyread;
    WaitFunc wait_func  = ttywait;
    ProcessFunc process = ttyplayback;
    FILE *input = NULL;
    int utf8_mode = 0;

    set_progname(argv[0]);
    while (1) {
        int ch = getopt(argc, argv, "s:npu8?h");
        if (ch == EOF) {
            break;
        }
        switch (ch) {
        case 's':
            if (optarg == NULL) {
                perror("-s option requires an argument");
                exit(EXIT_FAILURE);
            }
            sscanf(optarg, "%lf", &speed);
            break;
        case 'n':
            wait_func = ttynowait;
            break;
        case 'p':
            process = ttypeek;
            break;
        case 'u':
            utf8_mode = 1;
            break;
        /* for robustness, add non-UTF8 option, so the default above
            doesn't really matter */
        case '8':
            utf8_mode = 0;
            break;
        case '?':
        case 'h':
            help();
            break;  /* never taken */
        default:
            usage();
        }
    }

    if (optind < argc) {
    status.current_fileid = status.index_head =
        create_file_index(optind, argc, argv);
        status.time_elapsed.tv_sec = status.time_elapsed.tv_usec = 0;
#ifdef DEBUG
    char *fn = strdup(status.current_fileid->filename);
    fprintf(stderr, "Opening initial file %s\n\n", basename(fn));
    free(fn);
#endif                
        input = efopen(status.current_fileid->filename, "r");
    } else {
        input = input_from_stdin();
        status.index_head = NULL;
    }
    assert(input != NULL);
#ifndef USE_CURSES
    tcgetattr(0, &old); /* Get current terminal state */
    new = old;          /* Make a copy */
    new.c_lflag &= ~(ICANON | ECHO | ECHONL); /* unbuffered, no echo */
    new.c_cc[VMIN] = 1;
    new.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &new); /* Make it current */
#else
    initcurses(utf8_mode);
#endif
    signal(SIGINT, interrupt);
    process(input, speed, read_func, wait_func);

    if (status.index_head) 
        free_fileid(status.index_head);

#ifdef USE_CURSES
    endwin();
#else
    tcsetattr(0, TCSANOW, &old);  /* Return terminal state */
#endif

    return 0;
}
