/*
 * Geeqie
 * (C) 2006 John Ellis
 * Copyright (C) 2008 - 2012 The Geeqie Team
 *
 * Author: John Ellis
 * Author: Vladimir Nadvornik
 *
 * This software is released under the GNU General Public License (GNU GPL).
 * Please read the included file COPYING for more information.
 * This software comes with no warranty of any kind, use at your own risk!
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "main.h"
#include "pixbuf-renderer.h"
#include "renderer-clutter.h"

#include "intl.h"
#include "layout.h"

#include <gtk/gtk.h>

#ifdef HAVE_CLUTTER

#include <clutter/clutter.h>

#include <clutter-gtk/clutter-gtk.h>



#define GQ_BUILD 1

#ifdef GQ_BUILD
#include "main.h"
#include "pixbuf_util.h"
#include "exif.h"
#else
typedef enum {
	EXIF_ORIENTATION_UNKNOWN	= 0,
	EXIF_ORIENTATION_TOP_LEFT	= 1,
	EXIF_ORIENTATION_TOP_RIGHT	= 2,
	EXIF_ORIENTATION_BOTTOM_RIGHT	= 3,
	EXIF_ORIENTATION_BOTTOM_LEFT	= 4,
	EXIF_ORIENTATION_LEFT_TOP	= 5,
	EXIF_ORIENTATION_RIGHT_TOP	= 6,
	EXIF_ORIENTATION_RIGHT_BOTTOM	= 7,
	EXIF_ORIENTATION_LEFT_BOTTOM	= 8
} ExifOrientationType;
#endif

#define GET_RIGHT_PIXBUF_OFFSET(rc) \
        (( (rc->stereo_mode & PR_STEREO_RIGHT) && !(rc->stereo_mode & PR_STEREO_SWAP)) || \
         (!(rc->stereo_mode & PR_STEREO_RIGHT) &&  (rc->stereo_mode & PR_STEREO_SWAP)) ?  \
          rc->pr->stereo_pixbuf_offset_right : rc->pr->stereo_pixbuf_offset_left )

#define GET_LEFT_PIXBUF_OFFSET(rc) \
        ((!(rc->stereo_mode & PR_STEREO_RIGHT) && !(rc->stereo_mode & PR_STEREO_SWAP)) || \
         ( (rc->stereo_mode & PR_STEREO_RIGHT) &&  (rc->stereo_mode & PR_STEREO_SWAP)) ?  \
          rc->pr->stereo_pixbuf_offset_right : rc->pr->stereo_pixbuf_offset_left )


typedef struct _OverlayData OverlayData;
struct _OverlayData
{
	gint id;

	GdkPixbuf *pixbuf;
	ClutterActor *actor;

	gint x;
	gint y;

	OverlayRendererFlags flags;
};

typedef struct _RendererClutter RendererClutter;

struct _RendererClutter
{
	RendererFuncs f;
	PixbufRenderer *pr;

	
	gint stereo_mode;
	gint stereo_off_x;
	gint stereo_off_y;
	
	
	GList *pending_updates;
	gint idle_update;
	
	GList *overlay_list;
	
	GtkWidget *widget; /* widget and stage may be shared with other renderers */
	ClutterActor *stage;
	ClutterActor *texture;
	ClutterActor *group;
};

typedef struct _RendererClutterAreaParam RendererClutterAreaParam;
struct _RendererClutterAreaParam {
	RendererClutter *rc;
	gint x;
	gint y;
	gint w;
	gint h;
};

