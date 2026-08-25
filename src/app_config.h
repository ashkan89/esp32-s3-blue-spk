#pragma once

#ifndef APP_NAME
#define APP_NAME "esp32-blue-spk"
#endif

// Keep this in sync with GitHub release tags. A leading "v" on the release is
// ignored when the dashboard compares versions.
#ifndef FW_VERSION
#define FW_VERSION "2.0.1"
#endif

// Can be overridden with build flags, for example:
//   -DDEFAULT_GITHUB_REPO=\"owner/esp32-blue-spk\"
#ifndef DEFAULT_GITHUB_REPO
#define DEFAULT_GITHUB_REPO "ashkan89/esp32-s3-blue-spk"
#endif

#ifndef DEFAULT_GITHUB_ASSET
#define DEFAULT_GITHUB_ASSET "*.bin"
#endif

#ifndef MANAGEMENT_ENABLED
#define MANAGEMENT_ENABLED 1
#endif
