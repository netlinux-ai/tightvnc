/*
 *  Copyright (C) 1999 AT&T Laboratories Cambridge.  All Rights Reserved.
 *
 *  This is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This software is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this software; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 *  USA.
 */

/*
 * desktop.c - functions to deal with "desktop" window.
 */

#include <vncviewer.h>
#include <X11/Xaw/Viewport.h>
#include <X11/Xmu/Converters.h>
#include <X11/XKBlib.h>
#ifdef MITSHM
#include <X11/extensions/XShm.h>
#endif

GC gc;
GC srcGC, dstGC; /* used for debugging copyrect */
Window desktopWin;
Cursor dotCursor;
Widget form, viewport, desktop;

static Bool modifierPressed[256];

static XImage *image = NULL;
static XImage *scaledImage = NULL;

static void ScaleRect(int sx, int sy, int sw, int sh);
static void ScaleAndPutImage(int x, int y, int w, int h);
static void FillImageRect(unsigned long pixel, int x, int y, int w, int h);
static Cursor CreateDotCursor();
static void CopyBGR233ToScreen(CARD8 *buf, int x, int y, int width,int height);
static void HandleBasicDesktopEvent(Widget w, XtPointer ptr, XEvent *ev,
				    Boolean *cont);

static XtResource desktopBackingStoreResources[] = {
  {
    XtNbackingStore, XtCBackingStore, XtRBackingStore, sizeof(int), 0,
    XtRImmediate, (XtPointer) Always,
  },
};


/*
 * DesktopInitBeforeRealization creates the "desktop" widget and the viewport
 * which controls it.
 */

void
DesktopInitBeforeRealization()
{
  int i;
  int scaledWidth, scaledHeight;

  if (appData.scalePercent <= 0) {
    fprintf(stderr, "Invalid scale value: %d (must be > 0)\n",
	    appData.scalePercent);
    exit(1);
  }

  scaledWidth = SCALE_X(si.framebufferWidth);
  scaledHeight = SCALE_Y(si.framebufferHeight);

  form = XtVaCreateManagedWidget("form", formWidgetClass, toplevel,
				 XtNborderWidth, 0,
				 XtNdefaultDistance, 0, NULL);

  viewport = XtVaCreateManagedWidget("viewport", viewportWidgetClass, form,
				     XtNborderWidth, 0,
				     NULL);

  desktop = XtVaCreateManagedWidget("desktop", coreWidgetClass, viewport,
				    XtNborderWidth, 0,
				    NULL);

  XtVaSetValues(desktop, XtNwidth, scaledWidth,
		XtNheight, scaledHeight, NULL);

  XtAddEventHandler(desktop, LeaveWindowMask|ExposureMask,
		    True, HandleBasicDesktopEvent, NULL);

  for (i = 0; i < 256; i++)
    modifierPressed[i] = False;

  image = NULL;
  scaledImage = NULL;

#ifdef MITSHM
  /* Only use SHM when not scaling - with scaling we need separate images */
  if (appData.useShm && appData.scalePercent == 100) {
    image = CreateShmImage();
    if (!image)
      appData.useShm = False;
  } else {
    appData.useShm = False;
  }
#endif

  if (!image) {
    image = XCreateImage(dpy, vis, visdepth, ZPixmap, 0, NULL,
			 si.framebufferWidth, si.framebufferHeight,
			 BitmapPad(dpy), 0);

    image->data = malloc(image->bytes_per_line * image->height);
    if (!image->data) {
      fprintf(stderr,"malloc failed\n");
      exit(1);
    }
  }

  /* Create scaled image if scaling is active */
  if (appData.scalePercent != 100) {
    scaledImage = XCreateImage(dpy, vis, visdepth, ZPixmap, 0, NULL,
			       scaledWidth, scaledHeight,
			       BitmapPad(dpy), 0);

    scaledImage->data = malloc(scaledImage->bytes_per_line *
			       scaledImage->height);
    if (!scaledImage->data) {
      fprintf(stderr,"malloc failed\n");
      exit(1);
    }
  }
}


/*
 * DesktopInitAfterRealization does things which require the X windows to
 * exist.  It creates some GCs and sets the dot cursor.
 */

