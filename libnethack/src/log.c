/* vim:set cin ft=c sw=4 sts=4 ts=8 et ai cino=Ls\:0t0(0 : -*- mode:c;fill-column:80;tab-width:8;c-basic-offset:4;indent-tabs-mode:nil;c-file-style:"k&r" -*-*/
/* Last modified by Alex Smith, 2014-04-25 */
/* Copyright (c) Daniel Thaler, 2011.                             */
/* NetHack may be freely redistributed.  See license for details. */

#include "hack.h"
#include "patchlevel.h"
#include "iomodes.h"
#include <zlib.h>
/* stdint.h, inttypes.h let us printf long longs portably */
#define __STDC_FORMAT_MACROS
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <errno.h>

/* #define DEBUG */

#define MENU_ID_OFFSET 4

static void log_reset(void);
static void log_binary(const char *buf, int buflen);
static long get_log_offset(void);
static long get_log_last_newline(void);
static void load_gamestate_from_binary_save(boolean maybe_old_version);

static boolean full_read(int fd, void *buffer, int len);
static boolean full_write(int fd, const void *buffer, int len);

static const char * status_string(enum nh_log_status state);
static enum nh_log_status status_from_string(const char * str);

/* We assume ASCII. (The whole log thing is done in binary.) */
static_assert('A' == 65 && 'a' == 97,
              "The execution character set must be ASCII or extended ASCII");

/***** Error handling *****/

/* The save file format was incorrect. */
static noreturn void
error_reading_save(const char *reason)
{
    if (strchr(reason, '%'))
        raw_printf(reason, get_log_offset());
    else if (*reason)
        raw_printf("%s", reason);

    log_reset();
    terminate(ERR_RESTORE_FAILED);
}

/*
 * The save file is too long, and the end needs to be removed. This happens in
 * three cases:
 *
 * - The save file was mostly in a correct format, but contains incomplete lines
 *   or some similar issue that prevents us reading the end of it;
 *
 * - The save file contains a partial turn from a version of the game engine
 *   that the process responsible for updating it doesn't know how to replay;
 *
 * - A wizard-mode user has explicitly said "this save file needs its end
 *   removing".
 *
 * The argument is an offset just past a newline (get_log_last_newline(),
 * get_log_start_of_turn_offset(), program_state.end_of_gamestate_location
 * respectively for the three cases above).
 *
 * In every case, the user will be prompted to confirm the recovery, and given a
 * chance to decline it. If the recovery is declined, the game will be
 * terminated (ERR_RECOVER_REFUSED). If the recovery is accepted, it will be
 * performed, and then the engine will reload the game via RESTART_PLAY.
 *
 * This function must not call panic(), because it is called /from/ panic().
 */
noreturn void
log_recover(long offset)
{
    int n;
    const int *selected;
    char newline_check[2];
    const char *buf;
    struct nh_menulist menu;
    boolean ok = TRUE;

    /* Check to make sure that the offset we were given is actually sane.
       If it isn't, the recover will have to be done manually. */

    if (offset > 0) {
        lseek(program_state.logfile, offset - 1, SEEK_SET);
        ok = full_read(program_state.logfile, newline_check, 1);
        if (ok && *newline_check != '\n')
            ok = FALSE;
    } else
        ok = FALSE;

    if (!ok)
        error_reading_save("Trying to recover from corrupted backup save");

    /* Treat everything that happens here as outside the main flow of the game;
       we don't want to replay a recovery that happens in one process in another
       process.  (This works correctly even if multiple processes reach this
       menu at the same time; in that case, if any recovers the file, the menu
       will close for all the others and they will reload from the newly
       recovered file.) */
    program_state.in_zero_time_command = TRUE;

    init_menulist(&menu);

    add_menutext(&menu,
                 "The gamestate or save file is internally inconsistent.");
    add_menutext(&menu,
                 "However, the game can be recovered from a backup save.");
    buf = msgprintf("This will lose approximately %.4g%% of your progress.",
                    1.0 - ((float)offset /
                           (float)lseek(program_state.logfile, 0, SEEK_END)));
    add_menutext(&menu, buf);
    add_menutext(&menu, "");

    add_menuitem(&menu, 1, "Automatically recover the save file", 'R', FALSE);
    add_menuitem(&menu, 2, "Leave the save file to be recovered manually", 'Q',
                 FALSE);

    n = display_menu(&menu, "The save file is corrupted...",
                     PICK_ONE, PLHINT_URGENT, &selected);

    if (n && selected[0] == 1) {
        /* Automatic recovery. */

        /* Get a lock on the file. This is a little like start_updating_logfile,
           but without so many checks (because we /expect/ the file to be
           inconsistent in some way); we don't care that the file hasn't
           lengthened, and we don't care whether we're viewing or not (any
           process is allowed to recover a corrupted file). */

        if (!change_fd_lock(program_state.logfile, LT_WRITE, 2))
            panic("Could not upgrade to write lock on logfile");

        /* TODO: check recovery count */

        /* Increase the recovery count. */

        program_state.expected_recovery_count++;
        buf = msgprintf("NHGAME %08x %d.%03d.%03d\x0a",
                        program_state.expected_recovery_count,
                        VERSION_MAJOR, VERSION_MINOR, PATCHLEVEL);

        lseek(program_state.logfile, 0, SEEK_SET);
        if (!full_write(program_state.logfile, buf, strlen(buf))) {
            /* This is bad enough to panic, but we can't panic, so... */
            raw_printf("Could not write to save file to recover it!\n");
            terminate(ERR_RESTORE_FAILED);
        }

        /* Truncate the file. */
        if (ftruncate(program_state.logfile, offset) < 0) {
            raw_printf("Could not truncate save file during recovery!\n");
            terminate(ERR_RESTORE_FAILED);
        }

        /* Relinquish the lock, and reload the file. */
        if (!change_fd_lock(program_state.logfile, LT_READ, 1))
            panic("Could not downgrade to read lock on logfile");
        terminate(RESTART_PLAY);

    } else {
        /* Manual recovery. */
        terminate(ERR_RECOVER_REFUSED);
    }
}

/* The save file was in a correct format, but referred to something that
   couldn't possibly happen in the gamestate. */
static noreturn void
log_desync(void)
{
    /*
     * The behaviour we want for this function:
     *
     * - If there is a save diff or backup beyond this point, move the
     *   gamestate position forwards to the next such save diff or backup;
     *
     * - If we have write control over the save file (probably mutually
     *   exclusive with the previous case, but should defer to it if we
     *   implement a feature where it isn't), truncate the log to the
     *   start of the turn (i.e. just use log_recover);
     *
     * - Otherwise, ignore the rest of the current turn.
     *
     * The idea is that if the save file doesn't reflect the engine we're
     * using, then either updating it is our job, in which case we need to
     * switch over to our engine even if that involves replaying the turn;
     * or updating it isn't our job, in which case we simply reproduce the
     * output of some more qualified engine.
     */

    panic("log_desync unimplemented");
}

/***** Base 64 handling *****/

static const unsigned char b64e[64] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char b64d[256] = {
    /* 32 control chars */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* ' ' - '/' */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 62, 0, 0, 0, 63,
    /* '0' - '9' */ 52, 53, 54, 55, 56, 57, 58, 59, 60, 61,
    /* ':' - '@' */ 0, 0, 0, 0, 0, 0, 0,
    /* 'A' - 'Z' */ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
    17, 18, 19, 20, 21, 22, 23, 24, 25,
    /* '[' - '\'' */ 0, 0, 0, 0, 0, 0,
    /* 'a' - 'z' */ 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51
};

