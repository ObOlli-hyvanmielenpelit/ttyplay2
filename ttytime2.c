/*
 * Copyright (c) 1980 Regents of the University of California.
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

/*
 * ttytime2
 * 
 * small (and in places quite ugly) utility to analyze a bunch of ttyrec files
 * 
 * usage: ttytime2 file [file]...
 * 
 * prints for each file
 *  * duration in sec
 *  * same in HH:mm:ss
 *  * number of ttyrec records
 *  * filename
 * at end of run, prints 
 *  * log2 distribution of all record lengths with log2 histogram
 *  * number of total records with log2 magnitude
 *  * log2 distribution of time duration of records with log2 histogram
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <math.h>
#include <libgen.h>

#include "io.h"
#include "ttyrec.h"

int calc_time(const char *filename, int *times, int *lengths, int *records)
{
    Header start, end, prev, curr;
    FILE *fp;
    int i, j;

    fp = efopen(filename, "r");

    read_header(fp, &start);
    curr = start;
    fseek(fp, start.len, SEEK_CUR);
    while (1)
    {
        Header h;
        if (read_header(fp, &h) == 0)
        {
            break;
        }
        end = h;

        prev = curr;
        curr = h;

        (*records)++;
        i = curr.tv.tv_sec - prev.tv.tv_sec;
        if (i > 0)
        {
            i = log2(i);
        }
        times[i]++;
        j = curr.len;
        if (j > 0)
        {
            j = log2(j);
        }
        lengths[j]++;

        fseek(fp, h.len, SEEK_CUR);
    }
    return end.tv.tv_sec - start.tv.tv_sec;
}

int main(int argc, char **argv)
{
    int i;
    int lengths[20], times[30];
    set_progname(argv[0]);

    if (argc == 1)
    {
        char *pgmname = strdup(argv[0]);
        printf("Usage: %s file [file]...\n", basename(pgmname));
        exit(1);
    }

    for (i = 20; i; lengths[--i] = 0)
        ;
    for (i = 30; i; times[--i] = 0)
        ;

    printf("Replay time of file(s) (sec, HH:mm:ss) and number of records:\n");
    int total_seconds=0;
    for (i = 1; i < argc; i++)
    {
        char *filename = argv[i];
        int records = 0;

        int duration = calc_time(filename, times, lengths, &records);
        int hrs = (int)duration / 3600;
        int min = (int)(duration - hrs * 3600) / 60;
        int sec = duration - hrs * 3600 - min * 60;
        printf("(%7d	%d:%02d:%02d) %d %s\n", duration, hrs, min, sec, records, filename);
        total_seconds += duration;
    }
    printf("%d file(s) analyzed.\n\n", argc-1);

    int j;

    printf("Length distribution of screen updates:\n");
    int records = 0;
    for (j = 0; j < 20; j++)
    {
        if (lengths[j])
        {
            records += lengths[j];
            printf("< %d\t(2^%d)\t%d\t", (int)(pow(2, j)), j, lengths[j]);
            for (int foo, k = 20; k; --k)
            {
                if (foo = (int)(lengths[j] / pow(2, k)))
                {
                    putchar('*');
                    lengths[j] -= (int)pow(2, k);
                }
            }
            putchar('\n');
        }
    }
    printf("Total records: %d, magnitude = 2^%d\n", records, (int)log2(records));

    printf("Duration distribution of actions, sec:\n");

    for (j = 0; j < 30; j++)
    {
        if (times[j])
        {
            printf("< %d\t(2^%d)\t%d\t", (int)pow(2, j), j, times[j]);
            for (int k = 20; k; --k)
            {
                if ((int)(times[j] / pow(2, k)))
                {
                    putchar('*');
                    times[j] -= (int)pow(2, k);
                }
            }
            putchar('\n');
        }
    }
    printf("Total time: %d sec.\n", total_seconds);
    printf("Average of action durations: %1.2f sec\n", (float) total_seconds / records);
    return 0;
}
