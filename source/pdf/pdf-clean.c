// Copyright (C) 2004-2026 Artifex Software, Inc.
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

#include "mupdf/fitz.h"
#include "pdf-annot-imp.h"
#include "pdf-imp.h"

#include <string.h>
#include <assert.h>

static void
pdf_filter_xobject(fz_context *ctx, pdf_document *doc, pdf_obj *xobj, pdf_obj *page_res, pdf_filter_options *options, pdf_cycle_list *cycle_up);

static void
pdf_filter_type3(fz_context *ctx, pdf_document *doc, pdf_obj *obj, pdf_obj *page_res, pdf_filter_options *options, pdf_cycle_list *cycle_up);

static void
pdf_filter_resources(fz_context *ctx, pdf_document *doc, pdf_obj *in_res, pdf_obj *res, pdf_filter_options *options, pdf_cycle_list *cycle_up)
{
	pdf_cycle_list cycle;
	pdf_obj *obj;
	int i, n;

	if (!options->recurse)
		return;

	if (pdf_cycle(ctx, &cycle, cycle_up, in_res))
		return;

	/* ExtGState */
	obj = pdf_dict_get(ctx, res, PDF_NAME(ExtGState));
	if (obj)
	{
		n = pdf_dict_len(ctx, obj);
		for (i = 0; i < n; i++)
		{
			pdf_obj *smask = pdf_dict_get(ctx, pdf_dict_get_val(ctx, obj, i), PDF_NAME(SMask));
			if (smask)
			{
				pdf_obj *g = pdf_dict_get(ctx, smask, PDF_NAME(G));
				if (g)
				{
					/* Transparency group XObject */
					pdf_filter_xobject(ctx, doc, g, in_res, options, &cycle);
				}
			}
		}
	}

	/* Pattern */
	obj = pdf_dict_get(ctx, res, PDF_NAME(Pattern));
	if (obj)
	{
		n = pdf_dict_len(ctx, obj);
		for (i = 0; i < n; i++)
		{
			pdf_obj *pat = pdf_dict_get_val(ctx, obj, i);
			if (pat && pdf_dict_get_int(ctx, pat, PDF_NAME(PatternType)) == 1)
			{
				pdf_filter_xobject(ctx, doc, pat, in_res, options, &cycle);
			}
		}
	}

	/* XObject */
	if (!options->instance_forms)
	{
		obj = pdf_dict_get(ctx, res, PDF_NAME(XObject));
		if (obj)
		{
			n = pdf_dict_len(ctx, obj);
			for (i = 0; i < n; i++)
			{
				pdf_obj *xobj = pdf_dict_get_val(ctx, obj, i);
				if (xobj && pdf_dict_get(ctx, xobj, PDF_NAME(Subtype)) == PDF_NAME(Form))
				{
					pdf_filter_xobject(ctx, doc, xobj, in_res, options, &cycle);
				}
			}
		}
	}

	/* Font */
	obj = pdf_dict_get(ctx, res, PDF_NAME(Font));
	if (obj)
	{
		n = pdf_dict_len(ctx, obj);
		for (i = 0; i < n; i++)
		{
			pdf_obj *font = pdf_dict_get_val(ctx, obj, i);
			if (font && pdf_dict_get(ctx, font, PDF_NAME(Subtype)) == PDF_NAME(Type3))
			{
				pdf_filter_type3(ctx, doc, font, in_res, options, &cycle);
			}
		}
	}

}

/*
	Clean a content stream's rendering operations, with an optional post
	processing step.

	Firstly, this filters the PDF operators used to avoid (some cases of)
	repetition, and leaves the content stream in a balanced state with an
	unchanged top level matrix etc. At the same time, the resources actually
	used are collected into a new resource dictionary.

	Next, the resources themselves are recursively cleaned (as appropriate)
	in the same way, if the 'recurse' flag is set.
*/
static void
pdf_filter_content_stream(
	fz_context *ctx,
	pdf_document *doc,
	pdf_obj *in_stm,
	pdf_obj *in_res,
	fz_matrix transform,
	pdf_filter_options *options,
	int struct_parents,
	fz_buffer **out_buf,
	pdf_obj **out_res,
	pdf_cycle_list *cycle_up)
{
	pdf_processor *proc_buffer = NULL;
	pdf_processor *top = NULL;
	pdf_processor **list = NULL;
	int num_filters = 0;
	int i;

	fz_var(proc_buffer);

	*out_buf = NULL;
	*out_res = NULL;

	if (options->filters)
		for (; options->filters[num_filters].filter != NULL; num_filters++);

	if (num_filters > 0)
		list = fz_calloc(ctx, num_filters, sizeof(pdf_processor *));

	fz_try(ctx)
	{
		*out_buf = fz_new_buffer(ctx, 1024);
		top = proc_buffer = pdf_new_buffer_processor(ctx, *out_buf, options->ascii, options->newlines);
		if (num_filters > 0)
		{
			for (i = num_filters - 1; i >= 0; i--)
				top = list[i] = options->filters[i].filter(ctx, doc, top, struct_parents, transform, options, options->filters[i].options);
		}

		pdf_process_contents(ctx, top, doc, in_res, in_stm, NULL, out_res);
		pdf_close_processor(ctx, top);

		pdf_filter_resources(ctx, doc, in_res, *out_res, options, cycle_up);
	}
	fz_always(ctx)
	{
		for (i = 0; i < num_filters; i++)
			pdf_drop_processor(ctx, list[i]);
		pdf_drop_processor(ctx, proc_buffer);
		fz_free(ctx, list);
	}
	fz_catch(ctx)
	{
		fz_drop_buffer(ctx, *out_buf);
		*out_buf = NULL;
		pdf_drop_obj(ctx, *out_res);
		*out_res = NULL;
		fz_rethrow(ctx);
	}
}

/*
	Clean a Type 3 font's CharProcs content streams. This works almost
	exactly like pdf_filter_content_stream, but the resource dictionary is
	shared between all off the CharProcs.
*/
static void
pdf_filter_type3(fz_context *ctx, pdf_document *doc, pdf_obj *obj, pdf_obj *page_res, pdf_filter_options *options, pdf_cycle_list *cycle_up)
{
	pdf_cycle_list cycle;
	pdf_processor *proc_buffer = NULL;
	pdf_processor *proc_filter = NULL;
	pdf_obj *in_res;
	pdf_obj *out_res = NULL;
	pdf_obj *charprocs;
	int i, n;
	int num_filters = 0;
	pdf_processor **list = NULL;
	fz_buffer *buffer = NULL;
	pdf_processor *top = NULL;
	pdf_obj *res = NULL;
	fz_buffer *new_buf = NULL;

	fz_var(out_res);
	fz_var(proc_buffer);
	fz_var(proc_filter);
	fz_var(buffer);
	fz_var(res);
	fz_var(new_buf);
	fz_var(top);

	/* We cannot combine instancing with type3 fonts. The new names for
	 * instanced form/image resources would clash, since they start over for
	 * each content stream. This is not a problem for now, because we only
	 * use instancing with redaction, and redaction doesn't clean type3
	 * fonts.
	 */
	assert(!options->instance_forms);

	/* Avoid recursive cycles! */
	if (pdf_cycle(ctx, &cycle, cycle_up, obj))
		return;

	if (options->filters)
		for (; options->filters[num_filters].filter != NULL; num_filters++);

	if (num_filters > 0)
		list = fz_calloc(ctx, num_filters, sizeof(pdf_processor *));

	fz_try(ctx)
	{
		in_res = pdf_dict_get(ctx, obj, PDF_NAME(Resources));
		if (!in_res)
			in_res = page_res;

		buffer = fz_new_buffer(ctx, 1024);
		top = proc_buffer = pdf_new_buffer_processor(ctx, buffer, options->ascii, options->newlines);
		if (num_filters > 0)
		{
			for (i = num_filters - 1; i >= 0; i--)
				top = list[i] = options->filters[i].filter(ctx, doc, top, -1, fz_identity, options, options->filters[i].options);
		}

		pdf_processor_push_resources(ctx, top, in_res);
		charprocs = pdf_dict_get(ctx, obj, PDF_NAME(CharProcs));
		n = pdf_dict_len(ctx, charprocs);
		for (i = 0; i < n; i++)
		{
			pdf_obj *val = pdf_dict_get_val(ctx, charprocs, i);

			if (i > 0)
			{
				// reset all chained processors (and clear the buffer)
				pdf_reset_processor(ctx, top);
			}
			pdf_process_raw_contents(ctx, top, doc, val, NULL);

			pdf_close_processor(ctx, top);

			if (!options->no_update)
			{
				new_buf = fz_clone_buffer(ctx, buffer);
				pdf_update_stream(ctx, doc, val, new_buf, 0);
				fz_drop_buffer(ctx, new_buf);
				new_buf = NULL;
			}
		}

	}
	fz_always(ctx)
	{
		res = pdf_processor_pop_resources(ctx, top);
		for (i = 0; i < num_filters; i++)
			pdf_drop_processor(ctx, list[i]);
		pdf_drop_processor(ctx, proc_buffer);
		fz_free(ctx, list);
		fz_drop_buffer(ctx, new_buf);
		fz_drop_buffer(ctx, buffer);
	}
	fz_catch(ctx)
	{
		pdf_drop_obj(ctx, res);
		fz_rethrow(ctx);
	}
	pdf_dict_put_drop(ctx, obj, PDF_NAME(Resources), res);
}

