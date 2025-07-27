/*ki--	bare-bones, vi-like, in 800 lines.	*/
/*LICENSE: use it however you want.		*/
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
// Printable or not
#define PRINTABLE 0
#define NONPRINTABLE 1
// Modes
#define INSERT 2
#define COMMAND 1
#define NOMODE 0

/* This structure represents a single line of the file we are editing. */
typedef struct erow {
  unsigned int idx;   /* Row index in the file, zero-based. */
  unsigned int size;  /* Size of the row, excluding the null term. */
  unsigned int rsize; /* Size of the rendered row. */
  char *chars;        /* Row content. */
  char *render;       /* Row content "rendered" for screen (for TABs). */
  unsigned char *hl;  /* Syntax highlight type for each character in render.*/
} erow;

struct editorConfig {
  unsigned int cx, cy;     /* Cursor x and y position in characters */
  unsigned int rowoff;     /* Offset of row displayed. */
  unsigned int coloff;     /* Offset of column displayed. */
  unsigned int screenrows; /* Number of rows that we can show */
  unsigned int screencols; /* Number of cols that we can show */
  unsigned int numrows;    /* Number of rows */
  int rawmode;             /* Is terminal raw mode enabled? */
  erow *row;               /* Rows */
  int dirty;               /* File modified but not saved. */
  char *filename;          /* Currently open filename */
  char statusmsg[80];
};
enum KEY_ACTION {
  TAB = 9,         /* Tab */
  ENTER = 13,      /* Enter */
  ESC = 27,        /* Escape */
  BACKSPACE = 127, /* Backspace */
  /* The following are just soft codes, not really reported by the
   * terminal directly. */
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

void editorSetStatusMessage(const char *fmt, ...);
/* ======================= Low level terminal handling ====================== */
static struct editorConfig E;
static int mode;
static struct termios orig_termios; /* In order to restore at exit.*/

void disableRawMode(int fd) {
  /* Don't even check the return value as it's too late. */
  if (E.rawmode) {
    tcsetattr(fd, TCSAFLUSH, &orig_termios);
    E.rawmode = 0;
  }
}

/* Called at exit to avoid remaining in raw mode. */
void editorAtExit(void) { disableRawMode(STDIN_FILENO); }

/* Raw mode: 1960 magic shit. */
int enableRawMode(int fd) {
  struct termios raw;

  if (E.rawmode)
    return 0; /* Already enabled. */
  if (!isatty(STDIN_FILENO))
    goto fatal;
  atexit(editorAtExit);
  if (tcgetattr(fd, &orig_termios) == -1)
    goto fatal;

  raw = orig_termios; /* modify the original mode */
  /* input modes: no break, no CR to NL, no parity check, no strip char,
   * no start/stop output control. */
  raw.c_iflag &= (tcflag_t) ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  /* output modes - disable post processing */
  raw.c_oflag &= (tcflag_t) ~(OPOST);
  /* control modes - set 8 bit chars */
  raw.c_cflag |= (tcflag_t)(CS8);
  /* local modes - choing off, canonical off, no extended functions,
   * no signal chars (^Z,^C) */
  raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN | ISIG);
  /* control chars - set return condition: min number of bytes and timer. */
  raw.c_cc[VMIN] = 0;  /* Return each byte, or zero for timeout. */
  raw.c_cc[VTIME] = 1; /* 100 ms timeout (unit is tens of second). */

  /* put terminal in raw mode after flushing */
  if (tcsetattr(fd, TCSAFLUSH, &raw) < 0)
    goto fatal;
  E.rawmode = 1;
  return 0;

fatal:
  errno = ENOTTY;
  return -1;
}

/* Read a key from the terminal put in raw mode, trying to handle
 * escape sequences. */
