// Host-test stand-in for ESP-IDF's critical section API: the tests run in one thread, so the lock does nothing
#pragma once
#define DEFINE_CRIT_SECTION_LOCK_STATIC(name) static int name
#define esp_os_enter_critical(lock) ((void)(lock))
#define esp_os_exit_critical(lock) ((void)(lock))