static void
pdf_filter_xobject(fz_context *ctx, pdf_document *doc, pdf_obj *stm, pdf_obj *page_res, pdf_filter_options *options, pdf_cycle_list *cycle_up)
{
	pdf_cycle_list cycle;
	int struct_parents;
	pdf_obj *new_res = NULL;
	fz_buffer *new_buf = NULL;
	pdf_obj *old_res;

	fz_var(new_buf);
	fz_var(new_res);

	// TODO for RJW: XObject can also be a StructParent; how do we handle that case?

	struct_parents = pdf_dict_get_int_default(ctx, stm, PDF_NAME(StructParents), -1);

	old_res = pdf_dict_get(ctx, stm, PDF_NAME(Resources));
	if (!old_res)
		old_res = page_res;

	// TODO: don't clean objects more than once.

	/* Avoid recursive cycles! */
	if (pdf_cycle(ctx, &cycle, cycle_up, stm))
		return;
	fz_try(ctx)
	{
		pdf_filter_content_stream(ctx, doc, stm, old_res, fz_identity, options, struct_parents, &new_buf, &new_res, &cycle);
		if (!options->no_update)
		{
			pdf_update_stream(ctx, doc, stm, new_buf, 0);
			pdf_dict_put(ctx, stm, PDF_NAME(Resources), new_res);
		}
	}
	fz_always(ctx)
	{
		fz_drop_buffer(ctx, new_buf);
		pdf_drop_obj(ctx, new_res);
	}
	fz_catch(ctx)
		fz_rethrow(ctx);
}

pdf_obj *
pdf_filter_xobject_instance(fz_context *ctx, pdf_obj *old_xobj, pdf_obj *page_res, fz_matrix transform, pdf_filter_options *options, pdf_cycle_list *cycle_up)
{
	pdf_cycle_list cycle;
	pdf_document *doc = pdf_get_bound_document(ctx, old_xobj);
	pdf_obj *new_xobj = NULL;
	pdf_obj *new_res = NULL, *old_res;
	fz_buffer *new_buf = NULL;
	int struct_parents;
	fz_matrix matrix;

	fz_var(new_xobj);
	fz_var(new_buf);
	fz_var(new_res);

	// TODO for RJW: XObject can also be a StructParent; how do we handle that case?
	// TODO for RJW: will we run into trouble by duplicating StructParents stuff?

	struct_parents = pdf_dict_get_int_default(ctx, old_xobj, PDF_NAME(StructParents), -1);

	old_res = pdf_dict_get(ctx, old_xobj, PDF_NAME(Resources));
	if (!old_res)
		old_res = page_res;

	if (pdf_cycle(ctx, &cycle, cycle_up, old_xobj))
		return pdf_keep_obj(ctx, old_xobj);

	matrix = pdf_dict_get_matrix(ctx, old_xobj, PDF_NAME(Matrix));
	transform = fz_concat(matrix, transform);

	fz_try(ctx)
	{
		new_xobj = pdf_add_object_drop(ctx, doc, pdf_copy_dict(ctx, old_xobj));
		pdf_filter_content_stream(ctx, doc, old_xobj, old_res, transform, options, struct_parents, &new_buf, &new_res, &cycle);
		if (!options->no_update)
		{
			pdf_update_stream(ctx, doc, new_xobj, new_buf, 0);
			pdf_dict_put(ctx, new_xobj, PDF_NAME(Resources), new_res);
		}
	}
	fz_always(ctx)
	{
		fz_drop_buffer(ctx, new_buf);
		pdf_drop_obj(ctx, new_res);
	}
	fz_catch(ctx)
	{
		pdf_drop_obj(ctx, new_xobj);
		fz_rethrow(ctx);
	}

	return new_xobj;
}

void pdf_filter_page_contents(fz_context *ctx, pdf_document *doc, pdf_page *page, pdf_filter_options *options)
{
	pdf_obj *contents, *old_res;
	pdf_obj *new_res;
	fz_buffer *buffer;
	int struct_parents;

	struct_parents = pdf_dict_get_int_default(ctx, page->obj, PDF_NAME(StructParents), -1);

	contents = pdf_page_contents(ctx, page);
	old_res = pdf_page_resources(ctx, page);

	pdf_filter_content_stream(ctx, doc, contents, old_res, fz_identity, options, struct_parents, &buffer, &new_res, NULL);

	fz_try(ctx)
	{
		if (options->complete)
			options->complete(ctx, buffer, options->opaque);
		if (!options->no_update)
		{
			/* Always create a new stream object to replace the page contents. This is useful
			   both if the contents is an array of streams, is entirely missing or if the contents
			   are shared between pages. */
			contents = pdf_add_object_drop(ctx, doc, pdf_new_dict(ctx, doc, 1));
			pdf_dict_put_drop(ctx, page->obj, PDF_NAME(Contents), contents);
			pdf_update_stream(ctx, doc, contents, buffer, 0);
			pdf_dict_put(ctx, page->obj, PDF_NAME(Resources), new_res);
		}
	}
	fz_always(ctx)
	{
		fz_drop_buffer(ctx, buffer);
		pdf_drop_obj(ctx, new_res);
	}
	fz_catch(ctx)
		fz_rethrow(ctx);
}

void pdf_filter_annot_contents(fz_context *ctx, pdf_document *doc, pdf_annot *annot, pdf_filter_options *options)
{
	pdf_obj *ap = pdf_dict_get(ctx, annot->obj, PDF_NAME(AP));
	if (pdf_is_dict(ctx, ap))
	{
		int i, n = pdf_dict_len(ctx, ap);
		for (i = 0; i < n; i++)
		{
			pdf_obj *stm = pdf_dict_get_val(ctx, ap, i);
			if (pdf_is_stream(ctx, stm))
			{
				pdf_filter_xobject(ctx, doc, stm, NULL, options, NULL);
			}
		}
	}
}

/* REDACTIONS */

struct redact_filter_state {
	pdf_filter_options filter_opts;
	pdf_sanitize_filter_options sanitize_opts;
	pdf_filter_factory filter_list[2];
	pdf_page *page;
	pdf_annot *target; // NULL if all
	int line_art;
	int text;
};


static void pdf_run_obj_to_buf(fz_context *ctx, fz_buffer *buffer, pdf_obj *obj, pdf_page *page)
{
	pdf_processor *proc = pdf_new_buffer_processor(ctx, buffer, 0, 0);
	pdf_obj *res;


	fz_try(ctx)
	{
		res = pdf_xobject_resources(ctx, obj);
		if (res == NULL)
			res = pdf_page_resources(ctx, page);

		pdf_process_contents(ctx, proc, page->doc, res, obj, NULL, NULL);
		pdf_close_processor(ctx, proc);
	}
	fz_always(ctx)
		pdf_drop_processor(ctx, proc);
	fz_catch(ctx)
		fz_rethrow(ctx);
}

static void
pdf_redact_end_page(fz_context *ctx, fz_buffer *buf, void *opaque)
{
	struct redact_filter_state *red = opaque;
	pdf_page *page = red->page;
	pdf_annot *annot;
	pdf_obj *qp;
	int i, n;

	fz_append_string(ctx, buf, " 0 g\n");

	for (annot = pdf_first_annot(ctx, page); annot; annot = pdf_next_annot(ctx, annot))
	{
		if (red->target != NULL && red->target != annot)
			continue;
		if (pdf_dict_get(ctx, annot->obj, PDF_NAME(Subtype)) == PDF_NAME(Redact))
		{
			pdf_obj *ro = pdf_dict_get(ctx, annot->obj, PDF_NAME(RO));
			if (ro)
			{
				pdf_run_obj_to_buf(ctx, buf, ro, page);
			}
			else
			{
				qp = pdf_dict_get(ctx, annot->obj, PDF_NAME(QuadPoints));
				n = pdf_array_len(ctx, qp);
				if (n > 0)
				{
					for (i = 0; i < n; i += 8)
					{
						fz_quad q = pdf_to_quad(ctx, qp, i);
						fz_append_printf(ctx, buf, "%g %g m\n", q.ll.x, q.ll.y);
						fz_append_printf(ctx, buf, "%g %g l\n", q.lr.x, q.lr.y);
						fz_append_printf(ctx, buf, "%g %g l\n", q.ur.x, q.ur.y);
						fz_append_printf(ctx, buf, "%g %g l\n", q.ul.x, q.ul.y);
						fz_append_string(ctx, buf, "f\n");
					}
				}
				else
				{
					fz_rect r = pdf_dict_get_rect(ctx, annot->obj, PDF_NAME(Rect));
					fz_append_printf(ctx, buf, "%g %g m\n", r.x0, r.y0);
					fz_append_printf(ctx, buf, "%g %g l\n", r.x1, r.y0);
					fz_append_printf(ctx, buf, "%g %g l\n", r.x1, r.y1);
					fz_append_printf(ctx, buf, "%g %g l\n", r.x0, r.y1);
					fz_append_string(ctx, buf, "f\n");
				}
			}
		}
	}
}