int editorReadKey(int fd) {
  ssize_t nread;
  char c, seq[3];
  while ((nread = read(fd, &c, 1L)) == 0)
    ;
  if (nread == -1)
    exit(1);

  while (1) {
    switch (c) {
    case ESC: /* escape sequence */
      /* If this is just an ESC, we'll timeout here. */
      if (read(fd, seq, 1) == 0)
        return ESC;
      if (read(fd, seq + 1, 1) == 0)
        return ESC;

      /* ESC [ sequences. */
      if (seq[0] == '[') {
        if (seq[1] >= '0' && seq[1] <= '9') {
          /* Extended escape, read additional byte. */
          if (read(fd, seq + 2, 1) == 0)
            return ESC;
          if (seq[2] == '~') {
            switch (seq[1]) {
            case '3':
              return DEL_KEY;
            case '5':
              return PAGE_UP;
            case '6':
              return PAGE_DOWN;
            default:;
            }
          }
        } else {
          switch (seq[1]) {
          case 'A':
            return ARROW_UP;
          case 'B':
            return ARROW_DOWN;
          case 'C':
            return ARROW_RIGHT;
          case 'D':
            return ARROW_LEFT;
          case 'H':
            return HOME_KEY;
          case 'F':
            return END_KEY;
          default:;
          }
        }
      }

      /* ESC O sequences. */
      else if (seq[0] == 'O') {
        switch (seq[1]) {
        case 'H':
          return HOME_KEY;
        case 'F':
          return END_KEY;
        default:;
        }
      }
      break;
    default:
      return c;
    }
  }
}

/* Use the ESC [6n escape sequence to query the horizontal cursor position
 * and return it. On error -1 is returned, on success the position of the
 * cursor is stored at *rows and *cols and 0 is returned. */
int getCursorPosition(int ifd, int ofd, unsigned int *rows,
                      unsigned int *cols) {
  char buf[32];
  unsigned int i = 0;

  /* Report cursor location */
  if (write(ofd, "\x1b[6n", 4) != 4)
    return -1;

  /* Read the response: ESC [ rows ; cols R */
  while (i < sizeof(buf) - 1) {
    if (read(ifd, buf + i, 1) != 1)
      break;
    if (buf[i] == 'R')
      break;
    i++;
  }
  buf[i] = '\0';

  /* Parse it. */
  if (buf[0] != ESC || buf[1] != '[')
    return -1;
  if (sscanf(buf + 2, "%u;%u", rows, cols) != 2)
    return -1;
  return 0;
}

/* Try to get the number of columns in the current terminal. If the ioctl()
 * call fails the function will try to query the terminal itself.
 * Returns 0 on success, -1 on error. */
int getWindowSize(int ifd, int ofd, unsigned int *rows, unsigned int *cols) {
  struct winsize ws;

  if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    /* ioctl() failed. Try to query the terminal itself. */
    unsigned int orig_row, orig_col;
    int retval;

    /* Get the initial position so we can restore it later. */
    retval = getCursorPosition(ifd, ofd, &orig_row, &orig_col);
    if (retval == -1)
      goto failed;

    /* Go to right/bottom margin and get position. */
    if (write(ofd, "\x1b[999C\x1b[999B", 12) != 12)
      goto failed;
    retval = getCursorPosition(ifd, ofd, rows, cols);
    if (retval == -1)
      goto failed;

    /* Restore position. */
    char seq[32];
    snprintf(seq, 32, "\x1b[%d;%dH", orig_row, orig_col);
    if (write(ofd, seq, strlen(seq)) == -1) {
      /* Can't recover... */
    }
    return 0;
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }

failed:
  return -1;
}

/* ======================= Editor rows implementation ======================= */

/* Update the row */
void editorUpdateRow(erow *row) {
  unsigned int tabs = 0, nonprint = 0;
  unsigned int j, idx;

  /* Create a version of the row we can directly print on the screen,
   * respecting tabs, substituting non printable characters with '?'. */
  free(row->render);
  for (j = 0; j < row->size; j++)
    if (row->chars[j] == TAB)
      tabs++;

  unsigned long allocsize =
      (unsigned long)row->size + tabs * 8 + nonprint * 9 + 1;
  if (allocsize > UINT32_MAX) {
    printf("Some line of the edited file is too long for kilo\n");
    exit(1);
  }

  row->render = malloc(row->size + tabs * 8 + nonprint * 9 + 1);
  idx = 0;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == TAB) {
      row->render[idx++] = ' ';
      while ((idx + 1) % 8 != 0)
        row->render[idx++] = ' ';
    } else
      row->render[idx++] = row->chars[j];
  }
  row->rsize = idx;
  row->render[idx] = '\0';

  row->hl = realloc(row->hl, row->rsize);
  memset(row->hl, PRINTABLE, row->rsize);
}

/* Insert a row at the specified position, shifting the other rows on the bottom
 * if required. */