void
DesktopInitAfterRealization()
{
  XGCValues gcv;
  XSetWindowAttributes attr;
  unsigned long valuemask;

  desktopWin = XtWindow(desktop);

  gc = XCreateGC(dpy,desktopWin,0,NULL);

  gcv.function = GXxor;
  gcv.foreground = 0x0f0f0f0f;
  srcGC = XCreateGC(dpy,desktopWin,GCFunction|GCForeground,&gcv);
  gcv.foreground = 0xf0f0f0f0;
  dstGC = XCreateGC(dpy,desktopWin,GCFunction|GCForeground,&gcv);

  XtAddConverter(XtRString, XtRBackingStore, XmuCvtStringToBackingStore,
		 NULL, 0);

  XtVaGetApplicationResources(desktop, (XtPointer)&attr.backing_store,
			      desktopBackingStoreResources, 1, NULL);
  valuemask = CWBackingStore;

  if (!appData.useX11Cursor) {
    dotCursor = CreateDotCursor();
    attr.cursor = dotCursor;    
    valuemask |= CWCursor;
  }

  XChangeWindowAttributes(dpy, desktopWin, valuemask, &attr);
}


/*
 * HandleBasicDesktopEvent - deal with expose and leave events.
 */

static void
HandleBasicDesktopEvent(Widget w, XtPointer ptr, XEvent *ev, Boolean *cont)
{
  int i;

  switch (ev->type) {

  case Expose:
  case GraphicsExpose:
    /* sometimes due to scrollbars being added/removed we get an expose outside
       the actual desktop area.  Make sure we don't pass it on to the RFB
       server. */
    {
      int scaledFBW = SCALE_X(si.framebufferWidth);
      int scaledFBH = SCALE_Y(si.framebufferHeight);
      int ex = ev->xexpose.x;
      int ey = ev->xexpose.y;
      int ew = ev->xexpose.width;
      int eh = ev->xexpose.height;
      int sx, sy, sx2, sy2;

      /* Clamp to scaled framebuffer bounds */
      if (ex + ew > scaledFBW) {
	ew = scaledFBW - ex;
	if (ew <= 0) break;
      }
      if (ey + eh > scaledFBH) {
	eh = scaledFBH - ey;
	if (eh <= 0) break;
      }

      /* Convert display coords to server coords */
      sx = SERVER_X(ex);
      sy = SERVER_Y(ey);
      sx2 = SERVER_X(ex + ew) + 1;
      sy2 = SERVER_Y(ey + eh) + 1;
      if (sx2 > si.framebufferWidth) sx2 = si.framebufferWidth;
      if (sy2 > si.framebufferHeight) sy2 = si.framebufferHeight;

      SendFramebufferUpdateRequest(sx, sy, sx2 - sx, sy2 - sy, False);
    }
    break;

  case LeaveNotify:
    for (i = 0; i < 256; i++) {
      if (modifierPressed[i]) {
	SendKeyEvent(XkbKeycodeToKeysym(dpy, i, 0, 0), False);
	modifierPressed[i] = False;
      }
    }
    break;
  }
}


/*
 * SendRFBEvent is an action which sends an RFB event.  It can be used in two
 * ways.  Without any parameters it simply sends an RFB event corresponding to
 * the X event which caused it to be called.  With parameters, it generates a
 * "fake" RFB event based on those parameters.  The first parameter is the
 * event type, either "fbupdate", "ptr", "keydown", "keyup" or "key"
 * (down&up).  The "fbupdate" event requests full framebuffer update. For a
 * "key" event the second parameter is simply a keysym string as understood by
 * XStringToKeysym().  For a "ptr" event, the following three parameters are
 * just X, Y and the button mask (0 for all up, 1 for button1 down, 2 for
 * button2 down, 3 for both, etc).
 */

