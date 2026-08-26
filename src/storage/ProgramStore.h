#pragma once
#include <Arduino.h>
#include <cstddef>
#include <cstdint>

namespace prog {

constexpr size_t MAX_PROGRAMS = 20;
constexpr size_t MAX_NAME_LEN = 31;

struct ProgramMeta {
    uint8_t id;
    char    name[MAX_NAME_LEN + 1];
};

// Initialize the program store and restore the last active program at boot.
// Must be called after drv::init() but before the RT layer is initialized.
bool init();

// Notify the store that the RT layer is initialized.  After this call, loaded
// snapshots will also call rt::onConfigApplied().  This function is safe to
// call even if no program was restored.
void onRuntimeInitialized();

// List up to maxCount program metadata entries.  Returns true on success and
// writes the actual count into outCount.  Listing is read-only and never
// blocked by an active run.
bool list(ProgramMeta* out, size_t maxCount, size_t& outCount);

// Save the current active RuntimeConfig snapshot.
//   id == 0  -> create a new program (stable id is written to *outId if given)
//   id != 0  -> overwrite existing program
// Returns false while the system is active or on any I/O/validation error.
bool save(uint8_t id, const char* name, uint8_t* outId = nullptr);

// Load a stored snapshot by its stable numeric id.  Returns false while the
// system is active, if the id is unknown, or if the JSON fails strict validation.
bool load(uint8_t id);

// Rename an existing program.  Returns false while active or on error.
bool rename(uint8_t id, const char* name);

// Delete an existing program by stable numeric id.  If the deleted program was
// the active one, the next remaining program is loaded automatically; if no
// programs remain, a safe default program is seeded.  Returns false while
// active or on error.
bool erase(uint8_t id);

// Active program identity.
uint8_t activeId();
bool    activeName(char* buf, size_t cap);

// Mark the current active config as dirty so service() will autosave it once
// the system becomes inactive.
void markDirty();

// Call periodically from Core-0 task context (e.g. the network loop).  Performs
// one deferred autosave when the config is dirty and the system is inactive.
void service();

} // namespace prog