void editorInsertRow(unsigned int at, const char *s, unsigned int len) {
  if (at > E.numrows)
    return;
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  if (at != E.numrows) {
    memmove(E.row + at + 1, E.row + at, sizeof(E.row[0]) * (E.numrows - at));
    for (unsigned int j = at + 1; j <= E.numrows; j++)
      E.row[j].idx++;
  }
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len + 1);
  E.row[at].hl = NULL;
  E.row[at].render = NULL;
  E.row[at].rsize = 0;
  E.row[at].idx = at;
  editorUpdateRow(E.row + at);
  E.numrows++;
  E.dirty++;
}

/* Free row's heap allocated stuff. */
void editorFreeRow(erow *row) {
  free(row->render);
  free(row->chars);
  free(row->hl);
}

/* Remove the row at the specified position, shifting the remainign on the
 * top. */
void editorDelRow(unsigned int at) {
  erow *row;

  if (at >= E.numrows)
    return;
  row = E.row + at;
  editorFreeRow(row);
  memmove(E.row + at, E.row + at + 1, sizeof(E.row[0]) * (E.numrows - at - 1));
  for (unsigned int j = at; j < E.numrows - 1; j++)
    E.row[j].idx++;
  E.numrows--;
  E.dirty++;
}

/* Turn the editor rows into a single heap-allocated string.
 * Returns the pointer to the heap-allocated string and populate the
 * integer pointed by 'buflen' with the size of the string, escluding
 * the final nulterm. */
char *editorRowsToString(unsigned int *buflen) {
  char *buf = NULL, *p;
  unsigned int totlen = 0;
  unsigned int j;

  /* Compute count of bytes */
  for (j = 0; j < E.numrows; j++)
    totlen += E.row[j].size + 1; /* +1 is for "\n" at end of every row */
  *buflen = totlen;
  totlen++; /* Also make space for nulterm */

  p = buf = malloc(totlen);
  for (j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }
  *p = '\0';
  return buf;
}

/* Insert a character at the specified position in a row, moving the remaining
 * chars on the right if needed. */
void editorRowInsertChar(erow *row, unsigned int at, int c) {
  if (at > row->size) {
    /* Pad the string with spaces if the insert location is outside the
     * current length by more than a single character. */
    unsigned int padlen = at - row->size;
    /* In the next line +2 means: new char and null term. */
    row->chars = realloc(row->chars, row->size + padlen + 2);
    memset(row->chars + row->size, ' ', padlen);
    row->chars[row->size + padlen + 1] = '\0';
    row->size += padlen + 1;
  } else {
    /* If we are in the middle of the string just make space for 1 new
     * char plus the (already existing) null term. */
    row->chars = realloc(row->chars, row->size + 2);
    memmove(row->chars + at + 1, row->chars + at, row->size - at + 1);
    row->size++;
  }
  row->chars[at] = (char)c;
  editorUpdateRow(row);
  E.dirty++;
}