static void rc_set_shader(CoglHandle material)
{
  CoglHandle shader;
  CoglHandle program;

  shader = cogl_create_shader (COGL_SHADER_TYPE_FRAGMENT);
  cogl_shader_source (shader,
  "vec3 checker(vec2 texc, vec3 color0, vec3 color1)"
  "{"
  "  if (mod(int(floor(texc.x) + floor(texc.y)), 2) == 0)"
  "    return color0;"
  "  else"
  "    return color1;"
  "}"
  
  "uniform sampler2D tex;"
  "void main(void)"
  "{"
  "    vec3 bg = checker(gl_FragCoord.xy / 16, vec3(0.6, 0.6, 0.6), vec3(0.4, 0.4, 0.4));"
  "    vec4 img4 = texture2D(tex, gl_TexCoord[0].xy);"
  "    vec3 img3 = img4.rgb;"
  "    gl_FragColor = vec4(img3 * img4.a + bg * (1.0 - img4.a), 1.0);"
  "}"
  );
  cogl_shader_compile(shader);
  gchar *err = cogl_shader_get_info_log(shader);
  printf("%s\n",err);
  g_free(err);

  program = cogl_create_program ();
  cogl_program_attach_shader (program, shader);
  cogl_handle_unref (shader);
  cogl_program_link (program);

  gint uniform_no = cogl_program_get_uniform_location (program, "tex");
  cogl_program_set_uniform_1i (program, uniform_no, 0);

  cogl_material_set_user_program (material, program);
  cogl_handle_unref (program);
}


static void rc_sync_actor(RendererClutter *rc)
{
	PixbufRenderer *pr = rc->pr;
	gint anchor_x = 0;
	gint anchor_y = 0;
	
	clutter_actor_set_anchor_point(CLUTTER_ACTOR(rc->texture), 0, 0);

	printf("scale %d %d\n", rc->pr->width, rc->pr->height);
	printf("pos   %d %d\n", rc->pr->x_offset, rc->pr->y_offset);
	
	clutter_actor_set_scale(CLUTTER_ACTOR(rc->texture), 
			        (gfloat)pr->width / pr->image_width,
			        (gfloat)pr->height / pr->image_height);
			        
	switch (pr->orientation)
		{
		case EXIF_ORIENTATION_TOP_LEFT:
			/* normal  */
			clutter_actor_set_rotation(CLUTTER_ACTOR(rc->texture),
						CLUTTER_Z_AXIS,
						0, 0, 0, 0);
			clutter_actor_set_rotation(CLUTTER_ACTOR(rc->texture),
						CLUTTER_Y_AXIS,
						0, 0, 0, 0);
			anchor_x = 0;
			anchor_y = 0;
			break;
		case EXIF_ORIENTATION_TOP_RIGHT:
			/* mirrored */
			clutter_actor_set_rotation(CLUTTER_ACTOR(rc->texture),
						CLUTTER_Z_AXIS,
						0, 0, 0, 0);
			clutter_actor_set_rotation(CLUTTER_ACTOR(rc->texture),
						CLUTTER_Y_AXIS,
						180, 0, 0, 0);
			anchor_x = pr->width;
			anchor_y = 0;
			break;
		case EXIF_ORIENTATION_BOTTOM_RIGHT:
			/* upside down */
			clutter_actor_set_rotation(CLUTTER_ACTOR(rc->texture),
						CLUTTER_Z_AXIS,
						180, 0, 0, 0);
			clutter_actor_set_rotation(CLUTTER_ACTOR(rc->texture),
						CLUTTER_Y_AXIS,
						0, 0, 0, 0);
			anchor_x = pr->width;
			anchor_y = pr->height;
			break;
		case EXIF_ORIENTATION_BOTTOM_LEFT:
			/* flipped */
			clutter_actor_set_rotation(CLUTTER_ACTOR(rc->texture),
						CLUTTER_Z_AXIS,
						180, 0, 0, 0);
			clutter_actor_set_rotation(CLUTTER_ACTOR(rc->texture),
						CLUTTER_Y_AXIS,
						180, 0, 0, 0);
			anchor_x = 0;
			anchor_y = pr->height;
			break;
		case EXIF_ORIENTATION_LEFT_TOP:
			clutter_actor_set_rotation(CLUTTER_ACTOR(rc->texture),
						CLUTTER_Z_AXIS,
						-90, 0, 0, 0);
			clutter_actor_set_rotation(CLUTTER_ACTOR(rc->texture),
						CLUTTER_Y_AXIS,
						180, 0, 0, 0);
			anchor_x = 0;
			anchor_y = 0;
			break;
		case EXIF_ORIENTATION_RIGHT_TOP:
			/* rotated -90 (270) */
			clutter_actor_set_rotation(CLUTTER_ACTOR(rc->texture),
						CLUTTER_Z_AXIS,
						-90, 0, 0, 0);
			clutter_actor_set_rotation(CLUTTER_ACTOR(rc->texture),
						CLUTTER_Y_AXIS,
						0, 0, 0, 0);
			anchor_x = 0;
			anchor_y = pr->height;
			break;
		case EXIF_ORIENTATION_RIGHT_BOTTOM:
			clutter_actor_set_rotation(CLUTTER_ACTOR(rc->texture),
						CLUTTER_Z_AXIS,
						90, 0, 0, 0);
			clutter_actor_set_rotation(CLUTTER_ACTOR(rc->texture),
						CLUTTER_Y_AXIS,
						180, 0, 0, 0);
			anchor_x = pr->width;
			anchor_y = pr->height;
			break;
		case EXIF_ORIENTATION_LEFT_BOTTOM:
			/* rotated 90 */
			clutter_actor_set_rotation(CLUTTER_ACTOR(rc->texture),
						CLUTTER_Z_AXIS,
						90, 0, 0, 0);
			clutter_actor_set_rotation(CLUTTER_ACTOR(rc->texture),
						CLUTTER_Y_AXIS,
						0, 0, 0, 0);
			anchor_x = pr->width;
			anchor_y = 0;
			break;
		default:
			/* The other values are out of range */
			break;
		}
	
	clutter_actor_set_position(CLUTTER_ACTOR(rc->texture), 
				pr->x_offset - pr->x_scroll + anchor_x, 
				pr->y_offset - pr->y_scroll + anchor_y);

}


