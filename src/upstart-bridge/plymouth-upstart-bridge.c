/* plymouth-upstart-bridge.c - bridge Upstart job state changes to Plymouth
 *
 * Copyright (C) 2010, 2011 Canonical Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Written by: Colin Watson <cjwatson@ubuntu.com>
 */
#include "config.h"

#include <stdbool.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if defined(HAVE_NCURSESW_TERM_H)
#include <ncursesw/term.h>
#elif defined(HAVE_NCURSES_TERM_H)
#include <ncurses/term.h>
#else
#include <term.h>
#endif

#include "ply-boot-client.h"
#include "ply-command-parser.h"
#include "ply-event-loop.h"
#include "ply-logger.h"
#include "ply-upstart-monitor.h"

typedef struct
{
  ply_event_loop_t      *loop;
  ply_boot_client_t     *client;
  ply_upstart_monitor_t *upstart;
  ply_command_parser_t  *command_parser;
} state_t;

/* We don't care about the difference between "not a string capability" and
 * "cancelled or absent".
 */
static const char *
get_string_capability (const char *capname)
{
  const char *value;

  value = tigetstr (capname);
  if (value == (const char *) -1)
    value = NULL;

  return value;
}

static void
update_status (state_t *state,
               ply_upstart_monitor_job_properties_t *job,
               ply_upstart_monitor_instance_properties_t *instance,
               const char *action, bool ok)
{
  int xenl;
  const char *hpa;

  ply_boot_client_update_daemon (state->client, job->name, NULL, NULL, state);

  if (job->description == NULL || job->description[0] == '\0')
    return;

  printf (" * %s%s%s",
          action ? action : "", action ? " " : "", job->description);

  xenl = tigetflag ("xenl");
  hpa = get_string_capability ("hpa");

  if (xenl > 0 && hpa)
    {
      int cols, col;
      char *hpa_out;

      cols = tigetnum ("cols");
      if (cols < 6)
        cols = 80;
      col = cols - 7;

      hpa_out = tiparm (hpa, col);
      fputs (hpa_out, stdout);

      if (ok)
        puts ("[ OK ]");
      else
        {
          const char *setaf, *op;
          char *setaf_out = NULL;

          setaf = get_string_capability ("setaf");
          if (setaf)
            setaf_out = tiparm (setaf, 1); /* red */
          op = get_string_capability ("op");

          printf ("[%sfail%s]\n", setaf_out ? setaf_out : "", op ? op : "");
        }
    }
  else
    {
      if (ok)
        puts ("   ...done.");
      else
        puts ("   ...fail!");
    }
}

static void
on_failed (void *data,
           ply_upstart_monitor_job_properties_t *job,
           ply_upstart_monitor_instance_properties_t *instance,
           int status)
{
  state_t *state = data;

  if (job->task)
    update_status (state, job, instance, NULL, false);
  else
    {
      if (strcmp (instance->goal, "start") == 0)
        update_status (state, job, instance, "Starting", false);
      else if (strcmp (instance->goal, "stop") == 0)
        update_status (state, job, instance, "Stopping", false);
    }
}

static void
on_state_changed (void *data, const char *old_state,
                  ply_upstart_monitor_job_properties_t *job,
                  ply_upstart_monitor_instance_properties_t *instance)
{
  state_t      *state = data;

  if (instance->failed)
    return;

  if (job->task)
    {
      if (strcmp (instance->state, "waiting") == 0)
        update_status (state, job, instance, NULL, true);
    }
  else
    {
      if (strcmp (instance->goal, "start") == 0)
        {
          if (strcmp (instance->state, "running") == 0)
            update_status (state, job, instance, "Starting", true);
        }
      else if (strcmp (instance->goal, "stop") == 0)
        {
          if (strcmp (instance->state, "waiting") == 0)
            update_status (state, job, instance, "Stopping", true);
        }
    }
}

static void
on_disconnect (state_t *state)
{
  ply_trace ("disconnected from boot status daemon");
  ply_event_loop_exit (state->loop, 0);
}

int
main (int    argc,
      char **argv)
{
  state_t state = { 0 };
  bool should_help, should_be_verbose;
  bool is_connected;
  int exit_code;

  exit_code = 0;

  signal (SIGPIPE, SIG_IGN);

  state.loop = ply_event_loop_new ();
  state.client = ply_boot_client_new ();
  state.command_parser = ply_command_parser_new ("plymouth-upstart-bridge", "Upstart job state bridge");

  ply_command_parser_add_options (state.command_parser,
                                  "help", "This help message", PLY_COMMAND_OPTION_TYPE_FLAG,
                                  "debug", "Enable verbose debug logging", PLY_COMMAND_OPTION_TYPE_FLAG,
                                  NULL);

  if (!ply_command_parser_parse_arguments (state.command_parser, state.loop, argv, argc))
    {
      char *help_string;

      help_string = ply_command_parser_get_help_string (state.command_parser);

      ply_error ("%s", help_string);

      free (help_string);
      return 1;
    }

  ply_command_parser_get_options (state.command_parser,
                                  "help", &should_help,
                                  "debug", &should_be_verbose,
                                  NULL);

  if (should_help)
    {
      char *help_string;

      help_string = ply_command_parser_get_help_string (state.command_parser);

      puts (help_string);

      free (help_string);
      return 0;
    }

  if (should_be_verbose && !ply_is_tracing ())
    ply_toggle_tracing ();

  setupterm (NULL, STDOUT_FILENO, NULL);

  is_connected = ply_boot_client_connect (state.client,
                                          (ply_boot_client_disconnect_handler_t)
                                          on_disconnect, &state);
  if (!is_connected)
    {
      ply_trace ("daemon not running");
      return 1;
    }

  ply_boot_client_attach_to_event_loop (state.client, state.loop);
  state.upstart = ply_upstart_monitor_new (state.loop);
  if (!state.upstart)
    return 1;
  ply_upstart_monitor_add_state_changed_handler (state.upstart,
                                                 on_state_changed, &state);
  ply_upstart_monitor_add_failed_handler (state.upstart, on_failed, &state);

  exit_code = ply_event_loop_run (state.loop);

  ply_upstart_monitor_free (state.upstart);
  ply_boot_client_free (state.client);

  ply_event_loop_free (state.loop);

  return exit_code;
}
/* vim: set ts=4 sw=4 expandtab autoindent cindent cino={.5s,(0: */