static int
pdf_redact_text_filter(fz_context *ctx, void *opaque, int *ucsbuf, int ucslen, fz_matrix trm, fz_matrix ctm, fz_rect bbox, int tr, float ca, float CA)
{
	struct redact_filter_state *red = opaque;
	pdf_page *page = red->page;
	pdf_annot *annot;
	pdf_obj *qp;
	fz_rect r;
	fz_quad q;
	int i, n;
	float w, h;

	trm = fz_concat(trm, ctm);
	bbox = fz_transform_rect(bbox, trm);

	/* Shrink character bbox a bit */
	w = bbox.x1 - bbox.x0;
	h = bbox.y1 - bbox.y0;
	bbox.x0 += w / 10;
	bbox.x1 -= w / 10;
	bbox.y0 += h / 10;
	bbox.y1 -= h / 10;

	for (annot = pdf_first_annot(ctx, page); annot; annot = pdf_next_annot(ctx, annot))
	{
		if (red->target != NULL && red->target != annot)
			continue;
		if (pdf_dict_get(ctx, annot->obj, PDF_NAME(Subtype)) == PDF_NAME(Redact))
		{
			qp = pdf_dict_get(ctx, annot->obj, PDF_NAME(QuadPoints));
			n = pdf_array_len(ctx, qp);
			/* Note, we test for the intersection being a valid rectangle, NOT
			 * a non-empty one. This is because we can have 'empty' character
			 * boxes (say for diacritics), that while 0 width, do have a defined
			 * position on the plane, and hence inclusion makes sense. */
			if (n > 0)
			{
				for (i = 0; i < n; i += 8)
				{
					q = pdf_to_quad(ctx, qp, i);
					r = fz_rect_from_quad(q);
					if (fz_is_valid_rect(fz_intersect_rect(bbox, r)))
						return 1;
				}
			}
			else
			{
				r = pdf_dict_get_rect(ctx, annot->obj, PDF_NAME(Rect));
				if (fz_is_valid_rect(fz_intersect_rect(bbox, r)))
					return 1;
			}
		}
	}

	return 0;
}

static int
pdf_redact_invisible_text_filter(fz_context *ctx, void *opaque, int *ucsbuf, int ucslen, fz_matrix trm, fz_matrix ctm, fz_rect bbox, int tr, float ca, float CA)
{
	int invisible = 0;

	switch (tr)
	{
		case 0: /* Fill */
			invisible = (ca == 0);
			break;
		case 1: /* Stroke */
			invisible = (CA == 0);
			break;
		case 2: /* Fill + Stroke */
			invisible = (ca == 0 && CA == 0);
			break;
		case 3: /* Neither Fill nor stroke */
			invisible = 1;
			break;
	}

	if (!invisible)
		return 0;

	return pdf_redact_text_filter(ctx, opaque, ucsbuf, ucslen, trm, ctm, bbox, tr, ca, CA);
}

/* A copy of an image's pixels, in a colorspace in which redaction can write
 * white. */
static fz_pixmap *
pdf_redactable_pixmap_from_image(fz_context *ctx, fz_image *image)
{
	fz_pixmap *pixmap = NULL;
	fz_pixmap *original = fz_get_pixmap_from_image(ctx, image, NULL, NULL, NULL, NULL);
	int imagemask = image->imagemask;

	fz_try(ctx)
	{
		enum fz_colorspace_type type = fz_colorspace_type(ctx, original ? original->colorspace : NULL);
retry_with_base:
		switch (type)
		{
			case FZ_COLORSPACE_NONE:
			case FZ_COLORSPACE_RGB:
			case FZ_COLORSPACE_GRAY:
			case FZ_COLORSPACE_CMYK:
			case FZ_COLORSPACE_LAB:
				pixmap = fz_clone_pixmap(ctx, original);
				break;
			case FZ_COLORSPACE_INDEXED:
				type = original->colorspace->u.indexed.base ? original->colorspace->u.indexed.base->type : FZ_COLORSPACE_NONE;
				goto retry_with_base;
			case FZ_COLORSPACE_SEPARATION:
				pixmap = fz_convert_pixmap(ctx, original, fz_device_cmyk(ctx), NULL, NULL, fz_default_color_params, 1);
				break;
			default:
				pixmap = fz_convert_pixmap(ctx, original, fz_device_rgb(ctx), NULL, NULL, fz_default_color_params, 1);
				break;
		}
		if (imagemask)
			fz_invert_pixmap_alpha(ctx, pixmap);
	}
	fz_always(ctx)
		fz_drop_pixmap(ctx, original);
	fz_catch(ctx)
		fz_rethrow(ctx);

	return pixmap;
}

/* Blank the pixels x0 <= x < x1, y0 <= y < y1 of a pixmap from
 * pdf_redactable_pixmap_from_image. */
static void
pdf_redact_pixels(fz_context *ctx, fz_pixmap *pixmap, int x0, int y0, int x1, int y1)
{
	int x, y, k, n, bpp;
	unsigned char white;

	n = pixmap->n - pixmap->alpha;
	bpp = pixmap->n;
	if (fz_colorspace_is_subtractive(ctx, pixmap->colorspace))
		white = 0;
	else
		white = 255;

	for (y = y0; y < y1; ++y)
	{
		for (x = x0; x < x1; ++x)
		{
			unsigned char *s = &pixmap->samples[(size_t)y * pixmap->stride + (size_t)x * bpp];
			for (k = 0; k < n; ++k)
				s[k] = white;
			if (pixmap->alpha)
				s[k] = 255;
		}
	}
}

static fz_pixmap *
pdf_redact_image_imp(fz_context *ctx, fz_matrix ctm, fz_image *image, fz_pixmap *pixmap, fz_pixmap **pmask, fz_quad q)
{
	fz_matrix inv_ctm;
	fz_irect r;
	int y;
	fz_pixmap *mask = *pmask;
	int pixmap_cloned = 0;

	if (!pixmap)
	{
		pixmap = pdf_redactable_pixmap_from_image(ctx, image);
		pixmap_cloned = 1;
	}

	if (!mask && image->mask)
	{
		fz_pixmap *original = fz_get_pixmap_from_image(ctx, image->mask, NULL, NULL, NULL, NULL);

		fz_try(ctx)
		{
			mask = fz_clone_pixmap(ctx, original);
			*pmask = mask;
		}
		fz_always(ctx)
		{
			fz_drop_pixmap(ctx, original);
		}
		fz_catch(ctx)
		{
			if (pixmap_cloned)
				fz_drop_pixmap(ctx, pixmap);
			fz_rethrow(ctx);
		}
	}

	/* If we have a 1x1 image, to which a mask is being applied
	 * then it's the mask we really want to change, not the
	 * image. We might have just a small section of the image
	 * being covered, and setting the whole thing to white
	 * will blank stuff outside the desired area. */
	if (!mask || pixmap->w > 1 || pixmap->h > 1)
	{
		inv_ctm = fz_post_scale(fz_invert_matrix(ctm), pixmap->w, pixmap->h);
		r = fz_round_rect(fz_transform_rect(fz_rect_from_quad(q), inv_ctm));
		r.x0 = fz_clampi(r.x0, 0, pixmap->w);
		r.x1 = fz_clampi(r.x1, 0, pixmap->w);
		r.y1 = fz_clampi(pixmap->h - r.y1, 0, pixmap->h);
		r.y0 = fz_clampi(pixmap->h - r.y0, 0, pixmap->h);
		pdf_redact_pixels(ctx, pixmap, r.x0, r.y1, r.x1, r.y0);
	}

	if (mask)
	{
		inv_ctm = fz_post_scale(fz_invert_matrix(ctm), mask->w, mask->h);
		r = fz_round_rect(fz_transform_rect(fz_rect_from_quad(q), inv_ctm));
		r.x0 = fz_clampi(r.x0, 0, mask->w);
		r.x1 = fz_clampi(r.x1, 0, mask->w);
		r.y1 = fz_clampi(mask->h - r.y1, 0, mask->h);
		r.y0 = fz_clampi(mask->h - r.y0, 0, mask->h);
		for (y = r.y1; y < r.y0; ++y)
		{
			unsigned char *s = &mask->samples[(size_t)y * mask->stride + (size_t)r.x0];
			memset(s, 0xff, r.x1-r.x0);
		}
	}

	return pixmap;
}

static fz_image *
pdf_redact_image_filter_remove(fz_context *ctx, void *opaque, fz_matrix ctm, const char *name, fz_image *image, fz_rect clip)
{
	fz_pixmap *redacted = NULL;
	struct redact_filter_state *red = opaque;
	pdf_page *page = red->page;
	pdf_annot *annot;
	pdf_obj *qp;
	fz_rect area;
	fz_rect r;
	int i, n;

	fz_var(redacted);

	area = fz_transform_rect(fz_unit_rect, ctm);

	for (annot = pdf_first_annot(ctx, page); annot; annot = pdf_next_annot(ctx, annot))
	{
		if (red->target != NULL && red->target != annot)
			continue;
		if (pdf_dict_get(ctx, annot->obj, PDF_NAME(Subtype)) == PDF_NAME(Redact))
		{
			qp = pdf_dict_get(ctx, annot->obj, PDF_NAME(QuadPoints));
			n = pdf_array_len(ctx, qp);
			if (n > 0)
			{
				for (i = 0; i < n; i += 8)
				{
					r = fz_rect_from_quad(pdf_to_quad(ctx, qp, i));
					r = fz_intersect_rect(r, area);
					if (!fz_is_empty_rect(r))
						return NULL;
				}
			}
			else
			{
				r = pdf_dict_get_rect(ctx, annot->obj, PDF_NAME(Rect));
				r = fz_intersect_rect(r, area);
				if (!fz_is_empty_rect(r))
					return NULL;
			}
		}
	}

	return fz_keep_image(ctx, image);
}