static void renderer_area_clip_add(RendererClutter *rc, gfloat x, gfloat y, gfloat w, gfloat h)
{
	PixbufRenderer *pr = rc->pr;
	gfloat x2, y2;
	gfloat clip_x, clip_y, clip_w, clip_h, clip_x2, clip_y2;
	
	x2 = x + w;
	y2 = y + h;
	
	clutter_actor_get_clip(rc->texture, &clip_x, &clip_y, &clip_w, &clip_h);
	
	clip_x2 = clip_x + clip_w;
	clip_y2 = clip_y + clip_h;
	
	if (clip_x > x) clip_x = x;
	if (clip_x2 < x2) clip_x2 = x2;
	if (clip_y > y) clip_y = y;
	if (clip_y2 < y2) clip_y2 = y2;
	
	clip_w = clip_x2 - clip_x;
	clip_h = clip_y2 - clip_y;
	
	printf("clip %f %f %f %f\n", clip_x, clip_y, clip_w, clip_h);
	clutter_actor_set_clip(rc->texture, clip_x, clip_y, clip_w, clip_h);
}

#define MAX_REGION_AREA (32768 * 1024)

static gboolean renderer_area_changed_cb(gpointer data)
{
	RendererClutter *rc = (RendererClutter *)data;
	PixbufRenderer *pr = rc->pr;
	
	RendererClutterAreaParam *par = rc->pending_updates->data;
	
	gint h = MAX_REGION_AREA / par->w;
	if (h == 0) h = 1;
	if (h > par->h) h = par->h;
	
	
	printf("renderer_area_changed_cb %d %d %d %d  (%d)\n", par->x, par->y, par->w, h, par->h);
	DEBUG_0("%s upload start", get_exec_time());
	if (pr->pixbuf)
		{
		CoglHandle texture = clutter_texture_get_cogl_texture(CLUTTER_TEXTURE(rc->texture));
		
		cogl_texture_set_region(texture,
					par->x + GET_RIGHT_PIXBUF_OFFSET(rc),
					par->y,
					par->x,
					par->y,
					par->w,
					h,
					par->w,
					h,
					gdk_pixbuf_get_has_alpha(pr->pixbuf) ? COGL_PIXEL_FORMAT_RGBA_8888 : COGL_PIXEL_FORMAT_RGB_888,
					gdk_pixbuf_get_rowstride(pr->pixbuf),
					gdk_pixbuf_get_pixels(pr->pixbuf));
		}
	DEBUG_0("%s upload end", get_exec_time());
	renderer_area_clip_add(rc, par->x, par->y, par->w, h);

		
	par->y += h;
	par->h -= h;
	
	if (par->h == 0)
		{
		rc->pending_updates = g_list_remove(rc->pending_updates, par);
		g_free(par);
		}
	if (!rc->pending_updates)
		{
		clutter_actor_queue_redraw(CLUTTER_ACTOR(rc->texture));
		rc->idle_update = 0;
		return FALSE;
		}
	return TRUE;
}