/* Append the string 's' at the end of a row */
void editorRowAppendString(erow *row, char *s, unsigned int len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(row->chars + row->size, s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  editorUpdateRow(row);
  E.dirty++;
}

/* Delete the character at offset 'at' from the specified row. */
void editorRowDelChar(erow *row, unsigned int at) {
  if (row->size <= at)
    return;
  memmove(row->chars + at, row->chars + at + 1, row->size - at);
  editorUpdateRow(row);
  row->size--;
  E.dirty++;
}

/* Insert the specified char at the current prompt position. */
void editorInsertChar(int c) {
  unsigned int filerow = E.rowoff + E.cy;
  unsigned int filecol = E.coloff + E.cx;
  erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

  /* If the row where the cursor is currently located does not exist in our
   * logical representaion of the file, add enough empty rows as needed. */
  if (!row) {
    while (E.numrows <= filerow)
      editorInsertRow(E.numrows, (const char *)"", 0);
  }
  row = &E.row[filerow];
  editorRowInsertChar(row, filecol, c);
  if (E.cx == E.screencols - 1)
    E.coloff++;
  else
    E.cx++;
  E.dirty++;
}

/* Inserting a newline is slightly complex as we have to handle inserting a
 * newline in the middle of a line, splitting the line as needed. */
void editorInsertNewline(void) {
  unsigned int filerow = E.rowoff + E.cy;
  unsigned int filecol = E.coloff + E.cx;
  erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

  if (!row) {
    if (filerow == E.numrows) {
      editorInsertRow(filerow, (const char *)"", 0);
      goto fixcursor;
    }
    return;
  }
  /* If the cursor is over the current line size, we want to conceptually
   * think it's just over the last character. */
  if (filecol >= row->size)
    filecol = row->size;
  if (filecol == 0)
    editorInsertRow(filerow, (const char *)"", 0);
  else {
    /* We are in the middle of a line. Split it between two rows. */
    editorInsertRow(filerow + 1, row->chars + filecol, row->size - filecol);
    row = &E.row[filerow];
    row->chars[filecol] = '\0';
    row->size = filecol;
    editorUpdateRow(row);
  }
fixcursor:
  if (E.cy == E.screenrows - 1)
    E.rowoff++;
  else
    E.cy++;
  E.cx = 0;
  E.coloff = 0;
}

/* Delete the char at the current prompt position. */
void editorDelChar(void) {
  unsigned int filerow = E.rowoff + E.cy;
  unsigned int filecol = E.coloff + E.cx;
  erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

  if (!row || (filecol == 0 && filerow == 0))
    return;
  if (filecol == 0) {
    /* Handle the case of column 0, we need to move the current line
     * on the right of the previous one. */
    filecol = E.row[filerow - 1].size;
    editorRowAppendString(&E.row[filerow - 1], row->chars, row->size);
    editorDelRow(filerow);
    row = NULL;
    if (E.cy == 0)
      E.rowoff--;
    else
      E.cy--;
    E.cx = filecol;
    if (E.cx >= E.screencols) {
      unsigned int shift = (E.screencols - E.cx) + 1;
      E.cx -= shift;
      E.coloff += shift;
    }
  } else {
    editorRowDelChar(row, filecol - 1);
    if (E.cx == 0 && E.coloff)
      E.coloff--;
    else
      E.cx--;
  }
  if (row)
    editorUpdateRow(row);
  E.dirty++;
}

/* Load the specified program in the editor memory and returns 0 on success
 * or 1 on error. */
int editorOpen(char *filename) {
  FILE *fp;

  E.dirty = 0;
  free(E.filename);
  size_t fnlen = strlen(filename) + 1;
  E.filename = malloc(fnlen);
  memcpy(E.filename, filename, fnlen);

  fp = fopen(filename, "r");
  if (!fp) {
    if (errno != ENOENT) {
      perror("Opening file");
      exit(1);
    }
    return 1;
  }

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    if (linelen && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
      line[--linelen] = '\0';
    editorInsertRow(E.numrows, line, (unsigned int)linelen);
  }
  free(line);
  fclose(fp);
  E.dirty = 0;
  return 0;
}

/* Save the current file on disk. Return 0 on success, 1 on error. */
int editorSave(void) {
  unsigned int len;
  char *buf = editorRowsToString(&len);
  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if (fd == -1)
    goto writeerr;

  /* Use truncate + a single write(2) call in order to make saving
   * a bit safer, under the limits of what we can do in a small editor. */
  if (ftruncate(fd, len) == -1)
    goto writeerr;
  if ((unsigned int)write(fd, buf, len) != len)
    goto writeerr;

  close(fd);
  free(buf);
  E.dirty = 0;
  editorSetStatusMessage("%d bytes written on disk", len);
  return 0;

writeerr:
  free(buf);
  if (fd != -1)
    close(fd);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
  return 1;
}

/* ============================= Terminal update ============================ */

/* We define a very simple "append buffer" structure, that is an heap
 * allocated string where we can append to. This is useful in order to
 * write all the escape sequences in a buffer and flush them to the standard
 * output in a single call, to avoid flickering effects. */
struct abuf {
  char *b;
  unsigned int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, unsigned int len) {
  char *new = (char *)realloc(ab->b, ab->len + len);

  if (!new)
    return;
  memcpy(new + ab->len, s, len);
  ab->b = new;
  ab->len += len;
}

// void abFree(struct abuf *ab) {
//     free(ab->b);
// }
#define abFree(x) free((x)->b)
/* This function writes the whole screen using VT100 escape characters
 * starting from the logical state of the editor in the global state 'E'. */
void editorRefreshScreen(void) {
  unsigned int y;
  erow *r;
  char buf[32];
  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6); /* Hide cursor. */
  abAppend(&ab, "\x1b[H", 3);    /* Go home. */
  for (y = 0; y < E.screenrows; y++) {
    unsigned int filerow = E.rowoff + y;
    if (filerow >= E.numrows) {
      abAppend(&ab, "~\x1b[0K\r\n", 7);
      continue;
    }

    r = &E.row[filerow];

    unsigned int len = r->rsize - E.coloff;
    if (len > 0) {
      if (len > E.screencols)
        len = E.screencols;
      char *c = r->render + E.coloff;
      unsigned char *hl = r->hl + E.coloff;
      unsigned int j;
      for (j = 0; j < len; j++) {
        if (hl[j] == NONPRINTABLE) {
          char sym;
          abAppend(&ab, "\x1b[7m", 4);
          if (c[j] <= 26)
            sym = '@' + c[j];
          else
            sym = '?';
          abAppend(&ab, &sym, 1);
          abAppend(&ab, "\x1b[0m", 4);
        } else
          abAppend(&ab, c + j, 1);
      }
    }
    abAppend(&ab, "\x1b[39m", 5);
    abAppend(&ab, "\x1b[0K", 4);
    abAppend(&ab, "\r\n", 2);
  }

  /* Create a two rows status. First row: */
  abAppend(&ab, "\x1b[0K", 4);
  abAppend(&ab, "\x1b[7m", 4);
  char status[80], rstatus[80];
  int err = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.filename,
                     E.numrows, E.dirty ? "(modified)" : "");
  if (err == -1)
    exit(1);
  unsigned int len = (unsigned int)err;
  err = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.rowoff + E.cy + 1,
                 E.numrows);
  if (err == -1)
    exit(1);
  unsigned int rlen = (unsigned int)err;
  if (len > E.screencols)
    len = E.screencols;
  abAppend(&ab, status, len);
  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      abAppend(&ab, rstatus, rlen);
      break;
    } else {
      abAppend(&ab, " ", 1);
      len++;
    }
  }
  abAppend(&ab, "\x1b[0m\r\n", 6);

  /* Second row depends on E.statusmsg and the status message update time. */
  abAppend(&ab, "\x1b[0K", 4);
  size_t s_msglen = strlen(E.statusmsg);
  if (s_msglen >= UINT_MAX)
    exit(1);
  unsigned int msglen = (unsigned int)s_msglen;
  if (msglen)
    abAppend(&ab, E.statusmsg, msglen <= E.screencols ? msglen : E.screencols);

  /* Put cursor at its current position. Note that the horizontal position
   * at which the cursor is displayed may be different compared to 'E.cx'
   * because of TABs. */
  unsigned int j;
  unsigned int cx = 1;
  unsigned int filerow = E.rowoff + E.cy;
  erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
  if (row) {
    for (j = E.coloff; j < (E.cx + E.coloff); j++) {
      if (j < row->size && row->chars[j] == TAB)
        cx += 7 - ((cx) % 8);
      cx++;
    }
  }
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, cx);
  size_t slen = strlen(buf);
  if (slen >= UINT_MAX)
    exit(1);
  unsigned int ulen = (unsigned int)slen;
  abAppend(&ab, buf, ulen);
  abAppend(&ab, "\x1b[?25h", 6); /* Show cursor. */
  if (-1 == write(STDOUT_FILENO, ab.b, ab.len))
    exit(-1);
  abFree(&ab);
}