void
SendRFBEvent(Widget w, XEvent *ev, String *params, Cardinal *num_params)
{
  KeySym ks;
  char keyname[256];
  int buttonMask, x, y;

  if (appData.fullScreen && ev->type == MotionNotify) {
    if (BumpScroll(ev))
      return;
  }

  if (appData.viewOnly) return;

  if (*num_params != 0) {
    if (strncasecmp(params[0],"key",3) == 0) {
      if (*num_params != 2) {
	fprintf(stderr,
		"Invalid params: SendRFBEvent(key|keydown|keyup,<keysym>)\n");
	return;
      }
      ks = XStringToKeysym(params[1]);
      if (ks == NoSymbol) {
	fprintf(stderr,"Invalid keysym '%s' passed to SendRFBEvent\n",
		params[1]);
	return;
      }
      if (strcasecmp(params[0],"keydown") == 0) {
	SendKeyEvent(ks, 1);
      } else if (strcasecmp(params[0],"keyup") == 0) {
	SendKeyEvent(ks, 0);
      } else if (strcasecmp(params[0],"key") == 0) {
	SendKeyEvent(ks, 1);
	SendKeyEvent(ks, 0);
      } else {
	fprintf(stderr,"Invalid event '%s' passed to SendRFBEvent\n",
		params[0]);
	return;
      }
    } else if (strcasecmp(params[0],"fbupdate") == 0) {
      if (*num_params != 1) {
	fprintf(stderr, "Invalid params: SendRFBEvent(fbupdate)\n");
	return;
      }
      SendFramebufferUpdateRequest(0, 0, si.framebufferWidth,
				   si.framebufferHeight, False);
    } else if (strcasecmp(params[0],"ptr") == 0) {
      if (*num_params == 4) {
	x = atoi(params[1]);
	y = atoi(params[2]);
	buttonMask = atoi(params[3]);
	SendPointerEvent(x, y, buttonMask);
      } else if (*num_params == 2) {
	switch (ev->type) {
	case ButtonPress:
	case ButtonRelease:
	  x = SERVER_X(ev->xbutton.x);
	  y = SERVER_Y(ev->xbutton.y);
	  break;
	case KeyPress:
	case KeyRelease:
	  x = SERVER_X(ev->xkey.x);
	  y = SERVER_Y(ev->xkey.y);
	  break;
	default:
	  fprintf(stderr,
		  "Invalid event caused SendRFBEvent(ptr,<buttonMask>)\n");
	  return;
	}
	buttonMask = atoi(params[1]);
	SendPointerEvent(x, y, buttonMask);
      } else {
	fprintf(stderr,
		"Invalid params: SendRFBEvent(ptr,<x>,<y>,<buttonMask>)\n"
		"             or SendRFBEvent(ptr,<buttonMask>)\n");
	return;
      }

    } else {
      fprintf(stderr,"Invalid event '%s' passed to SendRFBEvent\n", params[0]);
    }
    return;
  }

  switch (ev->type) {

  case MotionNotify:
    while (XCheckTypedWindowEvent(dpy, desktopWin, MotionNotify, ev))
      ;	/* discard all queued motion notify events */

    SendPointerEvent(SERVER_X(ev->xmotion.x), SERVER_Y(ev->xmotion.y),
		     (ev->xmotion.state & 0x1f00) >> 8);
    return;

  case ButtonPress:
    SendPointerEvent(SERVER_X(ev->xbutton.x), SERVER_Y(ev->xbutton.y),
		     (((ev->xbutton.state & 0x1f00) >> 8) |
		      (1 << (ev->xbutton.button - 1))));
    return;

  case ButtonRelease:
    SendPointerEvent(SERVER_X(ev->xbutton.x), SERVER_Y(ev->xbutton.y),
		     (((ev->xbutton.state & 0x1f00) >> 8) &
		      ~(1 << (ev->xbutton.button - 1))));
    return;

  case KeyPress:
  case KeyRelease:
    XLookupString(&ev->xkey, keyname, 256, &ks, NULL);

    if (IsModifierKey(ks)) {
      ks = XkbKeycodeToKeysym(dpy, ev->xkey.keycode, 0, 0);
      modifierPressed[ev->xkey.keycode] = (ev->type == KeyPress);
    }

    SendKeyEvent(ks, (ev->type == KeyPress));
    return;

  default:
    fprintf(stderr,"Invalid event passed to SendRFBEvent\n");
  }
}


/*
 * CreateDotCursor.
 */

static Cursor
CreateDotCursor()
{
  Cursor cursor;
  Pixmap src, msk;
  static char srcBits[] = { 0, 14,14,14, 0 };
  static char mskBits[] = { 14,31,31,31,14 };
  XColor fg, bg;

  src = XCreateBitmapFromData(dpy, DefaultRootWindow(dpy), srcBits, 5, 5);
  msk = XCreateBitmapFromData(dpy, DefaultRootWindow(dpy), mskBits, 5, 5);
  XAllocNamedColor(dpy, DefaultColormap(dpy,DefaultScreen(dpy)), "black",
		   &fg, &fg);
  XAllocNamedColor(dpy, DefaultColormap(dpy,DefaultScreen(dpy)), "white",
		   &bg, &bg);
  cursor = XCreatePixmapCursor(dpy, src, msk, &fg, &bg, 2, 2);
  XFreePixmap(dpy, src);
  XFreePixmap(dpy, msk);

  return cursor;
}


