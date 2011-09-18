#include "scanner-tag.h"

#include "parse_args.h"
#include "scanner-common.h"
#include "nproc.h"

#define REFERENCE_LEVEL -18.0

extern gboolean verbose;
static gboolean track = FALSE;
static gboolean dry_run = FALSE;

static GOptionEntry entries[] =
{
    { "track", 't', 0, G_OPTION_ARG_NONE, &track, NULL, NULL },
    { "dry-run", 'n', 0, G_OPTION_ARG_NONE, &dry_run, NULL, NULL },
    { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, 0 }
};

static double clamp_rg(double x) {
  if (x < -51.0) return -51.0;
  else if (x > 51.0) return 51.0;
  else return x;
}

static void print_file_data(gpointer user, gpointer user_data)
{
    struct filename_list_node *fln = (struct filename_list_node *) user;
    struct file_data *fd = (struct file_data *) fln->d;

    (void) user_data;
    if (fd->scanned) {
        double track_gain = clamp_rg(REFERENCE_LEVEL - fd->loudness);
        g_print("%7.2f dB, %7.2f dB, %10.6f, %10.6f",
                fd->gain_album,
                track_gain,
                fd->peak_album,
                fd->peak);
        if (fln->fr->display[0]) {
            g_print(", ");
            print_utf8_string(fln->fr->display);
        }
        putchar('\n');
    }
}

void loudness_tag(GSList *files)
{
    struct scan_opts opts = {FALSE, "sample"};
    GThreadPool *pool;
    GThread *progress_bar_thread;

    pool = g_thread_pool_new(init_state_and_scan_work_item,
                             &opts, nproc(), FALSE, NULL);
    g_slist_foreach(files, init_and_get_number_of_frames, NULL);
    g_slist_foreach(files, init_state_and_scan, pool);
    progress_bar_thread = g_thread_create(print_progress_bar,
                                          files, TRUE, NULL);
    g_thread_pool_free(pool, FALSE, TRUE);
    g_thread_join(progress_bar_thread);

    // g_slist_foreach(files, calculate_album_gain_and_peak, NULL);
    clear_line();
    fprintf(stderr, "Album gain, Track gain, Album peak, Track peak\n");
    g_slist_foreach(files, print_file_data, NULL);
    // print_summary(files);
    g_slist_foreach(files, destroy_state, NULL);
}

gboolean loudness_tag_parse(int *argc, char **argv[])
{
    gboolean success = parse_mode_args(argc, argv, entries);
    if (!success) {
        if (*argc == 1) fprintf(stderr, "Missing arguments\n");
        return FALSE;
    }
    return TRUE;
}
