/*
 * Host stand-in for drivers/flash.c: the store's pages in RAM, with the
 * rules of the real flash. Erasing sets every bit; writing only clears bits
 * (new = old & value), and writing a double word twice between erases is
 * counted as a violation. A power cut can be set to happen at the Nth
 * operation from now: that operation is left half done (an erase partly
 * done, a write with only some of its bits programmed) and the test is
 * longjmp'd back to its reset point.
 */
#ifndef FAKE_FLASH_H
#define FAKE_FLASH_H

#include <setjmp.h>
#include <stdint.h>

#include "../../../drivers/flash.h"

extern uint16_t fake_flash_words[FLASH_STORE_PAGES][FLASH_PAGE_WORDS];
extern uint32_t fake_flash_violations; // double words written twice, or not erased
extern uint32_t fake_flash_erases[FLASH_STORE_PAGES];
extern long fake_flash_cut_after; // operations until the cut; -1: never
extern jmp_buf fake_flash_reset;
extern uint32_t fake_flash_resumes; // resume hook calls
extern int fake_flash_fail;         // every operation reports FLASH_FAILED

void fake_flash_wipe(uint16_t value); // every word, as if never erased (0xFFFF) or garbage
void fake_flash_seed(unsigned seed);

#endif
