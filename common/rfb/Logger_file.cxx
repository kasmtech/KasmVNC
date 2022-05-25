/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

// -=- Logger_file.cxx - Logger instance for a file

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <os/Mutex.h>

#include <network/datelog.h>
#include <rfb/util.h>
#include <rfb/Logger_file.h>
#include <rfb/LogWriter.h>

using namespace rfb;

Logger_File::Logger_File(const char* loggerName)
  : Logger(loggerName), indent(13), width(79), m_filename(0), m_file(0),
    m_lastLogTime(0)
{
  mutex = new os::Mutex();
}

Logger_File::~Logger_File()
{
  closeFile();
  delete mutex;
}

void Logger_File::write(int level, const char *logname, const char *message)
{
  os::AutoMutex a(mutex);

  if (!m_file) {
    if (!m_filename) return;
    CharArray bakFilename(strlen(m_filename) + 1 + 4);
    sprintf(bakFilename.buf, "%s.bak", m_filename);
    remove(bakFilename.buf);
    rename(m_filename, bakFilename.buf);
    m_file = fopen(m_filename, "w+");
    if (!m_file) return;
  }

  struct timeval tv;
  gettimeofday(&tv, NULL);

  if (tv.tv_sec != m_lastLogTime) {
    m_lastLogTime = tv.tv_sec;
//    fprintf(m_file, "\n%s", ctime(&m_lastLogTime));
  }

  char timebuf[128];
  struct tm local;
  localtime_r(&tv.tv_sec, &local);
  strftime(timebuf, sizeof(timebuf), DATELOGFMT, &local);

  const unsigned msec = tv.tv_usec / 1000;
  const char *levelname = "PRIO";
  if (level >= LogWriter::LEVEL_INFO)
    levelname = "INFO";
  if (level >= LogWriter::LEVEL_DEBUG)
    levelname = "DEBUG";

  int column = fprintf(m_file, " %s,%03u [%s] %s:", timebuf, msec, levelname, logname);

  if (column < indent) {
    fprintf(m_file,"%*s",indent-column,"");
    column = indent;
  }
  /*while (true) {
    const char* s = strchr(message, ' ');
    int wordLen;
    if (s) wordLen = s-message;
    else wordLen = strlen(message);

    if (column + wordLen + 1 > width) {
      fprintf(m_file,"\n%*s",indent,"");
      column = indent;
    }
    fprintf(m_file," %.*s",wordLen,message);
    column += wordLen + 1;
    message += wordLen + 1;
    if (!s) break;
  }*/
  fprintf(m_file," %s",message);
  fprintf(m_file,"\n");
  fflush(m_file);
}

void Logger_File::setFilename(const char* filename)
{
  closeFile();
  m_filename = strDup(filename);
}

void Logger_File::setFile(FILE* file)
{
  closeFile();
  m_file = file;
}

void Logger_File::closeFile()
{
  if (m_filename) {
    if (m_file) {
      fclose(m_file);
      m_file = 0;
    }
    strFree(m_filename);
    m_filename = 0;
  }
}

static Logger_File logger("file");

bool rfb::initFileLogger(const char* filename) {
  logger.setFilename(filename);
  logger.registerLogger();
  return true;
}
