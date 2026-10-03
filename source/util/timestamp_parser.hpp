#pragma once
#include <string>

double Util_convert_time_to_seconds(const std::string& timestamp_str);

int Util_find_timestamp_in_text(const std::string& text, int start_pos, int* timestamp_start, int* timestamp_end, double* timestamp_seconds);
