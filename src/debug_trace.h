#pragma once

#include <Arduino.h>

void debug_trace_boot_report();
void debug_trace_mark(const char* stage, const String& detail = String());
void debug_trace_clear();