static fz_image *
pdf_redact_image_filter_remove_invisible(fz_context *ctx, void *opaque, fz_matrix ctm, const char *name, fz_image *image, fz_rect clip)
{
	fz_pixmap *redacted = NULL;
	struct redact_filter_state *red = opaque;
	pdf_page *page = red->page;
	pdf_annot *annot;
	pdf_obj *qp;
	fz_rect area;
	fz_rect r;
	int i, n;

	fz_var(redacted);

	area = fz_transform_rect(fz_unit_rect, ctm);

	/* Restrict the are of the image to that which can actually be seen. */
	area = fz_intersect_rect(area, clip);

	for (annot = pdf_first_annot(ctx, page); annot; annot = pdf_next_annot(ctx, annot))
	{
		if (red->target != NULL && red->target != annot)
			continue;
		if (pdf_dict_get(ctx, annot->obj, PDF_NAME(Subtype)) == PDF_NAME(Redact))
		{
			qp = pdf_dict_get(ctx, annot->obj, PDF_NAME(QuadPoints));
			n = pdf_array_len(ctx, qp);
			if (n > 0)
			{
				for (i = 0; i < n; i += 8)
				{
					r = fz_rect_from_quad(pdf_to_quad(ctx, qp, i));
					r = fz_intersect_rect(r, area);
					if (!fz_is_empty_rect(r))
						return NULL;
				}
			}
			else
			{
				r = pdf_dict_get_rect(ctx, annot->obj, PDF_NAME(Rect));
				r = fz_intersect_rect(r, area);
				if (!fz_is_empty_rect(r))
					return NULL;
			}
		}
	}

	return fz_keep_image(ctx, image);
}

static fz_image *
pdf_redact_image_filter_pixels(fz_context *ctx, void *opaque, fz_matrix ctm, const char *name, fz_image *image, fz_rect clip)
{
	fz_pixmap *redacted = NULL;
	fz_pixmap *mask = NULL;
	struct redact_filter_state *red = opaque;
	pdf_page *page = red->page;
	pdf_annot *annot;
	pdf_obj *qp;
	fz_quad area, q;
	fz_rect r;
	int i, n;

	fz_var(redacted);
	fz_var(mask);

	area = fz_transform_quad(fz_quad_from_rect(fz_unit_rect), ctm);

	/* First see if we can redact the image completely */
	for (annot = pdf_first_annot(ctx, page); annot; annot = pdf_next_annot(ctx, annot))
	{
		if (red->target != NULL && red->target != annot)
			continue;
		if (pdf_dict_get(ctx, annot->obj, PDF_NAME(Subtype)) == PDF_NAME(Redact))
		{
			qp = pdf_dict_get(ctx, annot->obj, PDF_NAME(QuadPoints));
			n = pdf_array_len(ctx, qp);
			if (n > 0)
			{
				for (i = 0; i < n; i += 8)
				{
					q = pdf_to_quad(ctx, qp, i);
					if (fz_is_quad_inside_quad(area, q))
						return NULL;
				}
			}
			else
			{
				r = pdf_dict_get_rect(ctx, annot->obj, PDF_NAME(Rect));
				q = fz_quad_from_rect(r);
				if (fz_is_quad_inside_quad(area, q))
					return NULL;
			}
		}
	}

	/* Blank out redacted parts of the image if necessary */
	fz_try(ctx)
	{
		for (annot = pdf_first_annot(ctx, page); annot; annot = pdf_next_annot(ctx, annot))
		{
			if (red->target != NULL && red->target != annot)
				continue;
			if (pdf_dict_get(ctx, annot->obj, PDF_NAME(Subtype)) == PDF_NAME(Redact))
			{
				qp = pdf_dict_get(ctx, annot->obj, PDF_NAME(QuadPoints));
				n = pdf_array_len(ctx, qp);
				if (n > 0)
				{
					for (i = 0; i < n; i += 8)
					{
						q = pdf_to_quad(ctx, qp, i);
						if (fz_is_quad_intersecting_quad(area, q))
							redacted = pdf_redact_image_imp(ctx, ctm, image, redacted, &mask, q);
					}
				}
				else
				{
					r = pdf_dict_get_rect(ctx, annot->obj, PDF_NAME(Rect));
					q = fz_quad_from_rect(r);
					if (fz_is_quad_intersecting_quad(area, q))
						redacted = pdf_redact_image_imp(ctx, ctm, image, redacted, &mask, q);
				}
			}
		}
	}
	fz_catch(ctx)
	{
		fz_drop_pixmap(ctx, redacted);
		fz_drop_pixmap(ctx, mask);
		fz_rethrow(ctx);
	}

	if (redacted)
	{
		int imagemask = image->imagemask;
		fz_image *imask = fz_keep_image(ctx, image->mask);

		fz_var(imask);

		fz_try(ctx)
		{
			if (mask)
			{
				fz_drop_image(ctx, imask);
				imask = NULL;
				imask = fz_new_image_from_pixmap(ctx, mask, NULL);
			}
			image = fz_new_image_from_pixmap(ctx, redacted, NULL);
			image->imagemask = imagemask;
			image->mask = imask;
			imask = NULL;
		}
		fz_always(ctx)
		{
			fz_drop_pixmap(ctx, redacted);
			fz_drop_pixmap(ctx, mask);
			fz_drop_image(ctx, imask);
		}
		fz_catch(ctx)
			fz_rethrow(ctx);
		return image;
	}

	return fz_keep_image(ctx, image);
}

/* Returns 0 if area does not intersect with any of our redactions.
 * Returns 2 if area is completely included within one of our redactions.
 * Returns 1 otherwise. */
static int
rect_touches_redactions(fz_context *ctx, fz_rect area, struct redact_filter_state *red)
{
	pdf_annot *annot;
	pdf_obj *qp;
	fz_quad q;
	fz_rect r, s;
	int i, n;
	pdf_page *page = red->page;

	for (annot = pdf_first_annot(ctx, page); annot; annot = pdf_next_annot(ctx, annot))
	{
		if (red->target != NULL && red->target != annot)
			continue;
		if (pdf_dict_get(ctx, annot->obj, PDF_NAME(Subtype)) == PDF_NAME(Redact))
		{
			qp = pdf_dict_get(ctx, annot->obj, PDF_NAME(QuadPoints));
			n = pdf_array_len(ctx, qp);
			if (n > 0)
			{
				for (i = 0; i < n; i += 8)
				{
					q = pdf_to_quad(ctx, qp, i);
					r = fz_rect_from_quad(q);
					s = fz_intersect_rect(r, area);
					if (!fz_is_empty_rect(s))
					{
						if (fz_contains_rect(r, area))
							return 2;
						return 1;
					}
				}
			}
			else
			{
				r = pdf_dict_get_rect(ctx, annot->obj, PDF_NAME(Rect));
				s = fz_intersect_rect(r, area);
				if (!fz_is_empty_rect(s))
				{
					if (fz_contains_rect(r, area))
						return 2;
					return 1;
				}
			}
		}
	}
	return 0;
}

static void
remove_page_link(fz_context *ctx, pdf_page *page, pdf_obj *obj)
{
	pdf_link **linkp = (pdf_link **)&page->links;
	pdf_link *link;

	while ((link = *linkp) != NULL)
	{
		if (link->obj == obj)
		{
			*linkp = (pdf_link *)link->super.next;
			link->super.next = NULL;
			fz_drop_link(ctx, &link->super);
			break;
		}
		else
		{
			linkp = (pdf_link **)&link->super.next;
		}
	}
}

static void
pdf_redact_page_links(fz_context *ctx, struct redact_filter_state *red)
{
	pdf_obj *annots;
	pdf_obj *link;
	fz_rect area;
	int k;

	annots = pdf_dict_get(ctx, red->page->obj, PDF_NAME(Annots));
	k = 0;
	while (k < pdf_array_len(ctx, annots))
	{
		link = pdf_array_get(ctx, annots, k);
		if (pdf_dict_get(ctx, link, PDF_NAME(Subtype)) == PDF_NAME(Link))
		{
			area = pdf_dict_get_rect(ctx, link, PDF_NAME(Rect));
			if (rect_touches_redactions(ctx, area, red))
			{
				pdf_array_delete(ctx, annots, k);
				remove_page_link(ctx, red->page, link);
				continue;
			}
		}
		++k;
	}
}