static int
base64size(int n)
{
    return compressBound(n) * 4 / 3 + 4 + 12;   /* 12 for $4294967296$ */
}

static void
base64_encode_binary(const unsigned char *in, char *out, int len)
{
    int i, pos, rem;
    unsigned long olen = compressBound(len);
    unsigned char *o = malloc(olen);

    if (compress2(o, &olen, in, len, Z_BEST_COMPRESSION) != Z_OK) {
        panic("Could not compress input data!");
    }

    pos = sprintf(out, "$%d$", len);

    if (pos + olen >= len) {
        pos = 0;
        olen = len;
    } else
        in = o;

    for (i = 0; i < (olen / 3) * 3; i += 3) {
        out[pos] = b64e[in[i] >> 2];
        out[pos + 1] = b64e[(in[i] & 0x03) << 4 | (in[i + 1] & 0xf0) >> 4];
        out[pos + 2] = b64e[(in[i + 1] & 0x0f) << 2 | (in[i + 2] & 0xc0) >> 6];
        out[pos + 3] = b64e[in[i + 2] & 0x3f];
        pos += 4;
    }

    rem = olen - i;
    if (rem > 0) {
        out[pos] = b64e[in[i] >> 2];
        out[pos + 1] =
            b64e[(in[i] & 0x03) << 4 |
                 (rem == 1 ? 0 : (in[i + 1] & 0xf0) >> 4)];
        out[pos + 2] = (rem == 1) ? '=' : b64e[(in[i + 1] & 0x0f) << 2];
        out[pos + 3] = '=';
        pos += 4;
    }

    free(o);

    out[pos] = '\0';
}


static void
base64_encode(const char *in, char *out)
{
    base64_encode_binary((const unsigned char *)in, out, strlen(in));
}

static int
base64_strlen(const char *in)
{
    /* If the input is uncompressed, just return its size. If it's compressed,
       read the size from the header. */
    if (*in != '$')
        return strlen(in);
    return atoi(in + 1);
}

/* TODO: This should be communicating the end position of the base 64 data. */
static void
base64_decode(const char *in, char *out, int outlen)
{
    int i, len = strlen(in), pos = 0, olen;
    char *o = out;

    olen = outlen;
    if (*in == '$') {
        o = malloc(len);
        olen = len;
    }

    for (i = 0; i < len; i += 4) {

        /* skip data between $ signs, it's used for the header for compressed
           binary data */
        if (in[i] == '$')
            for (i += 2; in[i - 1] != '$' && in[i]; i++) {}

        /* decode blocks; padding '=' are converted to 0 in the decoding table
           */
        if (pos < olen)
            o[pos] = b64d[(int)in[i]] << 2 | b64d[(int)in[i + 1]] >> 4;
        if (pos + 1 < olen)
            o[pos + 1] = b64d[(int)in[i + 1]] << 4 | b64d[(int)in[i + 2]] >> 2;
        if (pos + 2 < olen)
            o[pos + 2] =
                ((b64d[(int)in[i + 2]] << 6) & 0xc0) | b64d[(int)in[i + 3]];
        pos += 3;

    }

    i -= 4;
    if ((in[i + 2] == '=' || !in[i + 2]) && (in[i + 3] == '=' || !in[i + 3]))
        pos--;
    if ((in[i + 1] == '=' || !in[i + 2]) && (in[i + 2] == '=' || !in[i + 3]))
        pos--;

    if (pos < olen)
        o[pos] = 0;
    else
        error_reading_save("Uncompressed base64 data was too long at %ld\n");

    if (*in == '$') {

        unsigned long blen = base64_strlen(in);
        if (blen > outlen) {
            free(o);
            error_reading_save("Compressed base64 data was too long at %ld\n");
        }
        int errcode = uncompress((unsigned char *)out, &blen,
                                 (unsigned char *)o, pos);

        free(o);
        if (errcode != Z_OK) {
            raw_printf("Decompressing save file failed at %ld: %s\n",
                       get_log_offset(),
                       errcode == Z_MEM_ERROR ? "Out of memory" : errcode ==
                       Z_BUF_ERROR ? "Invalid size" : errcode ==
                       Z_DATA_ERROR ? "Corrupted file" : "(unknown error)");
            error_reading_save("");
        }
    }
}

/***** Log I/O *****/

static int lvprintf(const char *fmt, va_list vargs) PRINTFLIKE(1,0);
static int lprintf(const char *fmt, ...) PRINTFLIKE(1,2);

/* full_read and full_write are versions of the POSIX read/write that never do
   short reads or short writes. They return TRUE if all the bytes requested were
   read/written; FALSE if something went wrong (either because there weren't
   enough bytes in the file, or because of an I/O error). */
static boolean
full_read(int fd, void *buffer, int len)
{
    int rv;
    long o = lseek(fd, 0, SEEK_CUR);
    errno = 0;
    rv = read(fd, buffer, len);
    if (rv < 0 && errno == EINTR) {
        lseek(fd, o, SEEK_SET);
        return full_read(fd, buffer, len);
    }
    if (rv <= 0 || rv > len)
        return FALSE;
    if (rv == len)
        return TRUE;
    return full_read(fd, ((char *)buffer) + rv, len - rv);
}
static boolean
full_write(int fd, const void *buffer, int len)
{
    int rv;
    long o = lseek(fd, 0, SEEK_CUR);
    errno = 0;
    rv = write(fd, buffer, len);
    if (rv < 0 && errno == EINTR) {
        lseek(fd, o, SEEK_SET);
        return full_write(fd, buffer, len);
    }
    if (rv <= 0 || rv > len)
        return FALSE;
    if (rv == len)
        return TRUE;
    return full_write(fd, ((const char *)buffer) + rv, len - rv);
}


/* TODO: Let's get rid of the hardcoded buffer length here some time.

   Warning to anyone attempting this: vsnprintf's return value is broken on
   Windows, so you'll need to use a loop expanding the buffer size there. (See
   libuncursed for an example of the technique.)
*/
static int
lvprintf(const char *fmt, va_list vargs)
{
    char outbuf[4096];
    int size;

    size = vsnprintf(outbuf, sizeof (outbuf), fmt, vargs);

    if (!full_write(program_state.logfile, outbuf, size))
        panic("Could not write text content to the log.");

    return size;
}

static int
lprintf(const char *fmt, ...)
{
    va_list vargs;
    int ret;

    va_start(vargs, fmt);
    ret = lvprintf(fmt, vargs);
    va_end(vargs);

    return ret;
}

static void
log_binary(const char *buf, int buflen)
{
    char *b64buf;

    if (program_state.logfile == -1)
        return;

    b64buf = malloc(base64size(buflen));
    base64_encode_binary((const unsigned char *)buf, b64buf, buflen);

    /* don't use lprintf, b64buf might be too big for the buffer used by
       lprintf */
    if (!full_write(program_state.logfile, b64buf, strlen(b64buf)))
        panic("Could not write binary content to the log.");

    free(b64buf);
}

/* Reads a line starting from the current file pointer. Returns NULL if the line
   is incomplete or spos is past EOF, otherwise mallocs enough space for the
   line and returns it. The file pointer is left at the newline, or in an
   unpredictable location in case of error. */
