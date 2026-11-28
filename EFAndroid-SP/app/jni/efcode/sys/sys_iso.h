/*
===========================================================================
The game files from an image of the Elite Force CD (sys_iso.c).

This file is part of VoyagerSP (GPLv2 or later, like the rest of the engine).
===========================================================================
*/

#ifndef SYS_ISO_H
#define SYS_ISO_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
	char		name[64];		// in BaseEF, lower case, like pak0.pk3
	uint64_t	size;
	uint64_t	sector;			// first sector of the file in the image
	int64_t		mtime;			// the disc's date for it (Unix time), -1 if none
} isoFile_t;

typedef struct {
	char		image[1024];	// the file holding the sectors (a .cue's .bin)
	uint32_t	sectorSize;		// 2048 (.iso) or 2352 (raw .bin)
	uint32_t	dataOffset;		// of the 2048 data bytes in each sector (0, 16 or 24)
	char		label[33];		// the volume label, like ELITEFORCE
	isoFile_t	*files;			// the .pk3 files of the disc's BaseEF folders
	int			numFiles;
	int			hasPak0;
} isoImage_t;

// 1 when path's name ends .iso, .bin, .img, .cue or .cdr (a file the
// launcher reads as a CD image; ISO_ReadImage checks what is in it)
int ISO_IsImageName( const char *path );

// Reads the ISO 9660 file system of a CD image (.iso, a raw .bin, or a .cue
// naming one) and lists the .pk3 files of every BaseEF folder on it. 0 with
// err set to the reason (a phrase that follows "This disc image can't be
// used:") when it can't be read or has no BaseEF .pk3 files. Each .pk3 has to
// end in a zip end record, so a misread image is refused before any copying.
int ISO_ReadImage( const char *path, isoImage_t *img, char *err, size_t errSize );
void ISO_Free( isoImage_t *img );

// Copies one file to <to>.part and renames it to <to> (with the disc's
// date); adds the bytes written to *progress; stops when *cancel is set.
// 0 with err set on failure (the .part file is removed).
int ISO_ExtractFile( const isoImage_t *img, const isoFile_t *f, const char *to,
	volatile unsigned long long *progress, volatile signed char *cancel, char *err, size_t errSize );

#endif
