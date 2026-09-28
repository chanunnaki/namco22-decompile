/* vita-elf-create appends SCE import/module metadata to the executable segment.
 * Keep one full extra page before writable data, regardless of code size. */
SECTIONS { . = ALIGN(0x1000) + 0x1000; }
INSERT BEFORE .init_array;