static char *
lgetline_malloc(int fd)
{
    char *inbuf = NULL;
    long inbuflen = 0;  /* number of bytes read */
    long fpos = 0;      /* file pointer, relative to its original location */
    void *nlloc = NULL;
    boolean at_eof = FALSE;

    do {
        inbuflen += 4;
        inbuflen *= 3;
        inbuflen /= 2;
        inbuf = realloc(inbuf, inbuflen);
        if (!inbuf)
            panic("Out of memory in lgetline_malloc");

        /* Return values from read:
           negative return = error
           zero return = EOF
           positive return = success, even if it didn't return as many
           bytes as expected (in which case we must rerun read) */
        inbuflen = read(fd, inbuf + fpos, inbuflen - fpos) + fpos;

        /* Most errors are a problem. However, if the read is interrupted with
           zero bytes read, then this is reported as an "error" EINTR rather
           than a count of zero, so as to distinguish it from EOF. (This API
           could have been better designed, really, but we're stuck with it
           now.) */
        at_eof = inbuflen == fpos;

        if (inbuflen < fpos) {
            if (errno != EINTR) {
                free(inbuf);
                return NULL;
            }
            inbuflen = fpos; /* we successfully read 0 bytes */
        }

        /* We want to break out of the loop if we're at EOF (inbuflen ==
           fpos) or if a newline was found. */
        nlloc = memchr(inbuf, '\x0a', fpos);
        if (at_eof)
            break;

        fpos = inbuflen;
    } while (!nlloc);

    if (!nlloc && fpos > 0) {
        /* The save file ends with a partial line, something that should never
           happen in normal operation (it indicates that a process crashed in
           the middle of a write). Get rid of the partial line. */

        free(inbuf);
        log_recover(get_log_last_newline());

    } else if (!nlloc && fpos == 0) {
        /* At EOF, which is at the start of the line. */
        free(inbuf);
        return NULL;
    }

    /* We could trim inbuf down to size, but there's not a lot of point,
       especially as in most cases it's going to be free'd almost
       immediately. We must, however, undo the excess read via seeking the
       cursor backwards to just after the final newline. */
    *(char *)nlloc = '\0';
    lseek(fd, ((char *)nlloc - inbuf) + 1 - fpos, SEEK_CUR);

    return inbuf;
}


/* Returns the offset to the current file pointer in the log. */
static long
get_log_offset(void)
{
    return lseek(program_state.logfile, 0, SEEK_CUR);
}

/* Returns the offset just past the end of the last valid line in the log.  This
   is used to determine how much of the save file is meaningful after a process
   crashes during a write. */
static long
get_log_last_newline(void)
{
    long o = get_log_offset();
    long rv;
    char inchar[2] = {0, 0};
    lseek(program_state.logfile, -1, SEEK_END);

    /* Run through the file backwards, reading one char at a time until we find
       a newline. (This is rather less efficient than reading blocks at a time
       and scanning them backwards, but the code is much simpler, and given that
       this only runs in very exceptional circumstances and most lines are short
       it's unlikely to be a performance bottleneck.) */

    while (full_read(program_state.logfile, inchar, 1) && *inchar != '\n')
        if (!lseek(program_state.logfile, -2, SEEK_CUR))
            break;

    if (*inchar == '\n') {
        /* We found our newline. The file pointer is now just past it. */
        rv = get_log_offset();
        lseek(program_state.logfile, o, SEEK_SET);
        return rv;
    }

    /* We didn't find any newlines in the file. This is pretty massively bad.
       However, it can only happen if we crashed right at the start of the new
       game process, in which case, may as well just start a new game. */
    error_reading_save("save file header is incomplete");
}

/* Returns the offset corresponding to the start of the current turn
   (technically speaking, the most recent neutral turnstate). This is
   program_state.binary_save_location plus one line.

   This function assumes we have at least a read lock on the log, but that's
   always the case during normal gameplay. */
long
get_log_start_of_turn_offset(void)
{
    long o = get_log_offset();
    long rv;
    void *save_diff_line;

    lseek(program_state.logfile, program_state.binary_save_location, SEEK_SET);

    /* Move forwards one line. In the exceptional case that we have a binary save
       location without a matching binary save (which is probably impossible, but
       may as well handle it just in case it isn't), we treat it the same way as
       an incomplete line. */

    save_diff_line = lgetline_malloc(program_state.logfile);

    if (!save_diff_line)
        log_recover(get_log_last_newline());

    free(save_diff_line);

    /* Now return the offset we found, taking care to restore the file
       pointer. */
    rv = get_log_offset();
    lseek(program_state.logfile, o, SEEK_SET);
    return rv;
}


/***** Locking *****/

/*
 * To be able to update the logfile, we need to ensure the following:
 *
 * * We have a write lock on the logfile;
 * * The logfile hasn't grown past where we expect it to be (i.e. there are
 *   no lines past the one that starts at the gamestate location)
 * * The recovery count is what we expect it to be.
 *
 * change_fd_lock is responsible for doing the necessary complexities to
 * grab the lock, but if something goes wrong with the other two rules, that
 * isn't an error: it just means that we're working from the wrong gamestate.
 * In some cases, the caller could recover via rereading the current line, but
 * a generic mechanism that works in all cases is simply to use terminate() to
 * replay the turn.
 *
 * Before calling this function, ensure that the current connection can play the
 * current game and the game has been synced to a target location of EOF
 * (i.e. !viewing).
 */
static boolean
start_updating_logfile(boolean ok_not_at_end)
{
    long o;

    if (program_state.viewing)
        panic("Logfile update while watching a game");

    if (!change_fd_lock(program_state.logfile, LT_WRITE, 2))
        panic("Could not upgrade to write lock on logfile");

    /* TODO: check recovery count */

    lseek(program_state.logfile,
          program_state.end_of_gamestate_location, SEEK_SET);

    /* now the file pointer should be at EOF */
    o = get_log_offset();

    if (o != lseek(program_state.logfile, 0, SEEK_END)) {
        if (ok_not_at_end)
            return FALSE;

        if (!change_fd_lock(program_state.logfile, LT_READ, 1))
            panic("Could not downgrade to read lock on logfile");
        terminate(RESTART_PLAY);
    }

    return TRUE;
}

static void
stop_updating_logfile(int lines_added)
{
    if (lines_added == 1) {

        program_state.gamestate_location =
            program_state.end_of_gamestate_location;

        program_state.end_of_gamestate_location =
            lseek(program_state.logfile, 0, SEEK_END);

    } else if (lines_added > 1) {
        panic("logfile updates may add at most 1 line");
    }

    if (!change_fd_lock(program_state.logfile, LT_READ, 1))
        panic("Could not downgrade to read lock on logfile");
}

/***** Creating specific log entries *****/

#define SECOND_LOGLINE_LEN      78
#define SECOND_LOGLINE_LEN_STR "78"
static_assert(SECOND_LOGLINE_LEN < COLNO, "SECOND_LOGLINE_LEN too long");


#define STATUS_LEN 4
#define STATUS_LEN_STR "4"
const char *
status_string(enum nh_log_status state) {
    switch (state) {
    case LS_SAVED:
        return "save";
    case LS_DONE:
        return "done";
    default:
        return 0;
    }
}


enum nh_log_status
status_from_string(const char * str) {
    if (!strcmp(str, "save"))
        return LS_SAVED;
    else if (!strcmp(str, "done"))
        return LS_DONE;
    else
        return LS_INVALID;
}


