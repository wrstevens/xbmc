/*
 *      Copyright (C) 2012 linaro
 *      http://www.linaro.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "threads/SystemClock.h"
#include "system.h"

#include "utils/LangCodeExpander.h"
#include "guilib/LocalizeStrings.h"

#include "utils/URIUtils.h"
#include "GUIInfoManager.h"
#include "guilib/GUIWindowManager.h"
#include "Application.h"
#include "filesystem/File.h"
#include "pictures/Picture.h"
#include "DllSwScale.h"
#ifdef HAS_VIDEO_PLAYBACK
#include "cores/VideoRenderers/RenderManager.h"
#endif
#ifdef HAS_PERFORMANCE_SAMPLE
#include "xbmc/utils/PerformanceSample.h"
#else
#define MEASURE_FUNCTION
#endif
#include "settings/AdvancedSettings.h"
#include "FileItem.h"
#include "settings/GUISettings.h"
#include "GUIUserMessages.h"
#include "settings/Settings.h"
#include "utils/log.h"
#include "utils/TimeUtils.h"
#include "utils/StreamDetails.h"
#include "utils/StreamUtils.h"
#include "utils/Variant.h"
#include "storage/MediaManager.h"
#include "dialogs/GUIDialogBusy.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "utils/StringUtils.h"
#include "Util.h"
#include "DVDInputStreams/DVDInputStream.h"
#include "DVDInputStreams/DVDInputStreamFile.h"
#include "DVDInputStreams/DVDFactoryInputStream.h"
#include "DVDInputStreams/DVDInputStreamNavigator.h"
#include "DVDInputStreams/DVDInputStreamTV.h"
#include <gst/app/gstappsink.h>
#include <gst/dmabuf/dmabuf.h>
#include <gst/video/video-crop.h>
#include <gst/base/gstbasesrc.h>

#include "../dvdplayer/DVDCodecs/Video/DVDVideoCodec.h"


#include "GstPlayer.h"


/******************************************************************************
 * stuff related to eglImage..
 */
#include "windowing/WindowingFactory.h"
#include <EGL/egl.h>

static bool has_ti_raw_video = false;
//static int has_dmabuf; // ???
extern "C" {
/******************************************************************/
G_BEGIN_DECLS

#define GST_TYPE_XBMC_SRC \
  (gst_xbmc_src_get_type())
#define GST_XBMC_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_XBMC_SRC,GstXbmcSrc))
#define GST_XBMC_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_XBMC_SRC,GstXbmcSrcClass))
#define GST_IS_XBMC_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_XBMC_SRC))
#define GST_IS_XBMC_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_XBMC_SRC))
#define GST_XBMC_SRC_CAST(obj) ((GstXbmcSrc*) obj)

typedef struct _GstXbmcSrc GstXbmcSrc;
typedef struct _GstXbmcSrcClass GstXbmcSrcClass;

/**
 * GstXbmcSrc:
 */
struct _GstXbmcSrc {
  GstBaseSrc element;

  /*< private >*/
  gchar *filename;			/* filename */
  gchar *uri;				/* caching the URI */
  guint64 read_position;		/* position of file */

  gboolean seekable;                    /* whether the file is seekable */
};

struct _GstXbmcSrcClass {
  GstBaseSrcClass parent_class;
};

GType gst_xbmc_src_get_type (void);

G_END_DECLS
/**********************************************************************/
static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

#ifndef O_BINARY
#define O_BINARY (0)
#endif

#define _(String) String
#define N_(String) String

#define USE_GSTXBMCSRC

static CDVDInputStream* m_pInputStream = NULL;

static gint64 xbmcsrc_seek(gint64 offset, int whence)
{
  return m_pInputStream->Seek(offset, whence);
}
static int xbmcsrc_read(guchar* buf, int size)
{
  return m_pInputStream->Read(buf,size);
}
static guint64 xbmcsrc_get_length()
{
  return m_pInputStream->GetLength();
}
static void xbmcsrc_close()
{
  return;
}
static gboolean xbmcsrc_open (const gchar * filename, int flags, int mode)
{
  return TRUE;
}

GST_DEBUG_CATEGORY_STATIC (gst_xbmc_src_debug);
#define GST_CAT_DEFAULT gst_xbmc_src_debug

enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  ARG_0,
  ARG_LOCATION
};

static void gst_xbmc_src_finalize (GObject * object);

static void gst_xbmc_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_xbmc_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_xbmc_src_start (GstBaseSrc * basesrc);
static gboolean gst_xbmc_src_stop (GstBaseSrc * basesrc);

static gboolean gst_xbmc_src_is_seekable (GstBaseSrc * src);
static gboolean gst_xbmc_src_get_size (GstBaseSrc * src, guint64 * size);
static GstFlowReturn gst_xbmc_src_create (GstBaseSrc * src, guint64 offset,
    guint length, GstBuffer ** buffer);
static gboolean gst_xbmc_src_query (GstBaseSrc * src, GstQuery * query);

static void gst_xbmc_src_uri_handler_init (gpointer g_iface,
    gpointer iface_data);

static void
_do_init (GType xbmcsrc_type)
{
  static const GInterfaceInfo urihandler_info = {
    gst_xbmc_src_uri_handler_init,
    NULL,
    NULL
  };

  g_type_add_interface_static (xbmcsrc_type, GST_TYPE_URI_HANDLER,
      &urihandler_info);
  GST_DEBUG_CATEGORY_INIT (gst_xbmc_src_debug, "xbmcsrc", 0, "xbmcsrc element");
}

GST_BOILERPLATE_FULL (GstXbmcSrc, gst_xbmc_src, GstBaseSrc, GST_TYPE_BASE_SRC,
    _do_init);

static void
gst_xbmc_src_base_init (gpointer g_class)
{
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (g_class);

  gst_element_class_set_details_simple (gstelement_class,
      "Xbmc Source",
      "Source/Xbmc",
      "Read from arbitrary point from xbmc inputstream",
      "xbmc.org");
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&srctemplate));
}

static void
gst_xbmc_src_class_init (GstXbmcSrcClass * klass)
{
  GObjectClass *gobject_class;
  GstBaseSrcClass *gstbasesrc_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gstbasesrc_class = GST_BASE_SRC_CLASS (klass);

  gobject_class->set_property = gst_xbmc_src_set_property;
  gobject_class->get_property = gst_xbmc_src_get_property;

  g_object_class_install_property (gobject_class, ARG_LOCATION,
      g_param_spec_string ("location", "File Location",
          "Location of the file to read", NULL,
          GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY)));

  gobject_class->finalize = gst_xbmc_src_finalize;

  gstbasesrc_class->start = GST_DEBUG_FUNCPTR (gst_xbmc_src_start);
  gstbasesrc_class->stop = GST_DEBUG_FUNCPTR (gst_xbmc_src_stop);
  gstbasesrc_class->is_seekable = GST_DEBUG_FUNCPTR (gst_xbmc_src_is_seekable);
  gstbasesrc_class->get_size = GST_DEBUG_FUNCPTR (gst_xbmc_src_get_size);
  gstbasesrc_class->create = GST_DEBUG_FUNCPTR (gst_xbmc_src_create);
  gstbasesrc_class->query = GST_DEBUG_FUNCPTR (gst_xbmc_src_query);
#if 0
  if (sizeof (off_t) < 8) {
    GST_LOG ("No large file support, sizeof (off_t) = %" G_GSIZE_FORMAT "!",
        sizeof (off_t));
  }
#endif
}

static void
gst_xbmc_src_init (GstXbmcSrc * src, GstXbmcSrcClass * g_class)
{
  src->filename = NULL;
  src->uri = NULL;
}