/*
 * CopyDataToScreen.
 */

void
CopyDataToScreen(char *buf, int x, int y, int width, int height)
{
  if (appData.rawDelay != 0) {
    XFillRectangle(dpy, desktopWin, gc,
		   SCALE_X(x), SCALE_Y(y),
		   SCALE_X(x + width) - SCALE_X(x),
		   SCALE_Y(y + height) - SCALE_Y(y));

    XSync(dpy,False);

    usleep(appData.rawDelay * 1000);
  }

  if (!appData.useBGR233) {
    int h;
    int widthInBytes = width * myFormat.bitsPerPixel / 8;
    int scrWidthInBytes = si.framebufferWidth * myFormat.bitsPerPixel / 8;

    char *scr = (image->data + y * scrWidthInBytes
		 + x * myFormat.bitsPerPixel / 8);

    for (h = 0; h < height; h++) {
      memcpy(scr, buf, widthInBytes);
      buf += widthInBytes;
      scr += scrWidthInBytes;
    }
  } else {
    CopyBGR233ToScreen((CARD8 *)buf, x, y, width, height);
  }

  ScaleAndPutImage(x, y, width, height);
}


/*
 * CopyBGR233ToScreen.
 */

static void
CopyBGR233ToScreen(CARD8 *buf, int x, int y, int width, int height)
{
  int p, q;
  int xoff = 7 - (x & 7);
  int xcur;
  int fbwb = si.framebufferWidth / 8;
  CARD8 *scr1 = ((CARD8 *)image->data) + y * fbwb + x / 8;
  CARD8 *scrt;
  CARD8 *scr8 = ((CARD8 *)image->data) + y * si.framebufferWidth + x;
  CARD16 *scr16 = ((CARD16 *)image->data) + y * si.framebufferWidth + x;
  CARD32 *scr32 = ((CARD32 *)image->data) + y * si.framebufferWidth + x;

  switch (visbpp) {

    /* thanks to Chris Hooper for single bpp support */

  case 1:
    for (q = 0; q < height; q++) {
      xcur = xoff;
      scrt = scr1;
      for (p = 0; p < width; p++) {
	*scrt = ((*scrt & ~(1 << xcur))
		 | (BGR233ToPixel[*(buf++)] << xcur));

	if (xcur-- == 0) {
	  xcur = 7;
	  scrt++;
	}
      }
      scr1 += fbwb;
    }
    break;

  case 8:
    for (q = 0; q < height; q++) {
      for (p = 0; p < width; p++) {
	*(scr8++) = BGR233ToPixel[*(buf++)];
      }
      scr8 += si.framebufferWidth - width;
    }
    break;

  case 16:
    for (q = 0; q < height; q++) {
      for (p = 0; p < width; p++) {
	*(scr16++) = BGR233ToPixel[*(buf++)];
      }
      scr16 += si.framebufferWidth - width;
    }
    break;

  case 32:
    for (q = 0; q < height; q++) {
      for (p = 0; p < width; p++) {
	*(scr32++) = BGR233ToPixel[*(buf++)];
      }
      scr32 += si.framebufferWidth - width;
    }
    break;
  }
}


/*
 * ScaleRect - scale a rectangle from image to scaledImage.
 * Uses bilinear interpolation for true-color 16/32bpp modes,
 * nearest-neighbor as fallback for 8bpp or indexed color.
 * sx,sy,sw,sh are in server (full-res) coordinates.
 */