static void
pdf_redact_page_annotations(fz_context *ctx, struct redact_filter_state *red)
{
	pdf_annot *annot;
	fz_rect area;

restart:
	for (annot = pdf_first_annot(ctx, red->page); annot; annot = pdf_next_annot(ctx, annot))
	{
		if (pdf_annot_type(ctx, annot) == PDF_ANNOT_FREE_TEXT)
		{
			area = pdf_dict_get_rect(ctx, pdf_annot_obj(ctx, annot), PDF_NAME(Rect));
			if (rect_touches_redactions(ctx, area, red))
			{
				pdf_delete_annot(ctx, red->page, annot);
				goto restart;
			}
		}
	}
}

static int culler(fz_context *ctx, void *opaque, fz_rect bbox, fz_cull_type type)
{
	struct redact_filter_state *red = opaque;

	switch (type)
	{
	case FZ_CULL_PATH_FILL:
	case FZ_CULL_PATH_STROKE:
	case FZ_CULL_PATH_FILL_STROKE:
	case FZ_CULL_CLIP_PATH_FILL:
	case FZ_CULL_CLIP_PATH_STROKE:
	case FZ_CULL_CLIP_PATH_FILL_STROKE:
		if (red->line_art == PDF_REDACT_LINE_ART_REMOVE_IF_COVERED)
			return (rect_touches_redactions(ctx, bbox, red) == 2);
		else if (red->line_art == PDF_REDACT_LINE_ART_REMOVE_IF_TOUCHED)
			return (rect_touches_redactions(ctx, bbox, red) != 0);
		return 0;
	default:
		return 0;
	}
}

static
void init_redact_filter(fz_context *ctx, pdf_redact_options *redact_opts, struct redact_filter_state *red, pdf_page *page, pdf_annot *target)
{
	int black_boxes = redact_opts ? redact_opts->black_boxes : 0;
	int image_method = redact_opts ? redact_opts->image_method : PDF_REDACT_IMAGE_PIXELS;
	int line_art = redact_opts ? redact_opts->line_art : PDF_REDACT_LINE_ART_NONE;
	int text = redact_opts ? redact_opts->text : PDF_REDACT_TEXT_REMOVE;

	memset(&red->filter_opts, 0, sizeof red->filter_opts);
	memset(&red->sanitize_opts, 0, sizeof red->sanitize_opts);

	red->filter_opts.recurse = 0; /* don't redact patterns, softmasks, and type3 fonts */
	red->filter_opts.instance_forms = 1; /* redact xobjects with instancing */
	red->filter_opts.ascii = 1;
	red->filter_opts.opaque = red;
	red->filter_opts.filters = red->filter_list;
	if (black_boxes)
		red->filter_opts.complete = pdf_redact_end_page;
	red->line_art = line_art;
	red->text = text;

	red->sanitize_opts.opaque = red;
	if (text == PDF_REDACT_TEXT_REMOVE)
		red->sanitize_opts.text_filter = pdf_redact_text_filter;
	if (text == PDF_REDACT_TEXT_REMOVE_INVISIBLE)
		red->sanitize_opts.text_filter = pdf_redact_invisible_text_filter;
	if (image_method == PDF_REDACT_IMAGE_PIXELS)
		red->sanitize_opts.image_filter = pdf_redact_image_filter_pixels;
	if (image_method == PDF_REDACT_IMAGE_REMOVE)
		red->sanitize_opts.image_filter = pdf_redact_image_filter_remove;
	if (image_method == PDF_REDACT_IMAGE_REMOVE_UNLESS_INVISIBLE)
		red->sanitize_opts.image_filter = pdf_redact_image_filter_remove_invisible;
	red->sanitize_opts.culler = culler;

	red->filter_list[0].filter = pdf_new_sanitize_filter;
	red->filter_list[0].options = &red->sanitize_opts;
	red->filter_list[1].filter = NULL;
	red->filter_list[1].options = NULL;

	red->page = page;
	red->target = target;
}

static int
pdf_apply_redaction_imp(fz_context *ctx, pdf_page *page, pdf_annot *target, pdf_redact_options *redact_opts)
{
	pdf_annot *annot;
	int has_redactions = 0;
	struct redact_filter_state red;
	pdf_document *doc = page->doc;

	for (annot = pdf_first_annot(ctx, page); annot; annot = pdf_next_annot(ctx, annot)) {
		if (target != NULL && target != annot)
			continue;
		if (pdf_dict_get(ctx, annot->obj, PDF_NAME(Subtype)) == PDF_NAME(Redact))
			has_redactions = 1;
	}

	if (!has_redactions)
		return 0;

	init_redact_filter(ctx, redact_opts, &red, page, target);

	if (target)
		pdf_begin_operation(ctx, doc, "Apply redaction");
	else
		pdf_begin_operation(ctx, doc, "Apply redactions on page");
	fz_try(ctx)
	{
		pdf_filter_page_contents(ctx, doc, page, &red.filter_opts);
		pdf_redact_page_links(ctx, &red);
		pdf_redact_page_annotations(ctx, &red);

		annot = pdf_first_annot(ctx, page);
		while (annot)
		{
			if (target == NULL || annot == target)
			{
				if (pdf_dict_get(ctx, annot->obj, PDF_NAME(Subtype)) == PDF_NAME(Redact))
				{
					pdf_delete_annot(ctx, page, annot);
					annot = pdf_first_annot(ctx, page);
					continue;
				}
			}
			annot = pdf_next_annot(ctx, annot);
		}

		doc->redacted = 1;
		pdf_end_operation(ctx, doc);
	}
	fz_catch(ctx)
	{
		pdf_abandon_operation(ctx, doc);
		fz_rethrow(ctx);
	}

	return 1;
}

int
pdf_redact_page(fz_context *ctx, pdf_document *doc, pdf_page *page, pdf_redact_options *redact_opts)
{
	if (page == NULL || page->doc != doc)
		fz_throw(ctx, FZ_ERROR_ARGUMENT, "Can't redact a page not from the doc");
	return pdf_apply_redaction_imp(ctx, page, NULL, redact_opts);
}

int
pdf_apply_redaction(fz_context *ctx, pdf_annot *annot, pdf_redact_options *redact_opts)
{
	return pdf_apply_redaction_imp(ctx, annot->page, annot, redact_opts);
}

/* Hard clipping of pages */

struct clip_filter_state {
	pdf_filter_options filter_opts;
	pdf_sanitize_filter_options sanitize_opts;
	pdf_filter_factory filter_list[2];
	pdf_page *page;
	fz_rect clip;
};

static int clip_culler(fz_context *ctx, void *opaque, fz_rect bbox, fz_cull_type type)
{
	struct clip_filter_state *hc = opaque;

	switch (type)
	{
	case FZ_CULL_PATH_FILL:
	case FZ_CULL_PATH_STROKE:
	case FZ_CULL_PATH_FILL_STROKE:
	case FZ_CULL_CLIP_PATH_FILL:
	case FZ_CULL_CLIP_PATH_STROKE:
	case FZ_CULL_CLIP_PATH_FILL_STROKE:
	case FZ_CULL_GLYPH:
	case FZ_CULL_IMAGE:
	case FZ_CULL_SHADING:
		return fz_is_empty_rect(fz_intersect_rect(bbox, hc->clip));
	default:
		return 0;
	}
}

static
void init_clip_filter(fz_context *ctx, struct clip_filter_state *hc, pdf_page *page, fz_rect clip)
{
	memset(&hc->filter_opts, 0, sizeof hc->filter_opts);
	memset(&hc->sanitize_opts, 0, sizeof hc->sanitize_opts);

	hc->filter_opts.recurse = 0; /* don't redact patterns, softmasks, and type3 fonts */
	hc->filter_opts.instance_forms = 1; /* redact xobjects with instancing */
	hc->filter_opts.ascii = 0;
	hc->filter_opts.opaque = hc;
	hc->filter_opts.filters = hc->filter_list;
	hc->clip = clip;

	hc->sanitize_opts.opaque = hc;
	hc->sanitize_opts.culler = clip_culler;

	hc->filter_list[0].filter = pdf_new_sanitize_filter;
	hc->filter_list[0].options = &hc->sanitize_opts;
	hc->filter_list[1].filter = NULL;
	hc->filter_list[1].options = NULL;

	hc->page = page;
}

static void
pdf_clip_page_links(fz_context *ctx, struct clip_filter_state *hc)
{
	pdf_obj *annots;
	pdf_obj *link;
	fz_rect area;
	int k;

	annots = pdf_dict_get(ctx, hc->page->obj, PDF_NAME(Annots));
	k = 0;
	while (k < pdf_array_len(ctx, annots))
	{
		link = pdf_array_get(ctx, annots, k);
		if (pdf_dict_get(ctx, link, PDF_NAME(Subtype)) == PDF_NAME(Link))
		{
			area = pdf_dict_get_rect(ctx, link, PDF_NAME(Rect));
			if (fz_is_empty_rect(fz_intersect_rect(area, hc->clip)))
			{
				pdf_array_delete(ctx, annots, k);
				continue;
			}
		}
		++k;
	}
}

static void
pdf_clip_page_annotations(fz_context *ctx, struct clip_filter_state *hc)
{
	pdf_annot *annot;
	fz_rect area;

restart:
	for (annot = pdf_first_annot(ctx, hc->page); annot; annot = pdf_next_annot(ctx, annot))
	{
		if (pdf_annot_type(ctx, annot) == PDF_ANNOT_FREE_TEXT)
		{
			area = pdf_dict_get_rect(ctx, pdf_annot_obj(ctx, annot), PDF_NAME(Rect));
			if (fz_is_empty_rect(fz_intersect_rect(area, hc->clip)))
			{
				pdf_delete_annot(ctx, hc->page, annot);
				goto restart;
			}
		}
	}
}

