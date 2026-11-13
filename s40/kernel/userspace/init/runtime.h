/* SPDX-License-Identifier: MIT */
#ifndef INIT_RUNTIME_H
#define INIT_RUNTIME_H

void log_open(void);
int log_descriptor(void);
void log_append(const char *text);
void log_decimal(unsigned long value);
void log_begin(const char *text);
void log_emit(void);
void log_message(const char *text);
void log_file(const char *path, unsigned int maxlines);
unsigned long uptime_seconds(void);

#endif