void
log_newgame(microseconds start_time, unsigned int seed)
{
    char encbuf[ENCBUFSZ];
    const char *role;
    long start_of_third_line;

    /* There's no portable way to print an unsigned long long. The standards say
       %llu / %llx, but Windows doesn't follow them. It does, however, correctly
       obey the standards for uint_least64_t (because those require the
       specifier to be provided by a header file, which compensates for the fact
       that it's nonstandard). */
    uint_least64_t start_time_l64 = (uint_least64_t) start_time;

    if (u.initgend == 1 && roles[u.initrole].name.f)
        role = roles[u.initrole].name.f;
    else
        role = roles[u.initrole].name.m;

    lprintf("NHGAME %" STATUS_LEN_STR "." STATUS_LEN_STR "s "
            "00000001 %d.%03d.%03d\x0a",
            status_string(LS_SAVED), VERSION_MAJOR, VERSION_MINOR, PATCHLEVEL);
    lprintf("%" SECOND_LOGLINE_LEN_STR "s\x0a", "(new game)");
    start_of_third_line = get_log_offset();

    base64_encode(u.uplname, encbuf);
    lprintf("%0" PRIxLEAST64 " %x %d %s %.3s %.3s %.3s %.3s\x0a",
            start_time_l64, seed, wizard ? MODE_WIZARD : discover ? MODE_EXPLORE
            : MODE_NORMAL, encbuf, role, races[u.initrace].noun,
            genders[u.initgend].adj, aligns[u.initalign].adj);

    /* The gamestate location is meant to be set to the start of the last line
       of the log, when the log's in a state ready to be updated. Ensure that
       invariant now. */
    program_state.gamestate_location = start_of_third_line;
    program_state.end_of_gamestate_location = get_log_offset();

    program_state.expected_recovery_count = 1;
}

void
log_game_over(const char *death)
{
    start_updating_logfile(FALSE);
    lseek(program_state.logfile,
          strlen("NHGAME "), SEEK_SET);
    lprintf("%" STATUS_LEN_STR "." STATUS_LEN_STR "s", status_string(LS_DONE));

    lseek(program_state.logfile,
          strlen("NHGAME  00000001 4.000.000\x0a") + STATUS_LEN, SEEK_SET);
    lprintf("%" SECOND_LOGLINE_LEN_STR "." SECOND_LOGLINE_LEN_STR "s", death);
    stop_updating_logfile(0);
}

void
log_backup_save(void)
{
    if (program_state.logfile == -1)
        panic("log_backup_save called with no logfile");

    if (!start_updating_logfile(TRUE))
        return;

    program_state.binary_save_location = 0;
    if (program_state.binary_save_allocated)
        mfree(&program_state.binary_save);
    mnew(&program_state.binary_save, NULL);
    program_state.binary_save_allocated = TRUE;
    savegame(&program_state.binary_save);

    long o = get_log_offset();
    boolean is_newgame = program_state.save_backup_location == 0;
    lprintf("*%08lx ", program_state.save_backup_location);
    program_state.save_backup_location = o;
    program_state.binary_save_location = o;
    log_binary(program_state.binary_save.buf, program_state.binary_save.pos);
    lprintf("\x0a");

    /* Record the location of this save backup in the appropriate place. */
    lseek(program_state.logfile, is_newgame ? o + 1 :
          program_state.last_save_backup_location_location, SEEK_SET);
    lprintf("%08lx", o);
    lseek(program_state.logfile, 0, SEEK_END);

    stop_updating_logfile(1);

    /* Verify that the save file loads correctly; it's better to fail fast
       than end up with a corrupted save. */
    load_gamestate_from_binary_save(FALSE);
}

void
log_neutral_turnstate(void)
{
    if (program_state.logfile == -1)
        panic("log_neutral_turnstate called with no logfile");

    /* A heuristic to work out whether to use a save diff or save backup
       line. */
    if (program_state.binary_save.pos <
        (program_state.gamestate_location -
         program_state.save_backup_location) / 2 || !program_state.ok_to_diff)

        log_backup_save();

    else {

        /* We're generating a save diff line. */
        struct memfile mf = program_state.binary_save;

        /* start_updating_logfile can cause a turn restart, so place it
           outside the allocation of the new binary save */
        if (!start_updating_logfile(TRUE))
            return;

        mnew(&program_state.binary_save, &mf);
        program_state.binary_save_location = 0;

        /* Save the game, and calculate a diff against the old location in
           the process. */
        savegame(&program_state.binary_save);

        program_state.binary_save_location = get_log_offset();

        mdiffflush(&program_state.binary_save);

        lprintf("~");
        log_binary(program_state.binary_save.diffbuf,
                   program_state.binary_save.diffpos);
        lprintf("\x0a");

        /* Make the new binary save absolute rather than relative, so that
           we can free the old one. */
        program_state.binary_save.relativeto = NULL;
        mfree(&mf);

        stop_updating_logfile(1);

        /* Check the gamestate, for the same reason as in log_backup_save(). */
        load_gamestate_from_binary_save(FALSE);
    }

    /* TODO: Periodically update the second line of the logfile. */
}

/* Update turntime in a manner that's safe within the log. */
void
log_time_line(void)
{
    uint_least64_t timediff;

    /* If we're in a zero-time command, this function would change turntime
       without logging it, which isn't what we'd want. Don't change it and don't
       log it either; we'll change it on the next real command instead. */
    if (program_state.in_zero_time_command)
        return;

    if (!log_replay_input(1, "+%" SCNxLEAST64, &timediff)) {
        log_replay_no_more_options();
        timediff = time_for_time_line() - flags.turntime;
    }

    flags.turntime += timediff;
    log_record_input("+%" PRIxLEAST64, timediff);
}

/* Ensure that a command that was meant to have no effect actually did have no
   effect. If it did have an effect, complain and restart the turn, which will
   have the effect of undoing the command (as informational-only commands are
   not saved anywhere). */
void
log_revert_command(void)
{
    struct memfile mf;

    mnew(&mf, NULL);
    savegame(&mf);

    if (!mequal(&program_state.binary_save, &mf, TRUE)) {
        mfree(&mf);
        impossible("Informational command changed the gamestate");
        terminate(RESTART_PLAY);
    }

    mfree(&mf);
}

/* Bones files must also be logged, since they are an input into the game
   state */
void
log_record_bones(struct memfile *mf)
{
    if (program_state.in_zero_time_command || program_state.viewing)
        return;

    if (program_state.input_was_just_replayed) {
        program_state.input_was_just_replayed = FALSE;
        return;
    }

    start_updating_logfile(FALSE);

    lprintf("B");
    log_binary(mf->buf, mf->len);
    lprintf("\x0a");

    stop_updating_logfile(1);
}

void
log_record_input(const char *fmt, ...)
{
    va_list vargs;

    if (program_state.in_zero_time_command)
        return;

    if (program_state.input_was_just_replayed) {
        program_state.input_was_just_replayed = FALSE;
        return;
    }

    start_updating_logfile(FALSE);

    va_start(vargs, fmt);
    lvprintf(fmt, vargs);
    lprintf("\x0a");
    va_end(vargs);

    stop_updating_logfile(1);
}

void
log_record_line(const char *l)
{
    if (program_state.in_zero_time_command || program_state.viewing)
        return;

    if (program_state.input_was_just_replayed) {
        program_state.input_was_just_replayed = FALSE;
        return;
    }

    start_updating_logfile(FALSE);

    lprintf("L");
    log_binary(l, strlen(l) + 1);
    lprintf("\x0a");

    stop_updating_logfile(1);
}

