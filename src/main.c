/* main.c - boot messages monitor
 *
 * Copyright (C) 2007 Red Hat, Inc
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Written by: Ray Strode <rstrode@redhat.com>
 */
#include "config.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <sysexits.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wchar.h>
#include <paths.h>
#include <assert.h>

#include <linux/kd.h>
#include <linux/vt.h>

#include "ply-buffer.h"
#include "ply-command-parser.h"
#include "ply-boot-server.h"
#include "ply-boot-splash.h"
#include "ply-event-loop.h"
#include "ply-list.h"
#include "ply-logger.h"
#include "ply-terminal-session.h"
#include "ply-trigger.h"
#include "ply-utils.h"
#include "ply-progress.h"

#ifndef PLY_MAX_COMMAND_LINE_SIZE
#define PLY_MAX_COMMAND_LINE_SIZE 512
#endif

#define BOOT_DURATION_FILE     PLYMOUTH_TIME_DIRECTORY "/boot-duration"
#define SHUTDOWN_DURATION_FILE PLYMOUTH_TIME_DIRECTORY "/shutdown-duration"

typedef enum {
  PLY_MODE_BOOT,
  PLY_MODE_SHUTDOWN
} ply_mode_t;

typedef struct 
{
  const char    *keys;
  ply_trigger_t *trigger;
} ply_keystroke_watch_t;

typedef struct 
{
  enum {PLY_ENTRY_TRIGGER_TYPE_PASSWORD,
        PLY_ENTRY_TRIGGER_TYPE_QUESTION}
        type;
  const char    *prompt;
  ply_trigger_t *trigger;
} ply_entry_trigger_t;

typedef struct
{
  ply_event_loop_t *loop;
  ply_boot_server_t *boot_server;
  ply_list_t *pixel_displays;
  ply_list_t *text_displays;
  ply_keyboard_t *keyboard;
  ply_boot_splash_t *boot_splash;
  ply_terminal_session_t *session;
  ply_buffer_t *boot_buffer;
  ply_progress_t *progress;
  ply_list_t *keystroke_triggers;
  ply_list_t *entry_triggers;
  ply_buffer_t *entry_buffer;
  ply_command_parser_t *command_parser;
  ply_mode_t mode;
  ply_renderer_t *renderer;
  ply_terminal_t *terminal;

  ply_trigger_t *deactivate_trigger;
  ply_trigger_t *quit_trigger;

  char kernel_command_line[PLY_MAX_COMMAND_LINE_SIZE];
  uint32_t no_boot_log : 1;
  uint32_t showing_details : 1;
  uint32_t system_initialized : 1;
  uint32_t is_redirected : 1;
  uint32_t is_attached : 1;
  uint32_t should_be_attached : 1;
  uint32_t should_retain_splash : 1;
  uint32_t is_inactive : 1;

  char *kernel_console_tty;
  char *override_splash_path;
  char *system_default_splash_path;
  char *distribution_default_splash_path;
  const char *default_tty;

  int number_of_errors;
} state_t;

static ply_boot_splash_t *start_boot_splash (state_t    *state,
                                             const char *theme_path);

static void add_display_and_keyboard_for_terminal (state_t    *state,
                                                   const char *tty_name);

static void add_default_displays_and_keyboard (state_t *state);

static bool attach_to_running_session (state_t *state);
static void on_escape_pressed (state_t *state);
static void dump_details_and_quit_splash (state_t *state);
static void update_display (state_t *state);

static void on_error_message (ply_buffer_t *debug_buffer,
                              const void   *bytes,
                              size_t        number_of_bytes);
static ply_buffer_t *debug_buffer;
static char *debug_buffer_path = NULL;
static char *pid_file = NULL;
static void check_for_consoles (state_t    *state,
                                const char *default_tty,
                                bool        should_add_displays);
static void toggle_between_splash_and_details (state_t *state);

static void
on_session_output (state_t    *state,
                   const char *output,
                   size_t      size)
{
  ply_buffer_append_bytes (state->boot_buffer, output, size);
  if (state->boot_splash != NULL)
    ply_boot_splash_update_output (state->boot_splash,
                                   output, size);
}

static void
on_session_finished (state_t *state)
{
  ply_trace ("got hang up on terminal session fd");
}

static void
on_update (state_t     *state,
           const char  *status)
{
  ply_trace ("updating status to '%s'", status);
  ply_progress_status_update (state->progress,
                               status);
  if (state->boot_splash != NULL)
    ply_boot_splash_update_status (state->boot_splash,
                                   status);
}

static void
show_detailed_splash (state_t *state)
{
  if (state->boot_splash != NULL)
    return;

  ply_trace ("Showing detailed splash screen");
  state->boot_splash = start_boot_splash (state,
                                          PLYMOUTH_THEME_PATH "details/details.plymouth");

  if (state->boot_splash == NULL)
    {
      ply_trace ("Could not start detailed splash screen, exiting");
      exit (1);
    }
}

static void
find_override_splash (state_t *state)
{
  char *splash_string;

  if (state->override_splash_path != NULL)
      return;

  splash_string = strstr (state->kernel_command_line, "plymouth:splash=");

  if (splash_string != NULL)
    {
      char *end;
      splash_string = strdup (splash_string + strlen ("plymouth:splash="));

      end = strstr (splash_string, " ");

      if (end != NULL)
        *end = '\0';

      ply_trace ("Splash is configured to be '%s'", splash_string);

      asprintf (&state->override_splash_path,
                PLYMOUTH_THEME_PATH "%s/%s.plymouth",
                splash_string, splash_string);
    }
}

static void
find_system_default_splash (state_t *state)
{
  ply_key_file_t *key_file;
  char *splash_string;

  if (state->system_default_splash_path != NULL)
      return;

  ply_trace ("Trying to load " PLYMOUTH_CONF_DIR "plymouthd.conf");
  key_file = ply_key_file_new (PLYMOUTH_CONF_DIR "plymouthd.conf");

  if (!ply_key_file_load (key_file))
    {
      ply_trace ("failed to load " PLYMOUTH_CONF_DIR "plymouthd.conf");
      ply_key_file_free (key_file);
      return;
    }

  splash_string = ply_key_file_get_value (key_file, "Daemon", "Theme");

  ply_trace ("System default splash is configured to be '%s'", splash_string);

  asprintf (&state->system_default_splash_path,
            PLYMOUTH_THEME_PATH "%s/%s.plymouth",
            splash_string, splash_string);
  free (splash_string);
}

