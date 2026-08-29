/* ----------------------------------------------------------------------- *
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 *   Copyright 2001-2025 H. Peter Anvin - All Rights Reserved
 *
 * ----------------------------------------------------------------------- */

/*
 * remap.h
 *
 * Prototypes for regular-expression based filename remapping.
 */

#ifndef TFTPD_REMAP_H
#define TFTPD_REMAP_H

/* Opaque type */
struct rule;

#ifdef WITH_REGEX

/*
 * This is called by the remap engine when it encounters macros such
 * as \i. It should put the output in a static buffer and put the
 * buffer address in *output, then return the length of the output
 * not including the terminal null.
 *
 * Return (size_t)-1 for an invalid macro, which then will be handled
 * by the substitution code.
 */
typedef size_t (*match_pattern_callback) (char, const char **);

/* Read a rule file */
struct rule *parserulefile(FILE *);

/* Destroy a rule file data structure */
void freerules(struct rule *);

/* Execute a rule set on a string; returns a malloc'd new string. */
struct formats;
char *rewrite_string(const struct formats *, const char *,
		     const struct rule *, int, int,
                     match_pattern_callback, const char **);

/* Remapping deadman counter */
extern int deadman_max_steps;

#endif                          /* WITH_REGEX */
#endif                          /* TFTPD_REMAP_H */