void
log_record_menu(boolean isobjmenu, const void *el)
{
    int nresults;
    const struct nh_objresult *cur_objresult = NULL; /* lint suppression */
    const int *cur_menuresult = NULL; /* lint suppression */

    if (isobjmenu) {
        const struct display_objects_callback_data *elcast = el;
        nresults = elcast->nresults;
        cur_objresult = elcast->results;
    } else {
        const struct display_menu_callback_data *elcast = el;
        nresults = elcast->nresults;
        cur_menuresult = elcast->results;
    }

    if (program_state.in_zero_time_command)
        return;

    if (program_state.input_was_just_replayed) {
        program_state.input_was_just_replayed = FALSE;
        return;
    }

    start_updating_logfile(FALSE);

    lprintf(isobjmenu ? "O" : "M");

    while (nresults) {
        if (isobjmenu) {
            if (cur_objresult->count == -1)
                lprintf("%" PRIx32,
                        (int32_t)(cur_objresult->id + MENU_ID_OFFSET));
            else if (cur_objresult->count >= 0)
                lprintf("%" PRIx32 ",%d",
                        (int32_t)(cur_objresult->id + MENU_ID_OFFSET),
                        cur_objresult->count);
            else
                panic("negative count in log_record_menu");
            cur_objresult++;
        } else {
            lprintf("%" PRIx32, (*cur_menuresult + MENU_ID_OFFSET));
            cur_menuresult++;
        }
        if (--nresults)
            lprintf(":");
    }

    lprintf("\x0a");

    stop_updating_logfile(1);
}

void
log_record_command(const char *cmd, const struct nh_cmd_arg *arg)
{
    if (program_state.in_zero_time_command)
        return;

    if (program_state.input_was_just_replayed) {
        program_state.input_was_just_replayed = FALSE;
        return;
    }

    start_updating_logfile(FALSE);

    if (*cmd < 'a' || *cmd > 'z')
        panic("command '%s' does not start with lowercase letter", cmd);

    if (strchr(cmd, ' '))
        panic("command '%s' contains a space", cmd);

    lprintf("%s", cmd);

    if (arg->argtype & CMD_ARG_DIR)
        lprintf(" D%d", arg->dir);
    if (arg->argtype & CMD_ARG_POS)
        lprintf(" P%d,%d", arg->pos.x, arg->pos.y);
    if (arg->argtype & CMD_ARG_OBJ)
        lprintf(" I%c", arg->invlet);
    if (arg->argtype & CMD_ARG_STR) {
        lprintf(" T");
        log_binary(arg->str, strlen(arg->str) + 1);
    }
    if (arg->argtype & CMD_ARG_SPELL)
        lprintf(" S%c", arg->spelllet);
    if (arg->argtype & CMD_ARG_LIMIT)
        lprintf(" L%d", arg->limit);

    lprintf("\x0a");

    stop_updating_logfile(1);
}

/***** Reading the log *****/

/* Called when reading non-command input from the log. This is idempotent. */
static char *
start_replaying_logfile(char firstchar)
{
    char *logline;

    if (program_state.in_zero_time_command)
        return NULL;

    if (program_state.input_was_just_replayed)
        panic("log_replay_* was not followed by a call to log_record_*");

    lseek(program_state.logfile,
          program_state.end_of_gamestate_location, SEEK_SET);

    logline = lgetline_malloc(program_state.logfile);

    if (logline && firstchar && firstchar != *logline) {
        /* Desync: the log contains one sort of input, but the engine is
           requesting another. */
        free(logline);
        log_desync();
    }

    return logline;
}

/* Called when non-command input was /successfully/ read from the log.
   (This is omitted if start_replaying_logfile was called but then the log
   entry turned out to be wrong; start_replaying_logfile is idempotent.) */
static void
stop_replaying_logfile(void)
{
    program_state.gamestate_location = program_state.end_of_gamestate_location;
    program_state.end_of_gamestate_location = get_log_offset();
    program_state.input_was_just_replayed = TRUE;
}

static int
parse_decimal_number(char **ptr)
{
    int rv = 0;
    while (**ptr >= '0' && **ptr <= '9') {
        rv *= 10;
        rv += **ptr - '0';
        (*ptr)++;
    }
    return rv;
}

/* This needs a defined bitwidth because otherwise we can't handle negative
   numbers correctly. */
static int32_t
parse_hex_number(char **ptr)
{
    uint32_t rv = 0;
    while ((**ptr >= '0' && **ptr <= '9') ||
           (**ptr >= 'a' && **ptr <= 'f')) {
        rv *= 16;
        if (**ptr >= '0' && **ptr <= '9')
            rv += **ptr - '0';
        else
            rv += **ptr - 'a' + 10;
        (*ptr)++;
    }
    return (int32_t)rv;
}

boolean
log_replay_bones(struct memfile *mf)
{
    char *logline = start_replaying_logfile('B');
    if (!logline)
        return FALSE;

    mf->len = base64_strlen(logline + 1);
    mf->buf = malloc(mf->len);
    base64_decode(logline + 1, mf->buf, mf->len);

    stop_replaying_logfile();

    free(logline);

    return TRUE;
}

boolean
log_replay_input(int count, const char *fmt, ...)
{
    char *logline;
    va_list vargs;
    char fmtbuf[strlen(fmt) * 2 + 1];
    int actual_count;
    char *p1;
    const char *p2;

    logline = start_replaying_logfile(*fmt);
    if (!logline)
        return FALSE;

    /* We need to check that the entire line matches the format string (it's
       not uncommon for one format to be a prefix of another). We do this by
       making sure it matches up to %n with assignment suppressions, and then
       making sure all the arguments parse. */
    p1 = fmtbuf;
    p2 = fmt;
    while (*p2) {
        *p1 = *p2;
        if (*p2 == '%') {
            if (p2[1] == '%' || p2[1] == '*') {
                /* a %% or %* specifier; just copy the next character and skip
                   special handling on both this and the next character */
                *++p1 = *++p2;
            } else {
                *++p1 = '*'; /* suppress assignment */
            }
        }
        p1++;
        p2++;
    }

    *p1 = '%';
    *++p1 = 'n';
    *++p1 = '\0';

    /* Does the format line parse all the characters in logline? */
    actual_count = -1;
    sscanf(logline, fmtbuf, &actual_count);
    if (strlen(logline) != actual_count) {
        free(logline);
        return FALSE;
    }

    /* OK, now make sure there's enough input in logline to assign to all
       the arguments. */
    va_start(vargs, fmt);
    actual_count = vsscanf(logline, fmt, vargs);
    va_end(vargs);

    free(logline);

    if (count != actual_count)
        return FALSE;

    stop_replaying_logfile();

    return TRUE;
}

boolean
log_replay_line(const char **buf)
{
    char *logline = start_replaying_logfile('L');
    if (!logline)
        return FALSE;

    /* The string must be shorter in plain than in base 64. */
    char decode_buffer[strlen(logline + 1) + 2];
    base64_decode(logline+1, decode_buffer, (sizeof decode_buffer) - 1);
    *buf = msg_from_string(decode_buffer);

    stop_replaying_logfile();

    free(logline);

    return TRUE;
}

boolean
log_replay_menu(boolean isobjmenu, void *el)
{
    char *logline = start_replaying_logfile(isobjmenu ? 'O' : 'M');
    char *lp;

    int *itemcount;
    const struct nh_objresult **objresultlist = 0;
    struct nh_objresult *orl;
    const int **menuresultlist = 0;
    int *mrl;

    if (isobjmenu) {
        struct display_objects_callback_data *elcast = el;
        itemcount = &(elcast->nresults);
        objresultlist = &(elcast->results);
        *objresultlist = NULL;
        orl = NULL;
    } else {
        struct display_menu_callback_data *elcast = el;
        itemcount = &(elcast->nresults);
        menuresultlist = &(elcast->results);
        *menuresultlist = NULL;
        mrl = NULL;
    }

    *itemcount = 0;

    if (!logline)
        return FALSE;

    lp = logline + 1;

    while (*lp) {
        uint32_t id = 0;
        int count = -1;

        id = parse_hex_number(&lp);

        (*itemcount)++;

        if (*lp == ',') {

            if (!isobjmenu) {
                free(logline);
                error_reading_save("non-obj menu has counts\n");
            }

            lp++;
            count = parse_decimal_number(&lp);
        }

        if (*lp != ':' && *lp) {
            free(logline);
            error_reading_save("bad number format in menu\n");
        }

        if (isobjmenu) {
            orl = xrealloc(&turnstate.message_chain, orl,
                           *itemcount * sizeof *orl);
            orl[*itemcount-1].id = ((int32_t)id) - MENU_ID_OFFSET;
            orl[*itemcount-1].count = count;
        } else {
            mrl = xrealloc(&turnstate.message_chain, mrl,
                           *itemcount * sizeof *mrl);
            mrl[*itemcount-1] = ((int32_t)id) - MENU_ID_OFFSET;
        }

        if (*lp)
            lp++;
    }

    stop_replaying_logfile();

    free(logline);

    if (isobjmenu)
        *objresultlist = orl;
    else
        *menuresultlist = mrl;

    return TRUE;
}