static void renderer_area_changed(void *renderer, gint src_x, gint src_y, gint src_w, gint src_h)
{
	RendererClutter *rc = (RendererClutter *)renderer;
	PixbufRenderer *pr = rc->pr;
	RendererClutterAreaParam *par;

	gint width = gdk_pixbuf_get_width(pr->pixbuf);
	gint height = gdk_pixbuf_get_height(pr->pixbuf);
		
	if (pr->stereo_data == STEREO_PIXBUF_SBS || pr->stereo_data == STEREO_PIXBUF_CROSS) 
			{
			width /= 2;
			}
	
	if (!pr_clip_region(src_x, src_y, src_w, src_h,
                            GET_RIGHT_PIXBUF_OFFSET(rc), 0, width, height,
                            &src_x, &src_y, &src_w, &src_h)) return;
	
	par = g_new0(RendererClutterAreaParam, 1);
	par->rc = rc;
	par->x = src_x - GET_RIGHT_PIXBUF_OFFSET(rc);
	par->y = src_y;
	par->w = src_w;
	par->h = src_h;
	rc->pending_updates = g_list_append(rc->pending_updates, par);
	if (!rc->idle_update) 
		{
		rc->idle_update = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, renderer_area_changed_cb, rc, NULL);
		}
}

static void renderer_remove_pending_updates(RendererClutter *rc)
{
	if (rc->idle_update) g_idle_remove_by_data(rc);
	rc->idle_update = 0;
	while (rc->pending_updates)
		{
		RendererClutterAreaParam *par = rc->pending_updates->data;
		rc->pending_updates = g_list_remove(rc->pending_updates, par);
		g_free(par);
		}
}

static void renderer_update_pixbuf(void *renderer, gboolean lazy)
{
	RendererClutter *rc = (RendererClutter *)renderer;
	PixbufRenderer *pr = rc->pr;
	
	renderer_remove_pending_updates(rc);
	
	if (pr->pixbuf)
		{
		gint width = gdk_pixbuf_get_width(pr->pixbuf);
		gint height = gdk_pixbuf_get_height(pr->pixbuf);
		
		gint prev_width, prev_height;
		
		if (pr->stereo_data == STEREO_PIXBUF_SBS || pr->stereo_data == STEREO_PIXBUF_CROSS) 
			{
			width /= 2;
			}

		
		printf("renderer_update_pixbuf\n");
		clutter_texture_get_base_size(CLUTTER_TEXTURE(rc->texture), &prev_width, &prev_height);
		printf("change from %d %d to %d %d\n", prev_width, prev_height, width, height);
		
		if (width != prev_width || height != prev_height)
			{
			/* FIXME use CoglMaterial with multiple textures for background, color management, anaglyph, ... */
			CoglHandle texture = cogl_texture_new_with_size(width,
									height,
									COGL_TEXTURE_NO_AUTO_MIPMAP,
									gdk_pixbuf_get_has_alpha(pr->pixbuf) ? COGL_PIXEL_FORMAT_RGBA_8888 : COGL_PIXEL_FORMAT_RGB_888);

			if (texture != COGL_INVALID_HANDLE)
				{
				clutter_texture_set_cogl_texture(CLUTTER_TEXTURE(rc->texture), texture);
				cogl_handle_unref(texture);
				}
			}
		clutter_actor_set_clip(rc->texture, 0, 0, 0, 0); /* visible area is extended as area_changed events arrive */
		if (!lazy)
			{
			renderer_area_changed(renderer, GET_RIGHT_PIXBUF_OFFSET(rc), 0, width, height);
			}
		}


	printf("renderer_update_pixbuf\n");
	rc_sync_actor(rc);
}



