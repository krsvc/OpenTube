#pragma once
#include "types.hpp"

void Downloads_init(void);
void Downloads_exit(void);
void Downloads_suspend(void);
void Downloads_resume(std::string arg);
void Downloads_draw(void);