static void
ScaleRect(int sx, int sy, int sw, int sh)
{
  int scale = appData.scalePercent;
  int bytesPerPixel = image->bits_per_pixel / 8;
  int dx, dy, dw, dh;
  int i, j;

  dx = sx * scale / 100;
  dy = sy * scale / 100;
  dw = (sx + sw) * scale / 100 - dx;
  dh = (sy + sh) * scale / 100 - dy;

  if (dw <= 0 || dh <= 0) return;

  /* Bilinear interpolation for true-color 16/32bpp */
  if (myFormat.trueColour && !appData.useBGR233 && bytesPerPixel >= 2) {
    unsigned int rMask = myFormat.redMax;
    unsigned int gMask = myFormat.greenMax;
    unsigned int bMask = myFormat.blueMax;
    int rShift = myFormat.redShift;
    int gShift = myFormat.greenShift;
    int bShift = myFormat.blueShift;
    int srcBPL = image->bytes_per_line;
    int dstBPL = scaledImage->bytes_per_line;
    int maxSrcX = si.framebufferWidth - 1;
    int maxSrcY = si.framebufferHeight - 1;

    for (j = 0; j < dh; j++) {
      /* Fixed-point source Y (8 fractional bits) */
      int sy_fp = j * 25600 / scale + sy * 256;
      int y0 = sy_fp >> 8;
      int fy = sy_fp & 0xFF;
      int y1 = y0 < maxSrcY ? y0 + 1 : y0;
      char *dst_row = scaledImage->data + (dy + j) * dstBPL;
      char *srow0 = image->data + y0 * srcBPL;
      char *srow1 = image->data + y1 * srcBPL;

      for (i = 0; i < dw; i++) {
	/* Fixed-point source X (8 fractional bits) */
	int sx_fp = i * 25600 / scale + sx * 256;
	int x0 = sx_fp >> 8;
	int fx = sx_fp & 0xFF;
	int x1 = x0 < maxSrcX ? x0 + 1 : x0;
	unsigned long p00, p10, p01, p11;
	unsigned int w00, w10, w01, w11;
	unsigned int r, g, b;
	unsigned long pixel;

	/* Read 4 source pixels */
	if (bytesPerPixel == 4) {
	  p00 = *(CARD32 *)(srow0 + x0 * 4);
	  p10 = *(CARD32 *)(srow0 + x1 * 4);
	  p01 = *(CARD32 *)(srow1 + x0 * 4);
	  p11 = *(CARD32 *)(srow1 + x1 * 4);
	} else {
	  p00 = *(CARD16 *)(srow0 + x0 * 2);
	  p10 = *(CARD16 *)(srow0 + x1 * 2);
	  p01 = *(CARD16 *)(srow1 + x0 * 2);
	  p11 = *(CARD16 *)(srow1 + x1 * 2);
	}

	/* Bilinear weights (sum = 65536) */
	w00 = (256 - fx) * (256 - fy);
	w10 = fx * (256 - fy);
	w01 = (256 - fx) * fy;
	w11 = fx * fy;

	/* Blend each channel separately */
	r = (w00 * ((p00 >> rShift) & rMask) +
	     w10 * ((p10 >> rShift) & rMask) +
	     w01 * ((p01 >> rShift) & rMask) +
	     w11 * ((p11 >> rShift) & rMask)) >> 16;
	g = (w00 * ((p00 >> gShift) & gMask) +
	     w10 * ((p10 >> gShift) & gMask) +
	     w01 * ((p01 >> gShift) & gMask) +
	     w11 * ((p11 >> gShift) & gMask)) >> 16;
	b = (w00 * ((p00 >> bShift) & bMask) +
	     w10 * ((p10 >> bShift) & bMask) +
	     w01 * ((p01 >> bShift) & bMask) +
	     w11 * ((p11 >> bShift) & bMask)) >> 16;

	pixel = (r << rShift) | (g << gShift) | (b << bShift);

	if (bytesPerPixel == 4)
	  *(CARD32 *)(dst_row + (dx + i) * 4) = (CARD32)pixel;
	else
	  *(CARD16 *)(dst_row + (dx + i) * 2) = (CARD16)pixel;
      }
    }
    return;
  }

  /* Fallback: nearest-neighbor for 8bpp or indexed color */
  for (j = 0; j < dh; j++) {
    int src_y = j * 100 / scale + sy;
    char *dst_row, *src_row;
    if (src_y >= si.framebufferHeight)
      src_y = si.framebufferHeight - 1;
    dst_row = scaledImage->data + (dy + j) * scaledImage->bytes_per_line;
    src_row = image->data + src_y * image->bytes_per_line;

    for (i = 0; i < dw; i++) {
      int src_x = i * 100 / scale + sx;
      if (src_x >= si.framebufferWidth)
	src_x = si.framebufferWidth - 1;

      memcpy(dst_row + (dx + i) * bytesPerPixel,
	     src_row + src_x * bytesPerPixel,
	     bytesPerPixel);
    }
  }
}