static void
find_distribution_default_splash (state_t *state)
{
  ply_key_file_t *key_file;
  char *splash_string;

  if (state->distribution_default_splash_path != NULL)
      return;

  ply_trace ("Trying to load " PLYMOUTH_POLICY_DIR "plymouthd.defaults");
  key_file = ply_key_file_new (PLYMOUTH_POLICY_DIR "plymouthd.defaults");

  if (!ply_key_file_load (key_file))
    {
      ply_trace ("failed to load " PLYMOUTH_POLICY_DIR "plymouthd.defaults");
      ply_key_file_free (key_file);
      return;
    }

  splash_string = ply_key_file_get_value (key_file, "Daemon", "Theme");

  ply_trace ("Distribution default splash is configured to be '%s'", splash_string);

  asprintf (&state->distribution_default_splash_path,
            PLYMOUTH_THEME_PATH "%s/%s.plymouth",
            splash_string, splash_string);
  free (splash_string);
}

static void
show_default_splash (state_t *state)
{
  if (state->boot_splash != NULL)
    return;

  ply_trace ("Showing splash screen");
  find_override_splash (state);
  if (state->override_splash_path != NULL)
    {
      ply_trace ("Trying override splash at '%s'", state->override_splash_path);
      state->boot_splash = start_boot_splash (state,
                                              state->override_splash_path);
    }

  find_system_default_splash (state);
  if (state->boot_splash == NULL &&
      state->system_default_splash_path != NULL)
    {
      ply_trace ("Trying system default splash");
      state->boot_splash = start_boot_splash (state,
                                              state->system_default_splash_path);
    }

  find_distribution_default_splash (state);
  if (state->boot_splash == NULL &&
      state->distribution_default_splash_path != NULL)
    {
      ply_trace ("Trying distribution default splash");
      state->boot_splash = start_boot_splash (state,
                                              state->distribution_default_splash_path);
    }

  if (state->boot_splash == NULL)
    {
      ply_trace ("Trying old scheme for default splash");
      state->boot_splash = start_boot_splash (state,
                                              PLYMOUTH_THEME_PATH "default.plymouth");
    }

  if (state->boot_splash == NULL)
    {
      ply_trace ("Could not start default splash screen,"
                 "showing text splash screen");
      state->boot_splash = start_boot_splash (state,
                                              PLYMOUTH_THEME_PATH "text/text.plymouth");
    }

  if (state->boot_splash == NULL)
    ply_error ("could not start boot splash: %m");
}

static void
on_ask_for_password (state_t      *state,
                     const char   *prompt,
                     ply_trigger_t *answer)
{
  ply_entry_trigger_t *entry_trigger;

  /* No splash, client will have to get password
   */
  if (state->boot_splash == NULL)
    {
      ply_trace ("no splash loaded, replying immediately with no password");
      ply_trigger_pull (answer, NULL);
      return;
    }

  entry_trigger = calloc (1, sizeof (ply_entry_trigger_t));
  entry_trigger->type = PLY_ENTRY_TRIGGER_TYPE_PASSWORD;
  entry_trigger->prompt = prompt;
  entry_trigger->trigger = answer;
  ply_trace ("queuing password request with boot splash");
  ply_list_append_data (state->entry_triggers, entry_trigger);
  update_display (state);
}

static void
on_ask_question (state_t      *state,
                 const char   *prompt,
                 ply_trigger_t *answer)
{
  ply_entry_trigger_t *entry_trigger;

  entry_trigger = calloc (1, sizeof (ply_entry_trigger_t));
  entry_trigger->type = PLY_ENTRY_TRIGGER_TYPE_QUESTION;
  entry_trigger->prompt = prompt;
  entry_trigger->trigger = answer;
  ply_trace ("queuing question with boot splash");
  ply_list_append_data (state->entry_triggers, entry_trigger);
  update_display (state);
}

static void
on_display_message (state_t       *state,
                    const char    *message)
{
  ply_trace ("displaying message %s", message);
  if (state->boot_splash != NULL)
    ply_boot_splash_display_message (state->boot_splash, message);
}

static void
on_watch_for_keystroke (state_t      *state,
                     const char    *keys,
                     ply_trigger_t *trigger)
{
  ply_keystroke_watch_t *keystroke_trigger =
                                  calloc (1, sizeof (ply_keystroke_watch_t));
  ply_trace ("watching for keystroke");
  keystroke_trigger->keys = keys;
  keystroke_trigger->trigger = trigger;
  ply_list_append_data (state->keystroke_triggers, keystroke_trigger);
}

static void
on_ignore_keystroke (state_t      *state,
                     const char    *keys)
{
  ply_list_node_t *node;

  ply_trace ("ignoring for keystroke");

  for (node = ply_list_get_first_node (state->keystroke_triggers); node;
                    node = ply_list_get_next_node (state->keystroke_triggers, node))
    {
      ply_keystroke_watch_t* keystroke_trigger = ply_list_node_get_data (node);
      if ((!keystroke_trigger->keys && !keys) ||
          (keystroke_trigger->keys && keys && strcmp(keystroke_trigger->keys, keys)==0))
        {
          ply_trigger_pull (keystroke_trigger->trigger, NULL);
          ply_list_remove_node (state->keystroke_triggers, node);
          return;
        }
    }
}

static void
on_progress_pause (state_t *state)
{
  ply_trace ("pausing progress");
  ply_progress_pause (state->progress);
}

static void
on_progress_unpause (state_t *state)
{
  ply_trace ("unpausing progress");
  ply_progress_unpause (state->progress);
}

static void
on_newroot (state_t    *state,
            const char *root_dir)
{
  if (state->mode != PLY_MODE_BOOT)
    {
      ply_trace ("new root is only supported in boot mode ");
      return;
    }

  ply_trace ("new root mounted at \"%s\", switching to it", root_dir);
  chdir(root_dir);
  chroot(".");
  chdir("/");
  ply_progress_load_cache (state->progress, BOOT_DURATION_FILE);
  if (state->boot_splash != NULL)
    ply_boot_splash_root_mounted (state->boot_splash);
}

static const char *
get_cache_file_for_mode (ply_mode_t mode)
{
  const char *filename;

  switch ((int)mode)
    {
    case PLY_MODE_BOOT:
      filename = BOOT_DURATION_FILE;
      break;
    case PLY_MODE_SHUTDOWN:
      filename = SHUTDOWN_DURATION_FILE;
      break;
    default:
      fprintf (stderr, "Unhandled case in %s line %d\n", __FILE__, __LINE__);
      abort ();
      break;
    }

  ply_trace ("returning cache file '%s'", filename);
  return filename;
}

static const char *
get_log_file_for_mode (ply_mode_t mode)
{
  const char *filename;

  switch ((int)mode)
    {
    case PLY_MODE_BOOT:
      filename = PLYMOUTH_LOG_DIRECTORY "/boot.log";
      break;
    case PLY_MODE_SHUTDOWN:
      filename = _PATH_DEVNULL;
      break;
    default:
      fprintf (stderr, "Unhandled case in %s line %d\n", __FILE__, __LINE__);
      abort ();
      break;
    }

  ply_trace ("returning log file '%s'", filename);
  return filename;
}

