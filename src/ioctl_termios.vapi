[CCode (cprefix = "", lower_case_cprefix = "", cheader_filename = "ioctl_termios.h")]
namespace IoctlTermios {
    [CCode (cname = "is_termios_ioctl")]
    public bool is_termios_ioctl(ulong request);

    [CCode (cname = "get_tcgets_ioctl")]
    public ulong get_tcgets_ioctl();
}
