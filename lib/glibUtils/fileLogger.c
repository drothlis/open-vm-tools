/*********************************************************
 * Copyright (C) 2010 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

/**
 * @file fileLogger.c
 *
 * Logger that uses file streams and provides optional log rotation.
 */

#include "glibUtils.h"
#include <stdio.h>
#include <string.h>
#include <glib/gstdio.h>
#if defined(G_PLATFORM_WIN32)
#  include <process.h>
#  include <windows.h>
#else
#  include <unistd.h>
#endif


typedef struct FileLogger {
   GlibLogger     handler;
   FILE          *file;
   gchar         *path;
   gint           logSize;
   guint64        maxSize;
   guint          maxFiles;
   gboolean       append;
   gboolean       error;
   GStaticRWLock  lock;
} FileLogger;


/*
 *******************************************************************************
 * FileLoggerGetPath --                                                   */ /**
 *
 * Parses the given template file name and expands embedded variables, and
 * places the log index information at the right position.
 *
 * The following variables are expanded:
 *
 *    - ${USER}:  user's login name.
 *    - ${PID}:   current process's pid.
 *    - ${IDX}:   index of the log file (for rotation).
 *
 * @param[in] data         Log handler data.
 * @param[in] index        Index of the log file.
 *
 * @return The expanded log file path.
 *
 ******************************************************************************
 */

static gchar *
FileLoggerGetPath(FileLogger *data,
                  gint index)
{
   gboolean hasIndex = FALSE;
   gchar indexStr[11];
   gchar *logpath;
   gchar *vars[] = {
      "${USER}",  NULL,
      "${PID}",   NULL,
      "${IDX}",   indexStr,
   };
   gchar *tmp;
   size_t i;

   logpath = g_strdup(data->path);
   vars[1] = (char *) g_get_user_name();
   vars[3] = g_strdup_printf("%u", (unsigned int) getpid());
   g_snprintf(indexStr, sizeof indexStr, "%d", index);

   for (i = 0; i < G_N_ELEMENTS(vars); i += 2) {
      char *last = logpath;
      char *start;
      while ((start = strstr(last, vars[i])) != NULL) {
         gchar *tmp;
         char *end = start + strlen(vars[i]);
         size_t offset = (start - last) + strlen(vars[i+1]);

         *start = '\0';
         tmp = g_strdup_printf("%s%s%s", logpath, vars[i+1], end);
         g_free(logpath);
         logpath = tmp;
         last = logpath + offset;

         /* XXX: ugly, but well... */
         if (i == 4) {
            hasIndex = TRUE;
         }
      }
   }

   g_free(vars[3]);

   /*
    * Always make sure we add the index if it's not 0, since that's used for
    * backing up old log files.
    */
   if (index != 0 && !hasIndex) {
      char *sep = strrchr(logpath, '.');
      char *pathsep = strrchr(logpath, '/');

      if (pathsep == NULL) {
         pathsep = strrchr(logpath, '\\');
      }

      if (sep != NULL && sep > pathsep) {
         *sep = '\0';
         sep++;
         tmp = g_strdup_printf("%s.%d.%s", logpath, index, sep);
      } else {
         tmp = g_strdup_printf("%s.%d", logpath, index);
      }
      g_free(logpath);
      logpath = tmp;
   }

   return logpath;
}


/*
 *******************************************************************************
 * FileLoggerOpen --                                                      */ /**
 *
 * Opens a log file for writing, backing up the existing log file if one is
 * present. Only one old log file is preserved.
 *
 * @note Make sure this function is called with the write lock held.
 *
 * @param[in] data   Log handler data.
 *
 * @return Log file pointer (NULL on error).
 *
 *******************************************************************************
 */

static FILE *
FileLoggerOpen(FileLogger *data)
{
   FILE *logfile = NULL;
   gchar *path;

   g_return_val_if_fail(data != NULL, NULL);
   path = FileLoggerGetPath(data, 0);

   if (g_file_test(path, G_FILE_TEST_EXISTS)) {
      struct stat fstats;
      if (g_stat(path, &fstats) > -1) {
#if GLIB_CHECK_VERSION(2, 10, 0)
         g_atomic_int_set(&data->logSize, (gint) fstats.st_size);
#else
         data->logSize = (gint) fstats.st_size;
#endif
      }

      if (!data->append || g_atomic_int_get(&data->logSize) >= data->maxSize) {
         /*
          * Find the last log file and iterate back, changing the indices as we go,
          * so that the oldest log file has the highest index (the new log file
          * will always be index "0"). When not rotating, "maxFiles" is 1, so we
          * always keep one backup.
          */
         gchar *log;
         guint id;
         GPtrArray *logfiles = g_ptr_array_new();

         /*
          * Find the id of the last log file. The pointer array will hold
          * the names of all existing log files + the name of the last log
          * file, which may or may not exist.
          */
         for (id = 0; id < data->maxFiles; id++) {
            log = FileLoggerGetPath(data, id);
            g_ptr_array_add(logfiles, log);
            if (!g_file_test(log, G_FILE_TEST_IS_REGULAR)) {
               break;
            }
         }

         /* Rename the existing log files, increasing their index by 1. */
         for (id = logfiles->len - 1; id > 0; id--) {
            gchar *dest = g_ptr_array_index(logfiles, id);
            gchar *src = g_ptr_array_index(logfiles, id - 1);

            if (!g_file_test(dest, G_FILE_TEST_IS_DIR) &&
                (!g_file_test(dest, G_FILE_TEST_EXISTS) ||
                 g_unlink(dest) == 0)) {
               g_rename(src, dest);
            } else {
               g_unlink(src);
            }
         }

         /* Cleanup. */
         for (id = 0; id < logfiles->len; id++) {
            g_free(g_ptr_array_index(logfiles, id));
         }
         g_ptr_array_free(logfiles, TRUE);
#if GLIB_CHECK_VERSION(2, 10, 0)
         g_atomic_int_set(&data->logSize, 0);
#else
         data->logSize = 0;
#endif
         data->append = FALSE;
      }
   }

   logfile = g_fopen(path, data->append ? "a" : "w");
   g_free(path);
   return logfile;
}