static const char *
get_log_spool_file_for_mode (ply_mode_t mode)
{
  const char *filename;

  switch ((int)mode)
    {
    case PLY_MODE_BOOT:
      filename = PLYMOUTH_SPOOL_DIRECTORY "/boot.log";
      break;
    case PLY_MODE_SHUTDOWN:
      filename = NULL;
      break;
    default:
      fprintf (stderr, "Unhandled case in %s line %d\n", __FILE__, __LINE__);
      abort ();
      break;
    }

  ply_trace ("returning spool file '%s'", filename);
  return filename;
}

static void
spool_error (state_t *state)
{
  const char *logfile;
  const char *logspool;

  ply_trace ("spooling error for viewer");

  logfile = get_log_file_for_mode (state->mode);
  logspool = get_log_spool_file_for_mode (state->mode);

  if (logfile != NULL && logspool != NULL)
    {
      unlink (logspool);

      ply_create_file_link (logfile, logspool);
    }
}

static void
prepare_logging (state_t *state)
{
  const char *logfile;

  if (!state->system_initialized)
    {
      ply_trace ("not preparing logging yet, system not initialized");
      return;
    }

  if (state->session == NULL)
    {
      ply_trace ("not preparing logging, no session");
      return;
    }

  logfile = get_log_file_for_mode (state->mode);
  if (logfile != NULL)
    {
      ply_trace ("opening log '%s'", logfile);
      ply_terminal_session_open_log (state->session, logfile);

      if (state->number_of_errors > 0)
        spool_error (state);
    }
}

static void
on_system_initialized (state_t *state)
{
  ply_trace ("system now initialized, opening log");
  state->system_initialized = true;

  prepare_logging (state);
}

static void
on_error (state_t *state)
{
  ply_trace ("encountered error during boot up");

  if (state->system_initialized && state->number_of_errors == 0)
    spool_error (state);
  else
    ply_trace ("not spooling because number of errors %d", state->number_of_errors);

  state->number_of_errors++;
}

static bool
plymouth_should_ignore_show_splash_calls (state_t *state)
{
  ply_trace ("checking if plymouth should be running");
  if (state->mode != PLY_MODE_BOOT || ply_string_has_prefix (state->kernel_command_line, "plymouth:force-splash") || strstr (state->kernel_command_line, " plymouth:force-splash") != NULL)
      return false;

  return ply_string_has_prefix (state->kernel_command_line, "init=") || strstr (state->kernel_command_line, " init=") != NULL;
}

static bool
plymouth_should_show_default_splash (state_t *state)
{
  ply_trace ("checking if plymouth should show default splash");

  const char const *strings[] = {
      " single ", " single\n", "^single ",
      " 1 ", " 1\n", "^1 ",
      " s ", " s\n", "^s ",
      " S ", " S\n", "^S ",
      " -s ", " -s\n", "^-s ",
      NULL
  };
  int i;

  if (state->kernel_console_tty != NULL)
    return false;

  for (i = 0; strings[i] != NULL; i++)
    {
      int cmp;
      if (strings[i][0] == '^')
          cmp = strncmp(state->kernel_command_line, strings[i]+1,
                        strlen(strings[i]+1)) == 0;
      else
          cmp = strstr (state->kernel_command_line, strings[i]) != NULL;

      if (cmp)
        {
          ply_trace ("kernel command line has option \"%s\"", strings[i]);
          return false;
        }
    }

  return strstr (state->kernel_command_line, "rhgb") != NULL || (strstr (state->kernel_command_line, "splash") != NULL && strstr(state->kernel_command_line, "splash=verbose") == NULL);
}

static void
remove_displays_and_keyboard (state_t *state)
{
  ply_list_node_t *node;
  ply_trace ("removing displays and keyboard");

  node = ply_list_get_first_node (state->pixel_displays);
  while (node != NULL)
    {
      ply_list_node_t *next_node;
      ply_pixel_display_t *display;

      ply_trace ("removing pixel display");
      next_node = ply_list_get_next_node (state->pixel_displays, node);
      display = ply_list_node_get_data (node);
      ply_pixel_display_free (display);

      ply_list_remove_node (state->pixel_displays, node);

      node = next_node;
    }

  node = ply_list_get_first_node (state->text_displays);
  while (node != NULL)
    {
      ply_list_node_t *next_node;
      ply_text_display_t *display;

      ply_trace ("removing text display");
      next_node = ply_list_get_next_node (state->text_displays, node);
      display = ply_list_node_get_data (node);
      ply_text_display_free (display);

      ply_list_remove_node (state->text_displays, node);

      node = next_node;
    }

  if (state->keyboard != NULL)
    {
      ply_trace ("removing keyboard");
      ply_keyboard_stop_watching_for_input (state->keyboard);
      ply_keyboard_free (state->keyboard);
      state->keyboard = NULL;
    }
}

static void
on_show_splash (state_t *state)
{
  bool has_display;

  if (state->is_inactive)
    {
      ply_trace ("show splash called while inactive");
      return;
    }

  if (plymouth_should_ignore_show_splash_calls (state))
    {
      ply_trace ("show splash called while ignoring show splash calls");
      dump_details_and_quit_splash (state);
      return;
    }

  check_for_consoles (state, state->default_tty, true);

  has_display = ply_list_get_length (state->pixel_displays) > 0 ||
                ply_list_get_length (state->text_displays) > 0;

  if (!state->is_attached && state->should_be_attached && has_display)
    attach_to_running_session (state);

  if (!has_display && state->is_attached)
    {
      ply_trace ("no open seats, detaching session");
      ply_terminal_session_detach (state->session);
      state->is_redirected = false;
      state->is_attached = false;
    }

  if (plymouth_should_show_default_splash (state))
    {
      show_default_splash (state);
      state->showing_details = false;
    }
  else
    {
      show_detailed_splash (state);
      state->showing_details = true;
    }
}

static void
quit_splash (state_t *state)
{
  ply_trace ("quiting splash");
  if (state->boot_splash != NULL)
    {
      ply_trace ("freeing splash");
      ply_boot_splash_free (state->boot_splash);
      state->boot_splash = NULL;
    }

  ply_trace ("removing displays and keyboard");
  remove_displays_and_keyboard (state);

  if (state->renderer != NULL)
    {
      ply_renderer_close (state->renderer);
      ply_renderer_free (state->renderer);
      state->renderer = NULL;
    }

  if (state->terminal != NULL)
    {
      if (!state->should_retain_splash)
        {
          ply_trace ("Not retaining splash, so deallocating VT");
          ply_terminal_deactivate_vt (state->terminal);
        }
      ply_terminal_close (state->terminal);
      ply_terminal_free (state->terminal);
      state->terminal = NULL;
    }

  if (state->session != NULL)
    {
      ply_trace ("detaching session");
      ply_terminal_session_detach (state->session);
      state->is_redirected = false;
      state->is_attached = false;
    }
}