static void renderer_update_zoom(void *renderer, gboolean lazy)
{
	RendererClutter *rc = (RendererClutter *)renderer;
	PixbufRenderer *pr = rc->pr;

	printf("renderer_update_zoom\n");
	rc_sync_actor(rc);
}

static void renderer_invalidate_region(void *renderer, gint x, gint y, gint w, gint h)
{
}

static OverlayData *rc_overlay_find(RendererClutter *rc, gint id)
{
	GList *work;

	work = rc->overlay_list;
	while (work)
		{
		OverlayData *od = work->data;
		work = work->next;

		if (od->id == id) return od;
		}

	return NULL;
}

static void rc_overlay_actor_destroy_cb(ClutterActor *actor, gpointer user_data)
{
	OverlayData *od = user_data;
	od->actor = NULL;
}

static void rc_overlay_free(RendererClutter *rc, OverlayData *od)
{
	rc->overlay_list = g_list_remove(rc->overlay_list, od);

	if (od->pixbuf) g_object_unref(G_OBJECT(od->pixbuf));
	if (od->actor) clutter_actor_destroy(od->actor);
	g_free(od);
}

static void rc_overlay_update_position(RendererClutter *rc, OverlayData *od)
{
	gint px, py, pw, ph;

	pw = gdk_pixbuf_get_width(od->pixbuf);
	ph = gdk_pixbuf_get_height(od->pixbuf);
	px = od->x;
	py = od->y;

	if (od->flags & OVL_RELATIVE)
		{
		if (px < 0) px = rc->pr->viewport_width - pw + px;
		if (py < 0) py = rc->pr->viewport_height - ph + py;
		}
	if (od->actor) clutter_actor_set_position(od->actor, px, py);
}

static void rc_overlay_update_positions(RendererClutter *rc)
{
	GList *work;

	work = rc->overlay_list;
	while (work)
		{
		OverlayData *od = work->data;
		work = work->next;

		rc_overlay_update_position(rc, od);
		}
}

static void rc_overlay_free_all(RendererClutter *rc)
{
	GList *work;

	work = rc->overlay_list;
	while (work)
		{
		OverlayData *od = work->data;
		work = work->next;

		rc_overlay_free(rc, od);
		}
}


static void renderer_overlay_draw(void *renderer, gint x, gint y, gint w, gint h)
{
}

static gint renderer_overlay_add(void *renderer, GdkPixbuf *pixbuf, gint x, gint y, OverlayRendererFlags flags)
{
	RendererClutter *rc = (RendererClutter *)renderer;
	PixbufRenderer *pr = rc->pr;
	OverlayData *od;
	gint id;

	g_return_val_if_fail(IS_PIXBUF_RENDERER(pr), -1);
	g_return_val_if_fail(pixbuf != NULL, -1);

	id = 1;
	while (rc_overlay_find(rc, id)) id++;

	od = g_new0(OverlayData, 1);
	od->id = id;
	od->pixbuf = pixbuf;
	g_object_ref(G_OBJECT(od->pixbuf));
	od->x = x;
	od->y = y;
	od->flags = flags;
	
	od->actor = gtk_clutter_texture_new();
	g_signal_connect (od->actor, "destroy", G_CALLBACK(rc_overlay_actor_destroy_cb), od);
	
	gtk_clutter_texture_set_from_pixbuf(GTK_CLUTTER_TEXTURE (od->actor), pixbuf, NULL);
  	clutter_container_add_actor(CLUTTER_CONTAINER(rc->group), od->actor);

	rc->overlay_list = g_list_append(rc->overlay_list, od);
	rc_overlay_update_position(rc, od);

	return od->id;
}

