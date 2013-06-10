[CCode (cprefix = "", lower_case_cprefix = "")]
namespace Posix {
#if !VALA_0_20
    // https://bugzilla.gnome.org/show_bug.cgi?id=693411
    [CCode (cheader_filename = "sys/types.h")]
    uint major (dev_t dev);
    [CCode (cheader_filename = "sys/types.h")]
    uint minor (dev_t dev);
#endif

    // https://bugzilla.gnome.org/show_bug.cgi?id=693410
    [CCode (cheader_filename = "stdlib.h", cname="realpath")]
    public string? fixed_realpath (string path, [CCode (array_length = false)] uint8[]? resolved_path = null);
}

