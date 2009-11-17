#include <string.h>
#include <sane/sane.h>
#include <sane/saneopts.h>

#include "scanner.h"


enum {
    READY,
    UPDATE_DEVICES,
    GOT_PAGE_INFO,
    GOT_LINE,
    SCAN_FAILED,
    IMAGE_DONE,
    LAST_SIGNAL
};
static guint signals[LAST_SIGNAL] = { 0, };

typedef struct
{
    Scanner *instance;
    guint sig;
    gpointer data;
} SignalInfo;

typedef enum
{
    STATE_IDLE = 0,
    STATE_GET_OPTION,
    STATE_START,
    STATE_GET_PARAMETERS,
    STATE_READ,
    STATE_CLOSE
} ScanState;

struct ScannerPrivate
{
    GAsyncQueue *scan_queue;
    gint dpi;
};

G_DEFINE_TYPE (Scanner, scanner, G_TYPE_OBJECT);


static gboolean
send_signal (SignalInfo *info)
{
    g_signal_emit (info->instance, signals[info->sig], 0, info->data);
    
    switch (info->sig) {
    case UPDATE_DEVICES:
        {
            GList *iter, *devices = info->data;
            for (iter = devices; iter; iter = iter->next) {
                ScanDevice *device = iter->data;
                g_free (device->name);
                g_free (device->label);
                g_free (device);
            }
            g_list_free (devices);
        }
        break;
    case GOT_PAGE_INFO:
        {
            ScanPageInfo *page_info = info->data;
            g_free (page_info);
        }
        break;
    case GOT_LINE:
        {
            ScanLine *line = info->data;
            g_free(line->data);
            g_free(line);
        }
        break;
    case SCAN_FAILED:
        {
            GError *error = info->data;
            g_error_free (error);
        }
        break;
    default:
    case READY:
    case IMAGE_DONE:
    case LAST_SIGNAL:
        g_assert (info->data == NULL);
        break;
    }
    g_free (info);

    return FALSE;
}


/* Emit signals in main loop */
static void
emit_signal (Scanner *scanner, guint sig, gpointer data)
{
    SignalInfo *info;
    
    info = g_malloc(sizeof(SignalInfo));
    info->instance = scanner;
    info->sig = sig;
    info->data = data;
    g_idle_add ((GSourceFunc) send_signal, info);
}


static void
poll_for_devices (Scanner *scanner)
{
    const SANE_Device **device_list, **device_iter;
    SANE_Status status;
    GList *devices = NULL;

    status = sane_get_devices (&device_list, SANE_FALSE);
    if (status != SANE_STATUS_GOOD) {
        g_warning ("Unable to get SANE devices: Error %s", sane_strstatus(status));
        return;
    }

    for (device_iter = device_list; *device_iter; device_iter++) {
        const SANE_Device *device = *device_iter;
        ScanDevice *scan_device;
        GString *label;
        
        scan_device = g_malloc(sizeof(ScanDevice));

        scan_device->name = g_strdup (device->name);
        label = g_string_new ("");
        g_string_printf (label, "%s %s", device->vendor, device->model);
        scan_device->label = label->str;
        g_string_free (label, FALSE);

        devices = g_list_append (devices, scan_device);
    }

    emit_signal (scanner, UPDATE_DEVICES, devices);
}