static void
dump_details_and_quit_splash (state_t *state)
{
  state->showing_details = false;
  toggle_between_splash_and_details (state);

  if (state->renderer != NULL)
    ply_renderer_deactivate (state->renderer);
  if (state->boot_splash != NULL)
    ply_boot_splash_hide (state->boot_splash);

  quit_splash (state);
}

static void
on_hide_splash (state_t *state)
{
  if (state->is_inactive)
    return;

  if (state->boot_splash == NULL)
    return;

  ply_trace ("hiding boot splash");
  dump_details_and_quit_splash (state);
}

#ifdef PLY_ENABLE_GDM_TRANSITION
static void
tell_gdm_to_transition (void)
{
  int fd;

  fd = creat ("/var/spool/gdm/force-display-on-active-vt", 0644);
  close (fd);
}
#endif

static void
quit_program (state_t *state)
{
  ply_trace ("exiting event loop");
  ply_event_loop_exit (state->loop, 0);

  if (pid_file != NULL)
    {
      unlink (pid_file);
      free (pid_file);
      pid_file = NULL;
    }

#ifdef PLY_ENABLE_GDM_TRANSITION
  if (state->should_retain_splash)
    {
      tell_gdm_to_transition ();
    }
#endif

  if (state->deactivate_trigger != NULL)
    {
      ply_trigger_pull (state->deactivate_trigger, NULL);
      state->deactivate_trigger = NULL;
    }
  if (state->quit_trigger != NULL)
    {
      ply_trigger_pull (state->quit_trigger, NULL);
      state->quit_trigger = NULL;
    }
}

static void
deactivate_splash (state_t *state)
{
  assert (!state->is_inactive);

  if (state->renderer != NULL)
    {
      ply_trace ("deactivating renderer");
      ply_renderer_deactivate (state->renderer);
    }

  if (state->keyboard != NULL)
    {
      ply_trace ("deactivating keyboard");
      ply_keyboard_stop_watching_for_input (state->keyboard);
    }

  if ((state->session != NULL) && state->is_attached)
    {
      ply_trace ("deactivating terminal session");
      ply_terminal_session_detach (state->session);
      state->is_redirected = false;
      state->is_attached = false;
    }

  if (state->terminal != NULL)
    {
      ply_trace ("deactivating terminal");
      ply_terminal_stop_watching_for_vt_changes (state->terminal);
      ply_terminal_set_buffered_input (state->terminal);
      ply_terminal_ignore_mode_changes (state->terminal, true);
    }

  state->is_inactive = true;

  ply_trigger_pull (state->deactivate_trigger, NULL);
  state->deactivate_trigger = NULL;
}

static void
on_boot_splash_idle (state_t *state)
{
  ply_trace ("boot splash idle");

  /* In the case where we've received both a deactivate command and a
   * quit command, the quit command takes precedence.
   */
  if (state->quit_trigger != NULL)
    {
      if (!state->should_retain_splash)
        {
          ply_trace ("hiding splash");
          if (state->renderer != NULL)
            ply_renderer_deactivate (state->renderer);
          if (state->boot_splash != NULL)
            ply_boot_splash_hide (state->boot_splash);
        }

      ply_trace ("quitting splash");
      quit_splash (state);
      ply_trace ("quitting program");
      quit_program (state);
    }
  else if (state->deactivate_trigger != NULL)
    {
      ply_trace ("deactivating splash");
      deactivate_splash (state);
    }
}


static void
on_deactivate (state_t       *state,
               ply_trigger_t *deactivate_trigger)
{
  if ((state->deactivate_trigger != NULL) || state->is_inactive)
    {
      ply_trigger_add_handler (state->deactivate_trigger,
                               (ply_trigger_handler_t)
                               ply_trigger_pull,
                               deactivate_trigger);
      return;
    }

  state->deactivate_trigger = deactivate_trigger;

  ply_trace ("deactivating");
  if (state->boot_splash != NULL)
    {
      ply_boot_splash_become_idle (state->boot_splash,
                                   (ply_boot_splash_on_idle_handler_t)
                                   on_boot_splash_idle,
                                   state);
    }
  else
    {
      ply_trace ("deactivating splash");
      deactivate_splash (state);
    }
}

static void
on_reactivate (state_t *state)
{
  if (!state->is_inactive)
    return;

  if (state->terminal != NULL)
    {
      ply_terminal_watch_for_vt_changes (state->terminal);
      ply_terminal_set_unbuffered_input (state->terminal);
      ply_terminal_ignore_mode_changes (state->terminal, false);
    }

  if ((state->session != NULL) && state->should_be_attached)
    {
      ply_trace ("reactivating terminal session");
      attach_to_running_session (state);
    }

  if (state->keyboard != NULL)
    {
      ply_trace ("activating keyboard");
      ply_keyboard_watch_for_input (state->keyboard);
    }

  if (state->renderer != NULL)
    {
      ply_trace ("activating renderer");
      ply_renderer_activate (state->renderer);
    }

  state->is_inactive = false;

  update_display (state);
}

static void
on_quit (state_t       *state,
         bool           retain_splash,
         ply_trigger_t *quit_trigger)
{
  if (state->quit_trigger != NULL)
    {
      ply_trigger_add_handler (state->quit_trigger,
                               (ply_trigger_handler_t)
                               ply_trigger_pull,
                               quit_trigger);
      return;
    }

  state->quit_trigger = quit_trigger;
  state->should_retain_splash = retain_splash;

  ply_trace ("time to quit, closing log");
  if (state->session != NULL)
    ply_terminal_session_close_log (state->session);
  ply_trace ("unloading splash");

  if (state->is_inactive && !retain_splash)
    {
      /* We've been deactivated and X failed to start
       */
      dump_details_and_quit_splash (state);
      quit_program (state);
    }
  else if (state->boot_splash != NULL)
    {
      ply_boot_splash_become_idle (state->boot_splash,
                                   (ply_boot_splash_on_idle_handler_t)
                                   on_boot_splash_idle,
                                   state);
    }
  else
    quit_program (state);
}

static bool
on_has_active_vt (state_t *state)
{
  if (state->terminal != NULL)
    return ply_terminal_is_active (state->terminal);
  else
    return false;
}

