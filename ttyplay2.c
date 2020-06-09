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

typedef double	(*WaitFunc)	(struct timeval prev, 
				 struct timeval cur, 
				 double speed);
typedef int	(*ReadFunc)	(FILE *fp, Header *h, char **buf);
typedef void	(*WriteFunc)	(char *buf, int len);
typedef void	(*ProcessFunc)	(FILE *fp, double speed, 
				 ReadFunc read_func, WaitFunc wait_func);

/****** ObOlli *****/
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

            /* ugly hack to enable seeking */
static struct timeval time_elapsed, seek_request;
static File_ID *index_head;

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
/****** /ObOlli *****/

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

/****** ObOlli *****/
/* Keeping with original code, will not attempt complete portability
    in timeval_sub and timeval_add */
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

/* index_one_file returns length of file in timeval */
struct timeval index_one_file(File_ID *file_id, struct timeval whence_cls)
{
    int cur_record, bytes_read, clrscr_pos;
    FILE *fp = efopen(file_id->filename, "r");
    Header cur_header, prev_header, last_header;
#define BUFSIZE 8192 /* max record length (investigated length 4095) */
#define CLRSCR "\x1b[2J"
    char buf[BUFSIZE+1], *clrscr_loc; /* leave space in buf for null */
    Clrscr_ID *first_clrscr, *prev_clrscr, *cur_clrscr;
    struct timeval header_tdelta;

    prev_clrscr = NULL;
#ifdef DEBUG
    int iteration_count = 0;
    int cls_count = 0;
#endif
    prev_header.tv.tv_sec=0;    /* never happens, this is seconds since epoch */
    while (1)
    {
        cur_record = ftell(fp);                             /* to set start of current header */
#ifdef DEBUG
        iteration_count++;
#endif
        int read_temp = read_header(fp, &cur_header);
        if (read_temp  == 0)              /* read the header  */
            break;                                          /* EOF              */
        if (cur_header.len > BUFSIZE) {
            printf("Record payload of %d exceeds buffer size %d. Exiting.", 
                   cur_header.len, BUFSIZE);
            exit(EXIT_FAILURE);
        }
        if (prev_header.tv.tv_sec == 0)                     /* first header of file */
            prev_header = cur_header;                       /* init for time arithmetic */
        bytes_read = fread(buf, sizeof(char), cur_header.len, fp); /* record payload*/
        buf[bytes_read] = 0;                                /* for strstr, need it? */
        if (!(clrscr_loc = strstr(buf, CLRSCR))) {
            last_header = cur_header;                       /* no CLRSCR in record */
            continue;
        }
        /* here we have header and payload with CLRSCR */
        clrscr_pos = clrscr_loc - buf;      /* position of clrscr in buf    */
        cur_clrscr = (Clrscr_ID*) malloc(sizeof(Clrscr_ID));
        if (prev_clrscr == NULL) 
            prev_clrscr = first_clrscr = cur_clrscr;        /* first record of file */

        /* update when_we_are to end of record, then update time_elapsed_cls    */
        header_tdelta = timeval_sub(cur_header.tv, prev_header.tv);
        whence_cls = timeval_add(whence_cls, header_tdelta);
        cur_clrscr->time_elapsed_cls = whence_cls;

        /* update of the rest of cur_clrscr is straightforward     */
        cur_clrscr->prev = prev_clrscr;
        prev_clrscr->next = cur_clrscr;
        cur_clrscr->next = NULL;
        cur_clrscr->fileidx = file_id;
        cur_clrscr->record_start = cur_record;              /* pointer into file    */
        cur_clrscr->position = cur_record + sizeof(cur_header) + clrscr_pos;
        /* for next iteration */
        prev_clrscr = cur_clrscr;
        prev_header = cur_header;
#ifdef DEBUG
        cls_count++;
#endif
    }
    /* update file_id relevant fields   */
    file_id->first_clrscr = first_clrscr;
    file_id->last_clrscr = cur_clrscr;

    /* final time arithmetic. prev_header is the last header existent, whence_cls
        has been updated to up to it. cur_header may have been clobbered.  */
    header_tdelta = timeval_sub(last_header.tv, prev_header.tv);
    whence_cls = timeval_add(whence_cls, header_tdelta);
    efclose(fp);
    return(whence_cls);
}