static void
gst_xbmc_src_finalize (GObject * object)
{
  GstXbmcSrc *src;

  src = GST_XBMC_SRC (object);

  xbmcsrc_close();

  g_free (src->filename);
  g_free (src->uri);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_xbmc_src_set_location (GstXbmcSrc * src, const gchar * location)
{
  GstState state;

  /* the element must be stopped in order to do this */
  GST_OBJECT_LOCK (src);
  state = GST_STATE (src);
  if (state != GST_STATE_READY && state != GST_STATE_NULL)
    goto wrong_state;
  GST_OBJECT_UNLOCK (src);

  g_free (src->filename);
  g_free (src->uri);

  /* clear the filename if we get a NULL (is that possible?) */
  if (location == NULL) {
    src->filename = NULL;
    src->uri = NULL;
  } else {
    /* we store the filename as received by the application. On Windoes this
     * should be UTF8 */
    src->filename = g_strdup (location);
    src->uri = gst_uri_construct ("xbmc", src->filename);
  }
  g_object_notify (G_OBJECT (src), "location");
  gst_uri_handler_new_uri (GST_URI_HANDLER (src), src->uri);

  return TRUE;

  /* ERROR */
wrong_state:
  {
    g_warning ("Changing the `location' property on xbmcsrc when it is open"
        "is not supported.");
    GST_OBJECT_UNLOCK (src);
    return FALSE;
  }
}

static void
gst_xbmc_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstXbmcSrc *src;

  g_return_if_fail (GST_IS_XBMC_SRC (object));

  src = GST_XBMC_SRC (object);

  switch (prop_id) {
    case ARG_LOCATION:
      gst_xbmc_src_set_location (src, g_value_get_string (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_xbmc_src_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstXbmcSrc *src;

  g_return_if_fail (GST_IS_XBMC_SRC (object));

  src = GST_XBMC_SRC (object);

  switch (prop_id) {
    case ARG_LOCATION:
      g_value_set_string (value, src->filename);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstFlowReturn
gst_xbmc_src_create_read (GstXbmcSrc * src, guint64 offset, guint length,
    GstBuffer ** buffer)
{
  int ret;
  GstBuffer *buf;
  int rest_bytes = 0;
  int timeout = 256;

  if (G_UNLIKELY (src->read_position != offset)) {
    off_t res;

    res = xbmcsrc_seek (offset, SEEK_SET);
    if (G_UNLIKELY (res < 0 || res != offset))
      goto seek_failed;

    src->read_position = offset;
  }

  buf = gst_buffer_try_new_and_alloc (length);
  if (G_UNLIKELY (buf == NULL && length > 0)) {
    GST_ERROR_OBJECT (src, "Failed to allocate %u bytes", length);
    return GST_FLOW_ERROR;
  }

  /* No need to read anything if length is 0 */
  if (length > 0) {
    GST_LOG_OBJECT (src, "Reading %d bytes at offset 0x%" G_GINT64_MODIFIER "x",
        length, offset);

    ret = 0;
    rest_bytes = length;

    do{

      int readed_size;
      readed_size = xbmcsrc_read (GST_BUFFER_DATA (buf)+ret, rest_bytes);

      if (G_UNLIKELY (readed_size < 0))
        goto could_not_read;

      /* seekable regular files should have given us what we expected */
      //if (G_UNLIKELY ((guint) ret < length && src->seekable))
      //  goto unexpected_eos;

      /* other files should eos if they read 0 and more was requested */
      if (G_UNLIKELY (readed_size == 0 && length > 0))
        goto eos;

      ret += readed_size;
      rest_bytes = length - ret;

    }while(rest_bytes && ((timeout--)>0));

    if(timeout<0)
      goto unexpected_eos;

    length = ret;
    GST_BUFFER_SIZE (buf) = length;
    GST_BUFFER_OFFSET (buf) = offset;
    GST_BUFFER_OFFSET_END (buf) = offset + length;

    src->read_position += length;
  }

  *buffer = buf;

  return GST_FLOW_OK;

  /* ERROR */
seek_failed:
  {
    GST_ELEMENT_ERROR (src, RESOURCE, READ, (NULL), GST_ERROR_SYSTEM);
    return GST_FLOW_ERROR;
  }
could_not_read:
  {
    GST_ELEMENT_ERROR (src, RESOURCE, READ, (NULL), GST_ERROR_SYSTEM);
    gst_buffer_unref (buf);
    return GST_FLOW_ERROR;
  }
unexpected_eos:
  {
    GST_ELEMENT_ERROR (src, RESOURCE, READ, (NULL),
        ("unexpected end of file."));
    gst_buffer_unref (buf);
    return GST_FLOW_ERROR;
  }
eos:
  {
    GST_DEBUG ("non-regular file hits EOS");
    gst_buffer_unref (buf);
    return GST_FLOW_UNEXPECTED;
  }
}

static GstFlowReturn
gst_xbmc_src_create (GstBaseSrc * basesrc, guint64 offset, guint length,
    GstBuffer ** buffer)
{
  GstXbmcSrc *src;
  GstFlowReturn ret;

  src = GST_XBMC_SRC_CAST (basesrc);

  ret = gst_xbmc_src_create_read (src, offset, length, buffer);

  return ret;
}

static gboolean
gst_xbmc_src_query (GstBaseSrc * basesrc, GstQuery * query)
{
  gboolean ret = FALSE;
  GstXbmcSrc *src = GST_XBMC_SRC (basesrc);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_URI:
      gst_query_set_uri (query, src->uri);
      ret = TRUE;
      break;
    default:
      ret = FALSE;
      break;
  }

  if (!ret)
    ret = GST_BASE_SRC_CLASS (parent_class)->query (basesrc, query);

  return ret;
}

static gboolean
gst_xbmc_src_is_seekable (GstBaseSrc * basesrc)
{
  GstXbmcSrc *src = GST_XBMC_SRC (basesrc);

  return src->seekable;
}

static gboolean
gst_xbmc_src_get_size (GstBaseSrc * basesrc, guint64 * size)
{
  GstXbmcSrc *src;

  src = GST_XBMC_SRC (basesrc);

  if (!src->seekable) {
    /* If it isn't seekable, we won't know the length (but fstat will still
     * succeed, and wrongly say our length is zero. */
    return FALSE;
  }

  *size = xbmcsrc_get_length();
  return TRUE;
}

/* open the file and mmap it, necessary to go to READY state */
static gboolean
gst_xbmc_src_start (GstBaseSrc * basesrc)
{
  gboolean ret = FALSE;
  GstXbmcSrc *src = GST_XBMC_SRC (basesrc);

  if (src->filename == NULL || src->filename[0] == '\0')
    goto no_filename;

  GST_INFO_OBJECT (src, "opening file %s", src->filename);
  /* open the file */
  ret = xbmcsrc_open (src->filename, O_RDONLY | O_BINARY, 0);

  if (!ret)
    goto open_failed;

  src->read_position = 0;

  src->seekable = xbmcsrc_seek(0, SEEK_POSSIBLE);
  if(src->seekable)
    xbmcsrc_seek(0, SEEK_SET);

  return TRUE;

  /* ERROR */
no_filename:
  {
    GST_ELEMENT_ERROR (src, RESOURCE, NOT_FOUND,
        (_("No file name specified for reading.")), (NULL));
    return FALSE;
  }
open_failed:
  {
    return FALSE;
  }
}

static gboolean
gst_xbmc_src_stop (GstBaseSrc * basesrc)
{
  //GstXbmcSrc *src = GST_XBMC_SRC (basesrc);

  /* close the file */
  //xbmcsrc_close ();

  return TRUE;
}

/*** GSTURIHANDLER INTERFACE *************************************************/

static GstURIType
gst_xbmc_src_uri_get_type (void)
{
  return GST_URI_SRC;
}

static gchar **
gst_xbmc_src_uri_get_protocols (void)
{
  static gchar *protocols[] = { (char *) "xbmc", NULL };

  return protocols;
}

static const gchar *
gst_xbmc_src_uri_get_uri (GstURIHandler * handler)
{
  GstXbmcSrc *src = GST_XBMC_SRC (handler);

  return src->uri;
}

static gboolean
gst_xbmc_src_uri_set_uri (GstURIHandler * handler, const gchar * uri)
{
  gchar *location;
  gboolean ret = FALSE;
  GstXbmcSrc *src = GST_XBMC_SRC (handler);

  if (strcmp (uri, "xbmc://") == 0) {
    /* Special case for "file://" as this is used by some applications
     *  to test with gst_element_make_from_uri if there's an element
     *  that supports the URI protocol. */
    gst_xbmc_src_set_location (src, NULL);
    return TRUE;
  }

  location = g_strdup(uri+strlen("xbmc://"));

  if (!location) {
    GST_WARNING_OBJECT (src, "Invalid URI '%s' for filesrc", uri);
    goto beach;
  }

  ret = gst_xbmc_src_set_location (src, location);

beach:
  if (location)
    g_free (location);

  return ret;
}

static void
gst_xbmc_src_uri_handler_init (gpointer g_iface, gpointer iface_data)
{
  GstURIHandlerInterface *iface = (GstURIHandlerInterface *) g_iface;

  iface->get_type = gst_xbmc_src_uri_get_type;
  iface->get_protocols = gst_xbmc_src_uri_get_protocols;
  iface->get_uri = gst_xbmc_src_uri_get_uri;
  iface->set_uri = gst_xbmc_src_uri_set_uri;
}
/**********************************************************************/
/* ************************************************************************* */
static PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
static PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;

/* store the eglImage in buffer private qdata, to avoid needing to
 * destroy and re-create the eglImage
 *
 * TODO this could also be useful for an eglsink, gst-clutter, or really any
 * app that wants to render via eglImage, so perhaps we should move it into
 * gst-plugins-base or somewhere common..
 */

#define GST_TYPE_EGL_BUF      \
  (gst_egl_buf_get_type ())
#define GST_EGL_BUF(obj)      \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_EGL_BUF, GstEGLBuf))
#define GST_IS_EGL_BUF(obj)     \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_EGL_BUF))

GType gst_egl_buf_get_type (void);

typedef struct _GstEGLBuf      GstEGLBuf;
typedef struct _GstEGLBufClass GstEGLBufClass;

struct _GstEGLBuf
{
  GstMiniObject parent;
  EGLImageKHR egl_img;
  EGLDisplay egl_dpy;
};

struct _GstEGLBufClass
{
  GstMiniObjectClass parent_class;
};

#define GST_EGL_BUF_QUARK gst_egl_buf_quark_get_type()
static GST_BOILERPLATE_QUARK(GstEGLBuf, gst_egl_buf_quark);

#define EGL_BUF_QUARK egl_buf_quark_get_type()
static GST_BOILERPLATE_QUARK(EGLBuf, egl_buf_quark);

static void
set_egl_buf (GstBuffer * buf, GstEGLBuf * eglbuf)
{
  gst_buffer_set_qdata (buf, GST_EGL_BUF_QUARK,
      gst_structure_id_new (GST_EGL_BUF_QUARK,
          EGL_BUF_QUARK, GST_TYPE_EGL_BUF, eglbuf, NULL));
}

static GstEGLBuf *
get_egl_buf (GstBuffer * buf)
{
  const GstStructure *s;
  const GValue *val;

  s = gst_buffer_get_qdata (buf, GST_EGL_BUF_QUARK);
  if (s == NULL)
    return NULL;

  val = gst_structure_id_get_value (s, EGL_BUF_QUARK);
  if (val == NULL)
    return NULL;

  return GST_EGL_BUF (gst_value_get_mini_object (val));
}

static void
gst_egl_buf_finalize (GstMiniObject * mini_obj)
{
  GstEGLBuf *eglbuf = (GstEGLBuf *) mini_obj;

  eglDestroyImageKHR (eglbuf->egl_dpy, eglbuf->egl_img);

  /* not chaining up to GstMiniObject's finalize for now, we know it's empty */
}

static void
gst_egl_buf_class_init (GstEGLBufClass * klass)
{
  GST_MINI_OBJECT_CLASS (klass)->finalize = gst_egl_buf_finalize;
}

GST_BOILERPLATE_MINI_OBJECT(GstEGLBuf, gst_egl_buf);

#ifndef EGLIMAGE_FLAGS_YUV_CONFORMANT_RANGE
// XXX these should come from some egl header??
#define EGLIMAGE_FLAGS_YUV_CONFORMANT_RANGE (0 << 0)
#define EGLIMAGE_FLAGS_YUV_FULL_RANGE       (1 << 0)
#define EGLIMAGE_FLAGS_YUV_BT601            (0 << 1)
#define EGLIMAGE_FLAGS_YUV_BT709            (1 << 1)
#endif
#ifndef EGL_TI_raw_video
#  define EGL_TI_raw_video 1
#  define EGL_RAW_VIDEO_TI            0x333A  /* eglCreateImageKHR target */
#  define EGL_RAW_VIDEO_TI2           0x333B  /* eglCreateImageKHR target */
#  define EGL_GL_VIDEO_FOURCC_TI        0x3331  /* eglCreateImageKHR attribute */
#  define EGL_GL_VIDEO_WIDTH_TI         0x3332  /* eglCreateImageKHR attribute */
#  define EGL_GL_VIDEO_HEIGHT_TI        0x3333  /* eglCreateImageKHR attribute */
#  define EGL_GL_VIDEO_BYTE_STRIDE_TI     0x3334  /* eglCreateImageKHR attribute */
#  define EGL_GL_VIDEO_BYTE_SIZE_TI       0x3335  /* eglCreateImageKHR attribute */
#  define EGL_GL_VIDEO_YUV_FLAGS_TI       0x3336  /* eglCreateImageKHR attribute */
#endif

GstEGLBuf *
gst_egl_buf (EGLDisplay egl_dpy, GstBuffer * buf)
{
  GstEGLBuf *eglbuf = get_egl_buf (buf);
  if (!eglbuf) {
    GstCaps * caps = GST_BUFFER_CAPS (buf);
    if (caps) {
      GstStructure * s = gst_caps_get_structure (caps, 0);
      gint width, height;
      guint32 format;
      if (s && gst_structure_get_int (s, "width", &width) &&
          gst_structure_get_int (s, "height", &height) &&
          gst_structure_get_fourcc (s, "format", &format)) {
        GstDmaBuf *dmabuf = gst_buffer_get_dma_buf (buf);
        EGLImageKHR egl_img;
        EGLint attr[] = {
            EGL_GL_VIDEO_FOURCC_TI,      format,
            EGL_GL_VIDEO_WIDTH_TI,       width,
            EGL_GL_VIDEO_HEIGHT_TI,      height,
            EGL_GL_VIDEO_BYTE_SIZE_TI,   GST_BUFFER_SIZE (buf),
            // TODO: pick proper YUV flags..
            EGL_GL_VIDEO_YUV_FLAGS_TI,   EGLIMAGE_FLAGS_YUV_CONFORMANT_RANGE |
            EGLIMAGE_FLAGS_YUV_BT601,
            EGL_NONE
        };
        if (dmabuf) {
          int fd = gst_dma_buf_get_fd (dmabuf);
          egl_img = eglCreateImageKHR(egl_dpy, EGL_NO_CONTEXT,
              EGL_RAW_VIDEO_TI2, (EGLClientBuffer) fd, attr);
        } else {
          egl_img = eglCreateImageKHR(egl_dpy, EGL_NO_CONTEXT,
              EGL_RAW_VIDEO_TI, GST_BUFFER_DATA(buf), attr);
        }

        if (egl_img) {
          eglbuf = (GstEGLBuf *)gst_mini_object_new (GST_TYPE_EGL_BUF);
          eglbuf->egl_dpy = egl_dpy;
          eglbuf->egl_img = egl_img;
          set_egl_buf (buf, eglbuf);
        }
      }
    }
  }
  return eglbuf;
}

EGLImageKHR
gst_egl_buf_egl_image (GstEGLBuf * eglbuf)
{
  return eglbuf->egl_img;
}
/* ************************************************************************* */
}

void CGstPlayer::InitEglImage()
{
  static bool initialized = false;
  if (!initialized)
  {
    const char *exts = (const char *)glGetString(GL_EXTENSIONS);

    CLog::Log(LOGDEBUG, "EGLIMG: extensions: %s", exts);

    eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)
                eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)
                eglGetProcAddress("eglDestroyImageKHR");

    // check for various extensions for creating egl-images from video:
    if (strstr(exts, "GL_TI_image_external_raw_video"))
    {
      CLog::Log(LOGDEBUG, "EGLIMG: has GL_TI_image_external_raw_video");
      has_ti_raw_video = true;
    }

    initialized = true;
  }
}

/* TODO: split this into a separate class for dmabuf case.. and use standard extension.. */
class TIRawVideoEGLImageHandle : public EGLImageHandle
{
public:
  TIRawVideoEGLImageHandle(GstBuffer *buf, gint width, gint height, guint32 format)
    : EGLImageHandle()
  {
    this->eglImage = NULL;
    this->buf    = gst_buffer_ref(buf);
    this->width  = width;
    this->height = height;
    this->format = format;
    this->refcnt = 1;
  }

  virtual ~TIRawVideoEGLImageHandle()
  {
    gst_buffer_unref(buf);
  }

  virtual EGLImageHandle * Ref()
  {
    CSingleLock lock(m_monitorLock);
    refcnt++;
    return this;
  }

  void UnRef()
  {
    CSingleLock lock(m_monitorLock);
    --refcnt;
    if (refcnt == 0)
      delete this;
  }
  virtual EGLImageKHR Get()
  {
    if (!eglImage) {
      GstEGLBuf *eglbuf = gst_egl_buf (g_Windowing.GetEGLDisplay(), buf);
      if (eglbuf)
        eglImage = gst_egl_buf_egl_image (eglbuf);
    }
    return eglImage;
  }

private:
  static CCriticalSection m_monitorLock;
  int refcnt;
  int dmabuf_fd;
  EGLImageKHR eglImage;
  gint width, height;
  guint32 format;
  GstBuffer *buf;
};
CCriticalSection TIRawVideoEGLImageHandle::m_monitorLock;

/*****************************************************************************/


#define LOAD_TAG(data, tags, tagid) \
  do {\
    if ((data)==NULL){\
      gchar * value;\
      if (gst_tag_list_get_string ((tags), (tagid), &value)){\
        (data) = value;\
      }\
    }\
  }while(0)

#define APPEND_STR(string, data, prefix, postfix) \
  do {\
    if ((data)){\
      (string) += (prefix);\
      (string) += (data);\
      (string) += (postfix);\
    }\
  }while(0)


CGstPlayer::CGstPlayer(IPlayerCallback& callback)
: IPlayer(callback),
  CThread(),
  m_ready(true)
{
  m_bStop = true;
  m_paused = false;
  m_clock = 0;
  m_lastTime = 0;
  m_speed = 1;
  m_playbin = NULL;
  m_voutput_init = false;
  m_srcRect.SetRect(0.0f,0.0f,640.0f,480.0f);
  m_buffering = false;
  m_cache_level = 100;
  m_file_cnt = 0;
  gst_init(NULL, NULL);
  InitEglImage();
}

CGstPlayer::~CGstPlayer()
{
  CloseFile();
}

bool CGstPlayer::LoadMediaInfo()
{
  if (m_playbin){
    int i;
    GstQuery *query;

    g_object_get( G_OBJECT(m_playbin), "n-video", &m_mediainfo.video_num, NULL);
    g_object_get( G_OBJECT(m_playbin), "n-audio", &m_mediainfo.audio_num, NULL);

    g_object_get( G_OBJECT(m_playbin), "current-audio", &m_audio_current, NULL);
    if (m_audio_current<0) m_audio_current=0;
    g_object_get( G_OBJECT(m_playbin), "current-video", &m_video_current, NULL);
    if (m_video_current<0) m_video_current=0;

    query = gst_query_new_seeking (GST_FORMAT_TIME);
    if(gst_element_query (m_playbin, query))
      gst_query_parse_seeking (query, NULL, &m_mediainfo.seekable, NULL, NULL);
    else
      m_mediainfo.seekable=false;
    gst_query_unref (query);

    if (m_mediainfo.audio_num){
      m_mediainfo.audio_info = (MediaAudioInfo*)g_malloc(sizeof(MediaAudioInfo)*m_mediainfo.audio_num);
      memset(m_mediainfo.audio_info, 0, (sizeof(MediaAudioInfo)*m_mediainfo.audio_num));
    }

    if (m_mediainfo.video_num){
      m_mediainfo.video_info = (MediaVideoInfo*)g_malloc(sizeof(MediaVideoInfo)*m_mediainfo.video_num);
      memset(m_mediainfo.video_info, 0, (sizeof(MediaVideoInfo)*m_mediainfo.video_num));
    }

    for (i=0;i<m_mediainfo.audio_num;i++)
    {
      GstTagList *tags = NULL;

      g_signal_emit_by_name (G_OBJECT (m_playbin), "get-audio-tags", i, &tags);

      if (tags) {
        MediaAudioInfo * ainfo = &m_mediainfo.audio_info[i];

        LOAD_TAG(ainfo->lang, tags, GST_TAG_LANGUAGE_CODE);
        LOAD_TAG(ainfo->codec, tags, GST_TAG_CODEC);
        if (ainfo->codec==NULL){
          LOAD_TAG(ainfo->codec, tags, GST_TAG_AUDIO_CODEC);
        }

        gst_tag_list_get_uint(tags, GST_TAG_BITRATE, &ainfo->bitrate);

        LOAD_TAG(m_mediainfo.container_format, tags, GST_TAG_CONTAINER_FORMAT);
        LOAD_TAG(m_mediainfo.title, tags, GST_TAG_TITLE);
        LOAD_TAG(m_mediainfo.artist, tags, GST_TAG_ARTIST);
        LOAD_TAG(m_mediainfo.description, tags, GST_TAG_DESCRIPTION);
        LOAD_TAG(m_mediainfo.album, tags, GST_TAG_ALBUM);
        LOAD_TAG(m_mediainfo.genre, tags, GST_TAG_GENRE);

        gst_tag_list_free (tags);
      }
    }

    for (i=0;i<m_mediainfo.video_num;i++)
    {
      GstTagList *tags = NULL;

      g_signal_emit_by_name (G_OBJECT (m_playbin), "get-video-tags", i, &tags);

      if (tags) {
        MediaVideoInfo * vinfo = &m_mediainfo.video_info[i];

        LOAD_TAG(vinfo->codec, tags, GST_TAG_CODEC);
        if (vinfo->codec==NULL){
          LOAD_TAG(vinfo->codec, tags, GST_TAG_VIDEO_CODEC);
        }
        gst_tag_list_get_uint(tags, GST_TAG_BITRATE, &vinfo->bitrate);

        LOAD_TAG(m_mediainfo.container_format, tags, GST_TAG_CONTAINER_FORMAT);
        LOAD_TAG(m_mediainfo.title, tags, GST_TAG_TITLE);
        LOAD_TAG(m_mediainfo.artist, tags, GST_TAG_ARTIST);
        LOAD_TAG(m_mediainfo.description, tags, GST_TAG_DESCRIPTION);
        LOAD_TAG(m_mediainfo.album, tags, GST_TAG_ALBUM);
        LOAD_TAG(m_mediainfo.genre, tags, GST_TAG_GENRE);

        gst_tag_list_free (tags);
      }
    }

    m_mediainfo.loaded = true;
    return true;
  } 
  return false;
}

int CGstPlayer::GetAudioStreamCount()
{
  return m_mediainfo.audio_num;
}

void CGstPlayer::GetAudioStreamName(int iStream, CStdString& strStreamName)
{
  strStreamName = "";
  if ((m_playbin) && (m_mediainfo.audio_info) && (iStream<m_mediainfo.audio_num)){
    if (m_mediainfo.audio_info[iStream].lang){
      CStdString code = m_mediainfo.audio_info[iStream].lang;
      if (!g_LangCodeExpander.Lookup(strStreamName, code)) strStreamName = m_mediainfo.audio_info[iStream].lang;
    }
    strStreamName += " ";
    if (m_mediainfo.audio_info[iStream].codec){
      strStreamName += m_mediainfo.audio_info[iStream].codec;
    }
  }else{
    strStreamName += "Unknown";
  }
}

int CGstPlayer::GetAudioStream()
{
  return m_audio_current;
}


int CGstPlayer::OutputPicture(GstBuffer * gstbuffer)
{
  /* picture buffer is not allowed to be modified in this call */
  DVDVideoPicture picture;
  DVDVideoPicture* pPicture = &picture;
  int result = 0;
  gint flags;
  double config_framerate = 30.0;

  memset(pPicture, 0, sizeof(DVDVideoPicture));

#ifdef HAS_VIDEO_PLAYBACK

  GstCaps * caps = GST_BUFFER_CAPS(gstbuffer);

  if (caps) {
    gint width, height;
    guint32 format;
    GstStructure * structure = gst_caps_get_structure (caps, 0);
    GstVideoCrop * crop = gst_buffer_get_video_crop (gstbuffer);

    if (structure == NULL ||
        !gst_structure_get_int (structure, "width", &width) ||
        !gst_structure_get_int (structure, "height", &height) ||
        !gst_structure_get_fourcc (structure, "format", &format)){
      
      CLog::Log(LOGERROR, "Unsupport output format");
      return -1;
    }

    pPicture->iWidth = width;
    pPicture->iHeight = height;

    if (crop) {
      pPicture->iDisplayWidth  = gst_video_crop_width (crop);
      pPicture->iDisplayHeight = gst_video_crop_height (crop);
      pPicture->iDisplayX = gst_video_crop_left (crop);
      pPicture->iDisplayY = gst_video_crop_top (crop);
    } else {
      pPicture->iDisplayWidth  = pPicture->iWidth;
      pPicture->iDisplayHeight = pPicture->iHeight;
      pPicture->iDisplayX = 0;
      pPicture->iDisplayY = 0;
    }

    /* try various ways to create an eglImg from the decoded buffer: */
    if (has_ti_raw_video) {
      pPicture->eglImageHandle =
          new TIRawVideoEGLImageHandle(gstbuffer, width, height, format);
    }

    if (pPicture->eglImageHandle) {
      pPicture->format = DVDVideoPicture::FMT_EGLIMG;
      flags = CONF_FLAGS_FORMAT_EGLIMG;
    } else if (format==GST_STR_FOURCC("NV12")) {
      pPicture->data[0] = GST_BUFFER_DATA(gstbuffer);
      pPicture->data[1] = GST_BUFFER_DATA(gstbuffer)+m_output.width*m_output.height;
      pPicture->iLineSize[0] = pPicture->iWidth;
      pPicture->iLineSize[1] = pPicture->iWidth;
      pPicture->format = DVDVideoPicture::FMT_NV12;
      flags = CONF_FLAGS_FORMAT_NV12;
    } else if (format==GST_STR_FOURCC("I420")) {
      pPicture->data[0] = GST_BUFFER_DATA(gstbuffer);
      pPicture->data[1] = GST_BUFFER_DATA(gstbuffer)+m_output.width*m_output.height;
      pPicture->data[2] = GST_BUFFER_DATA(gstbuffer)+m_output.width*m_output.height*5/4;
      pPicture->iLineSize[0] = pPicture->iWidth;
      pPicture->iLineSize[1] = pPicture->iWidth/2;
      pPicture->iLineSize[2] = pPicture->iWidth/2;
      pPicture->format = DVDVideoPicture::FMT_YUV420P;
      flags = CONF_FLAGS_FORMAT_YV12;
    } else {
      CLog::Log(LOGERROR, "Unsupport output format");
      return -1;
    }
    
  }else{
    return -1;
  }


  if ((m_output.width!=pPicture->iWidth) || (m_output.height!=pPicture->iHeight)){
    if(!g_renderManager.Configure(pPicture->iWidth, pPicture->iHeight, pPicture->iDisplayWidth, pPicture->iDisplayHeight, config_framerate, flags, pPicture->extended_format))
    {
      CLog::Log(LOGNOTICE, "Failed to configure renderer");
      return -1;
    }

    m_output.width=pPicture->iWidth;
    m_output.height=pPicture->iHeight;
  }
  
  int index = g_renderManager.AddVideoPicture(*pPicture);
#if 0
  // video device might not be done yet
  while (index < 0 && !CThread::m_bStop &&
         CDVDClock::GetAbsoluteClock(false) < iCurrentClock + iSleepTime + DVD_MSEC_TO_TIME(500) )
  {
    Sleep(1);
    index = g_renderManager.AddVideoPicture(*pPicture);
  }
#endif
  if (index < 0)
    return -1;

  g_renderManager.FlipPage(CThread::m_bStop, 0LL, -1, FS_NONE);

  if (pPicture->eglImageHandle)
    pPicture->eglImageHandle->UnRef();

  return result;

#endif
}


void CGstPlayer::SetAudioStream(int iStream)
{
  if ((m_playbin) && (iStream>=0) && (iStream<m_mediainfo.audio_num)){
    m_audio_current = iStream;
    g_object_set( G_OBJECT(m_playbin), "current-audio", iStream, NULL);
  }
}

void CGstPlayer::CleanMediaInfo()
{
  int i;

  if (m_mediainfo.container_format){
    g_free(m_mediainfo.container_format);
    m_mediainfo.container_format = NULL;
  }
  if (m_mediainfo.genre){
    g_free(m_mediainfo.genre);
    m_mediainfo.genre = NULL;
  }
  if (m_mediainfo.title){
    g_free(m_mediainfo.title);
    m_mediainfo.title = NULL;
  }
  if (m_mediainfo.artist){
    g_free(m_mediainfo.artist);
    m_mediainfo.artist = NULL;
  }
  if (m_mediainfo.description){
    g_free(m_mediainfo.description);
    m_mediainfo.description = NULL;
  }
  if (m_mediainfo.album){
    g_free(m_mediainfo.album);
    m_mediainfo.album = NULL;
  }
  



  if (m_mediainfo.audio_info){
    for (i=0;i<m_mediainfo.audio_num;i++){
      MediaAudioInfo * ainfo = &m_mediainfo.audio_info[i];

      if (ainfo->lang){
    g_free(ainfo->lang);
    ainfo->lang = NULL;
  }
      if (ainfo->codec){
    g_free(ainfo->codec);
    ainfo->codec = NULL;
  }
      
    }
    g_free(m_mediainfo.audio_info);
    m_mediainfo.audio_info = NULL;
  }

  if (m_mediainfo.video_info){
    for (i=0;i<m_mediainfo.video_num;i++){
      MediaVideoInfo * vinfo = &m_mediainfo.video_info[i];
      if (vinfo->codec){
    g_free(vinfo->codec);
    vinfo->codec = NULL;
  }
    }
    g_free(m_mediainfo.video_info);
    m_mediainfo.video_info = NULL;
  }
}

void CGstPlayer::OnDecodedBuffer(GstElement *appsink, void *data)
{
  CGstPlayer *decoder = (CGstPlayer *)data;

  GstBuffer *buffer = gst_app_sink_pull_buffer(GST_APP_SINK(appsink));
  if (buffer)
  {
      decoder->OutputPicture(buffer);
      
      gst_buffer_unref(buffer);
  }
  else{
    CLog::Log(LOGERROR,"GStreamer: OnDecodedBuffer - Null Buffer");
  }
}


bool CGstPlayer::CreatePlayBin()
{
  GstElement *videosink, *audiosink=NULL;
  gst_element_register (NULL, "xbmcsrc", GST_RANK_PRIMARY,gst_xbmc_src_get_type ());
  m_playbin = gst_element_factory_make("playbin2", "playbin0");
  if(!m_playbin)
  {
    CLog::Log(LOGERROR, "CreatePlayBin - gst_element_factory_make playbin2 failed");
    return false;
  }

  //prepare videosink
  videosink = gst_element_factory_make("appsink", "videosink0");
  if(!videosink)
  {
    CLog::Log(LOGERROR, "CreatePlayBin - gst_element_factory_make appsink failed");
    return false;
  }

  {
    g_object_set(G_OBJECT(videosink), "emit-signals", TRUE, "sync", TRUE, NULL);
    g_signal_connect(videosink, "new-buffer", G_CALLBACK(OnDecodedBuffer), this);
  }

  g_object_set(G_OBJECT(m_playbin), "video-sink", videosink, NULL);
  //GST_PLAY_FLAG_NATIVE_VIDEO | GST_PLAY_FLAG_VIDEO | GST_PLAY_FLAG_AUDIO | GST_PLAY_FLAG_TEXT | GST_PLAY_FLAG_SOFT_VOLUME
  g_object_set(G_OBJECT(m_playbin), "flags", 0x5F, NULL);

  m_bus = gst_pipeline_get_bus(GST_PIPELINE(m_playbin));
  if( NULL == m_bus )
  {
    CLog::Log(LOGERROR, "%s(): Failed in gst_pipeline_get_bus()!", __FUNCTION__);
    return false;
  }
  return true;
}

bool CGstPlayer::DestroyPlayBin()
{
  gst_object_unref(m_playbin);
  m_playbin = NULL;
  gst_object_unref(m_bus);
  m_bus = NULL;
  return true;
}

bool CGstPlayer::OpenFile(const CFileItem& file, const CPlayerOptions &options)
{
  if (m_playbin){
    CloseFile();
  }

  m_clock = 0;
  m_bStop = false;
  m_item = file;
  m_starttime = (gint64)(options.starttime*1000000000);

  memset(&m_mediainfo, 0, sizeof(MediaInfo));

  m_audio_current = 0;
  m_video_current = 0;


  g_renderManager.PreInit();

  m_buffering = false;
  m_cache_level = 100;

  m_quit_msg = false;
  m_cancelled = false;

  if(!CreatePlayBin())
    return false;

  m_ready.Reset();
  //CLog::Log(LOGNOTICE, "Player thread create");
  Create();
  //if(!m_ready.WaitMSec(2000))
  {
    //CLog::Log(LOGNOTICE, "Player thread start timeout");
    CGUIDialogBusy* dialog = (CGUIDialogBusy*)g_windowManager.GetWindow(WINDOW_DIALOG_BUSY);
    dialog->Show();
    while(!m_ready.WaitMSec(10))
    {
      g_windowManager.Process(CTimeUtils::GetFrameTime());
      if(dialog->IsCanceled())
      {
        m_cancelled = true;
        m_ready.Wait();
        break;
      }
    }
    dialog->Close();
  }

  //CLog::Log(LOGNOTICE, "Player thread create success");
  if(m_bStop)
    return false;

  m_callback.OnPlayBackStarted();

  //CLog::Log(LOGNOTICE, "GxPlayer OpenFile return");
  return true;
}

bool CGstPlayer::CloseFile()
{
  CLog::Log(LOGNOTICE, "---[%s]---", __FUNCTION__);

  m_bStop = true;
  m_paused = false;
  m_quit_msg = true;

  if(m_bus)
    gst_bus_post(m_bus, gst_message_new_custom(GST_MESSAGE_APPLICATION, NULL, gst_structure_new("quit", NULL)));

  CLog::Log(LOGNOTICE, "CloseFile - Stopping Thread");
  StopThread();

  if (m_playbin)
    DestroyPlayBin();    
  
  CleanMediaInfo();

  g_renderManager.UnInit();

  m_callback.OnPlayBackStopped();
  return true;
}

bool CGstPlayer::IsPlaying() const
{
  return !m_bStop;
}

void CGstPlayer::ResetUrlInfo()
{
  m_username.clear();
  m_password.clear();
}

CStdString CGstPlayer::ParseAndCorrectUrl(CURL &url)
{
  CStdString strProtocol = url.GetTranslatedProtocol();
  CStdString ret;
  url.SetProtocol(strProtocol);

  ResetUrlInfo();
  if(url.IsLocal()) {	//local file
    CStdString path = url.GetFileName();
    gchar *fname = NULL;
    if(url.IsFullPath(path))
      fname = g_strdup(path.c_str());
    else {
      gchar *pwd = g_get_current_dir();
      fname = g_strdup_printf("%s/%s", pwd, path.c_str());
      g_free(pwd);
    }
    url.SetFileName(CStdString(fname));
    url.SetProtocol("file");
    g_free(fname);
  }
  else if( strProtocol.Equals("http")
      ||  strProtocol.Equals("https"))
  {
    
    // replace invalid spaces
    CStdString strFileName = url.GetFileName();
    strFileName.Replace(" ", "%20");
    url.SetFileName(strFileName);

    // get username and password
    m_username = url.GetUserName();
    m_password = url.GetPassWord();

  }

  if (m_username.length() > 0 && m_password.length() > 0)
    ret = url.GetWithoutUserDetails();
  else
    ret = url.Get();
  return ret;
}

bool CGstPlayer::SetAndWaitPlaybinState(GstState newstate, int timeout)
{
  GstStateChangeReturn ret;
 

  if (m_playbin){
    ret = gst_element_set_state(m_playbin, newstate);
    if (ret==GST_STATE_CHANGE_ASYNC){
      GstState current, pending;
      do{
      ret =  gst_element_get_state(m_playbin, &current, &pending, GST_SECOND);
        }while((m_cancelled==false)&&(ret==GST_STATE_CHANGE_ASYNC) && (timeout-->=0));
    }

    if ((ret==GST_STATE_CHANGE_FAILURE)||(ret==GST_STATE_CHANGE_ASYNC)){
      return false;
    }else{
      return true;
    }
  }
  return false;
  
}



bool CGstPlayer::OpenInputStream()
{
#ifdef USE_GSTXBMCSRC
  std::string filename = m_item.GetPath();
  std::string mimetype = m_item.GetMimeType();
  m_pInputStream = CDVDFactoryInputStream::CreateInputStream(this, filename, mimetype);
  if(m_pInputStream == NULL)
  {
    CLog::Log(LOGERROR, "CGstPlayer::OpenInputStream - unable to create input stream for [%s]", filename.c_str());
    return false;
  }
  else
    m_pInputStream->SetFileItem(m_item);

  if (!m_pInputStream->Open(filename.c_str(), mimetype))
  {
    CLog::Log(LOGERROR, "CGstPlayer::OpenInputStream - error opening [%s]", filename.c_str());
    return false;
  }
#endif
  return true;
}


void CGstPlayer::Process()
{
  bool ret;
  GstEvent *seek_event;
  GstMessage *msg;
  GstState old, current, pending;
  bool eos = false;
  int i;

  if (!OpenInputStream())
    return;

  CLog::Log(LOGNOTICE, "Player thread start %p", m_playbin);
  if (m_playbin) {
#ifdef USE_GSTXBMCSRC
    g_object_set(G_OBJECT(m_playbin), "uri", "xbmc:///filestub", NULL);
#else
    CURL url = m_item.GetAsUrl();
    CStdString uri = ParseAndCorrectUrl(url);
    CLog::Log(LOGNOTICE, "Play uri %s", uri.c_str());
    g_object_set(G_OBJECT(m_playbin), "uri", uri.c_str(), NULL);
#endif

    if ((SetAndWaitPlaybinState(GST_STATE_PAUSED, 60)==false) || (m_cancelled)){
      m_bStop = true;
      m_ready.Set();
      goto finish;
    }
    if (m_starttime) {
      seek_event = gst_event_new_seek(1.0, GST_FORMAT_TIME,
          (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
          GST_SEEK_TYPE_SET, m_starttime,
          GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
      gst_element_send_event(m_playbin, seek_event);
    }
    LoadMediaInfo();

    if (SetAndWaitPlaybinState(GST_STATE_PLAYING, 10)==false){
      
      m_bStop = true;
      m_ready.Set();
      goto finish;
      }

    g_renderManager.SetViewMode(g_settings.m_currentVideoSettings.m_ViewMode);
  }

  m_ready.Set();

  while(!m_quit_msg && !eos) {
    msg = gst_bus_timed_pop(m_bus, 1000000000);
    if(msg) {
      if(GST_MESSAGE_SRC(msg) == GST_OBJECT(m_playbin)) { //playbin message
        switch(msg->type) {
          case GST_MESSAGE_EOS:
            {
              CLog::Log(LOGNOTICE, "Media EOS");
              eos = true;
              break;
            }
          case GST_MESSAGE_ERROR:
            {
              CLog::Log(LOGERROR, "Media Error");
              eos = true;
              break;
            }

          case GST_MESSAGE_STATE_CHANGED:
            {
              break;
            }

          default:
            break;
        }
      }
      else {
        if(msg->type == GST_MESSAGE_APPLICATION && GST_MESSAGE_SRC(msg) == NULL) {
          const GstStructure *s = gst_message_get_structure(msg);
          const gchar *name = gst_structure_get_name(s);
          if(!g_strcmp0(name, "pause")) {
            if(!m_buffering && !m_paused)
              gst_element_set_state(m_playbin, GST_STATE_PLAYING);
            else
              gst_element_set_state(m_playbin, GST_STATE_PAUSED);
          }
          else if(!g_strcmp0(name, "quit")) {
            //will quit by m_quit_msg
          }
        }
        else if(msg->type == GST_MESSAGE_BUFFERING)
        {
          gst_message_parse_buffering(msg, &m_cache_level);
          if(m_cache_level == 0) {
            m_buffering = true;
            gst_element_set_state(m_playbin, GST_STATE_PAUSED);
          }
          else if(m_cache_level >= 100) {
            m_buffering = false;
            if(!m_paused) {
              gst_element_set_state(m_playbin, GST_STATE_PLAYING);
            }
          }
        }
      }
      gst_message_unref(msg);
    }
  }
finish:  
  CLog::Log(LOGNOTICE, "Player stop begin");
  gst_bus_set_flushing(m_bus, TRUE);

  if (SetAndWaitPlaybinState(GST_STATE_NULL,30)==false){

    CLog::Log(LOGERROR, "Stop player failed");
    }
  m_bStop = true;
  if(eos)
    m_callback.OnPlayBackEnded();
  CLog::Log(LOGNOTICE, "Player thread end");
}

void CGstPlayer::Pause()
{
  CLog::Log(LOGNOTICE, "---[%s]---", __FUNCTION__);

  m_paused = !m_paused;
  if (m_bus){
    gst_bus_post(m_bus, gst_message_new_custom(GST_MESSAGE_APPLICATION, NULL, gst_structure_new("pause", NULL)));
  }
  if (!m_paused) {
    m_callback.OnPlayBackResumed();
  }
  else {
    m_callback.OnPlayBackPaused();
  }
}

bool CGstPlayer::IsPaused() const
{
  return m_paused || m_buffering;
}

bool CGstPlayer::HasVideo() const
{
  return (m_mediainfo.video_num>0);
}

bool CGstPlayer::HasAudio() const
{
  return (m_mediainfo.audio_num>0);
}

int CGstPlayer::GetCacheLevel() const
{
  return m_cache_level;
}

void CGstPlayer::SwitchToNextLanguage()
{
  CLog::Log(LOGNOTICE, "---[%s]---", __FUNCTION__);
}

void CGstPlayer::ToggleSubtitles()
{
  CLog::Log(LOGNOTICE, "---[%s]---", __FUNCTION__);
}

bool CGstPlayer::CanSeek()
{
  return m_mediainfo.seekable;
}

void CGstPlayer::Seek(bool bPlus, bool bLargeStep)
{
  CLog::Log(LOGNOTICE, "---[%s]--- %d, %d", __FUNCTION__, bPlus, bLargeStep);
  /* TODO: add chapter support? */
  __int64 seek;
  if (g_advancedSettings.m_videoUseTimeSeeking &&
      GetTotalTime() > 2*g_advancedSettings.m_videoTimeSeekForwardBig)
  {
    if (bLargeStep)
      seek = bPlus ? g_advancedSettings.m_videoTimeSeekForwardBig :
          g_advancedSettings.m_videoTimeSeekBackwardBig;
    else
      seek = bPlus ? g_advancedSettings.m_videoTimeSeekForward :
          g_advancedSettings.m_videoTimeSeekBackward;
    seek *= 1000;
    seek += GetTime();
  }
  else
  {
    float percent;
    if (bLargeStep)
      percent = bPlus ? g_advancedSettings.m_videoPercentSeekForwardBig :
          g_advancedSettings.m_videoPercentSeekBackwardBig;
    else
      percent = bPlus ? g_advancedSettings.m_videoPercentSeekForward :
          g_advancedSettings.m_videoPercentSeekBackward;
    seek = (__int64)(GetTotalTime()*1000*(GetPercentage()+percent)/100);
  }

  GstEvent *seek_event;

  m_clock = seek;
  if (m_playbin){
    seek_event = gst_event_new_seek(1.0, GST_FORMAT_TIME,
        (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
        GST_SEEK_TYPE_SET, seek*1000000,
        GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
    gst_element_send_event(m_playbin, seek_event);
  }

  CLog::Log(LOGNOTICE, "---[%s finish]---%lld", __FUNCTION__, seek);
  g_infoManager.m_performingSeek = false;
  m_callback.OnPlayBackSeek((int)seek, (int)(seek - GetTime()));
  CLog::Log(LOGNOTICE, "---[%s nretun]---%lld", __FUNCTION__, seek);
}

void CGstPlayer::SetVolume(long nVolume)
{
  CLog::Log(LOGNOTICE, "---[%s]--- %ld", __FUNCTION__, nVolume);
  if(m_playbin)
  {
    double linear;
    if(nVolume <= VOLUME_MINIMUM)
      linear = 0.0;
    else
    {
      linear = pow(10.0, (double)nVolume/1000.0);
      linear = cbrt(linear);
      if(linear > 10.0)
        linear = 10.0;
    }
    g_object_set(G_OBJECT(m_playbin), "volume", linear, NULL);
  }
}

void CGstPlayer::GetAudioInfo(CStdString& strAudioInfo)
{
  strAudioInfo = "Audio (";
  if (m_mediainfo.audio_info){
    MediaAudioInfo * ainfo = &m_mediainfo.audio_info[m_audio_current];
    APPEND_STR(strAudioInfo, ainfo->codec, "Codec: ", ", ");
    APPEND_STR(strAudioInfo, ainfo->lang, "Language: ", ", ");
    if (ainfo->bitrate){
      CStdString bitstr;
      bitstr.Format("Bitrate: %d", ainfo->bitrate);
      strAudioInfo += bitstr;
    }
  }
  strAudioInfo += ")";

}

void CGstPlayer::GetVideoInfo(CStdString& strVideoInfo)
{
  strVideoInfo = "Video (";
  if (m_mediainfo.video_info){
    MediaVideoInfo * vinfo = &m_mediainfo.video_info[m_video_current];
    APPEND_STR(strVideoInfo, vinfo->codec, "Codec: ", ", ");
    if (vinfo->bitrate){
      CStdString bitstr;
      bitstr.Format("Bitrate: %d", vinfo->bitrate);
      strVideoInfo += bitstr;
    }
  }
  strVideoInfo += ")";
}

void CGstPlayer::GetGeneralInfo(CStdString& strGeneralInfo)
{
  strGeneralInfo = "";
  APPEND_STR(strGeneralInfo, m_mediainfo.title, "Title :", ", ");
  APPEND_STR(strGeneralInfo, m_mediainfo.artist, "Artist: ", ", ");
  APPEND_STR(strGeneralInfo, m_mediainfo.genre, "Genre: ", ", ");
  APPEND_STR(strGeneralInfo, m_mediainfo.album, "Album: ", ", ");
  APPEND_STR(strGeneralInfo, m_mediainfo.description, "Desc: ", ", ");
  APPEND_STR(strGeneralInfo, m_mediainfo.container_format, "Format: ", "");
}

void CGstPlayer::SwitchToNextAudioLanguage()
{
  CLog::Log(LOGNOTICE, "---[%s]---", __FUNCTION__);
}

void CGstPlayer::SeekPercentage(float iPercent)
{
  CLog::Log(LOGNOTICE, "---[%s]---", __FUNCTION__);
  __int64 iTotalMsec = GetTotalTime() * 1000;
  __int64 iTime = (__int64)(iTotalMsec * iPercent / 100);
  SeekTime(iTime);
}

float CGstPlayer::GetPercentage()
{
  __int64 iTotalTime = GetTotalTime() * 1000;
  //CLog::Log(LOGNOTICE, "---[%s]---", __FUNCTION__);
  if (iTotalTime != 0)
  {
    return GetTime() * 100 / (float)iTotalTime;
  }
  return 0.0f;
}


void CGstPlayer::SetSinkRenderDelay(GstElement * ele, guint64 renderDelay)
{
  if (GST_IS_BIN(ele)){
    GstIterator *elem_it = NULL;
    gboolean done = FALSE;
    elem_it = gst_bin_iterate_sinks (GST_BIN (ele));
    while (!done) {
      GstElement *element = NULL;

      switch (gst_iterator_next (elem_it,  ((void **)&element))) {
        case GST_ITERATOR_OK:
          if (element ) {
            g_object_set( G_OBJECT(element), "render-delay", renderDelay, NULL);
          }
          gst_object_unref (element);
          break;
        case GST_ITERATOR_RESYNC:
          gst_iterator_resync (elem_it);
          break;
        case GST_ITERATOR_ERROR:
          done = TRUE;
          break;
        case GST_ITERATOR_DONE:
          done = TRUE;
          break;
      }
    }
    gst_iterator_free (elem_it);
  }else{
    g_object_set( G_OBJECT(ele), "render-delay", renderDelay, NULL);
  }
}

//This is how much audio is delayed to video, we count the oposite in the dvdplayer
void CGstPlayer::SetAVDelay(float fValue)
{
  if ((m_playbin) && (m_mediainfo.audio_num) && (m_mediainfo.video_num)){
    GstElement * videosink = NULL;
    GstElement * audiosink = NULL;
    guint64 value64=0;

    g_object_get( G_OBJECT(m_playbin), "video-sink", &videosink, NULL);
    g_object_get( G_OBJECT(m_playbin), "audio-sink", &audiosink, NULL);

    if ((videosink==NULL) || (audiosink==NULL)){
      goto bail;
    }

    if (fValue>=0.0){
      SetSinkRenderDelay(videosink, value64);
      value64 = (guint64)(fValue*GST_SECOND);
      SetSinkRenderDelay(audiosink, value64);
    }else{
      SetSinkRenderDelay(audiosink, value64);
      value64 = (guint64)(-fValue*GST_SECOND);
      SetSinkRenderDelay(videosink, value64);
    }
bail:
    if (videosink){
      g_object_unref(G_OBJECT(videosink));
    }
    if (audiosink){
      g_object_unref(G_OBJECT(audiosink));
    }
  }
}

float CGstPlayer::GetAVDelay()
{
  CLog::Log(LOGNOTICE, "---[%s]---", __FUNCTION__);
  return 0.0f;
}

void CGstPlayer::SetSubTitleDelay(float fValue)
{
  CLog::Log(LOGNOTICE, "---[%s]---", __FUNCTION__);
}

float CGstPlayer::GetSubTitleDelay()
{
  CLog::Log(LOGNOTICE, "---[%s]---", __FUNCTION__);
  return 0.0;
}

void CGstPlayer::SeekTime(__int64 iTime)
{
  GstEvent *seek_event;
  CLog::Log(LOGNOTICE, "---[%s]---%lld", __FUNCTION__, iTime);

  m_clock = iTime;
  if (m_playbin){
    seek_event = gst_event_new_seek(1.0, GST_FORMAT_TIME,
        (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
        GST_SEEK_TYPE_SET, iTime*1000000,
        GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
    gst_element_send_event(m_playbin, seek_event);
  }

  CLog::Log(LOGNOTICE, "---[%s finish]---%lld", __FUNCTION__, iTime);
  g_infoManager.m_performingSeek = false;
  m_callback.OnPlayBackSeek((int)iTime, 0);
  CLog::Log(LOGNOTICE, "---[%s nretun]---%lld", __FUNCTION__, iTime);
}

// return the time in milliseconds
__int64 CGstPlayer::GetTime()
{
  gint64 elapsed = 0;
  if (m_playbin) {
    GstFormat fmt = GST_FORMAT_TIME;
    if (TRUE==gst_element_query_position(m_playbin, &fmt, &elapsed)){
      m_clock = elapsed/1000000;
    }
  }
  return m_clock;
}

// return length in seconds.. this should be changed to return in milleseconds throughout xbmc
int CGstPlayer::GetTotalTime()
{
  int totaltime = 0;
  if (m_playbin){
    gint64 duration = 0;
    GstFormat fmt = GST_FORMAT_TIME;
    gst_element_query_duration(m_playbin, &fmt, &duration);
    totaltime = duration/1000000000;
  }
  //CLog::Log(LOGNOTICE, "---[%s]--- %d", __FUNCTION__, totaltime);
  return totaltime;
}

void CGstPlayer::SetPlaybackRate(int iSpeed, gint64 pos)
{
  GstEvent *event;
  if(iSpeed > 0)
    event = gst_event_new_seek(iSpeed, GST_FORMAT_TIME,
        (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
        GST_SEEK_TYPE_SET, pos,
        GST_SEEK_TYPE_SET, GST_CLOCK_TIME_NONE);
  else
    event = gst_event_new_seek(iSpeed, GST_FORMAT_TIME,
        (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
        GST_SEEK_TYPE_SET, 0,
        GST_SEEK_TYPE_SET, pos);
  gst_element_send_event(m_playbin, event);
}

void CGstPlayer::ToFFRW(int iSpeed)
{
  gint64 elapsed = 0;
  GstFormat fmt = GST_FORMAT_TIME;
  CLog::Log(LOGNOTICE, "---[%s]-%d--", __FUNCTION__, iSpeed);
  m_speed = iSpeed;

  if (m_playbin){
    if(gst_element_query_position(m_playbin, &fmt, &elapsed)) {
      /*
         if(iSpeed == 1)
         g_object_set(G_OBJECT(m_playbin), "mute", FALSE, NULL);
         else
         g_object_set(G_OBJECT(m_playbin), "mute", TRUE, NULL);
       */
      SetPlaybackRate(iSpeed, elapsed);
      m_callback.OnPlayBackSpeedChanged(iSpeed);
    }
  }
}


CStdString CGstPlayer::GetPlayerState()
{
  //CLog::Log(LOGNOTICE, "---[%s]---", __FUNCTION__);
  return "";
}

bool CGstPlayer::SetPlayerState(CStdString state)
{
  CLog::Log(LOGNOTICE, "---[%s]---", __FUNCTION__);
  return true;
}

bool CGstPlayer::OnAction(const CAction &action)
{
  //CLog::Log(LOGNOTICE, "on action id %d", action.id);
  return false;
}

void CGstPlayer::GetVideoRect(CRect& SrcRect, CRect& DestRect)
{
  CLog::Log(LOGNOTICE, "---[%s]---", __FUNCTION__);
  g_renderManager.GetVideoRect(SrcRect, DestRect);
}

int CGstPlayer::OnDVDNavResult(void* pData, int iMessage)
{
  CLog::Log(LOGNOTICE, "---[%s]---", __FUNCTION__);
  return 0;
}

void CGstPlayer::OnExit()
{
#ifdef USE_GSTXBMCSRC
  // destroy the inputstream
  if (m_pInputStream)
  {
    CLog::Log(LOGNOTICE, "CGstPlayer::OnExit() deleting input stream");
    delete m_pInputStream;
  }
  m_pInputStream = NULL;
#endif
}
