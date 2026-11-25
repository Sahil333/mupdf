// Copyright (C) 2025 Artifex Software, Inc.
//
// This file is part of MuPDF.
//
// MuPDF is free software: you can redistribute it and/or modify it under the
// terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// MuPDF is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
// details.
//
// You should have received a copy of the GNU Affero General Public License
// along with MuPDF. If not, see <https://www.gnu.org/licenses/agpl-3.0.en.html>
//
// Alternative licensing terms are available from the licensor.
// For commercial licensing, see <https://www.artifex.com/> or contact
// Artifex Software, Inc., 39 Mesa Street, Suite 108A, San Francisco,
// CA 94129, USA, for further information.

// Internal PDF functions

#ifndef MUPDF_PDF_PDF_IMP_H

#define MUPDF_PDF_PDF_IMP_H

#include "mupdf/pdf.h"

void pdf_repair_xref_aux(fz_context *ctx, pdf_document *doc, void (*mid)(fz_context *ctx, pdf_document *doc));

pdf_obj *pdf_lookup_mcid_in_mcids(fz_context *ctx, int id, pdf_obj *mcids);

void pdf_repair_page_tree_parents(fz_context *ctx, pdf_document *doc);

pdf_obj *pdf_hash_obj(fz_context *ctx, pdf_document *doc, int num, int include_streams, uint32_t *hash);

/*
	Walk up a /Parent chain from 'start' and return the root object.

	Checks for cycles as it goes. If a cycle is detected, then
	(if supplied) the optional repair function will be called.
	The traversal will then start again. If a cycle is detected
	again (or the repair function was not supplied), an exception
	will be thrown.
*/
pdf_obj *pdf_parent_root(fz_context *ctx, pdf_obj *start, void (*repair)(fz_context *, pdf_document *doc));

/*
	Walk up a /Parent chain from 'start', calling 'step' at each
	point (including the initial node). If step returns non-NULL
	the traversal is stopped, and that value is returned. If the
	traversal completes, NULL will be returned.

	If a cycle is detected, and repair is non-NULL, then repair
	will be called, and the complete traversal will be retried.
	If a cycle is detect again (or the repair function was not
	supplied), an exception will be thrown.
*/
pdf_obj *pdf_walk_parent(fz_context *ctx, pdf_obj *start, void (*repair)(fz_context *, pdf_document *doc), pdf_obj *(*step)(fz_context *, pdf_obj *, void *), void *step_arg);

/*
	Blanking image pixels that no placement shows (the save option
	blank-unshown-pixels).

	pdf_new_unshown_pixels draws every page, its annotations and its
	widgets, as for viewing and as for printing, and records where each image is placed
	and the clips in force there. Then every image XObject that has
	pixels no placement shows (outside the page, or outside a clip) is
	blanked in place, so all its uses still share it. An image whose
	pixels are all shown is left as it was. Identical images are
	treated as one.

	Inline images are blanked by passing the result as the opaque
	value of a sanitize filter, with
	pdf_unshown_pixels_inline_image_filter as its image_filter.
*/
typedef struct pdf_unshown_pixels pdf_unshown_pixels;

pdf_unshown_pixels *pdf_new_unshown_pixels(fz_context *ctx, pdf_document *doc);
fz_image *pdf_unshown_pixels_inline_image_filter(fz_context *ctx, void *opaque, fz_matrix ctm, const char *name, fz_image *image, fz_rect clip);
void pdf_drop_unshown_pixels(fz_context *ctx, pdf_unshown_pixels *unshown);


#endif /* MUPDF_PDF_PDF_IMP_H */