boolean
log_replay_command(struct nh_cmd_and_arg *cmd)
{
    char *logline = start_replaying_logfile(0);
    char *lp, *lp2;
    char c;

    if (!logline)
        return FALSE;

    if (*logline < 'a' || *logline > 'z') {
        free(logline);
        log_desync();
    }

    lp = strchr(logline, ' ');
    cmd->cmd = lp ?
        msgchop(msg_from_string(logline), lp - logline) :
        msg_from_string(logline);

    if (strlen(cmd->cmd) > 60) /* sanity check */
        error_reading_save("Command name was too long\n");

    /* Zero the argtype, and make sure the rest of the structure is
       initialized. */
    memset(&(cmd->arg), 0, sizeof (struct nh_cmd_arg));

    while (lp && *lp == ' ') {
        lp += 2;
        switch (lp[-1]) {
        case 'D':
            cmd->arg.argtype |= CMD_ARG_DIR;
            cmd->arg.dir = parse_decimal_number(&lp);
            break;

        case 'P':
            cmd->arg.argtype |= CMD_ARG_POS;
            cmd->arg.pos.x = parse_decimal_number(&lp);
            if (*(lp++) != ',') {
                free(logline);
                error_reading_save("No comma in position argument\n");
            }
            cmd->arg.pos.y = parse_decimal_number(&lp);
            break;

        case 'I':
            cmd->arg.argtype |= CMD_ARG_OBJ;
            cmd->arg.invlet = *(lp++);
            break;

        case 'T':
            cmd->arg.argtype |= CMD_ARG_STR;
            lp2 = lp;
            while (*lp2 != ' ' && *lp2)
                lp2++;
            c = *lp2;
            *lp2 = '\0';
            /* A string is always shorter in plaintext than in base 64 */
            {
                char decode_buf[strlen(lp) + 2];
                base64_decode(lp, decode_buf, (sizeof decode_buf) - 1);
                cmd->arg.str = msg_from_string(decode_buf);
            }
            *lp2 = c;
            lp = lp2;
            break;

        case 'S':
            cmd->arg.argtype |= CMD_ARG_SPELL;
            cmd->arg.spelllet = *(lp++);
            break;

        case 'L':
            cmd->arg.argtype |= CMD_ARG_LIMIT;
            cmd->arg.limit = parse_decimal_number(&lp);
            break;

        default:
            free(logline);
            error_reading_save("Unrecognised command argument\n");
        }
    }

    stop_replaying_logfile();

    free(logline);

    return TRUE;
}

/* After trying a bunch of log_replay_* functions, this one is called to
   record that there were no options left; it desyncs if we're in a situation
   where we were expecting to be able to play something. */
void
log_replay_no_more_options(void)
{
    void *logline = start_replaying_logfile(0);
    if (logline) {
        free(logline);
        log_desync();
    }
}


/* Code common to nh_get_savegame_status and log loading */
static enum nh_log_status
read_log_header(int fd, struct nh_game_info *si,
                int *recovery_count, boolean do_locking)
{
    char *logline, *p;
    char namebuf[65]; /* matches %64s later */
    char statusbuf[STATUS_LEN + 1];
    int playmode, version_major, version_minor, version_patchlevel;
    enum nh_log_status result;

    if (do_locking && !change_fd_lock(fd, LT_READ, 1))
        return LS_IN_PROGRESS;

    lseek(fd, 0, SEEK_SET);
    logline = lgetline_malloc(fd);
    if (!logline)
        goto invalid_log;

    if (sscanf(logline, "NHGAME %" STATUS_LEN_STR "s %8x %d.%3d.%3d", statusbuf,
               recovery_count, &version_major, &version_minor,
               &version_patchlevel) != 5)
        goto invalid_logline;

    free(logline);

    if ((result = status_from_string(statusbuf)) == LS_INVALID)
        goto invalid_log;

    logline = lgetline_malloc(fd);
    if (!logline)
        goto invalid_log;

    if (strlen(logline) != SECOND_LOGLINE_LEN)
        goto invalid_logline;

    p = logline;
    while (*p == ' ')
        p++;
    strcpy(si->game_state, p);

    free(logline);

    logline = lgetline_malloc(fd);
    if (!logline)
        goto invalid_log;

    /* The numbers here are chosen to avoid overflow and underflow; the intended
       max lengths are (32 * 4 / 3) and 3, and the buffers that are eventually
       stored into are 32 and 16. Thus the temporary buffer in the case of
       namebuf. */
    if (sscanf(logline, "%*x %*x %d %64s %6s %6s %6s %6s",
               &playmode, namebuf, si->plrole, si->plrace,
               si->plgend, si->plalign) != 6)
        goto invalid_logline;

    free(logline);

    si->playmode = playmode;
    base64_decode(namebuf, si->name, sizeof (si->name));

    if (do_locking)
        change_fd_lock(fd, LT_NONE, 0);
    return result;

invalid_logline:
    free(logline);
invalid_log:
    if (do_locking)
        change_fd_lock(fd, LT_NONE, 0);
    return LS_INVALID;
}

enum nh_log_status
nh_get_savegame_status(int fd, struct nh_game_info *si)
{
    struct nh_game_info dummy;
    int dummy2;
    if (!si)
        si = &dummy;
    return read_log_header(fd, si, &dummy2, TRUE);
}

/* Sets the gamestate pointer and the actual gamestate from the binary save
   pointer and binary save. This function restores the gamestate-related
   program_state invariants. It also ensures that the binary save is correctly
   tagged and internally consistent.

   If maybe_old_version is false, then we insist that the save file in question
   loads correctly (i.e. loading and saving again produces an identical file);
   this helps catch errors where the gamestate is not saved correctly. If it's
   true, then we allow backwards-compatible changes to the save format; if
   loading and immediately re-saving does not produce an identical file, we
   force the next save-related line to be a backup not a diff. */
static void
load_gamestate_from_binary_save(boolean maybe_old_version)
{
    struct memfile mf;

    /* For the time being, there are no compatible old versions, thus we can
       safely force maybe_old_version to FALSE for even more aggressive error
       detection. */
    maybe_old_version = FALSE;

    /* Load the saved game. */
    program_state.gamestate_location = program_state.binary_save_location;
    lseek(program_state.logfile, program_state.binary_save_location,
          SEEK_SET);
    free(lgetline_malloc(program_state.logfile));
    program_state.end_of_gamestate_location = get_log_offset();

    freedynamicdata();
    startup_common(FALSE);
    dorecover(&program_state.binary_save);

    /* Save the loaded game. */
    mnew(&mf, NULL);
    savegame(&mf);

    if (!mequal(&program_state.binary_save, &mf, !maybe_old_version)) {

        mfree(&mf);
        if (maybe_old_version) {
            program_state.ok_to_diff = FALSE;
            return;
        }
        error_reading_save("");
    }

    /* Replace the old save file with the new save file. */
    mfree(&program_state.binary_save);
    program_state.binary_save = mf;
    program_state.ok_to_diff = TRUE;
}