static ply_boot_server_t *
start_boot_server (state_t *state)
{
  ply_boot_server_t *server;

  server = ply_boot_server_new ((ply_boot_server_update_handler_t) on_update,
                                (ply_boot_server_ask_for_password_handler_t) on_ask_for_password,
                                (ply_boot_server_ask_question_handler_t) on_ask_question,
                                (ply_boot_server_display_message_handler_t) on_display_message,
                                (ply_boot_server_watch_for_keystroke_handler_t) on_watch_for_keystroke,
                                (ply_boot_server_ignore_keystroke_handler_t) on_ignore_keystroke,
                                (ply_boot_server_progress_pause_handler_t) on_progress_pause,
                                (ply_boot_server_progress_unpause_handler_t) on_progress_unpause,
                                (ply_boot_server_show_splash_handler_t) on_show_splash,
                                (ply_boot_server_hide_splash_handler_t) on_hide_splash,
                                (ply_boot_server_newroot_handler_t) on_newroot,
                                (ply_boot_server_system_initialized_handler_t) on_system_initialized,
                                (ply_boot_server_error_handler_t) on_error,
                                (ply_boot_server_deactivate_handler_t) on_deactivate,
                                (ply_boot_server_reactivate_handler_t) on_reactivate,
                                (ply_boot_server_quit_handler_t) on_quit,
                                (ply_boot_server_has_active_vt_handler_t) on_has_active_vt,
                                state);

  if (!ply_boot_server_listen (server))
    {
      ply_save_errno ();
      ply_boot_server_free (server);
      ply_restore_errno ();
      return NULL;
    }

  ply_boot_server_attach_to_event_loop (server, state->loop);

  return server;
}


static void
update_display (state_t *state)
{
  if (!state->boot_splash) return;
  
  ply_list_node_t *node;
  node = ply_list_get_first_node (state->entry_triggers);
  if (node)
    {
      ply_entry_trigger_t* entry_trigger = ply_list_node_get_data (node);
      if (entry_trigger->type == PLY_ENTRY_TRIGGER_TYPE_PASSWORD)
        {
          int bullets = ply_utf8_string_get_length (ply_buffer_get_bytes (state->entry_buffer),
                                                    ply_buffer_get_size (state->entry_buffer));
          bullets = MAX(0, bullets);
          ply_boot_splash_display_password (state->boot_splash, 
                                            entry_trigger->prompt,
                                            bullets);
        }
      else if (entry_trigger->type == PLY_ENTRY_TRIGGER_TYPE_QUESTION)
        {
          ply_boot_splash_display_question (state->boot_splash,
                                            entry_trigger->prompt,
                                            ply_buffer_get_bytes (state->entry_buffer));
        }
      else {
          ply_trace("unkown entry type");
        }
    }
  else
    {
      ply_boot_splash_display_normal (state->boot_splash);
    }

}

static void
toggle_between_splash_and_details (state_t *state)
{
  ply_trace ("toggling between splash and details");
  if (state->boot_splash != NULL)
    {
      ply_trace ("hiding and freeing current splash");
      ply_boot_splash_hide (state->boot_splash);
      ply_boot_splash_free (state->boot_splash);
      state->boot_splash = NULL;
    }

  if (!state->showing_details)
    {
      show_detailed_splash (state);
      state->showing_details = true;
    }
  else
    {
      show_default_splash (state);
      state->showing_details = false;
    }
  update_display (state);
}

static void
on_escape_pressed (state_t *state)
{
  ply_trace ("escape key pressed");
  toggle_between_splash_and_details (state);
}

static void
on_keyboard_input (state_t                  *state,
                   const char               *keyboard_input,
                   size_t                    character_size)
{
  ply_list_node_t *node;
  node = ply_list_get_first_node (state->entry_triggers);
  if (node)
    {               /* \x3 (ETX) is Ctrl+C and \x4 (EOT) is Ctrl+D */
      if (character_size == 1 && ( keyboard_input[0] == '\x3' || keyboard_input[0] == '\x4' ))
        {
          ply_entry_trigger_t* entry_trigger = ply_list_node_get_data (node);
          ply_trigger_pull (entry_trigger->trigger, "\x3");
          ply_buffer_clear (state->entry_buffer);
          ply_list_remove_node (state->entry_triggers, node);
          free (entry_trigger);
        }
      else
        {
          ply_buffer_append_bytes (state->entry_buffer, keyboard_input, character_size);
        }
      update_display (state);
    }
  else
    {
      for (node = ply_list_get_first_node (state->keystroke_triggers); node;
                        node = ply_list_get_next_node (state->keystroke_triggers, node))
        {
          ply_keystroke_watch_t* keystroke_trigger = ply_list_node_get_data (node);
          if (!keystroke_trigger->keys || strstr(keystroke_trigger->keys, keyboard_input))  /* assume strstr works on utf8 arrays */
            {
              ply_trigger_pull (keystroke_trigger->trigger, keyboard_input);
              ply_list_remove_node (state->keystroke_triggers, node);
              free(keystroke_trigger);
              return;
            }
        }
      return;
    }
}

static void
on_backspace (state_t                  *state)
{
  ssize_t bytes_to_remove;
  ssize_t previous_character_size;
  const char *bytes;
  size_t size;
  ply_list_node_t *node = ply_list_get_first_node (state->entry_triggers);
  if (!node) return;

  bytes = ply_buffer_get_bytes (state->entry_buffer);
  size = ply_buffer_get_size (state->entry_buffer);

  bytes_to_remove = MIN(size, PLY_UTF8_CHARACTER_SIZE_MAX);
  while ((previous_character_size = ply_utf8_character_get_size (bytes + size - bytes_to_remove, bytes_to_remove)) < bytes_to_remove)
    {
      if (previous_character_size > 0)
        bytes_to_remove -= previous_character_size;
      else
        bytes_to_remove--;
    }

  ply_buffer_remove_bytes_at_end (state->entry_buffer, bytes_to_remove);
  update_display (state);
}

static void
on_enter (state_t                  *state,
          const char               *line)
{
  ply_list_node_t *node;
  node = ply_list_get_first_node (state->entry_triggers);
  if (node)
    {
      ply_entry_trigger_t* entry_trigger = ply_list_node_get_data (node);
      const char* reply_text = ply_buffer_get_bytes (state->entry_buffer);
      ply_trigger_pull (entry_trigger->trigger, reply_text);
      ply_buffer_clear (state->entry_buffer);
      ply_list_remove_node (state->entry_triggers, node);
      free (entry_trigger);
      update_display (state);
    }
  else
    {
      for (node = ply_list_get_first_node (state->keystroke_triggers); node;
                        node = ply_list_get_next_node (state->keystroke_triggers, node))
        {
          ply_keystroke_watch_t* keystroke_trigger = ply_list_node_get_data (node);
          if (!keystroke_trigger->keys || strstr(keystroke_trigger->keys, "\n"))  /* assume strstr works on utf8 arrays */
            {
              ply_trigger_pull (keystroke_trigger->trigger, line);
              ply_list_remove_node (state->keystroke_triggers, node);
              free(keystroke_trigger);
              return;
            }
        }
      return;
    }
}

