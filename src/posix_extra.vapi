[CCode (cprefix = "", lower_case_cprefix = "")]
namespace Posix {
    // https://bugzilla.gnome.org/show_bug.cgi?id=794651
    [CCode (cheader_filename = "sys/sysmacros.h", cname="major")]
    uint hack_major (dev_t dev);
}

