[CCode (cprefix = "", lower_case_cprefix = "", cheader_filename = "selinux/selinux.h")]
namespace Selinux {
    int lgetfilecon (string path, out string context);
    int lsetfilecon (string path, string context);
}
