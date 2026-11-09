#pragma once
#include "tracer.h"
#include <cstdint>
namespace lt::editor {
bool install();
void refresh(); // matching native UI callback only
void reset();
struct View { uint64_t selection = 0, revision = 0; Report report; };
View view();
}