/* Decodes the given save diff into program_state.binary_save. The caller should
   check that the string actually is a representation of a save diff, is
   responsible for fixing the invariants on program_state, and must move the
   binary save out of the way for safekeeping first.

   TODO: Perhaps this function would make more sense in memfile.c (with a
   slightly different calling convention), in case we need to apply diffs to
   anything else. It's here because this is the only file that needs to be able
   to apply diffs, for the time being. */
static void
apply_save_diff(char *s, struct memfile *diff_base)
{
    char *buf, *bufp, *mfp;
    long buflen, dbpos = 0;

    if (program_state.binary_save_allocated)
        panic("The caller of apply_save_diff must back up and deallocate "
              "the binary save");

    mnew(&program_state.binary_save, NULL);
    program_state.binary_save_allocated = TRUE;

    /* The header of a save diff is one byte, '~'. */
    s++;

    buflen = base64_strlen(s);
    buf = malloc(buflen + 2);
    memset(buf, 0, buflen + 2);
    base64_decode(s, buf, buflen);

    bufp = buf;
    while (bufp[0] || bufp[1]) {
        /* 0x0000 means "seek 0", which is never generated, and thus works as an
           EOF marker */

        signed short n = (unsigned char)(bufp[1]) & 0x3F;
        n *= 256;
        n += (unsigned char)(bufp[0]);

        bufp += 2;

        switch ((unsigned char)(bufp[-1]) >> 6) {
        case MDIFF_SEEK:

            if (n >= 0x2000)
                n -= 0x4000;

            if (dbpos < n) {
                free(buf);
                mfree(&program_state.binary_save);
                error_reading_save("binary diff seeks past start of file\n");
            }

            dbpos -= n;

            break;

        case MDIFF_COPY:

            mfp = mmmap(&program_state.binary_save, n,
                        program_state.binary_save.pos);

            if (dbpos + n > diff_base->pos) {
                free(buf);
                mfree(&program_state.binary_save);
                error_reading_save("binary diff reads past EOF\n");
            }

            memcpy(mfp, diff_base->buf + dbpos, n);
            dbpos += n;

            break;

        case MDIFF_EDIT:

            mfp = mmmap(&program_state.binary_save, n,
                        program_state.binary_save.pos);

            if (bufp - buf + n > buflen) {
                free(buf);
                mfree(&program_state.binary_save);
                error_reading_save("binary diff ends unexpectedly\n");
            }

            memcpy(mfp, bufp, n);
            dbpos += n;     /* can legally go past the end of diff_base! */
            bufp += n;

            break;

        default:
            free(buf);
            mfree(&program_state.binary_save);
            error_reading_save("unknown command in binary diff\n");
        }
    }

    free(buf);
}

/* Decodes the given string into program_state.binary_save. The caller should
   check that the string actually is a representation of a save backup, and is
   responsible for fixing the invariants on program_state. */
static void
load_save_backup_from_string(char *s)
{
    void *mp;
    long len;

    if (program_state.binary_save_allocated)
        mfree(&program_state.binary_save);
    mnew(&program_state.binary_save, NULL);
    program_state.binary_save_allocated = TRUE;

    /* The header is '*', an 8 digit hex number, and ' ', = 10 bytes. */
    s += 10;
    len = base64_strlen(s);

    mp = mmmap(&program_state.binary_save, len, 0);
    base64_decode(s, mp, len);
}

/* Sets the binary save and save backup locations from the argument (which
   should be the byte offset of a save backup; the caller must check this), and
   sets the binary save to match. This does /not/ enforce the invariant that
   binary_save_location <= gamestate_location; the caller may need to fix
   that. The log file pointer is left immediately after the save backup. */
static void
load_save_backup_from_offset(long offset)
{
    char *logline;

    program_state.binary_save_location = offset;
    program_state.save_backup_location = offset;
    lseek(program_state.logfile, offset, SEEK_SET);

    logline = lgetline_malloc(program_state.logfile);
    if (!logline)
        error_reading_save("EOF when reading save backup\n");

    load_save_backup_from_string(logline);
    free(logline);
}

/* Checks to see if a save backup exists at a given file location. Returns -1 if
   no save backup was found there, 0 if a save backup was found but with a
   garbage next-offset field, or the next-offset field otherwise. */
static long
get_save_backup_offset(long offset)
{
    long oldoffset = get_log_offset();
    long rv = -1;
    char sbbuf[11];
    long sbloc;
    char *sbptr;

    if (lseek(program_state.logfile, offset, SEEK_SET) < 0)
        goto cleanup;

    /* Read the save backup header. */
    if (!full_read(program_state.logfile, sbbuf, 10))
        goto cleanup;
    sbbuf[10] = '\0';

    /* A save backup starts with an asterisk, then an 8-digit hexadecimal
       number, then a space. */
    if (sbbuf[0] != '*' || sbbuf[9] != ' ')
        goto cleanup;
    sbloc = strtol(sbbuf+1, &sbptr, 16);
    if (sbptr != sbbuf+9) /* sbbuf+1 didn't contain 8 hex digits */
        goto cleanup;

    /* It looks like a save backup. Is the offset valid? strtol returns LONG_MAX
       or LONG_MIN if out of range. */
    rv = 0;
    if (sbloc > 0 && sbloc < LONG_MAX)
        rv = sbloc;

cleanup:
    lseek(program_state.logfile, oldoffset, SEEK_SET);
    return rv;
}

/* Returns positive if the binary save is ahead of the target location, negative
   if the binary save is behind the target location, zero if they're the
   same. The argument is the binary save location; while the invariants hold,
   this is just program_state.binary_save_location, but because this function is
   mostly useful to log_sync(), which breaks the invariants at times, it allows
   a different value to be given.

   Leaves the binary save pointer in its original location, but the log file
   pointer in an unpredictable location. */
static long
relative_to_target(long bsl)
{
    long targetpos = program_state.target_location;
    long temp_pos = program_state.binary_save.pos;
    int turncount;
    switch (program_state.target_location_units) {

    case TLU_EOF:
        targetpos = lseek(program_state.logfile, 0, SEEK_END);
        /* fall through */
    case TLU_BYTES:

        return bsl - targetpos;

    case TLU_TURNS:

        program_state.binary_save.pos = 0;
        if (!uptodate(&program_state.binary_save, NULL))
            error_reading_save(
                "binary save is from the wrong version of NetHack\n");

        turncount = mread32(&program_state.binary_save);
        program_state.binary_save.pos = temp_pos;

        return turncount - targetpos;

    default:
        panic("Invalid target_location_units");
    }
}