static void
set_keyboard (state_t        *state,
              ply_keyboard_t *keyboard)
{
  state->keyboard = keyboard;

  ply_keyboard_add_escape_handler (keyboard, (ply_keyboard_escape_handler_t)
                                   on_escape_pressed, state);
  ply_trace ("listening for keystrokes");
  ply_keyboard_add_input_handler (keyboard,
                                  (ply_keyboard_input_handler_t)
                                  on_keyboard_input, state);
  ply_trace ("listening for backspace");
  ply_keyboard_add_backspace_handler (keyboard,
                                      (ply_keyboard_backspace_handler_t)
                                      on_backspace, state);
  ply_trace ("listening for enter");
  ply_keyboard_add_enter_handler (keyboard,
                                  (ply_keyboard_enter_handler_t)
                                  on_enter, state);
}
static void
add_display_and_keyboard_for_terminal (state_t    *state,
                                       const char *tty_name)
{
  ply_text_display_t *display;
  ply_keyboard_t *keyboard;

  ply_trace ("adding display and keyboard for %s", tty_name);

  state->terminal = ply_terminal_new (tty_name);

  keyboard = ply_keyboard_new_for_terminal (state->terminal);
  display = ply_text_display_new (state->terminal);

  ply_list_append_data (state->text_displays, display);
  state->keyboard = keyboard;
  set_keyboard (state, keyboard);
}

static void
add_pixel_displays_from_renderer (state_t        *state,
                                  ply_renderer_t *renderer)
{
  ply_list_t *heads;
  ply_list_node_t *node;

  heads = ply_renderer_get_heads (renderer);

  ply_trace ("Adding displays for %d heads",
             ply_list_get_length (heads));

  node = ply_list_get_first_node (heads);
  while (node != NULL)
    {
      ply_list_node_t *next_node;
      ply_renderer_head_t *head;
      ply_pixel_display_t *display;

      head = ply_list_node_get_data (node);
      next_node = ply_list_get_next_node (heads, node);

      display = ply_pixel_display_new (renderer, head);

      ply_list_append_data (state->pixel_displays, display);

      node = next_node;
    }

}

static void
add_default_displays_and_keyboard (state_t *state)
{
  ply_renderer_t *renderer;
  ply_keyboard_t *keyboard;
  ply_terminal_t *terminal;
  ply_text_display_t *text_display;

  ply_trace ("adding default displays and keyboard");

  terminal = ply_terminal_new (state->default_tty);

  renderer = ply_renderer_new (NULL, terminal);

  if (!ply_renderer_open (renderer))
    {
      ply_trace ("could not open renderer /dev/fb");
      ply_renderer_free (renderer);
      ply_terminal_free (terminal);

      add_display_and_keyboard_for_terminal (state, state->default_tty);
      return;
    }

  state->terminal = terminal;

  keyboard = ply_keyboard_new_for_renderer (renderer);
  set_keyboard (state, keyboard);

  add_pixel_displays_from_renderer (state, renderer);

  text_display = ply_text_display_new (state->terminal);
  ply_list_append_data (state->text_displays, text_display);

  state->renderer = renderer;
}

static void
add_displays_and_keyboard_to_boot_splash (state_t           *state,
                                          ply_boot_splash_t *splash)
{
  ply_list_node_t *node;

  ply_trace ("setting keyboard on boot splash");
  if (state->keyboard != NULL)
    ply_boot_splash_set_keyboard (splash, state->keyboard);

  node = ply_list_get_first_node (state->pixel_displays);
  while (node != NULL)
    {
      ply_pixel_display_t *display;
      ply_list_node_t *next_node;

      display = ply_list_node_get_data (node);
      next_node = ply_list_get_next_node (state->pixel_displays, node);
      ply_trace ("adding pixel display on boot splash");
      ply_boot_splash_add_pixel_display (splash, display);

      node = next_node;
    }

  node = ply_list_get_first_node (state->text_displays);
  while (node != NULL)
    {
      ply_text_display_t *display;
      ply_list_node_t *next_node;

      display = ply_list_node_get_data (node);
      next_node = ply_list_get_next_node (state->text_displays, node);

      ply_trace ("adding text display on boot splash");
      ply_boot_splash_add_text_display (splash, display);

      node = next_node;
    }
}

static ply_boot_splash_t *
start_boot_splash (state_t    *state,
                   const char *theme_path)
{
  ply_boot_splash_t *splash;
  ply_boot_splash_mode_t splash_mode;

  ply_trace ("Loading boot splash theme '%s'",
             theme_path);

  splash = ply_boot_splash_new (theme_path,
                                PLYMOUTH_PLUGIN_PATH,
                                state->boot_buffer,
                                state->terminal);

  if (!ply_boot_splash_load (splash))
    {
      ply_save_errno ();
      ply_boot_splash_free (splash);
      ply_restore_errno ();
      return NULL;
    }

  ply_trace ("attaching plugin to event loop");
  ply_boot_splash_attach_to_event_loop (splash, state->loop);

  ply_trace ("attaching progress to plugin");
  ply_boot_splash_attach_progress (splash, state->progress);

  add_displays_and_keyboard_to_boot_splash (state, splash);

  ply_trace ("showing plugin");
  if (state->mode == PLY_MODE_SHUTDOWN)
    splash_mode = PLY_BOOT_SPLASH_MODE_SHUTDOWN;
  else
    splash_mode = PLY_BOOT_SPLASH_MODE_BOOT_UP;

  if (!ply_boot_splash_show (splash, splash_mode))
    {
      ply_save_errno ();
      ply_boot_splash_unset_keyboard (splash);
      ply_boot_splash_free (splash);
      ply_restore_errno ();
      return NULL;
    }

  ply_keyboard_watch_for_input (state->keyboard);

  update_display (state);
  return splash;
}

static bool
attach_to_running_session (state_t *state)
{
  ply_terminal_session_t *session;
  ply_terminal_session_flags_t flags;
  bool should_be_redirected;

  flags = 0;

  should_be_redirected = !state->no_boot_log;

  if (should_be_redirected)
    flags |= PLY_TERMINAL_SESSION_FLAGS_REDIRECT_CONSOLE;

 if (state->session == NULL)
   {
     ply_trace ("creating new terminal session");
     session = ply_terminal_session_new (NULL);

     ply_terminal_session_attach_to_event_loop (session, state->loop);
   }
 else
   {
     session = state->session;
     ply_trace ("session already created");
   }

  if (!ply_terminal_session_attach (session, flags,
                                 (ply_terminal_session_output_handler_t)
                                 on_session_output,
                                 (ply_terminal_session_done_handler_t)
                                 (should_be_redirected? on_session_finished: NULL),
                                 -1, state))
    {
      ply_save_errno ();
      ply_terminal_session_free (session);
      ply_buffer_free (state->boot_buffer);
      state->boot_buffer = NULL;
      ply_restore_errno ();

      state->is_redirected = false;
      state->is_attached = false;
      return false;
    }

  state->is_redirected = should_be_redirected;
  state->is_attached = true;
  state->session = session;

  return true;
}

