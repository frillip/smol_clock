/*
 * Updates pulled from https://<updater.server><updater.path>: the update
 * manifest, format 2 (manifest.h), then the files its configuration for this
 * clock names: the ESP's image, the PIC's application and the PIC's SPL.
 * Every request connects with IPv6/IPv4 fallback (net_connect.h).
 *
 * Newer only, unless forced. The PIC first, while this ESP is talking to it:
 * the link is proven, so this ESP can update the PIC whatever a new ESP turns
 * out to do. The ESP's update follows once the PIC's have ended, whatever
 * their result: the ESP can always update itself, link or not. The ESP goes
 * first only when the PIC's cannot start (not heard from, or its application
 * speaking an SML major this ESP does not): the new ESP then installs the
 * PIC's after its restart. A download is checked against the manifest's size
 * and SHA-256 before it is used. The PIC's files are staged in picfw
 * (pic_store.h) and installed from there (pic_boot.h), so a failed install is
 * retried without the network.
 *
 * The PIC's in order: its SPL first, then its application, chained once the
 * SPL's update has ended (DONE). The SPL's is started whenever it is offered
 * and not known to be current: its first STATUS ends it at once if it is, or
 * if a newer one runs. "update install" and the daily check do everything
 * due in one go. A pass of the PIC's updates that fails is tried once more,
 * once the PIC answers again or after a minute, unless the failure says the
 * same file cannot do better (pic_boot_outcome()).
 *
 * Automatic updates: once a day, in the local hour updater.auto_hour, as
 * "update install". Not in the hour it started in, so an update does not run
 * twice.
 */
#ifndef UPDATER_H
#define UPDATER_H

#include <Arduino.h>
#include <stdint.h>

// The firmware's own hardware name in the manifest.
#define UPDATER_HARDWARE "v2"

void updater_set_server(const char *server);
void updater_set_path(const char *path);
void updater_set_config(const char *config);

// Once a minute; local_seconds is local civil time, valid if have_local.
void updater_service(bool have_local, uint32_t local_seconds);
// Every pass of the main loop: the PIC's application after its SPL, the ESP's
// after the PIC's, and the PIC's after an ESP update's restart.
void updater_poll(bool network_up);

// Reports to output. check: what is offered, for the ESP and the PIC (and
// stages the PIC's files if no slot holds them). pull: everything due: the
// PIC's SPL, then its application, then the ESP's (restarting at once).
// esp: the ESP's image if newer (restart later); force: whatever its version,
// and restart. pic: the PIC's application if newer; force: whatever its
// version, from a staged file if the update server cannot be reached. spl:
// the PIC's SPL likewise, through its application.
bool updater_check(Stream *output);
void updater_pull(Stream *output);
void updater_esp(Stream *output, bool force);
void updater_pic(Stream *output, bool force);
void updater_spl(Stream *output, bool force);

bool updater_is_available(void);     // the ESP's
bool updater_pic_is_available(void); // the PIC's application or SPL
bool updater_is_running(void);

#endif // UPDATER_H
