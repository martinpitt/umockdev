[CCode (cheader_filename = "sys/utsname.h")]
namespace GLibc {
    [CCode (cname = "struct utsname", destroy_function="")]
    public struct Utsname {
        unowned string sysname;
        unowned string nodename;
        unowned string release;
        unowned string version;
        unowned string machine;
    }

    [CCode (cname = "uname")]
    public void uname (out Utsname buf);
}