/* creates file index, returns pointer to index head    */
File_ID * create_file_index(int start_arg, int argc, char **argv)
{
    File_ID *cur_file, *prev_file, *first_file;
    struct timeval whence_in_file;

    int argp;

    for (argp = start_arg; argp < argc; argp++)
    {
        cur_file = (File_ID*) malloc(sizeof(File_ID));
        if (argp == start_arg)
        {
            prev_file = first_file = cur_file;
            whence_in_file.tv_sec = whence_in_file.tv_usec = 0;
        }
        cur_file->prev = prev_file;
        prev_file->next = cur_file;
        cur_file->next = NULL;
        cur_file->filename = strdup(argv[argp]);
        whence_in_file = index_one_file(cur_file, whence_in_file);
        /* for next iteration */
        cur_file->time_elapsed_file = whence_in_file;
        prev_file = cur_file;
    }
    return (first_file);
}

/* seek_file_index sets fd to correct file and header position.
    returns seconds elapsed since start of file(s)               */
struct timeval seek_file_index(FILE *fp, struct timeval seek_target)
{
    /* static File_ID *index_head; */
    File_ID *cur_file;
    Clrscr_ID *cur_clrscr;
    struct timeval when_we_are;

#ifdef DEBUG
    fprintf(stderr, "Seeking from %lds to %lds\n", time_elapsed.tv_sec, seek_target.tv_sec);
#endif
    cur_file = index_head;

    /* First find the correct file               */
    if (cur_file->prev == cur_file)     /* special case first file  */
        when_we_are.tv_sec = when_we_are.tv_usec = 0;
    else
        when_we_are = cur_file->prev->time_elapsed_file;

    while (timeval_diff(cur_file->time_elapsed_file, seek_target).tv_sec >0 &&
            cur_file->next != NULL) {
        when_we_are = cur_file->time_elapsed_file;
        cur_file = cur_file->next;
    }    

#ifdef DEBUG
    fprintf(stderr, "seek_file_index: found file %s ranging %lds through %lds\n", 
            cur_file->filename, 
            cur_file->prev == cur_file ? 0 : cur_file->prev->time_elapsed_file.tv_sec,
            cur_file->time_elapsed_file.tv_sec);
#endif

    /* Now find the CLRSCR preceding seekpoint  */
    cur_clrscr = cur_file->first_clrscr;
    /* when_we_are is already correct            */ 
    while (timeval_diff(cur_clrscr->time_elapsed_cls, seek_target).tv_sec >0 &&
            cur_clrscr->next != NULL) {
        when_we_are = cur_clrscr->time_elapsed_cls;
        cur_clrscr = cur_clrscr->next;
    }

#ifdef DEBUG
    fprintf(stderr, "seek_file_index: found clrscr at %ldb ranging %lds through %lds\n", 
            cur_clrscr->record_start, 
            cur_clrscr->prev == cur_clrscr ? 0 : cur_clrscr->prev->time_elapsed_cls.tv_sec,
            cur_clrscr->time_elapsed_cls.tv_sec);
    if (cur_clrscr->next == NULL)
        fprintf(stderr, "Note: this is the last clrscr of this file\n");
#endif

    /* switch fp to whichever file/record the index points to */
#ifdef DEBUG
    fprintf(stderr, "seek_file_index: switching to file %s\n", cur_file->prev->filename);
#endif
    freopen(cur_file->filename, "r", fp);
    fseek(fp, cur_clrscr->record_start, SEEK_SET);
    /* the elapsed time is found at end of previous clrscr */
    return (cur_clrscr->prev->time_elapsed_cls);
}
/****** /ObOlli *****/