/*
 * ScaleAndPutImage - scale a server-coordinate rect and put it on screen.
 * Handles both scaled and non-scaled modes.
 */

static void
ScaleAndPutImage(int x, int y, int w, int h)
{
  if (appData.scalePercent == 100 || !scaledImage) {
    /* No scaling - put directly from image */
#ifdef MITSHM
    if (appData.useShm) {
      XShmPutImage(dpy, desktopWin, gc, image, x, y, x, y, w, h, False);
      return;
    }
#endif
    XPutImage(dpy, desktopWin, gc, image, x, y, x, y, w, h);
    return;
  }

  /* Scale the affected region from image to scaledImage */
  ScaleRect(x, y, w, h);

  {
    int scale = appData.scalePercent;
    int dx = x * scale / 100;
    int dy = y * scale / 100;
    int dw = (x + w) * scale / 100 - dx;
    int dh = (y + h) * scale / 100 - dy;

    if (dw <= 0 || dh <= 0) return;

    XPutImage(dpy, desktopWin, gc, scaledImage, dx, dy, dx, dy, dw, dh);
  }
}


/*
 * FillImageRect - fill a rectangle in the full-res image buffer with a pixel.
 */

static void
FillImageRect(unsigned long pixel, int x, int y, int w, int h)
{
  int bytesPerPixel = image->bits_per_pixel / 8;
  int row, col;
  char *ptr;

  for (row = y; row < y + h; row++) {
    ptr = image->data + row * image->bytes_per_line + x * bytesPerPixel;
    for (col = 0; col < w; col++) {
      switch (bytesPerPixel) {
      case 1: *(CARD8 *)ptr = (CARD8)pixel; break;
      case 2: *(CARD16 *)ptr = (CARD16)pixel; break;
      case 4: *(CARD32 *)ptr = (CARD32)pixel; break;
      }
      ptr += bytesPerPixel;
    }
  }
}


/*
 * FillRectOnScreen - fill a rectangle, updating both image and display.
 * When not scaling, uses XFillRectangle directly for performance.
 * Used by encoding handlers (RRE, CoRRE, Hextile, Tight).
 */

void
FillRectOnScreen(unsigned long pixel, int x, int y, int w, int h)
{
  if (appData.scalePercent != 100) {
    FillImageRect(pixel, x, y, w, h);
    ScaleAndPutImage(x, y, w, h);
  } else {
    XGCValues gcv;
    gcv.foreground = pixel;
    XChangeGC(dpy, gc, GCForeground, &gcv);
    XFillRectangle(dpy, desktopWin, gc, x, y, w, h);
  }
}


/*
 * CopyRectOnScreen - handle CopyRect encoding with scaling support.
 * Copies within image buffer, then scales and displays the destination.
 * When not scaling, uses XCopyArea directly for performance.
 */

void
CopyRectOnScreen(int srcX, int srcY, int dstX, int dstY, int w, int h)
{
  if (appData.scalePercent == 100) {
    XCopyArea(dpy, desktopWin, desktopWin, gc, srcX, srcY, w, h, dstX, dstY);
    return;
  }

  /* Copy within the full-res image buffer */
  {
    int bytesPerPixel = myFormat.bitsPerPixel / 8;
    int bytesPerLine = image->bytes_per_line;
    int row;
    int rowBytes = w * bytesPerPixel;

    /* Handle overlapping copies correctly */
    if (dstY < srcY || (dstY == srcY && dstX < srcX)) {
      for (row = 0; row < h; row++) {
	memmove(image->data + (dstY + row) * bytesPerLine + dstX * bytesPerPixel,
		image->data + (srcY + row) * bytesPerLine + srcX * bytesPerPixel,
		rowBytes);
      }
    } else {
      for (row = h - 1; row >= 0; row--) {
	memmove(image->data + (dstY + row) * bytesPerLine + dstX * bytesPerPixel,
		image->data + (srcY + row) * bytesPerLine + srcX * bytesPerPixel,
		rowBytes);
      }
    }
  }

  /* Scale and display the destination region */
  ScaleAndPutImage(dstX, dstY, w, h);
}