static bool
get_kernel_command_line (state_t *state)
{
  int fd;

  ply_trace ("opening /proc/cmdline");
  fd = open ("proc/cmdline", O_RDONLY);

  if (fd < 0)
    {
      ply_trace ("couldn't open it: %m");
      return false;
    }

  ply_trace ("reading kernel command line");
  if (read (fd, state->kernel_command_line, sizeof (state->kernel_command_line)) < 0)
    {
      ply_trace ("couldn't read it: %m");
      return false;
    }

  ply_trace ("Kernel command line is: '%s'", state->kernel_command_line);
  close (fd);
  return true;
}

static void
check_verbosity (state_t *state)
{
  char *path;

  ply_trace ("checking if tracing should be enabled");

  path = NULL;
  if ((strstr (state->kernel_command_line, " plymouth:debug ") != NULL)
     || (strstr (state->kernel_command_line, "plymouth:debug ") != NULL)
     || (strstr (state->kernel_command_line, " plymouth:debug") != NULL)
     || (path = strstr (state->kernel_command_line, " plymouth:debug=file:")) != NULL)
    {
#ifdef LOG_TO_DEBUG_FILE
      int fd;
#endif

      ply_trace ("tracing should be enabled!");
      if (!ply_is_tracing ())
        ply_toggle_tracing ();

      if (path != NULL && debug_buffer_path == NULL)
        {
          char *end;

          path += strlen (" plymouth:debug=file:");
          debug_buffer_path = strdup (path);
          end = strstr (debug_buffer_path, " ");

          if (end != NULL)
            *end = '\0';

          debug_buffer_path = path;
        }

        if (debug_buffer == NULL)
          debug_buffer = ply_buffer_new ();

#ifdef LOG_TO_DEBUG_FILE
      fd = open ("/dev/console", O_RDWR);
      ply_logger_set_output_fd (ply_logger_get_error_default (), fd);
#endif
    }
  else
    ply_trace ("tracing shouldn't be enabled!");

  if (debug_buffer != NULL)
    {
      if (debug_buffer_path == NULL)
        debug_buffer_path = strdup (PLYMOUTH_LOG_DIRECTORY "/plymouth-debug.log");

      ply_logger_add_filter (ply_logger_get_error_default (),
                             (ply_logger_filter_handler_t)
                             on_error_message,
                             debug_buffer);

    }
}

static void
check_logging (state_t *state)
{
  ply_trace ("checking if console messages should be redirected and logged");

  if ((strstr (state->kernel_command_line, " plymouth:nolog ") != NULL)
     || (strstr (state->kernel_command_line, "plymouth:nolog ") != NULL)
     || (strstr (state->kernel_command_line, " plymouth:nolog") != NULL))
    {
      ply_trace ("logging won't be enabled!");
      state->no_boot_log = true;
    }
  else
    {
      ply_trace ("logging will be enabled!");
      state->no_boot_log = false;
    }
}

static void
check_for_consoles (state_t    *state,
                    const char *default_tty,
                    bool        should_add_displays)
{
  char *console_key;
  char *remaining_command_line;

  ply_trace ("checking for consoles%s",
             should_add_displays? " and adding displays": "");

  remaining_command_line = state->kernel_command_line;
  while ((console_key = strstr (remaining_command_line, " console=")) != NULL)
    {
      char *end;
      ply_trace ("serial console found!");

      free (state->kernel_console_tty);
      state->kernel_console_tty = strdup (console_key + strlen (" console="));

      remaining_command_line = console_key + strlen (" console=");

      end = strpbrk (state->kernel_console_tty, " \n\t\v,");

      if (end != NULL)
        {
          *end = '\0';
          remaining_command_line += end - state->kernel_console_tty;
        }

      if (strcmp (state->kernel_console_tty, "tty0") == 0 || strcmp (state->kernel_console_tty, "/dev/tty0") == 0)
        {
          free (state->kernel_console_tty);
          state->kernel_console_tty = strdup (default_tty);
        }

      if (should_add_displays)
        add_display_and_keyboard_for_terminal (state, state->kernel_console_tty);
    }

    ply_trace ("There are currently %d text displays",
               ply_list_get_length (state->text_displays));
    if (should_add_displays && ply_list_get_length (state->text_displays) == 0)
      add_default_displays_and_keyboard (state);
}

static bool
redirect_standard_io_to_device (const char *device)
{
  int fd;
  char *file;

  ply_trace ("redirecting stdio to %s", device);

  if (strncmp (device, "/dev/", strlen ("/dev/")) == 0)
    file = strdup (device);
  else
    asprintf (&file, "/dev/%s", device);

  fd = open (file, O_RDWR | O_APPEND);

  free (file);

  if (fd < 0)
    return false;

  dup2 (fd, STDIN_FILENO);
  dup2 (fd, STDOUT_FILENO);
  dup2 (fd, STDERR_FILENO);

  close (fd);

  return true;
}

static bool
initialize_environment (state_t *state)
{
  ply_trace ("initializing minimal work environment");

  if (!get_kernel_command_line (state))
    return false;

  check_verbosity (state);
  check_logging (state);

  state->keystroke_triggers = ply_list_new ();
  state->entry_triggers = ply_list_new ();
  state->entry_buffer = ply_buffer_new();
  state->pixel_displays = ply_list_new ();
  state->text_displays = ply_list_new ();
  state->keyboard = NULL;

  if (!state->default_tty)
    {
      if (state->mode == PLY_MODE_SHUTDOWN)
        {
          state->default_tty = SHUTDOWN_TTY;
        }
      else
        state->default_tty = BOOT_TTY;
    }

  check_for_consoles (state, state->default_tty, false);

  if (state->kernel_console_tty != NULL)
    redirect_standard_io_to_device (state->kernel_console_tty);
  else
    redirect_standard_io_to_device (state->default_tty);

  ply_trace ("initialized minimal work environment");
  return true;
}

static void
on_error_message (ply_buffer_t *debug_buffer,
                  const void   *bytes,
                  size_t        number_of_bytes)
{
  ply_buffer_append_bytes (debug_buffer, bytes, number_of_bytes);
}

static void
dump_debug_buffer_to_file (void)
{
  int fd;
  const char *bytes;
  size_t size;

  fd = open (debug_buffer_path,
             O_WRONLY | O_CREAT | O_TRUNC, 0600);

  if (fd < 0)
    return;

  size = ply_buffer_get_size (debug_buffer);
  bytes = ply_buffer_get_bytes (debug_buffer);
  ply_write (fd, bytes, size);
  close (fd);
}

 #include <termios.h>
 #include <unistd.h>
