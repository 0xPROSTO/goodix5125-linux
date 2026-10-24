/*
 * libfprint driver for the Goodix 27c6:5125 (Huawei MateBook D, NbD-WDH9).
 *
 * The sensor needs a TLS session per capture session and a host-side
 * matcher, so this is a plain FpDevice: each action runs the blocking
 * libgx5125 sequence in a worker thread and reports back on the main
 * context. Templates are kept in the FpPrint (FPI_PRINT_RAW); fprintd stores
 * them on disk as for any other host-matching device.
 *
 * Cancellation completes the action at once, as fprintd's clients expect:
 * GNOME Settings sends VerifyStop and its next request back to back, and the
 * worker only notices within a USB poll slice. The worker finishes in the
 * background; the next job's worker waits for it, and close waits too.
 *
 * The TLS pre-shared key is the one the Windows driver provisioned; it is
 * read from /etc/goodix5125/psk.bin (32 raw bytes or 64 hex digits).
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define FP_COMPONENT "goodix5125"

#include <unistd.h>

#include <openssl/crypto.h>

#include "drivers_api.h"
#include "gx5125.h"

#define PSK_PATH          "/etc/goodix5125/psk.bin"
#define ENROLL_STAGES     15
#define PRINT_VERSION     1
#define USB_IFACE         1
#define EP_OUT            0x01
#define EP_IN             0x81
#define LIFT_TIMEOUT_MS   10000
#define AFTER_MATCH_LIFT_MS 3000

struct _FpiDeviceGoodix5125
{
  FpDevice      parent;
  GMainContext *main_ctx;

  /* Last finger-free background and the FDT levels learnt with it. Only the
   * worker touches these, and only one job runs at a time. */
  uint16_t      bg[GX_IMG_PIXELS];
  uint8_t       fdt_level[6];
  gboolean      bg_valid;

  GMutex        worker_lock;    /* held by a worker for its whole run */
  struct Job   *current;        /* job whose action is still open, main ctx */
  guint         serial;         /* id of the newest job */
  guint         workers;        /* workers not yet done */
  gboolean      close_pending;
  GCancellable *abort;          /* cancelled by close: workers stop now */
};

G_DECLARE_FINAL_TYPE (FpiDeviceGoodix5125, fpi_device_goodix5125, FPI, DEVICE_GOODIX5125, FpDevice);
G_DEFINE_TYPE (FpiDeviceGoodix5125, fpi_device_goodix5125, FP_TYPE_DEVICE);

/* ---- transport over GUsb ------------------------------------------------- */

