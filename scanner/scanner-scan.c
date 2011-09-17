#include "scanner-scan.h"

#include <glib/gstdio.h>
#include <stdlib.h>

#include "filetree.h"
#include "input.h"
#include "nproc.h"

static struct file_data empty;

static int open_plugin(const char *raw, const char *display,
                       struct input_ops **ops,
                       struct input_handle **ih,
                       FILE **file)
{
    int result;

    *ops = input_get_ops(raw);
    if (!(*ops)) {
        fprintf(stderr, "No plugin found for file '%s'\n", display);
        return 1;
    }
    *ih = (*ops)->handle_init();

    *file = g_fopen(raw, "rb");
    if (!(*file)) {
        fprintf(stderr, "Error opening file '%s'\n", display);
        return 1;
    }
    result = (*ops)->open_file(*ih, *file, raw);
    if (result) {
        fprintf(stderr, "Error opening file '%s'\n", display);
        return 1;
    }
    return 0;
}

static void init_and_get_number_of_frames(gpointer user, gpointer user_data)
{
    struct filename_list_node *fln = (struct filename_list_node *) user;
    struct file_data *fd;

    struct input_ops *ops = NULL;
    struct input_handle *ih = NULL;
    FILE *file = NULL;

    (void) user_data;
    fln->d = g_malloc(sizeof(struct file_data));
    memcpy(fln->d, &empty, sizeof empty);
    fd = (struct file_data *) fln->d;

    fd->mutex = g_mutex_new();

    if (open_plugin(fln->fr->raw, fln->fr->display, &ops, &ih, &file)) {
        goto free;
    }

    fd->number_of_frames = ops->get_total_frames(ih);

  free:
    if (file) ops->close_file(ih, file);
    if (ih) ops->handle_destroy(&ih);
}

static void sum_frames(gpointer user, gpointer user_data)
{
    struct filename_list_node *fln = (struct filename_list_node *) user;
    struct file_data *fd = (struct file_data *) fln->d;
    guint64 *fc = (guint64 *) user_data;

    g_mutex_lock(fd->mutex);
    fc[0] += fd->number_of_elapsed_frames;
    fc[1] += fd->number_of_frames;
    g_mutex_unlock(fd->mutex);
}


static void init_state_and_scan_work_item(gpointer user, gpointer user_data)
{
    struct filename_list_node *fln = (struct filename_list_node *) user;
    struct file_data *fd = (struct file_data *) fln->d;

    struct input_ops* ops = NULL;
    struct input_handle* ih = NULL;
    FILE *file = NULL;

    int result;
    float *buffer = NULL;
    size_t nr_frames_read;

    (void) user_data;
    if (open_plugin(fln->fr->raw, fln->fr->display, &ops, &ih, &file)) {
        goto free;
    }

    fd->st = ebur128_init(ops->get_channels(ih),
                          ops->get_samplerate(ih),
                          EBUR128_MODE_I);

    result = ops->allocate_buffer(ih);
    if (result) abort();
    buffer = ops->get_buffer(ih);

    while ((nr_frames_read = ops->read_frames(ih))) {
        g_mutex_lock(fd->mutex);
        fd->number_of_elapsed_frames += nr_frames_read;
        g_mutex_unlock(fd->mutex);
        result = ebur128_add_frames_float(fd->st, buffer, nr_frames_read);
        if (result) abort();
    }
    g_mutex_lock(fd->mutex);
    if (fd->number_of_elapsed_frames != fd->number_of_frames) {
        fprintf(stderr, "Warning: Could not read full file"
                        " or determine right length!\n");
        fd->number_of_frames = fd->number_of_elapsed_frames;
    }
    g_mutex_unlock(fd->mutex);
    ebur128_loudness_global(fd->st, &fd->loudness);

  free:
    if (ih) ops->free_buffer(ih);
    if (file) ops->close_file(ih, file);
    if (ih) ops->handle_destroy(&ih);
}

static void init_state_and_scan(gpointer user, gpointer user_data)
{
    GThreadPool *pool = (GThreadPool *) user_data;
    g_thread_pool_push(pool, user, NULL);
}

static void destroy_state(gpointer user, gpointer user_data)
{
    struct filename_list_node *fln = (struct filename_list_node *) user;
    struct file_data *fd = (struct file_data *) fln->d;

    (void) user_data;
    if (fd->st) {
        ebur128_destroy(&fd->st);
    }
    g_mutex_free(fd->mutex);
}


static void print_file_data(gpointer user, gpointer user_data)
{
    struct filename_list_node *fln = (struct filename_list_node *) user;
    struct file_data *fd = (struct file_data *) fln->d;

    (void) user_data;
    print_utf8_string(fln->fr->display);
    g_print(", %" G_GUINT64_FORMAT ", %f\n", fd->number_of_frames, fd->loudness);
}

static gpointer print_progress_bar(gpointer data)
{
    GSList *files = (GSList *) data;
    guint64 fc[] = {0, 1};
    int percent, bars, i;
    static char progress_bar[81];

    while (fc[0] != fc[1]) {
        fc[0] = fc[1] = 0;
        g_slist_foreach(files, sum_frames, &fc);
        if (fc[1] == 0) break;
        bars = fc[0] * 73 / fc[1];
        percent = fc[0] * 100 / fc[1];
        progress_bar[0] = '[';
        for (i = 1; i <= bars; ++i) {
            progress_bar[i] = '#';
        }
        for (; i < 74; ++i) {
            progress_bar[i] = ' ';
        }
        if (percent >= 0 && percent <= 100)
            sprintf(&progress_bar[74], "] %3d%%", percent);
        fprintf(stderr, "%s\r", progress_bar);
        g_usleep(G_USEC_PER_SEC / 10);
    }
    return NULL;
}

void loudness_scan(GSList *files)
{
    GThreadPool *pool = g_thread_pool_new(init_state_and_scan_work_item,
                                          NULL, nproc(), FALSE, NULL);
    GThread *progress_bar_thread;

    g_slist_foreach(files, init_and_get_number_of_frames, NULL);
    g_slist_foreach(files, init_state_and_scan, pool);
    progress_bar_thread = g_thread_create(print_progress_bar,
                                          files, TRUE, NULL);
    g_thread_pool_free(pool, FALSE, TRUE);
    g_thread_join(progress_bar_thread);

    g_slist_foreach(files, print_file_data, NULL);
    g_slist_foreach(files, destroy_state, NULL);
}