void
pdf_clip_page(fz_context *ctx, pdf_page *page, fz_rect clip)
{
	pdf_document *doc;
	struct clip_filter_state hc;
	fz_matrix page_ctm, inv_page_ctm;

	if (page == NULL)
		return;

	doc = page->doc;

	pdf_page_transform(ctx, page, NULL, &page_ctm);
	inv_page_ctm = fz_invert_matrix(page_ctm);
	clip = fz_transform_rect(clip, inv_page_ctm);

	init_clip_filter(ctx, &hc, page, clip);

	pdf_begin_operation(ctx, doc, "Apply hard clip to page");
	fz_try(ctx)
	{
		pdf_filter_page_contents(ctx, doc, page, &hc.filter_opts);
		pdf_clip_page_links(ctx, &hc);
		pdf_clip_page_annotations(ctx, &hc);
		pdf_end_operation(ctx, doc);
	}
	fz_catch(ctx)
	{
		pdf_abandon_operation(ctx, doc);
		fz_rethrow(ctx);
	}
}

/* Vectorisation of pages */

struct vectorize_filter_state {
	pdf_filter_options filter_opts;
	pdf_vectorize_filter_options vectorize_opts;
	pdf_filter_factory filter_list[2];
	pdf_page *page;
};

static
void init_vectorize_filter(fz_context *ctx, struct vectorize_filter_state *hc, pdf_page *page)
{
	memset(&hc->filter_opts, 0, sizeof hc->filter_opts);
	memset(&hc->vectorize_opts, 0, sizeof hc->vectorize_opts);

	hc->filter_opts.recurse = 0;
	hc->filter_opts.instance_forms = 0;
	hc->filter_opts.ascii = 0;
	hc->filter_opts.opaque = hc;
	hc->filter_opts.filters = hc->filter_list;
	hc->filter_opts.recurse = 1;

	hc->vectorize_opts.opaque = hc;

	hc->filter_list[0].filter = pdf_new_vectorize_filter;
	hc->filter_list[0].options = &hc->vectorize_opts;
	hc->filter_list[1].filter = NULL;
	hc->filter_list[1].options = NULL;

	hc->page = page;
}

void
pdf_vectorize_page(fz_context *ctx, pdf_page *page)
{
	pdf_document *doc;
	struct vectorize_filter_state hv;

	if (page == NULL)
		return;

	doc = page->doc;

	init_vectorize_filter(ctx, &hv, page);

	pdf_begin_operation(ctx, doc, "Vectorize text to page");
	fz_try(ctx)
	{
		pdf_filter_page_contents(ctx, doc, page, &hv.filter_opts);
		pdf_end_operation(ctx, doc);
	}
	fz_catch(ctx)
	{
		pdf_abandon_operation(ctx, doc);
		fz_rethrow(ctx);
	}
}

/* BLANKING IMAGE PIXELS THAT NO PLACEMENT SHOWS */

enum
{
	UNSHOWN_CLIP_PATH,
	UNSHOWN_CLIP_STROKE_PATH,
	UNSHOWN_CLIP_TEXT,
	UNSHOWN_CLIP_STROKE_TEXT,
	UNSHOWN_CLIP_IMAGE_MASK
};

/* One clip in force, with the ones under it. Shared by every placement made
 * while it is in force. */
typedef struct unshown_clip
{
	int refs;
	struct unshown_clip *prev;
	int kind;
	int even_odd;
	fz_path *path;
	fz_stroke_state *stroke;
	fz_text *text;
	fz_image *image;
	fz_matrix ctm;
} unshown_clip;

typedef struct
{
	fz_matrix ctm;
	fz_rect page;
	unshown_clip *clip;
	/* Drawn as part of a tiling pattern: treat every pixel as shown. */
	int whole;
} unshown_placement;

/* Every placement of one image. Images with the same pixel data are one. */
typedef struct
{
	unsigned char digest[16];
	int w, h;
	int len, max;
	unshown_placement *placements;
	int done;
	/* The image with its unshown pixels blanked, added to the document,
	 * or NULL if every pixel is shown. */
	pdf_obj *blanked;
} unshown_image;

struct pdf_unshown_pixels
{
	pdf_document *doc;
	int len, max;
	unshown_image *images;
};

typedef struct
{
	fz_device super;
	pdf_unshown_pixels *up;
	unshown_clip *clip;
	fz_rect page;
	int tiles;
} unshown_device;

static void
drop_unshown_clip(fz_context *ctx, unshown_clip *clip)
{
	while (clip && --clip->refs == 0)
	{
		unshown_clip *prev = clip->prev;
		fz_drop_path(ctx, clip->path);
		fz_drop_stroke_state(ctx, clip->stroke);
		fz_drop_text(ctx, clip->text);
		fz_drop_image(ctx, clip->image);
		fz_free(ctx, clip);
		clip = prev;
	}
}

static unshown_clip *
keep_unshown_clip(unshown_clip *clip)
{
	if (clip)
		clip->refs++;
	return clip;
}

static unshown_image *
find_unshown_image(fz_context *ctx, pdf_unshown_pixels *up, fz_image *image, int create)
{
	unsigned char digest[16] = { 0 };
	unshown_image *ui;
	int i;

	fz_image_digest(ctx, image, digest);
	for (i = 0; i < up->len; i++)
	{
		ui = &up->images[i];
		if (ui->w == image->w && ui->h == image->h && !memcmp(ui->digest, digest, 16))
			return ui;
	}
	if (!create)
		return NULL;

	if (up->len == up->max)
	{
		int max = up->max ? up->max * 2 : 16;
		up->images = fz_realloc_array(ctx, up->images, max, unshown_image);
		up->max = max;
	}
	ui = &up->images[up->len++];
	memset(ui, 0, sizeof *ui);
	memcpy(ui->digest, digest, 16);
	ui->w = image->w;
	ui->h = image->h;
	return ui;
}

static void
record_unshown_placement(fz_context *ctx, unshown_device *dev, fz_image *image, fz_matrix ctm)
{
	unshown_image *ui;
	unshown_placement *pl;

	if (image == NULL)
		return;

	ui = find_unshown_image(ctx, dev->up, image, 1);
	if (ui->len == ui->max)
	{
		int max = ui->max ? ui->max * 2 : 4;
		ui->placements = fz_realloc_array(ctx, ui->placements, max, unshown_placement);
		ui->max = max;
	}
	pl = &ui->placements[ui->len++];
	pl->ctm = ctm;
	pl->page = dev->page;
	pl->clip = keep_unshown_clip(dev->clip);
	pl->whole = dev->tiles > 0;

	/* A soft mask or stencil mask is an image of its own, placed where
	 * the image is. */
	record_unshown_placement(ctx, dev, image->mask, ctm);
}

static void
push_unshown_clip(fz_context *ctx, unshown_device *dev, int kind, const fz_path *path, int even_odd, const fz_stroke_state *stroke, const fz_text *text, fz_image *image, fz_matrix ctm)
{
	unshown_clip *clip = fz_malloc_struct(ctx, unshown_clip);

	clip->refs = 1;
	clip->kind = kind;
	clip->even_odd = even_odd;
	clip->path = fz_keep_path(ctx, path);
	clip->stroke = fz_keep_stroke_state(ctx, stroke);
	clip->text = fz_keep_text(ctx, text);
	clip->image = fz_keep_image(ctx, image);
	clip->ctm = ctm;
	clip->prev = dev->clip;
	dev->clip = clip;
}

static void
unshown_clip_path(fz_context *ctx, fz_device *dev_, const fz_path *path, int even_odd, fz_matrix ctm, fz_rect scissor)
{
	push_unshown_clip(ctx, (unshown_device *)dev_, UNSHOWN_CLIP_PATH, path, even_odd, NULL, NULL, NULL, ctm);
}

static void
unshown_clip_stroke_path(fz_context *ctx, fz_device *dev_, const fz_path *path, const fz_stroke_state *stroke, fz_matrix ctm, fz_rect scissor)
{
	push_unshown_clip(ctx, (unshown_device *)dev_, UNSHOWN_CLIP_STROKE_PATH, path, 0, stroke, NULL, NULL, ctm);
}

static void
unshown_clip_text(fz_context *ctx, fz_device *dev_, const fz_text *text, fz_matrix ctm, fz_rect scissor)
{
	push_unshown_clip(ctx, (unshown_device *)dev_, UNSHOWN_CLIP_TEXT, NULL, 0, NULL, text, NULL, ctm);
}

static void
unshown_clip_stroke_text(fz_context *ctx, fz_device *dev_, const fz_text *text, const fz_stroke_state *stroke, fz_matrix ctm, fz_rect scissor)
{
	push_unshown_clip(ctx, (unshown_device *)dev_, UNSHOWN_CLIP_STROKE_TEXT, NULL, 0, stroke, text, NULL, ctm);
}

