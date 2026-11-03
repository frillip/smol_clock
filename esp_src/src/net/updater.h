/*
 * Updates pulled from https://<updater.server><updater.path>: the update
 * manifest, format 2 (manifest.h), then the files its configuration for this
 * clock names: the ESP's image, the PIC's application and the PIC's SPL.
 * Every request connects with IPv6/IPv4 fallback (net_connect.h).
 *
 * Newer only, unless forced. The ESP first: the PIC's application is only
 * installed by an ESP at (or past) its configuration's ESP version, so a PIC
 * never runs against an ESP that does not know its SML version. A download is
 * checked against the manifest's size and SHA-256 before it is used. The
 * PIC's files are staged in picfw (pic_store.h) and installed from there
 * (pic_boot.h), so a failed install is retried without the network.
 *
 * The PIC's in order: its SPL first, then its application, chained once the
 * SPL's update has ended (DONE). The SPL's is started whenever it is offered
 * and not known to be current: its first STATUS ends it at once if it is, or
 * if a newer one runs. "update install" and the daily check do everything
 * due in one go: an ESP update restarts the ESP at once, and the PIC's
 * updates follow the restart (a flag in NVS) once the network is up
 * and the PIC heard from.
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
// Every pass of the main loop: the PIC's updates that follow an SPL update or
// an ESP update's restart.
void updater_poll(bool network_up);

// Reports to output. check: what is offered, for the ESP and the PIC (and
// stages the PIC's files if no slot holds them). pull: everything due, the
// ESP's (restarting at once), then the PIC's SPL, then its application.
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