static void
on_crash (int signum)
{
    struct termios term_attributes;
    int fd;

    fd = open ("/dev/tty1", O_RDWR | O_NOCTTY);
    if (fd < 0) fd = open ("/dev/hvc0", O_RDWR | O_NOCTTY);

    ioctl (fd, KDSETMODE, KD_TEXT);

    tcgetattr (fd, &term_attributes);

    term_attributes.c_iflag |= BRKINT | IGNPAR | ISTRIP | ICRNL | IXON;
    term_attributes.c_oflag |= OPOST;
    term_attributes.c_lflag |= ECHO | ICANON | ISIG | IEXTEN;

    tcsetattr (fd, TCSAFLUSH, &term_attributes);

    close (fd);

    if (debug_buffer != NULL)
      {
        dump_debug_buffer_to_file ();
        pause ();
      }

    if (pid_file != NULL)
      {
        unlink (pid_file);
        free (pid_file);
        pid_file = NULL;
      }

    signal (signum, SIG_DFL);
    raise(signum);
}

int
main (int    argc,
      char **argv)
{
  state_t state = { 0 };
  int exit_code;
  bool should_help = false;
  bool no_daemon = false;
  bool debug = false;
  bool attach_to_session;
  ply_daemon_handle_t *daemon_handle;
  char *mode_string = NULL;
  char *tty = NULL;

  state.command_parser = ply_command_parser_new ("plymouthd", "Boot splash control server");

  state.loop = ply_event_loop_get_default ();

  ply_command_parser_add_options (state.command_parser,
                                  "help", "This help message", PLY_COMMAND_OPTION_TYPE_FLAG,
                                  "attach-to-session", "Redirect console messages from screen to log", PLY_COMMAND_OPTION_TYPE_FLAG,
                                  "no-daemon", "Do not daemonize", PLY_COMMAND_OPTION_TYPE_FLAG,
                                  "debug", "Output debugging information", PLY_COMMAND_OPTION_TYPE_FLAG,
                                  "debug-file", "File to output debugging information to", PLY_COMMAND_OPTION_TYPE_STRING,
                                  "mode", "Mode is one of: boot, shutdown", PLY_COMMAND_OPTION_TYPE_STRING,
                                  "pid-file", "Write the pid of the daemon to a file", PLY_COMMAND_OPTION_TYPE_STRING,
                                  "tty", "TTY to use instead of default", PLY_COMMAND_OPTION_TYPE_STRING,
                                  NULL);

  if (!ply_command_parser_parse_arguments (state.command_parser, state.loop, argv, argc))
    {
      char *help_string;

      help_string = ply_command_parser_get_help_string (state.command_parser);

      ply_error ("%s", help_string);

      free (help_string);
      return EX_USAGE;
    }

  ply_command_parser_get_options (state.command_parser,
                                  "help", &should_help,
                                  "attach-to-session", &attach_to_session,
                                  "mode", &mode_string,
                                  "no-daemon", &no_daemon,
                                  "debug", &debug,
                                  "debug-file", &debug_buffer_path,
                                  "pid-file", &pid_file,
                                  "tty", &tty,
                                  NULL);

  if (should_help)
    {
      char *help_string;

      help_string = ply_command_parser_get_help_string (state.command_parser);

      if (argc < 2)
        fprintf (stderr, "%s", help_string);
      else
        printf ("%s", help_string);

      free (help_string);
      return 0;
    }

  if (debug && !ply_is_tracing ())
    ply_toggle_tracing ();

  if (mode_string != NULL)
    {
      if (strcmp (mode_string, "shutdown") == 0)
        state.mode = PLY_MODE_SHUTDOWN;
      else
        state.mode = PLY_MODE_BOOT;

      free (mode_string);
    }

  if (tty != NULL)
    {
      state.default_tty = tty;
    }

  if (geteuid () != 0)
    {
      ply_error ("plymouthd must be run as root user");
      return EX_OSERR;
    }

  chdir ("/");
  signal (SIGPIPE, SIG_IGN);

  if (! no_daemon)
    {
      daemon_handle = ply_create_daemon (pid_file);

      if (daemon_handle == NULL)
        {
          ply_error ("cannot daemonize: %m");
          return EX_UNAVAILABLE;
        }
    }

  if (debug)
    debug_buffer = ply_buffer_new ();

  signal (SIGABRT, on_crash);
  signal (SIGSEGV, on_crash);

  /* If we're shutting down we don't want to die until killed
   */
  if (state.mode == PLY_MODE_SHUTDOWN)
    signal (SIGTERM, SIG_IGN);

  /* before do anything we need to make sure we have a working
   * environment.
   */
  if (!initialize_environment (&state))
    {
      if (errno == 0)
        {
          if (! no_daemon)
            ply_detach_daemon (daemon_handle, 0);
          return 0;
        }

      ply_error ("could not setup basic operating environment: %m");
      if (! no_daemon)
        ply_detach_daemon (daemon_handle, EX_OSERR);
      return EX_OSERR;
    }

  state.boot_buffer = ply_buffer_new ();

  if (attach_to_session)
    {
      state.should_be_attached = attach_to_session;
      if (!attach_to_running_session (&state))
        {
          ply_error ("could not create session: %m");
          if (! no_daemon)
            ply_detach_daemon (daemon_handle, EX_UNAVAILABLE);
          return EX_UNAVAILABLE;
        }
    }

  state.boot_server = start_boot_server (&state);

  if (state.boot_server == NULL)
    {
      ply_error ("could not log bootup: %m");
      if (! no_daemon)
        ply_detach_daemon (daemon_handle, EX_UNAVAILABLE);
      return EX_UNAVAILABLE;
    }

  if (! no_daemon)
    if (!ply_detach_daemon (daemon_handle, 0))
      {
        ply_error ("could not tell parent to exit: %m");
        return EX_UNAVAILABLE;
      }

  state.progress = ply_progress_new ();

  ply_progress_load_cache (state.progress,
                           get_cache_file_for_mode (state.mode));

  ply_trace ("entering event loop");
  exit_code = ply_event_loop_run (state.loop);
  ply_trace ("exited event loop");

  ply_progress_save_cache (state.progress,
                           get_cache_file_for_mode (state.mode));

  ply_boot_splash_free (state.boot_splash);
  state.boot_splash = NULL;

  ply_command_parser_free (state.command_parser);

  ply_boot_server_free (state.boot_server);
  state.boot_server = NULL;

  ply_trace ("freeing terminal session");
  ply_terminal_session_free (state.session);

  ply_buffer_free (state.boot_buffer);
  ply_progress_free (state.progress);

  ply_trace ("exiting with code %d", exit_code);
  
  if (debug_buffer != NULL)
    {
      dump_debug_buffer_to_file ();
      ply_buffer_free (debug_buffer);
    }

  ply_free_error_log();

  return exit_code;
}
/* vim: set sts=4 ts=4 sw=4 expandtab autoindent cindent cino={.5s,(0: */