double
ttywait (struct timeval prev, struct timeval cur, double speed)
{
    static struct timeval drift = {0, 0};
    struct timeval start;
    struct timeval diff = timeval_diff(prev, cur);
    fd_set readfs;

    gettimeofday(&start, NULL);

    assert(speed != 0);
    diff = timeval_diff(drift, timeval_div(diff, speed));
    if (diff.tv_sec < 0) {
	diff.tv_sec = diff.tv_usec = 0;
    }

#define JUMPBASE 15         /* base of how much to jump, sec*speed    */
#define JUMP_SCALE 10       /* scaling for next bigger jump     */

    FD_SET(STDIN_FILENO, &readfs);
    /* 
     * We use select() for sleeping with subsecond precision.
     * select() is also used to wait user's input from a keyboard.
     *
     * Save "diff" since select(2) may overwrite it to {0, 0}. 
     */
    struct timeval orig_diff = diff;
    select(1, &readfs, NULL, NULL, &diff);
    diff = orig_diff;  /* Restore the original diff value. */
    if (FD_ISSET(0, &readfs)) { /* a user hits a character? */
        char c, c2, c3;;
        read(STDIN_FILENO, &c, 1); /* drain the character */
        switch (c) {
            case '+':
            case 'f':
                speed *= 2;
                break;
            case '-':
            case 's':
                speed /= 2;
                break;
            case '1':
                speed = 1.0;
                break;
/****** ObOlli *****/
            case '\033':    /* ESC starts a key sequence        */
#define JUMPBASE 15         /* base of how much to jump, sec    */
#define JUMP_SCALE 10       /* scaling for next bigger jump     */
                read(STDIN_FILENO, &c2, 1); /* drain the next character */
                switch (c2) {
                    case 'O':    /* for arrow keys (don't ask me)  */
                        read(STDIN_FILENO, &c3, 1); /* drain the next character */
                        switch (c3) {
                            case 'D':   /* right arrow  */
                                seek_request.tv_sec += (speed * -JUMPBASE);
                                break;
                            case 'C':   /* left arrow   */
                                seek_request.tv_sec += (speed * JUMPBASE);
                                break;
                            case 'A':   /* up arrow     */
                                seek_request.tv_sec += (speed * -JUMPBASE * JUMP_SCALE);
                                break;
                            case 'B':   /* down arrow   */
                                seek_request.tv_sec += (speed * JUMPBASE * JUMP_SCALE);
                                break;
                        }
                        break;
                    case '[':    /* for PgUp, PgDown */
                        read(STDIN_FILENO, &c3, 1); /* drain the next character */
                        switch (c3) {
                            case '5':   /* PgUp     */
                                seek_request.tv_sec += (speed * -JUMPBASE * JUMP_SCALE * JUMP_SCALE);
                                break;
                            case '6':   /* PgDown   */
                                seek_request.tv_sec += (speed * JUMPBASE * JUMP_SCALE * JUMP_SCALE);
                                break;
                        }
                        break;
                    default:               /* unknown esc sequence  */ 
                        // printf("Error: got keycode %d from keyboard", c);
                        // exit(EXIT_FAILURE);      /* should never happen */
                        break;
                }
                break;
        }
/****** /ObOlli *****/            
        drift.tv_sec = drift.tv_usec = 0;
    } else {
	    struct timeval stop;
	    gettimeofday(&stop, NULL);
	    /* Hack to accumulate the drift */
	    if (diff.tv_sec == 0 && diff.tv_usec == 0) {
            diff = timeval_diff(drift, diff);  // diff = 0 - drift.
        }
	    drift = timeval_diff(diff, timeval_diff(start, stop));
    }
    return speed;
}

double
ttynowait (struct timeval prev, struct timeval cur, double speed)
{
    /* do nothing */
    return 0; /* Speed isn't important. */
}