/*
 * Fastforwards/rewinds the gamestate to the target location.
 *
 * This must be called in the following situations:
 * * To load a save file for the first time (i.e. you call log_init then
 *   log_sync in order to load a save file);
 * * Whenever the target location moves to before the gamestate location
 *   (rewinding a replay, etc.).
 * * Whenever a section of the save file needs to be skipped due to desyncing.
 *
 * This also does something useful in the following situation:
 * * When the target location gets ahead of the gamestate location by more than
 *   a turn, calling log_sync() has the same effect as just running round the
 *   main loop in nh_play_game() would (assuming it syncs correctly), but is
 *   faster and less sensitive to engine changes.
 *
 * The gamestate location will not be moved to the target location itself if it
 * happens to be halfway through a turn; in this case, it will be moved to the
 * last save diff or save backup before the target location. (This happens even
 * if the previous gamestate location was between the target location and the
 * last save diff. In such a case, you may want to avoid using this function
 * altogether.)
 *
 * This should only be called from the nh_play_game main loop, or similar
 * locations that can handle arbitrary gamestate changes (including the
 * deallocation and reallocation of all game pointers). This also implies that
 * when this function is called, the gamestate location is equal to the binary
 * save location, or else the binary save location is zero (i.e. the game
 * restore sequence). (If you want to use this function in a situation where
 * that is not true, use terminate(RESTART_PLAY), which causes the main loop to
 * restart, and modify nh_play_game to set the target location you want rather
 * than its default of TLU_EOF.) It also requires us to hold a read lock on the
 * log (i.e. log_init() needs to have been called, and it can't be called from
 * the section of nh_input_aborted() that temporarily relinquishes the lock, not
 * that you'd want to do that anyway).
 *
 * The invariants on the gamestate are suspended during this function, and fixed
 * at the end. In particular, while the function is running, neither
 * program_state.gamestate_location, or any elements of the actual gamestate,
 * have any meaning at all.
 */
void
log_sync(void)
{
    struct nh_game_info si;
    struct memfile bsave;
    long sloc, loglineloc;
    char *logline;

    /* If the file is newly loaded, fill the locations with correct values
       rather than zeroes. TODO: Perhaps we should also do this when seeking to
       EOF, if the current save backup location is sufficiently far from the end
       of the file. */
    if (program_state.binary_save_location == 0) {
        /* Check it's a valid save file; simultaneously, move the file
           pointer to the start of line 4 (the first save backup). */
        if (read_log_header(program_state.logfile, &si,
                            &program_state.expected_recovery_count,
                            FALSE) != LS_SAVED)
            error_reading_save(
                "logfile has a bad header (is it from an old version?)\n");

        /* Now we know the location that the location of the last save backup
           should be stored in. This location is only advisory, though, and
           may contain an incorrect value. We also initialize the save backup
           location, while we're here. */
        program_state.save_backup_location = get_log_offset();
        program_state.last_save_backup_location_location =
            program_state.save_backup_location + 1;

        sloc = get_save_backup_offset(program_state.save_backup_location);
        if (sloc < 0) /* fourth line wasn't a save backup */
            error_reading_save(
                "fourth line of logfile is in the wrong format\n");

        /* Find a save backup. This should ideally be the last one in the file,
           but any save backup will do, and later is better (more efficient to
           load). We check to see if there actually is a save backup at the
           suggested location. If there is, we use it; it may be the one we
           want, and even if it isn't, it's going to be no earlier than the
           first save backup in the file, so it can't be a worse choice than the
           alternative. Otherwise, we use the first save backup in the file,
           because we already know where that is (and save_backup_location has
           that value already). */
        if (get_save_backup_offset(sloc) >= 0)
            program_state.save_backup_location = sloc;

        /* Set the binary save and gamestate locations, also the actual
           gamestate itself. Now all the log-related but gamestate-unrelated
           fields of program_state have sensible values (target_location is set
           by the caller, we set the save backup locations earlier, and this
           handles the binary save). This does not set the gamestate. */
        load_save_backup_from_offset(program_state.save_backup_location);

    } else if (program_state.gamestate_location !=
               program_state.binary_save_location)
        panic("log_sync called mid-turn");

    /* If we're ahead of the target, move back to the last save backup (because
       we can't run save diffs backwards, our only choice is to move forwards
       from the save backup location). */
    if (program_state.binary_save_location != program_state.save_backup_location
        && relative_to_target(program_state.binary_save_location) > 0) {

        load_save_backup_from_offset(program_state.save_backup_location);
    }

    /* While we're still ahead of the target, try progressively earlier
       backups. */
    while (relative_to_target(program_state.binary_save_location) > 0 &&
           program_state.save_backup_location >
           program_state.last_save_backup_location_location) {

        sloc = get_save_backup_offset(program_state.save_backup_location);
        load_save_backup_from_offset(sloc);
    }

    /* If we're behind the target, move forwards until we're at or ahead of the
       target, via adding together diffs.

       TODO: We could increase the loading speed via caching the end location of
       the save diff / backup, rather than skipping it each time round the
       loop. That isn't implemented yet in order to start off by getting the
       simplest possible version of the code working. */
    sloc = program_state.binary_save_location;
    while (relative_to_target(program_state.binary_save_location) < 0) {
        lseek(program_state.logfile, sloc, SEEK_SET);
        /* Skip the save diff or backup itself. */
        free(lgetline_malloc(program_state.logfile));

        /* Look for the next save diff or backup line. */
        for ((loglineloc = get_log_offset()),
                 (logline = lgetline_malloc(program_state.logfile));
             logline;
             free(logline), (loglineloc = get_log_offset()),
                 (logline = lgetline_malloc(program_state.logfile))) {
            if (*logline == '*' || *logline == '~')
                break;
        }

        if (!logline) {
            /* We're at EOF. The binary save and its location are already
               correct, so we just need to get the gamestate and its location
               correct. */
            load_gamestate_from_binary_save(TRUE);
            return;
        }

        /* Back up the binary save, so that we can go back if we overshoot.
           (This trick to avoid having to use mclone() is inspired by C++
           move constructors.) */
        bsave = program_state.binary_save;
        program_state.binary_save_allocated = FALSE;

        if (*logline == '*') {
            /* This is a save backup. */
            load_save_backup_from_string(logline);
        } else if (*logline == '~') {
            /* This is a save diff. */
            apply_save_diff(logline, &bsave);
        }

        if (relative_to_target(loglineloc) > 0) {

            /* We overshot. */
            program_state.binary_save_location = sloc;

            if (!program_state.binary_save_allocated) /* should never happen */
                panic("overshoot in log_sync but no binary save present");

            free(logline);
            mfree(&program_state.binary_save);
            program_state.binary_save = bsave;

            load_gamestate_from_binary_save(TRUE);
            return;

        } else {

            /* We didn't overshoot: set the locations to match this new save. */
            sloc = program_state.binary_save_location = loglineloc;
            if (*logline == '*')
                program_state.save_backup_location = loglineloc;

            mfree(&bsave);
        }

        free(logline);
    }

    /* Fix the invariant on the gamestate. */
    load_gamestate_from_binary_save(TRUE);
}


static void
log_reset(void)
{
    if (program_state.binary_save_allocated)
        mfree(&program_state.binary_save);
    program_state.binary_save_allocated = FALSE;
    program_state.ok_to_diff = FALSE;
    program_state.input_was_just_replayed = FALSE;

    program_state.expected_recovery_count = 0;
    program_state.save_backup_location = 0;
    program_state.binary_save_location = 0;
    program_state.gamestate_location = 0;
    program_state.target_location = 0;
    program_state.target_location_units = TLU_EOF;
    program_state.last_save_backup_location_location = 0;
}

void
log_init(int logfd)
{

    if (!change_fd_lock(logfd, LT_READ, 2))
        terminate(ERR_IN_PROGRESS);

    log_reset();

    program_state.logfile = logfd;
}

void
log_uninit(void)
{
    if (program_state.logfile > -1)
        change_fd_lock(program_state.logfile, LT_NONE, 0);

    program_state.logfile = -1;

    /* We need to do this so that init_data doesn't leak memory when it's told
       to reinitialize the program_state; it can't rely on program_state.
       binary_save_allocated because for all it knows, that's uninitialized
       data. */
    if (program_state.binary_save_allocated) {
        mfree(&program_state.binary_save);
        program_state.binary_save_allocated = 0;
    }
}