static void
unshown_clip_image_mask(fz_context *ctx, fz_device *dev_, fz_image *image, fz_matrix ctm, fz_rect scissor)
{
	unshown_device *dev = (unshown_device *)dev_;
	record_unshown_placement(ctx, dev, image, ctm);
	push_unshown_clip(ctx, dev, UNSHOWN_CLIP_IMAGE_MASK, NULL, 0, NULL, NULL, image, ctm);
}

/* A soft mask can only hide what lies inside its area. Its content is drawn
 * inside that area too. */
static void
unshown_begin_mask(fz_context *ctx, fz_device *dev_, fz_rect area, int luminosity, fz_colorspace *cs, const float *bc, fz_color_params color_params)
{
	fz_path *path = fz_new_path(ctx);
	fz_try(ctx)
	{
		fz_rectto(ctx, path, area.x0, area.y0, area.x1, area.y1);
		push_unshown_clip(ctx, (unshown_device *)dev_, UNSHOWN_CLIP_PATH, path, 0, NULL, NULL, NULL, fz_identity);
	}
	fz_always(ctx)
		fz_drop_path(ctx, path);
	fz_catch(ctx)
		fz_rethrow(ctx);
}

static void
unshown_pop_clip(fz_context *ctx, fz_device *dev_)
{
	unshown_device *dev = (unshown_device *)dev_;
	unshown_clip *clip = dev->clip;

	if (clip == NULL)
		return;
	dev->clip = keep_unshown_clip(clip->prev);
	drop_unshown_clip(ctx, clip);
}

static void
unshown_fill_image(fz_context *ctx, fz_device *dev_, fz_image *image, fz_matrix ctm, float alpha, fz_color_params color_params)
{
	record_unshown_placement(ctx, (unshown_device *)dev_, image, ctm);
}

static void
unshown_fill_image_mask(fz_context *ctx, fz_device *dev_, fz_image *image, fz_matrix ctm, fz_colorspace *cs, const float *color, float alpha, fz_color_params color_params)
{
	record_unshown_placement(ctx, (unshown_device *)dev_, image, ctm);
}

static int
unshown_begin_tile(fz_context *ctx, fz_device *dev_, fz_rect area, fz_rect view, float xstep, float ystep, fz_matrix ctm, int id, int doc_id)
{
	((unshown_device *)dev_)->tiles++;
	return 0;
}

static void
unshown_end_tile(fz_context *ctx, fz_device *dev_)
{
	((unshown_device *)dev_)->tiles--;
}

static void
unshown_drop_device(fz_context *ctx, fz_device *dev_)
{
	unshown_device *dev = (unshown_device *)dev_;
	drop_unshown_clip(ctx, dev->clip);
	dev->clip = NULL;
}

static void
gather_unshown_placements(fz_context *ctx, pdf_unshown_pixels *up, int page_num, const char *usage)
{
	pdf_page *page = pdf_load_page(ctx, up->doc, page_num);
	unshown_device *dev = NULL;

	fz_var(dev);

	fz_try(ctx)
	{
		dev = fz_new_derived_device(ctx, unshown_device);
		dev->super.clip_path = unshown_clip_path;
		dev->super.clip_stroke_path = unshown_clip_stroke_path;
		dev->super.clip_text = unshown_clip_text;
		dev->super.clip_stroke_text = unshown_clip_stroke_text;
		dev->super.clip_image_mask = unshown_clip_image_mask;
		dev->super.begin_mask = unshown_begin_mask;
		dev->super.pop_clip = unshown_pop_clip;
		dev->super.fill_image = unshown_fill_image;
		dev->super.fill_image_mask = unshown_fill_image_mask;
		dev->super.begin_tile = unshown_begin_tile;
		dev->super.end_tile = unshown_end_tile;
		dev->super.drop_device = unshown_drop_device;
		dev->up = up;
		dev->page = fz_bound_page(ctx, &page->super);

		pdf_run_page_with_usage(ctx, page, &dev->super, fz_identity, usage, NULL);
		fz_close_device(ctx, &dev->super);
	}
	fz_always(ctx)
	{
		if (dev)
			fz_drop_device(ctx, &dev->super);
		fz_drop_page(ctx, &page->super);
	}
	fz_catch(ctx)
		fz_rethrow(ctx);
}

static void
replay_unshown_clip(fz_context *ctx, fz_device *dev, unshown_clip *clip)
{
	switch (clip->kind)
	{
	case UNSHOWN_CLIP_PATH:
		fz_clip_path(ctx, dev, clip->path, clip->even_odd, clip->ctm, fz_infinite_rect);
		break;
	case UNSHOWN_CLIP_STROKE_PATH:
		fz_clip_stroke_path(ctx, dev, clip->path, clip->stroke, clip->ctm, fz_infinite_rect);
		break;
	case UNSHOWN_CLIP_TEXT:
		fz_clip_text(ctx, dev, clip->text, clip->ctm, fz_infinite_rect);
		break;
	case UNSHOWN_CLIP_STROKE_TEXT:
		fz_clip_stroke_text(ctx, dev, clip->text, clip->stroke, clip->ctm, fz_infinite_rect);
		break;
	case UNSHOWN_CLIP_IMAGE_MASK:
		fz_clip_image_mask(ctx, dev, clip->image, clip->ctm, fz_infinite_rect);
		break;
	}
}

/* Draw, in the image's own pixels, the part of the page each placement shows:
 * the page area, under every clip in force there. Returns NULL if every pixel
 * is shown. */
static fz_pixmap *
draw_shown_pixels(fz_context *ctx, unshown_image *ui)
{
	static const float white = 1;
	fz_pixmap *shown = fz_new_pixmap(ctx, fz_device_gray(ctx), ui->w, ui->h, NULL, 0);
	fz_device *dev = NULL;
	fz_path *path = NULL;
	unshown_clip **clips = NULL;
	int i, j, n;

	fz_var(dev);
	fz_var(path);
	fz_var(clips);

	fz_try(ctx)
	{
		fz_clear_pixmap_with_value(ctx, shown, 0);
		for (i = 0; i < ui->len; i++)
		{
			unshown_placement *pl = &ui->placements[i];
			unshown_clip *clip;
			fz_matrix inv;

			if (pl->whole)
			{
				fz_drop_pixmap(ctx, shown);
				shown = NULL;
				break;
			}
			/* A placement with no area shows nothing. */
			if (fz_try_invert_matrix(&inv, pl->ctm))
				continue;

			/* From page space to image pixels. A device's image matrix
			 * puts the image's first row at the top of its unit square. */
			dev = fz_new_draw_device(ctx, fz_concat(inv, fz_scale(ui->w, ui->h)), shown);

			n = 0;
			for (clip = pl->clip; clip; clip = clip->prev)
				n++;
			clips = fz_malloc_array(ctx, n, unshown_clip *);
			for (j = n, clip = pl->clip; clip; clip = clip->prev)
				clips[--j] = clip;
			for (j = 0; j < n; j++)
				replay_unshown_clip(ctx, dev, clips[j]);

			path = fz_new_path(ctx);
			fz_rectto(ctx, path, pl->page.x0, pl->page.y0, pl->page.x1, pl->page.y1);
			fz_fill_path(ctx, dev, path, 0, fz_identity, fz_device_gray(ctx), &white, 1, fz_default_color_params);

			for (j = 0; j < n; j++)
				fz_pop_clip(ctx, dev);
			fz_close_device(ctx, dev);

			fz_drop_path(ctx, path);
			path = NULL;
			fz_free(ctx, clips);
			clips = NULL;
			fz_drop_device(ctx, dev);
			dev = NULL;
		}
	}
	fz_always(ctx)
	{
		fz_drop_device(ctx, dev);
		fz_drop_path(ctx, path);
		fz_free(ctx, clips);
	}
	fz_catch(ctx)
	{
		fz_drop_pixmap(ctx, shown);
		fz_rethrow(ctx);
	}

	return shown;
}

/* Grow the shown area by one pixel, so that the pixels next to a shown one,
 * which smooth scaling of the image can blend into it, are kept. Returns the
 * number of pixels still not shown. */
static size_t
widen_shown_pixels(fz_context *ctx, fz_pixmap *shown)
{
	int w = shown->w, h = shown->h, x, y;
	unsigned char *s = shown->samples;
	ptrdiff_t stride = shown->stride;
	unsigned char *row = fz_malloc(ctx, (size_t)w * h);
	size_t unshown = 0;

	/* Across, into the copy. */
	for (y = 0; y < h; y++)
	{
		unsigned char *src = s + y * stride;
		unsigned char *dst = row + (size_t)y * w;
		for (x = 0; x < w; x++)
			dst[x] = src[x] | (x > 0 ? src[x-1] : 0) | (x < w-1 ? src[x+1] : 0);
	}
	/* Down, back into the pixmap. */
	for (y = 0; y < h; y++)
	{
		unsigned char *dst = s + y * stride;
		unsigned char *mid = row + (size_t)y * w;
		unsigned char *up = y > 0 ? mid - w : NULL;
		unsigned char *down = y < h-1 ? mid + w : NULL;
		for (x = 0; x < w; x++)
		{
			dst[x] = mid[x] | (up ? up[x] : 0) | (down ? down[x] : 0);
			if (dst[x] == 0)
				unshown++;
		}
	}
	fz_free(ctx, row);

	return unshown;
}

