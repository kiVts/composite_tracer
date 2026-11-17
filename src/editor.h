#pragma once
#include "tracer.h"
#include <cstdint>
namespace lt::editor {
bool install();
void refresh(); // matching native UI callback only
void reset();
struct View { uint64_t selection = 0, revision = 0; Report report; };
// Copies the report into `last` only when its selection or revision changed (the UI asks every frame).
void update_view(View& last);
}
