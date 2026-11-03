#ifndef PIC_STORE_FLASH_H
#define PIC_STORE_FLASH_H

// Finds the picfw partition and gives it to pic_store. False if the
// partition table has none (PIC updates are then refused).
bool pic_store_flash_init(void);

#endif // PIC_STORE_FLASH_H