static int
usb_xfer (GUsbDevice *usb, GCancellable *cancel, guint8 ep, guint8 *buf, gsize len,
          gsize *done, unsigned timeout)
{
  g_autoptr(GError) error = NULL;
  gsize actual = 0;

  if (done)
    *done = 0;
  if (!g_usb_device_bulk_transfer (usb, ep, buf, len, &actual, timeout, cancel, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return GX_ERR_CANCELLED;
      /* GUsb reports actual = (gsize) -1 on failure; nothing usable arrived. */
      if (g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
        return GX_ERR_TIMEOUT;
      g_debug ("USB transfer on %02x failed: %s", ep, error->message);
      return GX_ERR_IO;
    }
  if (actual > len)
    return GX_ERR_IO;
  if (done)
    *done = actual;
  return GX_OK;
}


static void G_GNUC_PRINTF (2, 3)
gx_log (void *ctx, const char *fmt, ...)
{
  g_autofree char *msg = NULL;
  va_list ap;

  va_start (ap, fmt);
  msg = g_strdup_vprintf (fmt, ap);
  va_end (ap);
  g_debug ("%s", msg);
}


/* ---- worker job ----------------------------------------------------------- */

typedef enum {
  JOB_OPEN,
  JOB_ENROLL,
  JOB_VERIFY,
  JOB_IDENTIFY,
} JobKind;

typedef struct Job
{
  FpiDeviceGoodix5125 *self;
  JobKind              kind;
  guint                serial;
  GUsbDevice          *usb;
  GCancellable        *cancellable;

  /* inputs */
  GPtrArray *gallery;        /* FpPrint*, verify (1) / identify (n) */

  /* outputs */
  GArray  *templates;        /* gx_template, enroll */
  FpPrint *matched;          /* identify/verify */
  gboolean match_ok;
  GError  *retry;            /* verify/identify retry reason */
  gboolean reported;         /* result already sent, main ctx */

  /* Cancels the worker's USB transfers: on the action's cancellable unless
   * ignore_cancel is set, and on close (abort). */
  GCancellable *io;
  GCancellable *abort;
  gint          ignore_cancel;
  gint          closing;     /* worker: ending the session, nothing stops it */
} Job;

static void
job_free (Job *job)
{
  g_clear_pointer (&job->gallery, g_ptr_array_unref);
  g_clear_pointer (&job->templates, g_array_unref);
  g_clear_object (&job->matched);
  g_clear_error (&job->retry);
  g_clear_object (&job->cancellable);
  g_clear_object (&job->io);
  g_clear_object (&job->abort);
  g_free (job);
}

static GCancellable *io_cancellable (Job *job);

/* Transport over the job's USB device. Reads stop when job->io is cancelled,
 * so no polling slices are needed. Writes are never cut short: the sensor
 * would take the next packs as the rest of a half-sent one and stop making
 * sense of anything until reset. */
static int
t_write (void *ctx, const uint8_t *buf, size_t len, unsigned timeout)
{
  gsize done = 0;
  int rc = usb_xfer (((Job *) ctx)->usb, NULL, EP_OUT, (guint8 *) buf, len, &done, timeout);

  return rc == GX_OK && done != len ? GX_ERR_IO : rc;
}

static int
t_read (void *ctx, uint8_t *buf, size_t cap, size_t *got, unsigned timeout)
{
  gsize done = 0;
  int rc = usb_xfer (((Job *) ctx)->usb, io_cancellable (ctx), EP_IN, buf, cap, &done, timeout);

  *got = done;
  return rc;
}

static bool
gx_is_cancelled (void *ctx)
{
  Job *job = ctx;

  return !g_atomic_int_get (&job->closing) && g_cancellable_is_cancelled (job->io);
}

/* Ending the TLS session must reach the sensor even after a cancel: one left
 * mid-handshake ignores plain commands until told otherwise. */
static GCancellable *
io_cancellable (Job *job)
{
  return g_atomic_int_get (&job->closing) ? NULL : job->io;
}

static void
on_action_cancelled (GCancellable *cancellable, Job *job)
{
  if (!g_atomic_int_get (&job->ignore_cancel))
    g_cancellable_cancel (job->io);
}

static void
on_abort (GCancellable *cancellable, Job *job)
{
  g_cancellable_cancel (job->io);
}

static GError *
gx_error (int rc)
{
  if (rc == GX_ERR_CANCELLED)
    return g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
  if (rc == GX_ERR_TLS)
    return fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                     "TLS handshake failed: wrong or stale key in "
                                     PSK_PATH "?");
  if (rc == GX_ERR_IO)
    return fpi_device_error_new (FP_DEVICE_ERROR_PROTO);
  return fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL, "goodix5125: %s",
                                   gx_strerror (rc));
}

/* Progress/status calls must run on the device's main context. */
typedef struct
{
  FpDevice *dev;
  guint     serial;
  int       stage;
  GError   *retry;
  int       finger;          /* FpFingerStatusFlags, or -1 */
  gboolean  result;          /* report the match result instead */
} Note;

static void complete_job (FpDevice *dev, Job *job, GError *error);

static gboolean
note_cb (gpointer data)
{
  Note *n = data;
  FpiDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (n->dev);

  /* The action may have been cancelled meanwhile. */
  if (!self->current || self->current->serial != n->serial)
    g_clear_error (&n->retry);
  else if (n->result)
    complete_job (n->dev, self->current, NULL);   /* reports, then ends */
  else if (n->finger >= 0)
    fpi_device_report_finger_status (n->dev, n->finger);
  else
    fpi_device_enroll_progress (n->dev, n->stage, NULL, n->retry);
  g_object_unref (n->dev);
  g_free (n);
  return G_SOURCE_REMOVE;
}

static void
post (Job *job, int stage, GError *retry, int finger)
{
  Note *n;

  /* No action is running during open; nothing to report to. */
  if (job->kind == JOB_OPEN)
    {
      g_clear_error (&retry);
      return;
    }
  n = g_new0 (Note, 1);

  n->dev = g_object_ref (FP_DEVICE (job->self));
  n->serial = job->serial;
  n->stage = stage;
  n->retry = retry;
  n->finger = finger;
  g_main_context_invoke (job->self->main_ctx, note_cb, n);
}

/* Ends the verify/identify action with its result while the worker still
 * waits for the finger to lift. */
