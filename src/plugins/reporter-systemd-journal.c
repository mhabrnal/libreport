/*
    Copyright (C) 2016  ABRT team
    Copyright (C) 2016  RedHat inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#include "internal_libreport.h"
#include <systemd/sd-journal.h>
#include "problem_report.h"

#define DEFAULT_MESSAGE_ID "1909f1302a5240c895d7c05566100dce"

#define PROBLEM_REPORT_DEFAULT_TEMPLATE \
    "%summary:: %reason%\n"

typedef struct msg_content
{
    unsigned allocated;
    unsigned used;
    struct iovec *data;

} msg_content_t;

static msg_content_t *msg_content_new()
{
    const int init_count = 2;

    msg_content_t *msg_c = xmalloc(sizeof(*msg_c));

    msg_c->data = xmalloc(init_count * sizeof(*(msg_c->data)));

    msg_c->allocated = init_count;
    msg_c->used = 0;

    return msg_c;
}

static struct iovec *msg_content_get_data(msg_content_t *msg_c)
{
    if (!msg_c)
        return NULL;

    return msg_c->data;
}

static unsigned msg_content_get_size(msg_content_t *msg_c)
{
    if (!msg_c)
        return 0;

    return msg_c->used;
}

#define IOVEC_ADD_FIELD(i, key, value, prefix)                 \
    do {                                                       \
        struct iovec *_i = &(i);                               \
        char *_s = xasprintf("%s%s=%s", prefix, key, value);   \
        for (char *c = _s; *c != '='; ++c) *c = toupper(*c);   \
        _i->iov_base = _s;                                     \
        _i->iov_len = strlen(_s);                              \
    } while(false)

static void msg_content_add_ext(msg_content_t *msg_c, const char *key, const char *value, const char *prefix)
{
    if (!msg_c)
        return;

    /* need more space */
    if (msg_c->used >= msg_c->allocated)
    {
        msg_c->allocated += 5;
        msg_c->data = xrealloc(msg_c->data, msg_c->allocated * sizeof(*(msg_c->data)));
    }

    struct iovec *iov = msg_c->data;
    IOVEC_ADD_FIELD(iov[msg_c->used++], key, value, prefix);

    return;
}

static void msg_content_add(msg_content_t *msg_c, const char *key, const char *value)
{
    msg_content_add_ext(msg_c, key, value, /* without prefix */"");
}

static void msg_content_free(msg_content_t *msg_c)
{
    if (!msg_c)
        return;

    struct iovec iov = msg_c->data[0];
    for (int i = 0; i < msg_c->used; iov = msg_c->data[++i])
        free(iov.iov_base);

    free(msg_c->data);
    free(msg_c);

    return;
}


enum {
    DUMP_NONE      = 1 << 0,
    DUMP_ESSENTIAL = 1 << 1,
    DUMP_FULL      = 1 << 2,
};

/* Elements needed by systemd journal messages */
static const char *const fields_default[] = {
    FILENAME_EXECUTABLE     ,
    FILENAME_PID            ,
    FILENAME_EXCEPTION_TYPE ,
    NULL
};

static const char *const fields_essential[] = {
    FILENAME_REASON             ,
    FILENAME_CRASH_FUNCTION     ,
    FILENAME_CMDLINE            ,
    FILENAME_COMPONENT          ,
    FILENAME_PKG_NAME           ,
    FILENAME_PKG_VERSION        ,
    FILENAME_PKG_RELEASE        ,
    FILENAME_PKG_FINGERPRINT    ,
    FILENAME_REPORTED_TO        ,
    FILENAME_TYPE               ,
    FILENAME_UID                ,
    NULL
};

static void msg_content_add_fields(msg_content_t *msg_c, problem_data_t *problem_data,
                                   const char *const *fields)
{
    for (int i = 0; fields[i] != NULL; ++i)
    {
        const char *value = problem_data_get_content_or_NULL(problem_data, fields[i]);
        if (value)
            msg_content_add_ext(msg_c, fields[i], value, "PROBLEM_");
    }

    return;
}

static msg_content_t *
create_journal_message(problem_data_t *problem_data, problem_report_t *pr,
                       const char *msg_id, unsigned dump_opts)
{
    msg_content_t *msg_c = msg_content_new();

    /* mandatory fields */
    msg_content_add(msg_c, "MESSAGE", problem_report_get_summary(pr));
    msg_content_add(msg_c, "MESSAGE_ID", msg_id ? msg_id : DEFAULT_MESSAGE_ID);
    msg_content_add(msg_c, "PRIORITY", "2");

    /* add problem report description into PROBLEM_REPORT field */
    char *description = NULL;
    if (problem_report_get_description(pr) != NULL)
        description = xasprintf("\n%s", problem_report_get_description(pr));

    msg_content_add(msg_c, "PROBLEM_REPORT", description ? description : "");
    free(description);

    if (!(dump_opts & DUMP_FULL))
    {
        msg_content_add_fields(msg_c, problem_data, fields_default);

        /* add defined default fields */
        if (dump_opts & DUMP_ESSENTIAL)
            msg_content_add_fields(msg_c, problem_data, fields_essential);
    }
    /* add all fields from problem directory */
    else
    {
        for (GList *elem = problem_data_get_all_elements(problem_data); elem != NULL; elem = elem->next)
        {
            const problem_item *item = problem_data_get_item_or_NULL(problem_data, elem->data);
            /* add only text elements */
            if (item && (item->flags & CD_FLAG_TXT))
                msg_content_add_ext(msg_c, elem->data, item->content, "PROBLEM_");
        }
    }

    return msg_c;
}

