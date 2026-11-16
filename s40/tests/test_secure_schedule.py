# SPDX-License-Identifier: MIT
"""Check synchronous first secure service and subsequent renewal/failure policy."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from support import function

MACH = Path(__file__).resolve().parents[1] / "kernel/overlay/arch/arm/mach-nokia-raps"


class SecureSchedule(unittest.TestCase):
    def test_first_service_renewal_tokens_and_failures(self):
        source = (MACH / "rm443_secure_wdt.c").read_text()
        refresh = function(source, "swd_refresh")
        # Replace only the foreign ARM entry address with a host callback;
        # keep request construction, result validation and scheduling intact.
        binding = "int (*call)(void *, u32) = (void *)swd + RM443_SWD_CODE_OFFSET;"
        self.assertEqual(refresh.count(binding), 1)
        refresh = refresh.replace(binding, "int (*call)(void *, u32) = service;")
        init = function(source, "rm443_secure_wdt_init")
        globals_start = source.index("static struct rm443_swd_state *swd;")
        globals_end = source.index("static bool swd_stopped;") + len("static bool swd_stopped;")
        globals_code = source[globals_start:globals_end]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include "rm443_secure_wdt_abi.h"
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define __init
#define HZ 100
#define max(a,b) ((a) > (b) ? (a) : (b))
#define pr_info(...) ((void)0)
#define pr_warn(...) ((void)0)
#define pr_warn_once(...) ((void)0)
#define pr_err(...) ((void)0)
struct work_struct { unsigned unused; };
struct delayed_work { struct work_struct work; void (*callback)(struct work_struct *); };
struct kobject { unsigned unused; };
struct device_node { unsigned unused; };
struct reserved_mem { unsigned unused; };
static struct device_node node;
static struct reserved_mem memory;
static struct kobject object, *kernel_kobj;
static struct { unsigned attr; } swd_status;
static int swd_reboot;
static unsigned long jiffies;
static unsigned rm443_rom_lock;
static void mutex_lock(unsigned *lock) { assert(!*lock); *lock = 1; }
static void mutex_unlock(unsigned *lock) { assert(*lock); *lock = 0; }
'''
        mocks = r'''
static _Alignas(16) u8 retained[RM443_SWD_BYTES];
static bool node_present, memory_present, object_present, init_running, peripherals_started;
static bool queued, service_active, notifier_registered;
static int prepare_error, notifier_error, sysfs_error, power_error;
static unsigned feeds, service_calls, schedules, expected_token, reply_token;
static unsigned transport_reply, status_reply, timeout_reply, callback_error_delta;
static unsigned long delay_ticks;
static struct delayed_work *queued_work;
static void init_work(struct delayed_work *work, void (*callback)(struct work_struct *)) {
    assert(init_running && !queued);
    work->callback = callback;
}
#define INIT_DELAYED_WORK(work, callback) init_work(work, callback)
static void *of_find_compatible_node(void *a, void *b, const char *name) {
    (void)a; (void)b;
    assert(!strcmp(name, "nokia,rm443-secure-watchdog"));
    return node_present ? &node : NULL;
}
static struct reserved_mem *of_reserved_mem_lookup(struct device_node *n) {
    assert(n == &node); return memory_present ? &memory : NULL;
}
static void of_node_put(struct device_node *n) { assert(n == &node); }
static int swd_prepare(struct reserved_mem *m) {
    assert(m == &memory); swd = (void *)retained; return prepare_error;
}
static int register_reboot_notifier(void *n) {
    assert(n == &swd_reboot && swd_work.callback);
    notifier_registered = !notifier_error; return notifier_error;
}
static struct kobject *kobject_create_and_add(const char *name, struct kobject *parent) {
    assert(!strcmp(name, "rm443_secure_watchdog") && parent == kernel_kobj);
    return object_present ? &object : NULL;
}
static int sysfs_create_file(struct kobject *k, void *attr) {
    assert(k == &object && attr == &swd_status.attr); return sysfs_error;
}
static int rm443_power_refresh(void) { ++feeds; return power_error; }
static int service(void *state, u32 address) {
    assert(rm443_rom_lock && state == swd && address == swd->block_pa);
    assert(notifier_registered && !queued && !service_active && !power_error);
    assert(feeds == service_calls + 1);
    /* First service must happen inside init, before peripheral startup. */
    assert(service_calls ? !init_running : init_running);
    assert(service_calls || !peripherals_started);
    service_active = true;
    u8 *request = retained + RM443_SWD_REQUEST_OFFSET;
    u8 *response = retained + RM443_SWD_RESPONSE_OFFSET;
    const u8 expected[8] = {0, 0, 0x1f, 0, expected_token & 255, expected_token >> 8, 0, 0};
    assert(!memcmp(request, expected, sizeof(expected)));
    const u8 zero[8] = {0};
    assert(!memcmp(response, zero, sizeof(zero)));
    *(u32 *)response = status_reply;
    *(u16 *)(response + 4) = timeout_reply;
    *(u16 *)(response + 6) = reply_token;
    swd->callback_error += callback_error_delta;
    ++service_calls;
    service_active = false;
    return transport_reply;
}
static bool schedule_delayed_work(struct delayed_work *work, unsigned long ticks) {
    assert(work == &swd_work && !queued && !service_active);
    assert(service_calls == schedules + 1 && !swd_stopped);
    queued = true; queued_work = work; delay_ticks = ticks; ++schedules;
    return true;
}
static void reset(void) {
    memset(retained, 0xa5, sizeof(retained));
    memset(retained, 0, sizeof(struct rm443_swd_state));
    swd = (void *)retained; swd->token = expected_token = 0x1234;
    swd->block_pa = 0x11434000; swd->work_pa = 0x11c06a00; swd->work_bytes = 8192;
    swd->callback_error = 7; /* Existing count must not itself reject a call. */
    swd_calls = swd_successes = swd_timeout = swd_next = 0;
    swd_result = swd_returned_timeout = swd_returned_token = 0;
    swd_error = 0; swd_last_ping = 0; swd_stopped = false;
    feeds = service_calls = schedules = 0;
    queued = service_active = notifier_registered = peripherals_started = false;
    init_running = false;
    node_present = memory_present = object_present = true;
    prepare_error = notifier_error = sysfs_error = power_error = 0;
    transport_reply = status_reply = 1; timeout_reply = 240; reply_token = 0xabcd;
    callback_error_delta = 0; jiffies = 600;
}
'''
        main = r'''
static int boot(void) {
    init_running = true;
    int result = rm443_secure_wdt_init();
    assert(!rm443_rom_lock);
    init_running = false;
    peripherals_started = true;
    return result;
}
static void renew(void) {
    assert(queued && queued_work == &swd_work);
    queued = false; jiffies += delay_ticks;
    queued_work->callback(&queued_work->work);
    assert(!rm443_rom_lock);
}
int main(void) {
    reset();
    assert(boot() == 0);
    assert(service_calls == 1 && schedules == 1 && queued && delay_ticks == 210 * HZ);
    assert(swd_calls == 1 && swd_successes == 1 && swd->token == 0xabcd);
    assert(swd_last_ping == 600 && swd->stage == 40);
    expected_token = 0xabcd; reply_token = 0x4321;
    renew();
    assert(service_calls == 2 && schedules == 2 && queued && delay_ticks == 210 * HZ);
    assert(swd->token == 0x4321 && swd_successes == 2 && swd_last_ping == 21600);
    /* Any failed first call stops renewal without accepting its new token. */
    for (unsigned failure = 0; failure < 5; failure++) {
        reset();
        if (failure == 0) transport_reply = 0;
        if (failure == 1) status_reply = 0;
        if (failure == 2) timeout_reply = 0;
        if (failure == 3) callback_error_delta = 1;
        if (failure == 4) power_error = -EBUSY;
        assert(boot() == (failure == 4 ? -EBUSY : -EIO));
        assert(swd_stopped && !queued && !schedules && !swd_successes);
        assert(swd->token == 0x1234 && service_calls == (failure == 4 ? 0 : 1));
        unsigned old_feeds = feeds, old_calls = service_calls;
        swd_refresh(&swd_work.work);
        assert(!rm443_rom_lock);
        assert(feeds == old_feeds && service_calls == old_calls && !queued);
    }
    /* A later failed renewal retains the last accepted token, without retry. */
    reset(); assert(boot() == 0);
    expected_token = 0xabcd; reply_token = 0x4321; status_reply = 0;
    renew();
    assert(swd_stopped && !queued && schedules == 1 && swd_successes == 1);
    assert(swd->token == 0xabcd && swd_returned_token == 0x4321);
    /* Diagnostic sysfs failures must not prevent the initial hardware service. */
    reset(); object_present = false; assert(boot() == 0 && service_calls == 1 && queued);
    reset(); sysfs_error = -ENOMEM; assert(boot() == 0 && service_calls == 1 && queued);
    /* Setup failures cannot invoke an unprepared bridge or schedule work. */
    for (unsigned failure = 0; failure < 4; failure++) {
        reset();
        if (failure == 0) node_present = false;
        if (failure == 1) memory_present = false;
        if (failure == 2) prepare_error = -EINVAL;
        if (failure == 3) notifier_error = -ENOMEM;
        assert(boot() < 0 && !service_calls && !queued && !schedules);
    }
    return 0;
}
'''
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler unavailable")
        with tempfile.TemporaryDirectory(prefix="rm443-secure-schedule-") as temporary:
            path = Path(temporary)
            (path / "schedule.c").write_text(harness + globals_code + mocks + refresh + init + main)
            subprocess.run([compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                            "-I", str(MACH), str(path / "schedule.c"), "-o", str(path / "schedule")], check=True)
            subprocess.run([str(path / "schedule")], check=True)


if __name__ == "__main__":
    unittest.main()