static void
post_result (Job *job)
{
  Note *n = g_new0 (Note, 1);

  n->dev = g_object_ref (FP_DEVICE (job->self));
  n->serial = job->serial;
  n->finger = -1;
  n->result = TRUE;
  g_main_context_invoke (job->self->main_ctx, note_cb, n);
}

typedef struct
{
  gx_dev   dev;
  gx_info  info;
  uint16_t bg[GX_IMG_PIXELS];
  uint16_t fp[GX_IMG_PIXELS];
} Session;

#define BG_ATTEMPTS 10

static int
session_open (Job *job, Session *s)
{
  FpiDeviceGoodix5125 *self = job->self;
  const char *path = g_getenv ("GOODIX5125_PSK") ? : PSK_PATH;
  gx_transport t = { t_write, t_read, job, true };
  uint8_t psk[GX_PSK_LEN];
  int rc;

  gx_dev_init (&s->dev, &t);
  s->dev.log = gx_log;
  s->dev.trace = g_getenv ("GOODIX5125_TRACE") != NULL;
  s->dev.cancelled = gx_is_cancelled;
  s->dev.cancel_ctx = job;

  rc = gx_psk_load (path, psk);
  if (rc < 0)
    {
      g_warning ("goodix5125: cannot read the sensor key %s", path);
      return rc;
    }
  rc = gx_sensor_open (&s->dev, &s->info, psk);
  if ((rc == GX_ERR_TIMEOUT || rc == GX_ERR_PROTOCOL) && !gx_is_cancelled (job))
    {
      g_warning ("goodix5125: sensor not answering (%s), recovering it", gx_strerror (rc));
      gx_sensor_unstick (&s->dev);
      rc = gx_sensor_open (&s->dev, &s->info, psk);
    }
  OPENSSL_cleanse (psk, sizeof (psk));
  if (rc < 0)
    return rc;

  /* The background is subtracted from every capture, so it must not contain
   * a finger. A finger still resting from the previous touch is common when
   * actions follow each other quickly; fall back to the last clean one. */
  for (int attempt = 0; attempt < BG_ATTEMPTS; attempt++)
    {
      rc = gx_sensor_background (&s->dev, &s->info, s->bg);
      if (rc < 0)
        return rc;
      if (gx_image_is_empty (s->bg))
        {
          memcpy (self->bg, s->bg, sizeof (self->bg));
          memcpy (self->fdt_level, s->info.fdt_level, sizeof (self->fdt_level));
          self->bg_valid = TRUE;
          return GX_OK;
        }
      g_debug ("background attempt %d shows a finger", attempt);
      if (self->bg_valid)
        break;
      if (attempt == 0)
        post (job, 0, NULL, FP_FINGER_STATUS_PRESENT);
      if (gx_is_cancelled (job))
        return GX_ERR_CANCELLED;
      g_usleep (300 * 1000);
    }
  if (!self->bg_valid)
    return GX_ERR_DEVICE;

  memcpy (s->bg, self->bg, sizeof (s->bg));
  memcpy (s->info.fdt_level, self->fdt_level, sizeof (s->info.fdt_level));
  s->info.fdt_level_valid = true;
  return GX_OK;
}

/* Waits for a touch and turns it into a template. */
static int
capture (Job *job, Session *s, gx_template *tpl, int *quality)
{
  gint64 t0;
  int rc;

  post (job, 0, NULL, FP_FINGER_STATUS_NEEDED);
  rc = gx_sensor_wait_finger (&s->dev, &s->info, 0);
  if (rc < 0)
    return rc;
  post (job, 0, NULL, FP_FINGER_STATUS_PRESENT);

  t0 = g_get_monotonic_time ();
  rc = gx_sensor_capture (&s->dev, s->fp);
  if (rc < 0)
    return rc;
  gx_image_prepare (s->fp, s->bg, tpl->img);
  /* A partial touch equalises into noise that matches nothing: ask again
   * rather than report a failed match. */
  double coverage = gx_image_coverage_raw (s->fp, s->bg);
  if (gx_image_is_empty (s->fp) || coverage < GX_MIN_COVERAGE)
    *quality = 0;
  else
    *quality = gx_template_quality (tpl);
  g_debug ("capture: coverage %.2f, %d keypoints, %" G_GINT64_FORMAT " ms",
           coverage, *quality, (g_get_monotonic_time () - t0) / 1000);
  return GX_OK;
}