/* Set an editor status message for the second line of the status, at the
 * end of the screen. */
__attribute__((format(printf, 1, 2))) void
editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
}
/* ========================= Editor events handling  ======================== */

/* Handle cursor position change because arrow keys were pressed. */
void editorMoveCursor(int key) {
  unsigned int filerow = E.rowoff + E.cy;
  unsigned int filecol = E.coloff + E.cx;
  unsigned int rowlen;
  erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

  switch (key) {
  case ARROW_LEFT:
    if (E.cx == 0) {
      if (E.coloff)
        E.coloff--;
      else {
        if (filerow > 0) {
          E.cy--;
          E.cx = E.row[filerow - 1].size;
          if (E.cx > E.screencols - 1) {
            E.coloff = E.cx - E.screencols + 1;
            E.cx = E.screencols - 1;
          }
        }
      }
    } else
      E.cx -= 1;
    break;
  case ARROW_RIGHT:
    if (row && filecol < row->size) {
      if (E.cx == E.screencols - 1)
        E.coloff++;
      else
        E.cx += 1;
    } else if (row && filecol == row->size) {
      E.cx = 0;
      E.coloff = 0;
      if (E.cy == E.screenrows - 1)
        E.rowoff++;
      else
        E.cy += 1;
    }
    break;
  case ARROW_UP:
    if (E.cy == 0) {
      if (E.rowoff)
        E.rowoff--;
    } else
      E.cy -= 1;
    break;
  case ARROW_DOWN:
    if (filerow < E.numrows) {
      if (E.cy == E.screenrows - 1)
        E.rowoff++;
      else
        E.cy += 1;
    }
    break;
  default:;
  }
  /* Fix cx if the current line has not enough chars. */
  filerow = E.rowoff + E.cy;
  filecol = E.coloff + E.cx;
  row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
  rowlen = row ? row->size : 0;
  if (filecol > rowlen)
    E.cx -= (filecol - rowlen);
}

