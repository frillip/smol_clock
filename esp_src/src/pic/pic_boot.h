/*
 * The PIC's bootloader from the ESP: a PIC application update from a file
 * staged in picfw (pic_store.h), run by pic_update.c from the main loop over
 * the PIC link, an SPL update the same way by spl_update.c, and the console's
 * view of the SPL.
 *
 * While an update runs the link is quiet (pic_link.h): only its frames reach
 * the PIC. Progress goes to every console as it happens: the step, and the
 * chunks written and read back in quarters.
 */
#ifndef PIC_BOOT_H
#define PIC_BOOT_H

#include <Arduino.h>

#include "pic_store.h"

// Finds picfw. Says so on output if there is none: PIC updates are then
// refused until the ESP is reflashed with the current partition table.
void pic_boot_init(Stream *output);
void pic_boot_service(void);

// An update from a staged application file. False, said why, if one is
// running already or the store is absent. force: even if the PIC has it
// already, and past an unconfirmed SPL (which refuses the erase anyway).
bool pic_boot_update(const PicStoredFile *file, bool force, Stream *output);
// An SPL update from a staged SPL file (spl_update.h), through the running
// application. False, said why, if any PIC update is running, the PIC is in
// its SPL, or the file is not an SPL's. A new SPL that is rejected is
// followed by the one kept, forced, by itself.
bool pic_boot_update_spl(const PicStoredFile *file, bool force, Stream *output);
// Either update running.
bool pic_boot_updating(void);
// The last SPL update has ended: true, and done is whether it is DONE.
// False while one runs, or if none has.
bool pic_boot_spl_result(bool *done);

// "pic boot": where the PIC is (its application, or its SPL and what the
// SPL reports), and the last update.
void pic_boot_print(Stream *output, bool verbose);
// The files staged in picfw: the application's slots (verbose: the SPL's).
void pic_boot_print_staged(Stream *output, bool verbose);
// "update erase": empties the application's slots, the SPL's, or both.
void pic_boot_erase(Stream *output, bool application, bool spl);
// "pic boot run|hold", to the SPL; its answer printed on output.
void pic_boot_command(Stream *output, uint8_t command);

#endif // PIC_BOOT_H