/* The next touch must be a new one, not the same finger still resting. */
static int
wait_lift (Job *job, Session *s, unsigned timeout_ms)
{
  int rc = gx_sensor_wait_lift (&s->dev, &s->info, timeout_ms);

  post (job, 0, NULL, FP_FINGER_STATUS_NONE);
  return rc == GX_ERR_TIMEOUT ? GX_OK : rc;
}

/* GOODIX5125_DUMP=dir writes every capture as PGM, for debugging only:
 * these are fingerprint images. */
static void
dump (const char *what, const Session *s, const gx_template *tpl)
{
  static int counter;
  const char *dir = g_getenv ("GOODIX5125_DUMP");
  g_autofree char *path = NULL;

  if (!dir)
    return;
  path = g_strdup_printf ("%s/%s-%d-%03d.pgm", dir, what, getpid (), counter);
  gx_pgm_write (path, tpl->img, GX_CROP_W, GX_CROP_H);
  g_free (path);
  path = g_strdup_printf ("%s/%s-%d-%03d-raw.pgm", dir, what, getpid (), counter);
  gx_pgm_write16 (path, s->fp, GX_IMG_W, GX_IMG_H);
  g_free (path);
  path = g_strdup_printf ("%s/%s-%d-%03d-bg.pgm", dir, what, getpid (), counter++);
  gx_pgm_write16 (path, s->bg, GX_IMG_W, GX_IMG_H);
}

static GArray *
print_templates (FpPrint *print)
{
  g_autoptr(GVariant) data = NULL;
  g_autoptr(GVariant) blob = NULL;
  const guint8 *bytes;
  gsize len = 0;
  guint16 version;
  GArray *out;

  g_object_get (print, "fpi-data", &data, NULL);
  if (!data || !g_variant_is_of_type (data, G_VARIANT_TYPE ("(qay)")))
    return NULL;
  g_variant_get (data, "(q@ay)", &version, &blob);
  bytes = g_variant_get_fixed_array (blob, &len, 1);
  if (version != PRINT_VERSION || len == 0 || len % sizeof (gx_template))
    return NULL;

  out = g_array_sized_new (FALSE, FALSE, sizeof (gx_template), len / sizeof (gx_template));
  g_array_append_vals (out, bytes, len / sizeof (gx_template));
  return out;
}

static int
run_enroll (Job *job, Session *s)
{
  job->templates = g_array_new (FALSE, FALSE, sizeof (gx_template));

  while (job->templates->len < ENROLL_STAGES)
    {
      gx_template tpl;
      int quality = 0, rc;

      rc = capture (job, s, &tpl, &quality);
      if (rc == GX_OK)
        rc = wait_lift (job, s, LIFT_TIMEOUT_MS);
      if (rc < 0)
        return rc;
      dump ("enroll", s, &tpl);

      if (quality < GX_MIN_KEYPOINTS)
        {
          g_debug ("enroll: poor capture (%d keypoints)", quality);
          post (job, job->templates->len,
                fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER), -1);
          continue;
        }

      /* No overlap check against earlier views: the sensor sees ~4x5 mm, so
       * natural touches mostly land on new skin.  More views cover more of
       * the finger for gx_match_set(). */
      g_array_append_val (job->templates, tpl);
      post (job, job->templates->len, NULL, -1);
    }
  return GX_OK;
}

/* The action has ended with the result (note_cb), so nobody waits for this.
 * On a miss the client starts a new action at once, which would capture the
 * same resting finger again: the next job's worker waits for this one
 * (worker_lock) and with it for the lift. Only close cuts it short. */
static int
wait_after_result (Job *job, Session *s)
{
  g_atomic_int_set (&job->ignore_cancel, 1);
  return wait_lift (job, s, AFTER_MATCH_LIFT_MS);
}

static int
run_match (Job *job, Session *s)
{
  gx_template probe;
  int quality = 0, best = -1;
  gint64 t0;
  int rc = capture (job, s, &probe, &quality);

  if (rc < 0)
    return rc;
  dump ("probe", s, &probe);
  if (quality < GX_MIN_KEYPOINTS)
    {
      job->retry = fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER);
      post_result (job);
      return wait_after_result (job, s);
    }

  t0 = g_get_monotonic_time ();
  for (guint i = 0; i < job->gallery->len; i++)
    {
      FpPrint *print = g_ptr_array_index (job->gallery, i);
      g_autoptr(GArray) templates = print_templates (print);
      int score;

      if (!templates)
        continue;
      score = gx_match_set ((const gx_template *) templates->data, templates->len, &probe);
      g_debug ("match: print %u scores %d (threshold %d)", i, score,
               GX_MATCH_THRESHOLD);
      if (score >= GX_MATCH_THRESHOLD && score > best)
        {
          best = score;
          g_set_object (&job->matched, print);
        }
    }
  job->match_ok = job->matched != NULL;
  g_debug ("match: %" G_GINT64_FORMAT " ms", (g_get_monotonic_time () - t0) / 1000);

  /* Answer while the finger is still down; unlocking should not wait for
   * the lift. */
  post_result (job);
  return wait_after_result (job, s);
}

