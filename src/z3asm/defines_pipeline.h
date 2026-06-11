#pragma once

#include "libstr.h"

bool validatedefinename(const char* name);
void resolvedefines(string& out, const char* start);
