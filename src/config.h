#ifndef HTERM_CONFIG_H
#define HTERM_CONFIG_H

/* ---- History / log management ---------------------------------------- */
#define HIST_DIR_NAME      ".hterm"          /* created inside $HOME          */
#define HIST_FILE_NAME     "history.log"     /* user, cwd, status, command    */
#define HIST_MAX_ENTRIES   1000              /* newest N commands kept        */
#define HIST_MAX_BYTES     (256 * 1024)      /* log file never grows past this*/

/* ---- UI --------------------------------------------------------------- */
#define TERM_MAX_LINES     5000              /* scrollback lines kept in view */
#define GRAPH_N            60                /* CPU graph samples             */
#define TASK_ROWS          6                 /* rows in the task table        */

#endif