/*
 *******************************************************************************
 * FileLoggerLog --                                                       */ /**
 *
 * Logs a message to the configured destination file. Also opens the file for
 * writing if it hasn't been done yet.
 *
 * @param[in] domain    Log domain.
 * @param[in] level     Log level.
 * @param[in] message   Message to log.
 * @param[in] data      File logger.
 *
 *******************************************************************************
 */

static void
FileLoggerLog(const gchar *domain,
              GLogLevelFlags level,
              const gchar *message,
              gpointer data)
{
   FileLogger *logger = data;

   g_static_rw_lock_reader_lock(&logger->lock);

   if (logger->error) {
      goto exit;
   }

   if (logger->file == NULL) {
      /*
       * We need to drop the read lock and acquire a write lock to open
       * the log file.
       */
      g_static_rw_lock_reader_unlock(&logger->lock);
      g_static_rw_lock_writer_lock(&logger->lock);
      if (logger->file == NULL) {
         logger->file = FileLoggerOpen(data);
      }
      g_static_rw_lock_writer_unlock(&logger->lock);
      g_static_rw_lock_reader_lock(&logger->lock);
      if (logger->file == NULL) {
         logger->error = TRUE;
         goto exit;
      }
   }

   /* Write the log file and do log rotation accounting. */
   if (fputs(message, logger->file) >= 0) {
      if (logger->maxSize > 0) {
         g_atomic_int_add(&logger->logSize, (gint) strlen(message));
#if defined(_WIN32)
         /* Account for \r. */
         g_atomic_int_add(&logger->logSize, 1);
#endif
         if (g_atomic_int_get(&logger->logSize) >= logger->maxSize) {
            /* Drop the reader lock, grab the writer lock and re-check. */
            g_static_rw_lock_reader_unlock(&logger->lock);
            g_static_rw_lock_writer_lock(&logger->lock);
            if (g_atomic_int_get(&logger->logSize) >= logger->maxSize) {
               fclose(logger->file);
               logger->append = FALSE;
               logger->file = FileLoggerOpen(logger);
            }
            g_static_rw_lock_writer_unlock(&logger->lock);
            g_static_rw_lock_reader_lock(&logger->lock);
         } else {
            fflush(logger->file);
         }
      } else {
         fflush(logger->file);
      }
   }

exit:
   g_static_rw_lock_reader_unlock(&logger->lock);
}


/*
 ******************************************************************************
 * FileLoggerDestroy --                                               */ /**
 *
 * Cleans up the internal state of a file logger.
 *
 * @param[in] _data     File logger data.
 *
 ******************************************************************************
 */

static void
FileLoggerDestroy(gpointer data)
{
   FileLogger *logger = data;
   if (logger->file != NULL) {
      fclose(logger->file);
   }
   g_static_rw_lock_free(&logger->lock);
   g_free(logger->path);
   g_free(logger);
}


/*
 *******************************************************************************
 * GlibUtils_CreateFileLogger --                                          */ /**
 *
 * @brief Creates a new file logger based on the given configuration.
 *
 * @param[in] path      Path to log file.
 * @param[in] append    Whether to append to existing log file.
 * @param[in] maxSize   Maximum log file size (in MB, 0 = no limit).
 * @param[in] maxFiles  Maximum number of old files to be kept.
 *
 * @return A new logger, or NULL on error.
 *
 *******************************************************************************
 */

GlibLogger *
GlibUtils_CreateFileLogger(const char *path,
                           gboolean append,
                           guint maxSize,
                           guint maxFiles)
{
   FileLogger *data = NULL;

   g_return_val_if_fail(path != NULL, NULL);

   data = g_new0(FileLogger, 1);
   data->handler.addsTimestamp = FALSE;
   data->handler.shared = FALSE;
   data->handler.logfn = FileLoggerLog;
   data->handler.dtor = FileLoggerDestroy;

   data->path = g_filename_from_utf8(path, -1, NULL, NULL, NULL);
   if (data->path == NULL) {
      g_free(data);
      return NULL;
   }

   data->append = append;
   data->maxSize = maxSize * 1024 * 1024;
   data->maxFiles = maxFiles + 1; /* To account for the active log file. */
   g_static_rw_lock_init(&data->lock);

   return &data->handler;
}

