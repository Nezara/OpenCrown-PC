// Guest device -> host directory mounts for SceIofilemgr.
#pragma once
#include <string>
#include <vector>

namespace hle {

// Adds `host_dir` as a root of guest device `device` ("app0", "savedata0", ...). Roots are searched in the order
// they were added; new files go to the first one. Writable devices get their directory created.
void io_mount(const std::string& device, const std::string& host_dir, bool writable);

// Host path of a guest path ("savedata0:/x"), "" if the device is unknown or the path leaves its root. `create`
// picks the first root for a file that doesn't exist yet. Used by AppUtil save data and trophies.
std::string io_host_path(const std::string& guest, bool create);

}  // namespace hle