static void renderer_overlay_set(void *renderer, gint id, GdkPixbuf *pixbuf, gint x, gint y)
{
	RendererClutter *rc = (RendererClutter *)renderer;
	PixbufRenderer *pr = rc->pr;
	OverlayData *od;

	g_return_if_fail(IS_PIXBUF_RENDERER(pr));

	od = rc_overlay_find(rc, id);
	if (!od) return;

	if (pixbuf)
		{
		g_object_ref(G_OBJECT(pixbuf));
		g_object_unref(G_OBJECT(od->pixbuf));
		od->pixbuf = pixbuf;

		od->x = x;
		od->y = y;

		if (od->actor) gtk_clutter_texture_set_from_pixbuf(GTK_CLUTTER_TEXTURE(od->actor), pixbuf, NULL);
		rc_overlay_update_position(rc, od);
		}
	else
		{
		rc_overlay_free(rc, od);
		}
}

static gboolean renderer_overlay_get(void *renderer, gint id, GdkPixbuf **pixbuf, gint *x, gint *y)
{
	RendererClutter *rc = (RendererClutter *)renderer;

	PixbufRenderer *pr = rc->pr;
	OverlayData *od;

	g_return_val_if_fail(IS_PIXBUF_RENDERER(pr), FALSE);

	od = rc_overlay_find(rc, id);
	if (!od) return FALSE;

	if (pixbuf) *pixbuf = od->pixbuf;
	if (x) *x = od->x;
	if (y) *y = od->y;

	return TRUE;
}


static void renderer_update_sizes(void *renderer)
{
	RendererClutter *rc = (RendererClutter *)renderer;
	ClutterColor stage_color = { 0x00, 0x00, 0x00, 0xff }; 

	rc->stereo_off_x = 0;
	rc->stereo_off_y = 0;
	
	if (rc->stereo_mode & PR_STEREO_RIGHT) 
		{
		if (rc->stereo_mode & PR_STEREO_HORIZ) 
			{
			rc->stereo_off_x = rc->pr->viewport_width;
			}
		else if (rc->stereo_mode & PR_STEREO_VERT) 
			{
			rc->stereo_off_y = rc->pr->viewport_height;
			}
		else if (rc->stereo_mode & PR_STEREO_FIXED) 
			{
			rc->stereo_off_x = rc->pr->stereo_fixed_x_right;
			rc->stereo_off_y = rc->pr->stereo_fixed_y_right;
			}
		}
	else
		{
		if (rc->stereo_mode & PR_STEREO_FIXED) 
			{
			rc->stereo_off_x = rc->pr->stereo_fixed_x_left;
			rc->stereo_off_y = rc->pr->stereo_fixed_y_left;
			}
		}
        DEBUG_1("update size: %p  %d %d   %d %d", rc, rc->stereo_off_x, rc->stereo_off_y, rc->pr->viewport_width, rc->pr->viewport_height);

	printf("renderer_update_sizes  scale %d %d\n", rc->pr->width, rc->pr->height);

        clutter_stage_set_color(CLUTTER_STAGE(rc->stage), &stage_color);


	clutter_actor_set_size(rc->group, rc->pr->viewport_width, rc->pr->viewport_height);
	clutter_actor_set_position(rc->group, rc->stereo_off_x, rc->stereo_off_y);
	
	clutter_actor_set_rotation(CLUTTER_ACTOR(rc->group),
						CLUTTER_Y_AXIS,
						(rc->stereo_mode & PR_STEREO_MIRROR) ? 180 : 0, 
						rc->pr->viewport_width / 2.0, 0, 0);

	clutter_actor_set_rotation(CLUTTER_ACTOR(rc->group),
						CLUTTER_X_AXIS,
						(rc->stereo_mode & PR_STEREO_FLIP) ? 180 : 0,
						0, rc->pr->viewport_height / 2.0, 0);

	rc_sync_actor(rc);
	rc_overlay_update_positions(rc);
}