static gpointer
scan_thread (Scanner *scanner)
{
    gchar *device;
    SANE_Status status;
    SANE_Handle handle = NULL;
    SANE_Parameters parameters;
    const SANE_Option_Descriptor *option;
    SANE_Int option_index = 0;
    ScanState state = STATE_IDLE;
    SANE_Int bytes_remaining, line_count = 0, n_read;
    SANE_Byte *data;
    SANE_Int version_code;

    status = sane_init (&version_code, NULL);
    if (status != SANE_STATUS_GOOD) {
        g_warning ("Unable to initialize SANE backend: Error %s", sane_strstatus(status));
        return FALSE;
    }
    g_debug ("SANE version %d.%d.%d",
             SANE_VERSION_MAJOR(version_code),
             SANE_VERSION_MINOR(version_code),
             SANE_VERSION_BUILD(version_code));

    while (1) {
        /* Get device to use */
        if (state == STATE_IDLE) {
            GTimeVal timeout = { 1, 0 };
            device = g_async_queue_timed_pop (scanner->priv->scan_queue, &timeout);
        } else if (state != STATE_CLOSE) {
            if (g_async_queue_length (scanner->priv->scan_queue) > 0)
                state = STATE_CLOSE;
        }
        
        switch (state) {
        case STATE_IDLE:
            if (device == NULL) {
                poll_for_devices (scanner);
            } else if (device[0] != '\0') {
                status = sane_open (device, &handle);
                if (status != SANE_STATUS_GOOD) {
                    g_warning ("Unable to get open device: Error %s", sane_strstatus (status));
                    emit_signal (scanner, SCAN_FAILED, g_error_new (SCANNER_TYPE, status, "Unable to connect to scanner"));
                    handle = NULL;
                    state = STATE_CLOSE;
                }
                else {
                    state = STATE_GET_OPTION;
                    option_index = 0;
                }
            }
            break;
            
        case STATE_GET_OPTION:
            option = sane_get_option_descriptor (handle, option_index);
            if (!option) {
                state = STATE_START;
            } else {
                g_debug ("Option %d: name='%s', title='%s'", option_index, option->name, option->title);
                if (option->desc)
                    g_debug ("  Description: %s", option->desc);
                switch (option->constraint_type) {
                case SANE_CONSTRAINT_RANGE:
                    g_debug ("  Range: min=%d, max=%d, quant=%d", option->constraint.range->min, option->constraint.range->max, option->constraint.range->quant);
                    break;
                default:
                    break;
                }
                if (option->name && strcmp (option->name, SANE_NAME_SCAN_RESOLUTION) == 0) {
                    SANE_Int dpi = scanner->priv->dpi;
                    sane_control_option (handle, option_index, SANE_ACTION_SET_VALUE, &dpi, NULL);
                }
                option_index++;
            }
            break;
            
        case STATE_START:
            status = sane_start (handle);
            if (status != SANE_STATUS_GOOD) {
                g_warning ("Unable to start device: Error %s", sane_strstatus (status));
                emit_signal (scanner, SCAN_FAILED, g_error_new (SCANNER_TYPE, status, "Unable to start scan"));
                state = STATE_CLOSE;
            } else {
                state = STATE_GET_PARAMETERS;
            }
            break;
            
        case STATE_GET_PARAMETERS:
            status = sane_get_parameters (handle, &parameters);
            if (status != SANE_STATUS_GOOD) {
                g_warning ("Unable to get device parameters: Error %s", sane_strstatus (status));
                emit_signal (scanner, SCAN_FAILED, g_error_new (SCANNER_TYPE, status, "Error communicating with scanner"));
                state = STATE_CLOSE;
            } else {
                ScanPageInfo *info;
                
                info = g_malloc(sizeof(ScanPageInfo));
                info->width = parameters.pixels_per_line;
                info->height = parameters.lines;
                info->depth = parameters.depth;
                emit_signal (scanner, GOT_PAGE_INFO, info);
                bytes_remaining = parameters.bytes_per_line;
                data = g_malloc(sizeof(SANE_Byte) * bytes_remaining);
                line_count = 0;
                state = STATE_READ;
            }
            break;
            
        case STATE_READ:
            status = sane_read (handle, data, bytes_remaining, &n_read);
            if (status == SANE_STATUS_EOF &&
                parameters.lines == -1 &&
                bytes_remaining == parameters.bytes_per_line) {
                state = STATE_CLOSE;
            } else if (status != SANE_STATUS_GOOD) {
                g_warning ("Unable to read frame from device: Error %s", sane_strstatus (status));
                emit_signal (scanner, SCAN_FAILED, g_error_new (SCANNER_TYPE, status, "Error communicating with scanner"));
                state = STATE_CLOSE;
            } else {
                bytes_remaining -= n_read;
                if (bytes_remaining == 0) {
                    ScanLine *line;

                    line = g_malloc(sizeof(ScanLine));
                    switch (parameters.format) {
                    case SANE_FRAME_GRAY:
                        line->format = LINE_GRAY;
                        break;
                    case SANE_FRAME_RGB:
                        line->format = LINE_RGB;
                        break;
                    case SANE_FRAME_RED:
                        line->format = LINE_RED;
                        break;
                    case SANE_FRAME_GREEN:
                        line->format = LINE_GREEN;
                        break;
                    case SANE_FRAME_BLUE:
                        line->format = LINE_BLUE;
                        break;
                    }
                    line->width = parameters.pixels_per_line;
                    line->depth = parameters.depth;
                    line->data = data;
                    line->data_length = parameters.bytes_per_line;
                    line->number = line_count;
                    emit_signal (scanner, GOT_LINE, line);

                    line_count++;
                    if (parameters.lines > 0 && line_count == parameters.lines) {
                        data = NULL;
                        state = STATE_CLOSE;
                    } else {
                        bytes_remaining = parameters.bytes_per_line;
                        data = g_malloc(sizeof(SANE_Byte) * bytes_remaining);
                    }
                }
            }
            break;
            
        case STATE_CLOSE:
            emit_signal (scanner, IMAGE_DONE, NULL);
            if (handle)
                sane_close (handle);
            handle = NULL;
            g_free (data);
            data = NULL;
            g_free (device);
            device = NULL;
            state = STATE_IDLE;
            emit_signal (scanner, READY, NULL);            
            break;
        }
    }
    
    return NULL;
}


