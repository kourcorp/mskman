#ifndef MSKMAN_MAZEGEN_H
#define MSKMAN_MAZEGEN_H

#include <stdint.h>

/* Builds the left half (31 rows x 14 columns, same legend as mazes.h) of a
 * new maze from seed. The right half is the mirror image. Every generated
 * maze keeps the ghost house, start and fruit spots of the built-in mazes,
 * has no dead ends, and is fully connected. */
void mazegen_build(uint32_t seed, char half[31][15]);

#endif