static void renderer_scroll(void *renderer, gint x_off, gint y_off)
{
	printf("renderer_scroll\n");
	RendererClutter *rc = (RendererClutter *)renderer;
	PixbufRenderer *pr = rc->pr;

	rc_sync_actor(rc);
}

static void renderer_stereo_set(void *renderer, gint stereo_mode)
{
	RendererClutter *rc = (RendererClutter *)renderer;

	rc->stereo_mode = stereo_mode;
}

static void renderer_free(void *renderer)
{
	RendererClutter *rc = (RendererClutter *)renderer;
	GtkWidget *widget = gtk_bin_get_child(GTK_BIN(rc->pr));

	renderer_remove_pending_updates(rc);

	rc_overlay_free_all(rc);
	
	if (widget)
		{
		/* widget still exists */
		clutter_actor_destroy(rc->group);
		if (clutter_group_get_n_children(CLUTTER_GROUP(rc->stage)) == 0)
			{
			printf("destroy %p\n", rc->widget);
			/* this was the last user */
			gtk_widget_destroy(rc->widget);
			}
		else
			{
			printf("keep %p\n", rc->widget);
			g_object_unref(G_OBJECT(rc->widget));
			}
		}
	g_free(rc);
}

RendererFuncs *renderer_clutter_new(PixbufRenderer *pr)
{
	RendererClutter *rc = g_new0(RendererClutter, 1);
	
	rc->pr = pr;
	
	rc->f.area_changed = renderer_area_changed;
	rc->f.update_pixbuf = renderer_update_pixbuf;
	rc->f.free = renderer_free;
	rc->f.update_zoom = renderer_update_zoom;
	rc->f.invalidate_region = renderer_invalidate_region;
	rc->f.scroll = renderer_scroll;
	rc->f.update_sizes = renderer_update_sizes;


	rc->f.overlay_add = renderer_overlay_add;
	rc->f.overlay_set = renderer_overlay_set;
	rc->f.overlay_get = renderer_overlay_get;
	rc->f.overlay_draw = renderer_overlay_draw;

	rc->f.stereo_set = renderer_stereo_set;
	
	
	rc->stereo_mode = 0;
	rc->stereo_off_x = 0;
	rc->stereo_off_y = 0;

	rc->idle_update = 0;
	rc->pending_updates = NULL;

  	rc->widget = gtk_bin_get_child(GTK_BIN(rc->pr));
  	
  	if (rc->widget)
  		{
  		if (!GTK_CLUTTER_IS_EMBED(rc->widget))
  			{
  			g_free(rc);
  			DEBUG_0("pixbuf renderer has a child of other type than gtk_clutter_embed");
  			return NULL;
  			}
  		}
  	else 
  		{
  		rc->widget = gtk_clutter_embed_new();
		gtk_container_add(GTK_CONTAINER(rc->pr), rc->widget);
  		}
  		
	gtk_event_box_set_above_child (GTK_EVENT_BOX(rc->pr), TRUE);
        rc->stage = gtk_clutter_embed_get_stage (GTK_CLUTTER_EMBED (rc->widget));
        
        rc->group = clutter_group_new();
  	clutter_container_add_actor(CLUTTER_CONTAINER(rc->stage), rc->group);
  	clutter_actor_set_clip_to_allocation(CLUTTER_ACTOR(rc->group), TRUE);
  
  	rc->texture = clutter_texture_new ();
  	clutter_container_add_actor(CLUTTER_CONTAINER(rc->group), rc->texture);
  	rc_set_shader(clutter_texture_get_cogl_material(rc->texture));
  	g_object_ref(G_OBJECT(rc->widget));
  
	gtk_widget_show(rc->widget);
	return (RendererFuncs *) rc;
}

#endif 
/* vim: set shiftwidth=8 softtabstop=0 cindent cinoptions={1s: */