int
ttyread (FILE *fp, Header *h, char **buf)
{
    if (read_header(fp, h) == 0) {
	return 0;
    }

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

void
ttyplay (FILE *fp, double speed, ReadFunc read_func, 
	 WriteFunc write_func, WaitFunc wait_func)
{
    int first_time = 1;
    struct timeval prev;
    /* zero seek_request flag/distance  */
    seek_request.tv_sec = seek_request.tv_usec = 0;
    time_elapsed.tv_sec = time_elapsed.tv_usec = 0;

    setbuf(stdout, NULL);
    setbuf(fp, NULL);

    while (1) {
        char *buf;
        Header h;

        if (read_func(fp, &h, &buf) == 0) {
            break;
        }

        if (!first_time) {
            speed = wait_func(prev, h.tv, speed);

/****** ObOlli *****/
            /* use index_head as flag we indeed have files to seek in */
            if (index_head != NULL && seek_request.tv_sec != 0) {
                struct timeval seek_target = timeval_add(time_elapsed, seek_request);
                /* seek_file_index seeks header preceding CLRSCR and 
                    adjusts fp to point to this file/pos. 
                    returns timeval elapsed from start-of-all.          */
#ifdef DEBUG
                fprintf(stderr, "Seek of %lds requested at %.3f, seek_target is %.3fs\n", 
                        seek_request.tv_sec, 
                        (float)time_elapsed.tv_sec+(float)time_elapsed.tv_usec/1000000,
                        (float)seek_target.tv_sec+(float)seek_target.tv_usec/1000000);
#endif
                time_elapsed = seek_file_index(fp, seek_target);
#ifdef DEBUG
                fprintf(stderr, "Position at beginning of clrscr record %lds\n", time_elapsed.tv_sec);
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
                                timeval_add(time_elapsed, time_diff)).tv_sec < 0) {
#ifdef DEBUG                            
                            fprintf(stderr, "Quitting seek: next step would go into future by %lds\n",
                                -(timeval_sub(seek_target,
                                timeval_add(time_elapsed, time_diff)).tv_sec));
#endif                        
                            write_func(buf, h.len);     /* output the record    */
                            break;
                        }
                    }
                    cur_pos = ftell(fp);
                    time_elapsed = timeval_add(time_elapsed, time_diff);   /* where-we-are         */
                    write_func(buf, h.len);             /* output the record    */
                    prev = h.tv;
                }
                /* sub-CLRSCR seek ends here, reposition back to 
                   preceding recordfp & clear seek pos/flag         */
                fseek(fp, cur_pos, SEEK_SET);
                seek_request.tv_sec = seek_request.tv_usec = 0;   /* seek all done    */
#ifdef DEBUG
                struct timeval offset = timeval_diff(seek_target, time_elapsed);
                fprintf(stderr, "Seek complete at position %.3fs %ldb, offset %.3fs\n\n", 
                        (float)time_elapsed.tv_sec + (float) time_elapsed.tv_usec/1000000,
                        cur_pos,
                        (float)offset.tv_sec + (float) offset.tv_usec/1000000);
#endif
            }
/****** /ObOlli *****/
            time_elapsed = timeval_add(time_elapsed, timeval_sub(h.tv, prev));
        }
        first_time = 0;

        write_func(buf, h.len);

        prev = h.tv;
        free(buf);
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
usage (void)
{
    printf("Usage: ttyplay [OPTION] [FILE]\n");
    printf("  -s SPEED Set speed to SPEED [1.0]\n");
    printf("  -n       No wait mode\n");
    printf("  -p       Peek another person's ttyrecord\n");
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
initcurses ()
{
  const char *defterm = "xterm";
  int utf8esc = 1; /* bool */
  printf("\033[2J");
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

  if (utf8esc) (void) write(1, "\033%G", 3);
#endif
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

int 
main (int argc, char **argv)
{
    double speed = 1.0;
    ReadFunc read_func  = ttyread;
    WaitFunc wait_func  = ttywait;
    ProcessFunc process = ttyplayback;
    FILE *input = NULL;

    set_progname(argv[0]);
    while (1) {
        int ch = getopt(argc, argv, "s:np");
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
        default:
            usage();
        }
    }

    if (optind < argc) {
        index_head = create_file_index(optind, argc, argv);
        time_elapsed.tv_sec = time_elapsed.tv_usec = 0;
#ifdef DEBUG
        fprintf(stderr, "Opening initial file %s\n", argv[optind]);
#endif                
        input = efopen(argv[optind], "r");
    } else {
        input = input_from_stdin();
        index_head = NULL;
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
    initcurses();
#endif
    signal(SIGINT,interrupt);
    if (index_head) {   /* we do have (indexed) file parms */
        while (1) {
            process(input, speed, read_func, wait_func);
            if (++optind < argc) {
#ifdef DEBUG
                fprintf(stderr, "Opening next file %s\n\n", argv[optind]);
#endif
                freopen(argv[optind], "r", input);
                assert(input != NULL);
            } else break;
        }
    } else {            /* input from stream (like stdin)   */
        process(input, speed, read_func, wait_func);
    }
#ifdef USE_CURSES
    endwin();
#else
    tcsetattr(0, TCSANOW, &old);  /* Return terminal state */
#endif

    return 0;
}