Scanner *
scanner_new ()
{
    return g_object_new (SCANNER_TYPE, NULL);
}


void
scanner_scan (Scanner *scanner, const char *device, gint dpi_)
{
    scanner->priv->dpi = dpi_;
    g_async_queue_push (scanner->priv->scan_queue, g_strdup (device));    
}


void
scanner_cancel (Scanner *scanner)
{
    g_async_queue_push (scanner->priv->scan_queue, "");
}


void scanner_free (Scanner *scanner)
{
    g_object_unref (scanner);
    sane_exit (); // crashes for some reason
}


static void
scanner_class_init (ScannerClass *klass)
{
    signals[READY] =
        g_signal_new ("ready",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (ScannerClass, ready),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);
    signals[UPDATE_DEVICES] =
        g_signal_new ("update-devices",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (ScannerClass, update_devices),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__POINTER,
                      G_TYPE_NONE, 1, G_TYPE_POINTER);
    signals[GOT_PAGE_INFO] =
        g_signal_new ("got-page-info",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (ScannerClass, got_page_info),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__POINTER,
                      G_TYPE_NONE, 1, G_TYPE_POINTER);
    signals[GOT_LINE] =
        g_signal_new ("got-line",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (ScannerClass, got_line),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__POINTER,
                      G_TYPE_NONE, 1, G_TYPE_POINTER);
    signals[SCAN_FAILED] =
        g_signal_new ("scan-failed",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (ScannerClass, scan_failed),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__POINTER,
                      G_TYPE_NONE, 1, G_TYPE_POINTER);
    signals[IMAGE_DONE] =
        g_signal_new ("image-done",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (ScannerClass, image_done),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);

    g_type_class_add_private (klass, sizeof (ScannerPrivate));
}


static void
scanner_init (Scanner *scanner)
{
    GError *error = NULL;
    
    scanner->priv = G_TYPE_INSTANCE_GET_PRIVATE (scanner, SCANNER_TYPE, ScannerPrivate);
    
    scanner->priv->scan_queue = g_async_queue_new ();
    g_thread_create ((GThreadFunc) scan_thread, scanner, FALSE, &error);
    if (error) {
        g_critical ("Unable to create thread: %s", error->message);
        g_error_free (error);
    }
}