static void
worker (GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  Job *job = task_data;
  Session *s = g_new0 (Session, 1);
  gulong on_cancel = 0, on_close;
  int rc;

  if (job->cancellable)
    on_cancel = g_cancellable_connect (job->cancellable, G_CALLBACK (on_action_cancelled),
                                       job, NULL);
  on_close = g_cancellable_connect (job->abort, G_CALLBACK (on_abort), job, NULL);

  g_mutex_lock (&job->self->worker_lock);
  rc = session_open (job, s);
  /* A finger resting on the sensor at open time only delays the background. */
  if (job->kind == JOB_OPEN && rc == GX_ERR_DEVICE)
    rc = GX_OK;
  if (rc == GX_OK && job->kind == JOB_ENROLL)
    rc = run_enroll (job, s);
  else if (rc == GX_OK && job->kind != JOB_OPEN)
    rc = run_match (job, s);
  g_atomic_int_set (&job->closing, 1);
  gx_sensor_close (&s->dev);
  g_free (s);
  g_mutex_unlock (&job->self->worker_lock);

  g_cancellable_disconnect (job->abort, on_close);
  if (on_cancel)
    g_cancellable_disconnect (job->cancellable, on_cancel);

  if (rc < 0)
    g_task_return_error (task, gx_error (rc));
  else
    g_task_return_boolean (task, TRUE);
}

static void do_close (FpDevice *dev);

static void
report_result (FpDevice *dev, Job *job)
{
  if (job->reported)
    return;
  job->reported = TRUE;
  if (job->kind == JOB_IDENTIFY)
    fpi_device_identify_report (dev, job->matched, NULL, g_steal_pointer (&job->retry));
  else if (job->retry)
    fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL, g_steal_pointer (&job->retry));
  else
    fpi_device_verify_report (dev, job->match_ok ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL,
                              NULL, NULL);
}

/* Reports the job's result as the end of the current action. */
static void
complete_job (FpDevice *dev, Job *job, GError *error)
{
  FpiDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (dev);

  self->current = NULL;
  switch (job->kind)
    {
    case JOB_OPEN:
      if (error)
        {
          /* libfprint opened the USB device for us but leaves it open on
           * failure: every later open would fail with "already open". */
          g_usb_device_release_interface (job->usb, USB_IFACE, 0, NULL);
          g_usb_device_close (job->usb, NULL);
          g_clear_pointer (&self->main_ctx, g_main_context_unref);
        }
      fpi_device_open_complete (dev, error);
      break;

    case JOB_ENROLL:
      {
        FpPrint *print = NULL;

        if (!error)
          {
            GVariant *blob, *data;

            fpi_device_get_enroll_data (dev, &print);
            blob = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, job->templates->data,
                                              job->templates->len * sizeof (gx_template), 1);
            data = g_variant_new ("(q@ay)", (guint16) PRINT_VERSION, blob);
            fpi_print_set_type (print, FPI_PRINT_RAW);
            fpi_print_set_device_stored (print, FALSE);
            g_object_set (print, "fpi-data", data, NULL);
            g_object_ref (print);
          }
        fpi_device_enroll_complete (dev, print, error);
        break;
      }

    case JOB_VERIFY:
      if (!error)
        report_result (dev, job);
      fpi_device_verify_complete (dev, error);
      break;

    case JOB_IDENTIFY:
      if (!error)
        report_result (dev, job);
      fpi_device_identify_complete (dev, error);
      break;
    }
}

static void
job_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  FpDevice *dev = FP_DEVICE (source);
  FpiDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (dev);
  Job *job = g_task_get_task_data (G_TASK (res));
  GError *error = NULL;

  g_task_propagate_boolean (G_TASK (res), &error);
  if (self->current == job)
    complete_job (dev, job, error);
  else
    g_clear_error (&error);      /* already completed by dev_cancel */

  if (--self->workers == 0 && self->close_pending)
    do_close (dev);
}