static fz_image *
blank_unshown_pixels(fz_context *ctx, fz_image *image, fz_pixmap *shown)
{
	fz_pixmap *pixmap = pdf_redactable_pixmap_from_image(ctx, image);
	fz_image *blanked = NULL;
	int x, x0, y;

	fz_try(ctx)
	{
		for (y = 0; y < pixmap->h; y++)
		{
			unsigned char *row = shown->samples + ((int64_t)y * shown->h / pixmap->h) * shown->stride;
			x = 0;
			while (x < pixmap->w)
			{
				if (row[(int64_t)x * shown->w / pixmap->w])
				{
					x++;
					continue;
				}
				x0 = x;
				while (x < pixmap->w && !row[(int64_t)x * shown->w / pixmap->w])
					x++;
				pdf_redact_pixels(ctx, pixmap, x0, y, x, y + 1);
			}
		}
		blanked = fz_new_image_from_pixmap(ctx, pixmap, NULL);
		blanked->imagemask = image->imagemask;
	}
	fz_always(ctx)
		fz_drop_pixmap(ctx, pixmap);
	fz_catch(ctx)
		fz_rethrow(ctx);

	return blanked;
}

/* pdf_add_image stores the samples of a pixmap image as they are. */
static void
deflate_image_stream(fz_context *ctx, pdf_document *doc, pdf_obj *obj)
{
	fz_buffer *raw;
	fz_buffer *zip = NULL;
	unsigned char *data;
	size_t len;

	if (obj == NULL || pdf_dict_get(ctx, obj, PDF_NAME(Filter)))
		return;

	fz_var(zip);

	raw = pdf_load_raw_stream(ctx, obj);
	fz_try(ctx)
	{
		data = fz_new_deflated_data_from_buffer(ctx, &len, raw, FZ_DEFLATE_DEFAULT);
		zip = fz_new_buffer_from_data(ctx, data, len);
		pdf_dict_put(ctx, obj, PDF_NAME(Filter), PDF_NAME(FlateDecode));
		pdf_update_stream(ctx, doc, obj, zip, 1);
	}
	fz_always(ctx)
	{
		fz_drop_buffer(ctx, zip);
		fz_drop_buffer(ctx, raw);
	}
	fz_catch(ctx)
		fz_rethrow(ctx);
}

/* Is there a blanked version of the image? Worked out once per image; the
 * blanked pixels are then kept only as ui->blanked, compressed. */
static int
unshown_has_blanked(fz_context *ctx, pdf_document *doc, unshown_image *ui, fz_image *image)
{
	fz_pixmap *shown;
	fz_image *blanked = NULL;

	if (ui->done)
		return ui->blanked != NULL;

	fz_var(blanked);

	shown = draw_shown_pixels(ctx, ui);
	fz_try(ctx)
	{
		if (shown && widen_shown_pixels(ctx, shown) > 0)
		{
			blanked = blank_unshown_pixels(ctx, image, shown);
			fz_drop_pixmap(ctx, shown);
			shown = NULL;
			ui->blanked = pdf_add_image(ctx, doc, blanked);
			deflate_image_stream(ctx, doc, ui->blanked);
			deflate_image_stream(ctx, doc, pdf_dict_get(ctx, ui->blanked, PDF_NAME(SMask)));
		}
	}
	fz_always(ctx)
	{
		fz_drop_image(ctx, blanked);
		fz_drop_pixmap(ctx, shown);
	}
	fz_catch(ctx)
		fz_rethrow(ctx);
	ui->done = 1;

	return ui->blanked != NULL;
}

/* Rewrite an image XObject's pixel data in place, so that every use of it
 * gets the blanked pixels. It shares its stream data with ui->blanked. */
static void
replace_image_data(fz_context *ctx, pdf_document *doc, pdf_obj *obj, unshown_image *ui)
{
	fz_buffer *buf;
	int i, n;

	buf = pdf_load_raw_stream(ctx, ui->blanked);
	fz_try(ctx)
	{
		pdf_dict_del(ctx, obj, PDF_NAME(Width));
		pdf_dict_del(ctx, obj, PDF_NAME(Height));
		pdf_dict_del(ctx, obj, PDF_NAME(ColorSpace));
		pdf_dict_del(ctx, obj, PDF_NAME(BitsPerComponent));
		pdf_dict_del(ctx, obj, PDF_NAME(Filter));
		pdf_dict_del(ctx, obj, PDF_NAME(DecodeParms));
		pdf_dict_del(ctx, obj, PDF_NAME(Decode));
		pdf_dict_del(ctx, obj, PDF_NAME(ImageMask));
		pdf_dict_del(ctx, obj, PDF_NAME(SMaskInData));
		/* Transparency the decoded pixels carried (a colour key mask, or
		 * a JPX image's own) now comes as a soft mask. */
		if (pdf_dict_get(ctx, ui->blanked, PDF_NAME(SMask)))
		{
			pdf_dict_del(ctx, obj, PDF_NAME(SMask));
			pdf_dict_del(ctx, obj, PDF_NAME(Mask));
		}

		n = pdf_dict_len(ctx, ui->blanked);
		for (i = 0; i < n; i++)
		{
			pdf_obj *key = pdf_dict_get_key(ctx, ui->blanked, i);
			if (pdf_name_eq(ctx, key, PDF_NAME(Length)) || pdf_name_eq(ctx, key, PDF_NAME(Type)) || pdf_name_eq(ctx, key, PDF_NAME(Subtype)))
				continue;
			pdf_dict_put_drop(ctx, obj, key, pdf_deep_copy_obj(ctx, pdf_dict_get_val(ctx, ui->blanked, i)));
		}
		pdf_update_stream(ctx, doc, obj, buf, 1);
	}
	fz_always(ctx)
		fz_drop_buffer(ctx, buf);
	fz_catch(ctx)
		fz_rethrow(ctx);

	pdf_purge_object_from_store(ctx, doc, pdf_to_num(ctx, obj));
}

static void
blank_unshown_image_xobjects(fz_context *ctx, pdf_unshown_pixels *up)
{
	pdf_document *doc = up->doc;
	int num, n = pdf_xref_len(ctx, doc);

	for (num = 1; num < n; num++)
	{
		pdf_obj *obj = pdf_new_indirect(ctx, doc, num, 0);
		fz_image *image = NULL;
		unshown_image *ui;

		fz_var(image);

		fz_try(ctx)
		{
			if (pdf_is_stream(ctx, obj) && pdf_name_eq(ctx, pdf_dict_get(ctx, obj, PDF_NAME(Subtype)), PDF_NAME(Image)))
			{
				/* An image that cannot be loaded cannot be blanked
				 * either, and another reader may still draw it all, so
				 * the save fails. */
				image = pdf_load_image(ctx, doc, obj);
				ui = find_unshown_image(ctx, up, image, 0);
				if (ui && unshown_has_blanked(ctx, doc, ui, image))
					replace_image_data(ctx, doc, obj, ui);
			}
		}
		fz_always(ctx)
		{
			fz_drop_image(ctx, image);
			pdf_drop_obj(ctx, obj);
		}
		fz_catch(ctx)
			fz_rethrow(ctx);
	}
}

pdf_unshown_pixels *
pdf_new_unshown_pixels(fz_context *ctx, pdf_document *doc)
{
	pdf_unshown_pixels *up = fz_malloc_struct(ctx, pdf_unshown_pixels);
	int i, n;

	up->doc = doc;
	fz_try(ctx)
	{
		/* A pixel is shown if it shows on screen or in print. */
		n = pdf_count_pages(ctx, doc);
		for (i = 0; i < n; i++)
		{
			gather_unshown_placements(ctx, up, i, "View");
			gather_unshown_placements(ctx, up, i, "Print");
		}
		blank_unshown_image_xobjects(ctx, up);
	}
	fz_catch(ctx)
	{
		pdf_drop_unshown_pixels(ctx, up);
		fz_rethrow(ctx);
	}

	return up;
}

fz_image *
pdf_unshown_pixels_inline_image_filter(fz_context *ctx, void *opaque, fz_matrix ctm, const char *name, fz_image *image, fz_rect clip)
{
	pdf_unshown_pixels *up = opaque;
	unshown_image *ui;

	/* Image XObjects were blanked in place already. The sanitize filter
	 * takes no reference to an XObject's image when it comes back as is. */
	if (strcmp(name, "<inline>"))
		return image;

	ui = find_unshown_image(ctx, up, image, 0);
	if (ui && unshown_has_blanked(ctx, up->doc, ui, image))
		return pdf_load_image(ctx, up->doc, ui->blanked);

	return fz_keep_image(ctx, image);
}

void
pdf_drop_unshown_pixels(fz_context *ctx, pdf_unshown_pixels *up)
{
	int i, j;

	if (up == NULL)
		return;
	for (i = 0; i < up->len; i++)
	{
		unshown_image *ui = &up->images[i];
		for (j = 0; j < ui->len; j++)
			drop_unshown_clip(ctx, ui->placements[j].clip);
		fz_free(ctx, ui->placements);
		pdf_drop_obj(ctx, ui->blanked);
	}
	fz_free(ctx, up->images);
	fz_free(ctx, up);
}