int main(int argc, char **argv)
{
    abrt_init(argv);

    /* I18n */
    setlocale(LC_ALL, "");
#if ENABLE_NLS
    bindtextdomain(PACKAGE, LOCALEDIR);
    textdomain(PACKAGE);
#endif

    /* Can't keep these strings/structs static: _() doesn't support that */
// TODO
    const char *program_usage_string = _(
        "& [-v] [-d DIR]\n"
        "\n"
        "Prints problem information to systemd-journal"
    );
    enum {
        OPT_v = 1 << 0,
        OPT_d = 1 << 1,
        OPT_m = 1 << 2,
        OPT_F = 1 << 3,
        OPT_p = 1 << 4,
        OPT_D = 1 << 5,
    };
    const char *dump_dir_name = ".";
    const char *message_id = DEFAULT_MESSAGE_ID;
    const char *fmt_file = NULL;
    const char *dump = NULL;
    /* Keep enum above and order of options below in sync! */
    struct options program_options[] = {
        OPT__VERBOSE(&g_verbose),
        OPT_STRING('d', NULL,         &dump_dir_name, "DIR"   , _("Problem directory")),
        OPT_STRING('m', "message-id", &message_id,    "STR"   , _("Catalog message id")),
        OPT_STRING('F', NULL        , &fmt_file     , "FILE"  , _("Formatting file for catalog message")),
        OPT_STRING('p', "dump"      , &dump         , "STR"   , _("Dump problem dir into systemd journal fields")),
        OPT_BOOL(  'D', NULL        , NULL                    , _("Debug")),
        OPT_END()
    };
    unsigned opts = parse_opts(argc, argv, program_options, program_usage_string);

    unsigned dump_opt = DUMP_NONE;
    if (opts & OPT_p)
    {
        if (dump && strcmp(dump, "NONE") == 0)
            ;
        else if (dump && strcmp(dump, "ESSENTIAL") == 0)
            dump_opt = DUMP_ESSENTIAL;
        else if (dump && strcmp(dump, "FULL") == 0)
            dump_opt = DUMP_FULL;
        else
        {
            error_msg("Parameter --dump takes NONE|ESSENTIAL|FULL values");
            show_usage_and_die(program_usage_string, program_options);
        }
    }

    export_abrt_envvars(0);

    problem_data_t *problem_data = create_problem_data_for_reporting(dump_dir_name);
    if (!problem_data)
        xfunc_die(); /* create_problem_data_for_reporting already emitted error msg */

    problem_formatter_t *pf = problem_formatter_new();

    if (fmt_file)
    {
        if (problem_formatter_load_file(pf, fmt_file))
            error_msg_and_die("Invalid format file: %s", fmt_file);
    }
    else
    {
        if (problem_formatter_load_string(pf, PROBLEM_REPORT_DEFAULT_TEMPLATE))
            error_msg_and_die("BUG: Invalid default problem report format string");
    }

    problem_report_settings_t report_settings = problem_formatter_get_settings(pf);
    report_settings.prs_shortbt_max_frames = 5;
    report_settings.prs_shortbt_max_text_size = 0; /* always short bt */
    problem_formatter_set_settings(pf, report_settings);

    /* Modify problem_data to meet reporter's needs */
    /* We want to have only binary name in problem report assigned to executable element */
    const char *exe = problem_data_get_content_or_NULL(problem_data, FILENAME_EXECUTABLE);
    char *binary_name = NULL;
    if (exe)
        binary_name = strrchr(exe, '/') + 1;

    if (binary_name)
        problem_data_add_text_noteditable(problem_data, FILENAME_EXECUTABLE, binary_name);

    /* crash_function element is neeeded by systemd journal messages, save ??, if it doesn't exist */
    const char *crash_function = problem_data_get_content_or_NULL(problem_data, FILENAME_CRASH_FUNCTION);
    if (!crash_function)
        problem_data_add_text_noteditable(problem_data, "crash_function", "??");

    /* Generating of problem report */
    problem_report_t *pr = NULL;
    if (problem_formatter_generate_report(pf, problem_data, &pr))
        error_msg_and_die("Failed to format bug report from problem data");

    /* Debug */
    if (opts & OPT_D)
    {
        log("Message: %s\n"
                "\n"
                "%s"
                "\n"
                , problem_report_get_summary(pr)
                , problem_report_get_description(pr)
        );

        problem_data_free(problem_data);
        problem_report_free(pr);
        problem_formatter_free(pf);
        return 0;
    }

    msg_content_t *msg_c = create_journal_message(problem_data, pr, message_id, dump_opt);

    /* post journal message */
    sd_journal_sendv(msg_content_get_data(msg_c), msg_content_get_size(msg_c));

    msg_content_free(msg_c);

    problem_data_free(problem_data);
    problem_formatter_free(pf);
    problem_report_free(pr);

    return 0;
}