static void
start_job (FpDevice *dev, JobKind kind, GPtrArray *gallery)
{
  FpiDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (dev);
  Job *job = g_new0 (Job, 1);
  GCancellable *cancellable = kind == JOB_OPEN ? NULL : fpi_device_get_cancellable (dev);
  GTask *task;

  job->self = self;
  job->kind = kind;
  job->serial = ++self->serial;
  job->io = g_cancellable_new ();
  job->abort = g_object_ref (self->abort);
  job->usb = fpi_device_get_usb_device (dev);
  job->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
  job->gallery = gallery;

  self->current = job;
  self->workers++;
  task = g_task_new (dev, NULL, job_done, NULL);
  g_task_set_task_data (task, job, (GDestroyNotify) job_free);
  g_task_run_in_thread (task, worker);
  g_object_unref (task);
}

/* ---- FpDevice vfuncs ------------------------------------------------------ */

static void
dev_open (FpDevice *dev)
{
  FpiDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (dev);
  GError *error = NULL;

  self->main_ctx = g_main_context_ref_thread_default ();

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (dev), USB_IFACE, 0, &error))
    {
      fpi_device_open_complete (dev, error);
      return;
    }
  start_job (dev, JOB_OPEN, NULL);
}

static void
do_close (FpDevice *dev)
{
  FpiDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (dev);
  GError *error = NULL;

  self->close_pending = FALSE;
  g_clear_object (&self->abort);
  self->abort = g_cancellable_new ();
  g_usb_device_release_interface (fpi_device_get_usb_device (dev), USB_IFACE, 0, &error);
  g_clear_pointer (&self->main_ctx, g_main_context_unref);
  fpi_device_close_complete (dev, error);
}

static void
dev_close (FpDevice *dev)
{
  FpiDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (dev);

  /* A worker may still be talking to the sensor: stop it, then close. */
  if (self->workers > 0)
    {
      self->close_pending = TRUE;
      g_cancellable_cancel (self->abort);
    }
  else
    {
      do_close (dev);
    }
}

static void
dev_cancel (FpDevice *dev)
{
  FpiDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (dev);

  /* The worker sees the cancellable on its own and stops talking to the
   * sensor within a poll slice; the action ends now. */
  if (self->current && self->current->kind != JOB_OPEN)
    complete_job (dev, self->current,
                  g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled"));
}

static void
dev_enroll (FpDevice *dev)
{
  start_job (dev, JOB_ENROLL, NULL);
}

static void
dev_verify (FpDevice *dev)
{
  GPtrArray *gallery = g_ptr_array_new_with_free_func (g_object_unref);
  FpPrint *print = NULL;

  fpi_device_get_verify_data (dev, &print);
  g_ptr_array_add (gallery, g_object_ref (print));
  start_job (dev, JOB_VERIFY, gallery);
}

static void
dev_identify (FpDevice *dev)
{
  GPtrArray *prints = NULL;

  fpi_device_get_identify_data (dev, &prints);
  start_job (dev, JOB_IDENTIFY, g_ptr_array_ref (prints));
}

static const FpIdEntry id_table[] = {
  { .vid = GX_VID, .pid = GX_PID },
  { .vid = 0, .pid = 0 },
};

static void
fpi_device_goodix5125_init (FpiDeviceGoodix5125 *self)
{
  g_mutex_init (&self->worker_lock);
  self->abort = g_cancellable_new ();
}

static void
fpi_device_goodix5125_finalize (GObject *object)
{
  FpiDeviceGoodix5125 *self = FPI_DEVICE_GOODIX5125 (object);

  g_mutex_clear (&self->worker_lock);
  g_clear_object (&self->abort);
  G_OBJECT_CLASS (fpi_device_goodix5125_parent_class)->finalize (object);
}

static void
fpi_device_goodix5125_class_init (FpiDeviceGoodix5125Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  G_OBJECT_CLASS (klass)->finalize = fpi_device_goodix5125_finalize;

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "Goodix 5125 Fingerprint Sensor (Huawei)";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = ENROLL_STAGES;
  dev_class->temp_hot_seconds = -1;

  dev_class->open = dev_open;
  dev_class->close = dev_close;
  dev_class->enroll = dev_enroll;
  dev_class->verify = dev_verify;
  dev_class->identify = dev_identify;
  dev_class->cancel = dev_cancel;

  fpi_device_class_auto_initialize_features (dev_class);
}