/* Process events arriving from the standard input, which is, the user
 * is typing stuff on the terminal. */
#define KILO_QUIT_TIMES 3
void editorProcessKeypress(int fd) {
  /* When the file is modified, requires Ctrl-q to be pressed N times
   * before actually quitting. */
  static int quit_times = KILO_QUIT_TIMES;

  int c = editorReadKey(fd);
  if (c == ESC) {
    mode = NOMODE;
    editorSetStatusMessage(" ");
  }
  if (mode == NOMODE) {
    if ((char)c == 'i') {
      mode = INSERT;
      editorSetStatusMessage("--INSERT--");
    } else if ((char)c == ':') {
      mode = COMMAND;
      editorSetStatusMessage(":");
    }
  } else if (mode == COMMAND) {
    if ((char)c == 'w') {
      editorSave();
      editorSetStatusMessage("written");
    } else if ((char)c == 'q') {
      if (E.dirty && quit_times) {
        editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                               "Press q %d more times to quit.",
                               quit_times);
        quit_times--;
        return;
      }
      exit(0);
    }
  } else {
    switch (c) {
    case ENTER: /* Enter */
      editorInsertNewline();
      break;
    case BACKSPACE: /* Backspace */
    case DEL_KEY:
      editorDelChar();
      break;
    case PAGE_UP:
    case PAGE_DOWN:
      if (c == PAGE_UP && E.cy != 0)
        E.cy = 0;
      else if (c == PAGE_DOWN && E.cy != E.screenrows - 1)
        E.cy = E.screenrows - 1;
      {
        unsigned int times = E.screenrows;
        while (times--)
          editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
      break;
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;
    case ESC:
      mode = NOMODE;
      break;
    default:
      editorInsertChar(c);
      break;
    }
  }
  quit_times = KILO_QUIT_TIMES; /* Reset it to the original value. */
}

int editorFileWasModified(void) { return E.dirty; }

void updateWindowSize(void) {
  if (getWindowSize(STDIN_FILENO, STDOUT_FILENO, &E.screenrows,
                    &E.screencols) == -1) {
    perror("Unable to query the screen for size (columns / rows)");
    exit(1);
  }
  E.screenrows -= 2; /* Get room for status bar. */
}

void handleSigWinCh(int unused __attribute__((unused))) {
  updateWindowSize();
  if (E.cy > E.screenrows)
    E.cy = E.screenrows - 1;
  if (E.cx > E.screencols)
    E.cx = E.screencols - 1;
  editorRefreshScreen();
}

void initEditor(void) {
  E.cx = 0;
  E.cy = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  updateWindowSize();
  signal(SIGWINCH, handleSigWinCh);
}
int printHelp(void) {
  printf("Usage: ki <file>\n"
         "Esc then : then q to quit\n"
         "Esc then : then w to save\n"
         "i to insert\n");
  return -1;
}
int main(int argc, char **argv) {
  if (argc != 2)
    return printHelp();
  initEditor();
  editorOpen(argv[1]);
  enableRawMode(STDIN_FILENO);
  while (1) {
    editorRefreshScreen();
    editorProcessKeypress(STDIN_FILENO);
  }
}
